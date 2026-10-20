#!/usr/bin/env python3
"""Restart a Wayland wall VNC server whose screencast stream has stopped (stdlib only).

The KDE wall tiles use TigerVNC's w0vncserver, which reads frames from a
PipeWire screencast stream that KWin provides through the portal. On k72 an
i915 GPU hang reset KWin's GL context on 2026-10-20, and KWin's stream stopped
delivering frames. w0vncserver doesn't notice a stream that stops or disconnects,
so it served its last frame indefinitely. This watchdog runs next to it in the
desktop session (`wall-vnc-watchdog.service`, see docs/remote-builders.md):

- It holds one shared, view-only RFB connection to the server and asks for
  incremental updates, at most one every --poll seconds. A live Plasma desktop
  changes at least once a minute, because the panel clock does.
- It restarts the server's user unit when no update has arrived for --stale
  seconds, or soon after the kernel logs an i915 "GPU HANG" or KWin logs a
  graphics reset. A new server opens a new portal session (the restore token
  keeps it silent), which gets a fresh stream.
- Restarts are rate-limited: at most one per --grace seconds, and while frames
  don't come back the wait doubles up to --max-backoff, so a GPU-hang loop
  can't make it thrash. `systemctl --user restart` keeps the unit's drop-ins,
  including the systemd-inhibit idle/sleep wrapper every lab desktop needs.

  wall_vnc_watchdog.py [--port 5900] [--unit w0vncserver.service] [--stale 300]
                       [--poll 10] [--grace 300] [--max-backoff 1800] [--dry-run]

It only reads the framebuffer: it sends no key or pointer events.
"""
import argparse
import datetime
import re
import socket
import struct
import subprocess
import sys
import time

# The kernel's i915 hang report and KWin's GL reset message. Case-sensitive, so the watchdog's own log lines never match.
RESET_PATTERN = re.compile(r'GPU HANG: |context reset due to GPU hang|A graphics reset attributable')


def log(message):
    print(message, flush=True)


class UpdateReader:
    """Shared RFB 3.8 client (security type None) that only counts framebuffer updates."""

    def __init__(self, port, host='127.0.0.1', timeout=10):
        self.sock = socket.create_connection((host, port), timeout)
        if not self._read(12).startswith(b'RFB '):
            raise RuntimeError('not an RFB server')
        self.sock.sendall(b'RFB 003.008\n')
        count = self._read(1)[0]
        if count == 0:
            raise RuntimeError('server refused: ' + self._read(struct.unpack('>I', self._read(4))[0]).decode(errors='replace'))
        if 1 not in self._read(count):
            raise RuntimeError('no None security type offered')
        self.sock.sendall(b'\x01')
        if struct.unpack('>I', self._read(4))[0] != 0:
            raise RuntimeError('security handshake failed')
        self.sock.sendall(b'\x01')  # shared: never disconnect the wall
        self.width, self.height = struct.unpack('>HH', self._read(4))
        self._read(16)
        self._read(struct.unpack('>I', self._read(4))[0])
        self.sock.sendall(struct.pack('>BxxxBBBBHHHBBBxxx', 0, 32, 24, 0, 1, 255, 255, 255, 16, 8, 0))
        self.sock.sendall(struct.pack('>BxHi', 2, 1, 0))  # raw only, so every rectangle's size is known

    def _read(self, n):
        data = bytearray()
        while len(data) < n:
            chunk = self.sock.recv(min(n - len(data), 1 << 20))
            if not chunk:
                raise ConnectionError('connection closed')
            data += chunk
        return bytes(data)

    def request(self, incremental=True):
        self.sock.sendall(struct.pack('>BBHHHH', 3, int(incremental), 0, 0, self.width, self.height))

    def wait_update(self, timeout):
        """Return the number of changed pixels in the next update, or None if none arrives within timeout."""
        self.sock.settimeout(timeout)
        try:
            kind = self._read(1)[0]
        except socket.timeout:
            return None
        self.sock.settimeout(30)
        if kind == 0:  # FramebufferUpdate
            self._read(1)
            pixels = 0
            for _ in range(struct.unpack('>H', self._read(2))[0]):
                x, y, w, h, encoding = struct.unpack('>HHHHi', self._read(12))
                if encoding != 0:
                    raise RuntimeError(f'unexpected encoding {encoding}')
                self._read(w * h * 4)
                pixels += w * h
            return pixels
        if kind == 1:  # SetColourMapEntries
            self._read(3)
            self._read(struct.unpack('>H', self._read(2))[0] * 6)
        elif kind == 2:  # Bell
            pass
        elif kind == 3:  # ServerCutText
            self._read(3)
            self._read(struct.unpack('>I', self._read(4))[0])
        else:
            raise RuntimeError(f'unexpected server message {kind}')
        return 0

    def close(self):
        self.sock.close()


