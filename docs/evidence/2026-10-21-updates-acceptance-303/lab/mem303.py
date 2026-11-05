#!/usr/bin/env python3
"""Idle resident memory before and after an update check (#303; mem293.py from #301 for any desktop).

Same private configuration and sampling as tests/performance/bench_idle.py
(local-only profile, no shortcuts, notifications or login startup; RSS from
/proc/<pid>/statm once a second, recorded every 10 s), on the physical Cinnamon
X11 session. Window A is cold idle. Then `update.check` runs (the branch) or
nothing does (the baseline, which has no such method), the resident settles,
and window B is idle again. Also records Pss/Private from smaps_rollup and the
TLS/network libraries the resident maps in each window.
"""
import argparse, hashlib, json, os, re, statistics, subprocess, sys, time
from pathlib import Path

SHELLS = {'cinnamon': 'cinnamon', 'plasma': 'plasmashell', 'gnome': 'gnome-shell'}
def desktop_environment(desktop):
    pid = subprocess.check_output(['pgrep', '-u', str(os.getuid()), '-x', SHELLS[desktop]], text=True).split()[0]
    raw = Path(f'/proc/{pid}/environ').read_bytes().split(b'\0')
    found = dict(item.decode().split('=', 1) for item in raw if b'=' in item)
    # gnome-shell is the compositor, so its own environ has no WAYLAND_DISPLAY or
    # DISPLAY; take what the session exported to systemd --user.
    if desktop == 'gnome':
        exported = dict(l.split('=', 1) for l in subprocess.check_output(['systemctl', '--user', 'show-environment'], text=True).splitlines() if '=' in l)
        for key in ('WAYLAND_DISPLAY', 'DISPLAY', 'XAUTHORITY'):
            if key in exported: found.setdefault(key, exported[key])
    env = {key: found[key] for key in ('DISPLAY', 'XAUTHORITY', 'WAYLAND_DISPLAY', 'DBUS_SESSION_BUS_ADDRESS', 'XDG_CURRENT_DESKTOP', 'XDG_SESSION_TYPE') if key in found}
    # The resident gets a private XDG_RUNTIME_DIR, so the Wayland socket must be absolute.
    if 'WAYLAND_DISPLAY' in env and not env['WAYLAND_DISPLAY'].startswith('/'):
        env['WAYLAND_DISPLAY'] = found.get('XDG_RUNTIME_DIR', f'/run/user/{os.getuid()}') + '/' + env['WAYLAND_DISPLAY']
    return env

def rollup(pid):
    values = {}
    for line in Path(f'/proc/{pid}/smaps_rollup').read_text().splitlines()[1:]:
        name, value = line.split(':', 1)
        values[name] = int(value.split()[0]) * 1024
    return {key: values.get(key) for key in ('Rss', 'Pss', 'Private_Clean', 'Private_Dirty', 'Anonymous')}

def tls_libraries(pid):
    names = set()
    for line in Path(f'/proc/{pid}/maps').read_text().splitlines():
        path = line.split()[-1] if len(line.split()) >= 6 else ''
        if re.search(r'libQt6Network|libssl|libcrypto|libqopensslbackend|libgnutls', path):
            names.add(Path(path).name)
    return sorted(names)

