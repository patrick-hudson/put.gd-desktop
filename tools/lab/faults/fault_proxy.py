#!/usr/bin/env python3
"""A small TCP fault proxy for the put.gd fault-injection harness (tools/lab/action_fuzz.py faults).

It sits between put.gd and a loopback fixture server (the custom uploader or the offline S3 oracle) and, per accepted
connection, picks a seeded plan: pass through, or one of the network failures an upload meets.

  reset_before          the connection is accepted and reset at once (nothing reached the server)
  reset_request         the request is cut after N bytes by a reset (the server holds a partial request)
  reset_after_request   the whole request goes through, the server answers, the answer is thrown away and the
                        connection is reset (a lost reply: the server did the work)
  cut_response          N bytes of the answer arrive, then the connection closes (a cut-off reply)
  stall                 after N request bytes nothing moves for `stall` seconds, then the connection closes
  stall_response        the request goes through, the answer is held for `stall` seconds (past the client's timeout)
  dup_response          the first chunk of the answer is sent twice
  slow                  the connection is throttled to `rate` bytes per second in both directions
  latency               only the latency below

Every plan may also carry `latency_ms` plus `jitter_ms` per forwarded chunk. The proxy is byte oriented: it never parses
HTTP, so it works the same for the custom uploader and the S3 wire. A profile (weights per kind and a probability that a
connection is faulted at all) is set while the proxy runs; the plan of connection N is a function of (seed, N, profile) so a
run replays the same failures for the same connection order. Every connection is logged to a JSON-lines file.

A plan is fixed for the life of its connection. A connection takes its plan and registers in one step under the proxy's lock, the
same lock a profile change takes, and every profile change starts a new generation. clear() then retires every open connection
of an earlier generation whose plan is not a plain pass: it resets both of its sockets, wakes its stalls and waits for its pumps to
end. When clear() returns, no request can meet an old fault, whether on a stalled connection or a kept-alive one; if a pump does
not end within the wait, clear() raises ProxyStuck instead of returning.

Library use: Proxy(upstream_port, seed, log_path).start(); proxy.port; proxy.set_profile({...}); proxy.clear(); proxy.stop().
Command line: fault_proxy.py --upstream PORT [--listen PORT] [--seed N] [--log FILE] [--profile JSON]
"""
import argparse
import json
import random
import socket
import struct
import sys
import threading
import time

KINDS = ('pass', 'reset_before', 'reset_request', 'reset_after_request', 'cut_response', 'stall', 'stall_response', 'dup_response', 'slow', 'latency')
CHUNK = 16384


class ProxyStuck(Exception):
    """clear() could not end a faulted connection within its wait."""


class Connection:
    """One accepted connection: its plan, the profile generation it was planned under, its sockets and the thread serving it.
    `retired` wakes every wait of its pumps."""

    def __init__(self, number, plan, client, generation):
        self.number, self.plan, self.client, self.upstream, self.generation = number, plan, client, None, generation
        self.retired = threading.Event()
        self.thread = threading.current_thread()

    def faulted(self):
        return self.plan.kind != 'pass' or self.plan.latency_ms or self.plan.jitter_ms

    def retire(self):
        self.retired.set()
        for sock in (self.client, self.upstream):
            if sock is not None:
                reset(sock)


class Plan:
    def __init__(self, kind='pass', **fields):
        self.kind = kind
        self.c2s_limit = fields.get('c2s_limit', 0)
        self.s2c_limit = fields.get('s2c_limit', 0)
        self.stall = fields.get('stall', 0.0)
        self.rate = fields.get('rate', 0)
        self.latency_ms = fields.get('latency_ms', 0)
        self.jitter_ms = fields.get('jitter_ms', 0)

    def as_dict(self):
        return {k: v for k, v in self.__dict__.items() if v or k == 'kind'}


def reset(sock):
    """Close with an RST rather than a FIN. The other pump of the connection may be blocked reading this socket, and a close alone
    does not wake it (it would wait for the peer or its 60 s idle timeout); a read shutdown first does, and sends nothing."""
    try:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack('ii', 1, 0))
    except OSError:
        pass
    try:
        sock.shutdown(socket.SHUT_RD)
    except OSError:
        pass
    try:
        sock.close()
    except OSError:
        pass