class Watchdog:
    """Decides when the stream is stale; time, restarts and the reset signal are injected."""

    # A stream that still delivers frames this long after a logged reset survived it.
    SURVIVED = 30

    def __init__(self, stale, grace, max_backoff, now):
        self.stale, self.grace, self.max_backoff, self.now = stale, grace, max_backoff, now
        self.started = self.last_update = now()
        self.failed_restarts = 0
        self.reset_pending = False
        self.reset_at = None

    def reset_logged(self):
        self.reset_pending = True
        self.reset_at = self.now()

    def updated(self):
        self.last_update = self.now()
        self.failed_restarts = 0
        if self.reset_pending and self.last_update - self.reset_at >= self.SURVIVED:
            # k72, 2026-10-20: a second hang logged 3 s after a restart, while the new stream kept running,
            # cost a needless restart when the grace period ended.
            self.reset_pending = False

    def wait_after_restart(self):
        return min(self.grace * 2 ** max(self.failed_restarts - 1, 0), self.max_backoff)

    def reason(self):
        """Return why the server needs a restart now, or None."""
        now = self.now()
        if now - self.started < self.wait_after_restart():
            return None
        if self.reset_pending:
            return 'the journal reports a GPU hang or KWin GL reset'
        if now - self.last_update >= self.stale:
            return f'no framebuffer update for {int(now - self.last_update)} s'
        return None

    def restarted(self):
        self.failed_restarts += 1
        self.reset_pending = False
        self.started = self.last_update = self.now()


def resets_since(since):
    """True when the journal (kernel and KWin) logged a GPU hang or GL reset after `since` (epoch seconds)."""
    result = subprocess.run(['journalctl', '--since', '@' + str(int(since)), '-q', '-o', 'cat', '--no-pager'],
                            capture_output=True, text=True, timeout=30)
    return any(RESET_PATTERN.search(line) for line in result.stdout.splitlines())


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    parser.add_argument('--port', type=int, default=5900)
    parser.add_argument('--unit', default='w0vncserver.service')
    parser.add_argument('--stale', type=int, default=300, help='seconds without any update before a restart')
    parser.add_argument('--poll', type=int, default=10, help='seconds between incremental update requests')
    parser.add_argument('--grace', type=int, default=300, help='minimum seconds between restarts (and after start)')
    parser.add_argument('--max-backoff', type=int, default=1800)
    parser.add_argument('--dry-run', action='store_true', help='log restarts without doing them')
    args = parser.parse_args(argv)

    watchdog = Watchdog(args.stale, args.grace, args.max_backoff, time.monotonic)
    journal_checked = time.time()
    reader = None
    log(f'watching {args.unit} on 127.0.0.1:{args.port}: stale after {args.stale} s')
    while True:
        try:
            if reader is None:
                reader = UpdateReader(args.port)
                reader.request(incremental=False)
                primed = False
            pixels = reader.wait_update(args.poll)
            if pixels:
                if primed:  # the first reply is the full frame we asked for, not a change
                    if watchdog.failed_restarts:
                        log(f'frames are back after {watchdog.failed_restarts} restart(s)')
                    watchdog.updated()
                primed = True
                time.sleep(args.poll)  # coalesce bursts: at most one update per poll interval
                reader.request()
        except (OSError, RuntimeError) as error:
            if reader is not None:
                reader.close()
                reader = None
                log(f'connection lost: {error}')
            time.sleep(args.poll)  # the unit may be restarting; staleness still counts

        checked_at = time.time()
        if resets_since(journal_checked):
            watchdog.reset_logged()
        journal_checked = checked_at
        why = watchdog.reason()
        if why:
            stamp = datetime.datetime.now(datetime.timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ')
            log(f'{stamp} restarting {args.unit}: {why}' + (' (dry run)' if args.dry_run else ''))
            if not args.dry_run:
                subprocess.run(['systemctl', '--user', 'restart', args.unit], check=False, timeout=60)
            if reader is not None:
                reader.close()
                reader = None
            watchdog.restarted()


if __name__ == '__main__':
    sys.exit(main())