def run(args):
    root = Path(args.output).resolve()
    root.mkdir(parents=True, exist_ok=False, mode=0o700)
    environment = os.environ.copy()
    for key in ('DISPLAY', 'WAYLAND_DISPLAY', 'XDG_SESSION_TYPE', 'QT_QPA_PLATFORM'):
        environment.pop(key, None)
    environment.update(desktop_environment(args.desktop))
    session_type = environment.get('XDG_SESSION_TYPE')
    if session_type not in ('x11', 'wayland'):
        raise RuntimeError(f'Needs a physical graphical session, found {session_type}')
    runtime = Path(subprocess.check_output(['mktemp', '-d', '/tmp/pm303.XXXXXX'], text=True).strip())
    for key, directory in [('XDG_CONFIG_HOME', 'config'), ('XDG_DATA_HOME', 'data'), ('XDG_CACHE_HOME', 'cache'), ('XDG_STATE_HOME', 'state')]:
        path = root / directory; path.mkdir(mode=0o700); environment[key] = str(path)
    environment['XDG_RUNTIME_DIR'] = str(runtime)
    app, cli = str(Path(args.app).resolve()), str(Path(args.cli).resolve())
    log = (root / 'resident.log').open('wb')
    resident = None
    def request(method, params=None, timeout=10):
        output = subprocess.run([cli, method, json.dumps(params or {})], env=environment, timeout=timeout, capture_output=True).stdout
        return json.loads(output)
    def ready():
        deadline = time.monotonic() + 20
        while time.monotonic() < deadline:
            if resident.poll() is not None:
                raise RuntimeError('Resident exited before readiness')
            try:
                reply = request('diagnostics')
                if reply['ok'] and reply['result']['journalReady']:
                    return
            except Exception:
                pass
            time.sleep(.2)
        raise RuntimeError('Resident readiness timeout')
    page = os.sysconf('SC_PAGE_SIZE'); ticks_per_second = os.sysconf('SC_CLK_TCK')
    def window(name, seconds):
        samples = []; start = time.monotonic(); first_ticks = None; last = -10
        while True:
            if resident.poll() is not None:
                raise RuntimeError('Resident exited during sampling')
            fields = Path(f'/proc/{resident.pid}/stat').read_text().split(') ', 1)[1].split()
            ticks = int(fields[11]) + int(fields[12]); rss = int(Path(f'/proc/{resident.pid}/statm').read_text().split()[1]) * page
            first_ticks = ticks if first_ticks is None else first_ticks
            elapsed = time.monotonic() - start
            if elapsed >= last + 10 or elapsed >= seconds:
                samples.append({'elapsedSeconds': round(elapsed, 1), 'rssBytes': rss}); last = elapsed
            if elapsed >= seconds:
                break
            time.sleep(1)
        rss_values = [s['rssBytes'] for s in samples]
        return {'window': name, 'seconds': round(elapsed, 1), 'samples': samples,
                'medianRssMiB': round(statistics.median(rss_values) / 2**20, 3), 'peakRssMiB': round(max(rss_values) / 2**20, 3),
                'finalRssMiB': round(rss_values[-1] / 2**20, 3), 'cpuPercentOneCore': round((ticks - first_ticks) / ticks_per_second / elapsed * 100, 4),
                'smapsRollup': rollup(resident.pid), 'tlsNetworkLibraries': tls_libraries(resident.pid)}
    try:
        # Every launch runs in the real session (lab rule: tests are on screen),
        # including this one, which only writes the private profile's settings.
        if session_type == 'x11': environment['QT_QPA_PLATFORM'] = 'xcb'
        resident = subprocess.Popen([app, '--background'], env=environment, stdout=log, stderr=log); ready()
        document = request('settings.developmentTemplate')['result']['document']
        document['profiles'] = []
        document['actions'] = {action: {'destination': 'local', 'profileId': None, 'shortcut': ''} for action in document['actions']}
        document['archive']['root'] = str(root / 'archive')
        document['preferences'] = {'notifications': False, 'launchAtLogin': False}
        revision = request('settings.get')['result']['metadata']['observedRevision']
        assert request('settings.patch', {'patch': document, 'expectedRevision': revision})['ok']
        resident.terminate(); resident.wait(timeout=20)
        resident = subprocess.Popen([app, '--background'], env=environment, stdout=log, stderr=log); ready()
        time.sleep(5)
        result = {'schemaVersion': 1, 'scope': f"physical {environment.get('XDG_CURRENT_DESKTOP')} {session_type} idle resident, private local-only profile; windows before and after update.check",
                  'session': {k: environment.get(k) for k in ('XDG_CURRENT_DESKTOP', 'XDG_SESSION_TYPE', 'WAYLAND_DISPLAY', 'DISPLAY')},
                  'appSha256': hashlib.sha256(Path(app).read_bytes()).hexdigest(),
                  'version': json.loads(subprocess.check_output([app, '--version-json']))['version'],
                  'updateInfo': json.loads(subprocess.run([cli, '--update-info'], capture_output=True, text=True).stdout or 'null'),
                  'host': os.uname().nodename, 'kernel': os.uname().release, 'loadBefore': os.getloadavg()}
        result['before'] = window('before', args.seconds)
        started = time.monotonic()
        result['check'] = request('update.check', timeout=70)
        result['checkSeconds'] = round(time.monotonic() - started, 2)
        time.sleep(args.settle)
        children = subprocess.run(['pgrep', '-P', str(resident.pid)], capture_output=True, text=True).stdout.split()
        result['childrenAfterSettle'] = [Path(f'/proc/{c}/comm').read_text().strip() for c in children if Path(f'/proc/{c}').exists()]
        result['after'] = window('after', args.seconds)
        result['deltaMedianRssMiB'] = round(result['after']['medianRssMiB'] - result['before']['medianRssMiB'], 3)
        result['loadAfter'] = os.getloadavg()
        (root / 'results.json').write_text(json.dumps(result, indent=2) + '\n')
        summary = {key: result[key] for key in ('version', 'checkSeconds', 'deltaMedianRssMiB', 'childrenAfterSettle')}
        for name in ('before', 'after'):
            summary[name] = {key: result[name][key] for key in ('medianRssMiB', 'peakRssMiB', 'cpuPercentOneCore', 'tlsNetworkLibraries')}
            summary[name]['PssMiB'] = round(result[name]['smapsRollup']['Pss'] / 2**20, 3)
        summary['check'] = {'ok': result['check'].get('ok'), 'error': result['check'].get('error', {}).get('code'),
                            'latest': (result['check'].get('result') or {}).get('result', {}).get('latest') if result['check'].get('ok') else None}
        print(json.dumps(summary, indent=1))
    finally:
        if resident is not None and resident.poll() is None:
            resident.terminate(); resident.wait(timeout=20)
        log.close()
        subprocess.run(['rm', '-rf', str(runtime)])

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--app', required=True); parser.add_argument('--cli', required=True)
    parser.add_argument('--output', required=True); parser.add_argument('--desktop', required=True, choices=sorted(SHELLS))
    parser.add_argument('--seconds', type=int, default=180); parser.add_argument('--settle', type=int, default=30)
    sys.exit(run(parser.parse_args()))