class Proxy:
    def __init__(self, upstream_port, seed=1, log_path=None, listen_port=0, upstream_host='127.0.0.1'):
        self.upstream = (upstream_host, upstream_port)
        self.seed = seed
        self.log_path = log_path
        self.profile = {'fault_rate': 0.0, 'weights': {}}
        self.generation = 0
        self.profile_lock = threading.RLock()  # the profile, its generation and the open connections change together
        self.counter = 0
        self.stats = {}
        self.listener = socket.socket()
        self.listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.listener.bind(('127.0.0.1', listen_port))
        self.listener.listen(128)
        self.port = self.listener.getsockname()[1]
        self.stopped = False
        self.log_lock = threading.Lock()
        self.live = set()
        self.connections = {}   # number -> Connection, while it is open
        self.connections_lock = self.profile_lock
        self.thread = None

    # --- control

    def set_profile(self, profile):
        """profile: {'fault_rate': 0..1, 'weights': {kind: weight}, 'latency_ms': N, 'jitter_ms': N, 'stall': seconds, 'rate': bytes/s}"""
        with self.profile_lock:
            self.profile = dict(profile)
            self.generation += 1
        self.write_log({'event': 'profile', 'profile': profile})

    def clear(self, wait=15.0):
        """No faults from now on: new connections pass, and open ones with a fault plan are retired and their pumps waited for.
        Returns the numbers of the connections it retired."""
        with self.profile_lock:
            self.set_profile({'fault_rate': 0.0, 'weights': {}})
            generation = self.generation
            doomed = [c for c in self.connections.values() if c.generation < generation and c.faulted()]
        for connection in doomed:
            connection.retire()
        end = time.monotonic() + wait
        for connection in doomed:
            connection.thread.join(max(0.0, end - time.monotonic()))
        stuck = [c.number for c in doomed if c.thread.is_alive()]
        self.write_log({'event': 'retired', 'connections': [c.number for c in doomed], 'still_running': stuck})
        if stuck:
            raise ProxyStuck('faulted proxy connections %s still run %.0f s after clear()' % (stuck, wait))
        return [c.number for c in doomed]

    def start(self):
        self.thread = threading.Thread(target=self.accept_loop, daemon=True)
        self.thread.start()
        return self

    def stop(self):
        self.stopped = True
        try:
            self.listener.close()
        except OSError:
            pass
        for sock in list(self.live):
            reset(sock)

    def write_log(self, record):
        if not self.log_path:
            return
        record['t'] = round(time.time(), 3)
        with self.log_lock:
            with open(self.log_path, 'a') as out:
                out.write(json.dumps(record) + '\n')

    # --- planning

    def plan_for(self, number):
        with self.profile_lock:
            profile = dict(self.profile)
        rng = random.Random((self.seed << 20) ^ number)
        plan = Plan(latency_ms=profile.get('latency_ms', 0), jitter_ms=profile.get('jitter_ms', 0))
        weights = profile.get('weights') or {}
        if rng.random() >= profile.get('fault_rate', 0.0) or not weights:
            return plan
        kinds = [k for k in weights if weights[k] > 0]
        if not kinds:
            return plan
        plan.kind = rng.choices(kinds, [weights[k] for k in kinds])[0]
        big = rng.random() < 0.5
        # Offsets: a request is a header (a few hundred bytes) and a body; a cut anywhere in either is interesting.
        plan.c2s_limit = rng.choice((0, 1, 40, 200, 700, 1500)) if not big else rng.randrange(1500, 6 * 1024 * 1024)
        plan.s2c_limit = rng.choice((0, 1, 5, 17, 60, 120, 200))
        plan.stall = profile.get('stall', rng.choice((2, 5, 12)))
        plan.rate = profile.get('rate', rng.choice((50_000, 300_000, 1_000_000, 4_000_000)))
        return plan

    # --- the connection

    def accept_loop(self):
        while not self.stopped:
            try:
                client, _ = self.listener.accept()
            except OSError:
                return
            self.counter += 1
            number = self.counter
            threading.Thread(target=self.serve, args=(client, number), daemon=True).start()

    def serve(self, client, number):
        # The plan is chosen and the connection registered in one step: a clear() either sees it (and retires it) or ran before it (and
        # this plan comes from the cleared profile).
        with self.profile_lock:
            plan = self.plan_for(number)
            connection = Connection(number, plan, client, self.generation)
            self.connections[number] = connection
        self.stats[plan.kind] = self.stats.get(plan.kind, 0) + 1
        record = {'event': 'connection', 'n': number, 'plan': plan.as_dict(), 'generation': connection.generation}
        state = {'c2s': 0, 's2c': 0, 'outcome': 'closed'}
        self.live.add(client)
        try:
            if plan.kind == 'reset_before':
                state['outcome'] = 'reset-before'
                reset(client)
                return
            try:
                upstream = socket.create_connection(self.upstream, timeout=10)
            except OSError as error:
                state['outcome'] = 'upstream-%s' % error
                reset(client)
                return
            self.live.add(upstream)
            connection.upstream = upstream
            if connection.retired.is_set():
                reset(upstream)
            threads = [threading.Thread(target=self.pump, args=(client, upstream, plan, state, 'c2s', connection.retired), daemon=True),
                       threading.Thread(target=self.pump, args=(upstream, client, plan, state, 's2c', connection.retired), daemon=True)]
            for thread in threads:
                thread.start()
            for thread in threads:
                thread.join()
            for sock in (client, upstream):
                try:
                    sock.close()
                except OSError:
                    pass
            self.live.discard(upstream)
        finally:
            self.live.discard(client)
            with self.connections_lock:
                self.connections.pop(number, None)
            if connection.retired.is_set():
                state['outcome'] += ' (retired)'
            record.update(c2s=state['c2s'], s2c=state['s2c'], outcome=state['outcome'])
            self.write_log(record)

    def delay(self, plan, rng, retired):
        if plan.latency_ms or plan.jitter_ms:
            retired.wait(max(0.0, (plan.latency_ms + rng.uniform(-plan.jitter_ms, plan.jitter_ms)) / 1000.0))

    def pump(self, source, sink, plan, state, direction, retired):
        """Copy one direction. `retired` (the connection's) cuts every wait short; the pump then ends on its reset sockets."""
        rng = random.Random(plan.c2s_limit * 31 + plan.s2c_limit + (1 if direction == 'c2s' else 2))
        sent_dup = False
        try:
            source.settimeout(60)
            while True:
                try:
                    data = source.recv(CHUNK)
                except socket.timeout:
                    state['outcome'] = 'idle-timeout'
                    break
                except OSError:
                    break
                if not data:
                    break
                if retired.is_set():
                    break
                self.delay(plan, rng, retired)
                if direction == 'c2s':
                    if plan.kind == 'reset_request' and state['c2s'] + len(data) > plan.c2s_limit:
                        keep = max(0, plan.c2s_limit - state['c2s'])
                        if keep:
                            self.send(sink, data[:keep], plan, rng, retired)
                            state['c2s'] += keep
                        state['outcome'] = 'reset-request'
                        reset(source)
                        reset(sink)
                        break
                    if plan.kind == 'stall' and state['c2s'] + len(data) > plan.c2s_limit:
                        keep = max(0, plan.c2s_limit - state['c2s'])
                        if keep:
                            self.send(sink, data[:keep], plan, rng, retired)
                            state['c2s'] += keep
                        state['outcome'] = 'stalled'
                        retired.wait(plan.stall)
                        # The connection then dies with a reset, as a timed-out path does. A clean close would look to a lenient server like a
                        # request that ended after its headers.
                        reset(source)
                        reset(sink)
                        break
                    self.send(sink, data, plan, rng, retired)
                    state['c2s'] += len(data)
                else:
                    if plan.kind == 'reset_after_request':
                        state['outcome'] = 'reply-dropped'
                        reset(sink)
                        reset(source)
                        break
                    if plan.kind == 'stall_response' and not state.get('held'):
                        state['held'] = True
                        state['outcome'] = 'response-held'
                        retired.wait(plan.stall)
                        if retired.is_set():
                            break
                    if plan.kind == 'cut_response' and state['s2c'] + len(data) > plan.s2c_limit:
                        keep = max(0, plan.s2c_limit - state['s2c'])
                        if keep:
                            self.send(sink, data[:keep], plan, rng, retired)
                            state['s2c'] += keep
                        state['outcome'] = 'response-cut'
                        try:
                            sink.shutdown(socket.SHUT_RDWR)
                        except OSError:
                            pass
                        break
                    self.send(sink, data, plan, rng, retired)
                    state['s2c'] += len(data)
                    if plan.kind == 'dup_response' and not sent_dup:
                        sent_dup = True
                        self.send(sink, data, plan, rng, retired)
                        state['outcome'] = 'response-duplicated'
        except OSError:
            pass
        finally:
            # Half-close so the other side's pump ends; a reset already closed both.
            for sock, how in ((sink, socket.SHUT_WR), (source, socket.SHUT_RD)):
                try:
                    sock.shutdown(how)
                except OSError:
                    pass

    def send(self, sink, data, plan, rng, retired):
        if plan.kind == 'slow' and plan.rate:
            step = max(512, plan.rate // 20)
            for offset in range(0, len(data), step):
                if retired.is_set():
                    raise OSError('retired')
                piece = data[offset:offset + step]
                sink.sendall(piece)
                retired.wait(len(piece) / plan.rate)
        else:
            sink.sendall(data)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--upstream', type=int, required=True)
    parser.add_argument('--listen', type=int, default=0)
    parser.add_argument('--seed', type=int, default=1)
    parser.add_argument('--log')
    parser.add_argument('--profile', default='{"fault_rate": 0.3, "weights": {"reset_before": 1, "reset_request": 1, "reset_after_request": 1, "cut_response": 1}}')
    options = parser.parse_args()
    proxy = Proxy(options.upstream, options.seed, options.log, options.listen)
    proxy.set_profile(json.loads(options.profile))
    proxy.start()
    print(proxy.port, flush=True)
    try:
        while True:
            time.sleep(3600)
    except KeyboardInterrupt:
        proxy.stop()
    return 0


if __name__ == '__main__':
    sys.exit(main())
