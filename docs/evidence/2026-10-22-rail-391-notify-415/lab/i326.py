#!/usr/bin/env python3
"""#326 desktop checks on top of afa.py (the AppImage acceptance driver), run on the host from SSH.

handler-on/off  make a URL recorder the user's real default http(s) handler (the portal reads the
                user's own mimeapps.list, not the resident's private XDG_CONFIG_HOME); off restores
                the exact previous files.
synth           a synthetic PNG at out/capture-region.png for `afa.py notify`.
open-link       history.openLink (MCP open_history_link) for the newest uploaded entry.
reveal          diagnostics.bundle, then diagnostics.reveal (MCP show_diagnostics_package),
                with dbus-monitor watching FileManager1 and the portal; full-desktop frame.
trace on/off    strace the resident's execve calls (with environments) to check every host
                program it starts sees no image path.
env-check       the traced execs: program, and any APPDIR/.mount_ path in its environment.
Every file this adds outside the run directory is recorded in run.json and removed by handler-off.
"""
import argparse, json, os, re, shutil, signal, struct, subprocess, sys, time, zlib
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
import afa
from afa import Run, out, iso

HANDLER = 'putgd326-url.desktop'
APPS = Path.home() / '.local/share/applications'
CONFIGS = [Path.home() / '.config/mimeapps.list', Path.home() / '.config/kde-mimeapps.list']


def gio_default(run):
    env = run.env(private=False)
    env['PATH'] = ':'.join(d for d in env.get('PATH', '/usr/bin:/bin').split(':') if not d.endswith('/hostbin'))
    return {s: afa.sh('gio', 'mime', f'x-scheme-handler/{s}', env=env).splitlines()[:1] for s in ('https', 'http')}


def handler_on(args):
    run = Run(args.name)
    if run.meta.get('handler'): raise SystemExit('handler already on')
    backup = {}
    for path in CONFIGS:
        backup[str(path)] = path.read_bytes().hex() if path.exists() else None
    entry = APPS / HANDLER
    if entry.exists(): raise SystemExit(f'{entry} exists')
    recorder = run.dir / 'record-url.sh'
    # The URL, and the opener's own environment: what a host program put.gd starts sees.
    recorder.write_text('#!/bin/sh\n'
                        f'printf "%s %s lib=%s plugins=%s appdir=%s parent=%s\\n" "$(date -u +%FT%TZ)" "$1" "${{LD_LIBRARY_PATH-unset}}" '
                        f'"${{QT_PLUGIN_PATH-unset}}" "${{APPDIR-unset}}" "$(cat /proc/$PPID/comm 2>/dev/null)" >> {run.dir}/opened-urls.txt\n'
                        # Something to see in the full-desktop picture; it closes itself.
                        'if command -v zenity >/dev/null; then zenity --info --title "put.gd #326 URL recorder" --text "Link opened by the default handler:\\n$1" --timeout 25 >/dev/null 2>&1 &\n'
                        'elif command -v kdialog >/dev/null; then kdialog --title "put.gd #326 URL recorder" --passivepopup "Link opened by the default handler: $1" 25 >/dev/null 2>&1 &\n'
                        'fi\n')
    recorder.chmod(0o700)
    run.meta['handler'] = {'backup': backup, 'entry': str(entry), 'before': gio_default(run), 'at': iso()}
    run.save()
    APPS.mkdir(parents=True, exist_ok=True)
    entry.write_text(f'[Desktop Entry]\nType=Application\nName=put.gd #326 URL recorder\nExec={recorder} %u\n'
                     'MimeType=x-scheme-handler/https;x-scheme-handler/http;\nNoDisplay=true\n')
    env = run.env(private=False)
    subprocess.run(['xdg-mime', 'default', HANDLER, 'x-scheme-handler/https', 'x-scheme-handler/http'], env=env, check=True)
    time.sleep(1)
    run.meta['handler']['after'] = gio_default(run); run.save()
    out(run.meta['handler'] | {'backup': {k: v is not None for k, v in backup.items()}})


def handler_off(args):
    run = Run(args.name); h = run.meta.get('handler')
    if not h: raise SystemExit('handler not on')
    for path, data in h['backup'].items():
        p = Path(path)
        if data is None:
            if p.exists(): p.unlink()
        else:
            p.write_bytes(bytes.fromhex(data))
    Path(h['entry']).unlink(missing_ok=True)
    time.sleep(1)
    restored = gio_default(run)
    rep = {'restored': restored, 'before': h['before'], 'same': restored == h['before'],
           'filesMatch': all((Path(k).read_bytes().hex() if Path(k).exists() else None) == v for k, v in h['backup'].items())}
    run.meta.pop('handler'); run.meta['handlerRestored'] = rep; run.save()
    out(run.record('handlerOff', rep))


def synth(args):
    run = Run(args.name); w, h = 480, 270; raw = bytearray()
    for y in range(h):
        raw.append(0)
        for x in range(w): raw += bytes(((x * 255) // w, 80, (y * 255) // h))
    chunk = lambda t, d: struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d))
    p = run.dir / 'out/capture-region.png'
    p.write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(bytes(raw))) + chunk(b'IEND', b''))
    out({'synthetic': str(p)})


def urls(run):
    f = run.dir / 'opened-urls.txt'
    return f.read_text().splitlines() if f.exists() else []


def open_link(args):
    run = Run(args.name); afa.require_recorder(run.env(private=False))
    items = run.ok('history.list', {'limit': 5}).get('records', [])
    uploaded = [i for i in items if i.get('linkAvailable')]
    if not uploaded: raise SystemExit('no uploaded entry with a link')
    before = len(urls(run))
    r = run.cli('history.openLink', {'id': uploaded[0]['id']}, timeout=30)
    got = run.wait(lambda: urls(run)[before:] or None, 15)
    rep = {'id': uploaded[0]['id'], 'link': (uploaded[0].get('receipt') or {}).get('directUrl'), 'reply': r, 'recorded': got or []}
    rep['pass'] = bool(r.get('ok')) and bool(got) and rep['link'] in got[0] and '.mount_putgd' not in got[0]
    out(run.record('openLink:' + args.label, rep))


FILE_MANAGERS = ('dolphin', 'nautilus', 'nemo')

def file_managers():
    found = {}
    for p in Path('/proc').iterdir():
        if not p.name.isdigit(): continue
        try:
            if p.stat().st_uid != os.getuid(): continue
            comm = (p / 'comm').read_text().strip()
        except OSError: continue
        if comm in FILE_MANAGERS: found[int(p.name)] = comm
    return found


def reveal(args):
    run = Run(args.name)
    managers = file_managers()
    mon = subprocess.Popen(['dbus-monitor', "interface='org.freedesktop.FileManager1'", "interface='org.freedesktop.portal.OpenURI'"],
                           env=run.env(private=False), stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
    time.sleep(.6)
    folder = run.dir / 'diagnostics'; folder.mkdir(exist_ok=True)
    bundle = run.ok('diagnostics.bundle', {'output': str(folder), 'since': '1h'}, timeout=120)
    path = bundle.get('path')
    r = run.cli('diagnostics.reveal', {'path': path})
    time.sleep(args.wait)
    frames, inp, size = afa.tools(run); afa.park(run, inp, size); f = frames.grab()
    full = afa.fullshot(run, f, f'{args.label}-show-in-folder')
    mon.terminate(); signals = mon.communicate(timeout=5)[0]
    # Close what Show in folder opened: a new file manager by its exact PID, else the focused window.
    started = {pid: comm for pid, comm in file_managers().items() if pid not in managers}
    for pid in started: os.kill(pid, signal.SIGTERM)
    if not started and args.close_key: inp.key(*args.close_key.split('+'))
    logged = run.logs('on the host')[-2:]
    rep = {'path': path, 'reply': r, 'dbusMembers': re.findall(r'member=(\w+)', signals), 'showItemsArgs': re.findall(r'string "(file://[^"]+)"', signals)[:3],
           'logged': logged, 'full': full, 'fileManagersStarted': started, 'closedWith': 'pid' if started else args.close_key}
    rep['pass'] = bool(r.get('ok')) and ('ShowItems' in rep['dbusMembers'] or 'OpenDirectory' in rep['dbusMembers'])
    out(run.record('reveal:' + args.label, rep))


def resident_pid(run):
    found = [p['pid'] for p in afa.tree(run) if p['comm'] == 'putgd']
    if len(found) != 1: raise SystemExit(f'resident not unique: {found}')
    return found[0]


def trace(args):
    run = Run(args.name)
    if args.action == 'on':
        pid = resident_pid(run); log = run.dir / 'logs/execve.trace'
        p = subprocess.Popen(['sudo', '-n', 'strace', '-f', '-qq', '-v', '-s', '4096', '-e', 'trace=execve', '-e', 'signal=none', '-o', str(log), '-p', str(pid)],
                             stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=open(run.dir / 'logs/strace.err', 'w'), start_new_session=True)
        time.sleep(1.5)
        run.meta['pids']['strace'] = p.pid; run.meta['traceLog'] = str(log); run.save(); out({'strace': p.pid, 'resident': pid})
    else:
        pid = run.meta['pids'].pop('strace', None)
        if pid:
            # sudo (root) forwards the signal to its exact strace child.
            subprocess.run(['sudo', '-n', 'kill', '-INT', str(pid)]); run.wait(lambda: not Path(f'/proc/{pid}').exists(), 10)
        run.save(); out({'stopped': pid})


def env_check(args):
    run = Run(args.name); text = Path(run.meta['traceLog']).read_text(errors='replace')
    own = re.compile(r'/usr/(bin|libexec)/putgd|\.mount_putgd[^/"]*/usr/(bin|libexec)/')
    execs = []
    for line in text.splitlines():
        m = re.match(r'(\d+)\s+execve\("([^"]+)", \[(.*?)\], \[(.*)\]\) = (-?\d+)', line)
        if not m: continue
        program, argv, envs, result = m.group(2), m.group(3), m.group(4), int(m.group(5))
        env = dict(e.split('=', 1) for e in re.findall(r'"((?:[^"\\]|\\.)*)"', envs) if '=' in e)
        image = {k: v for k, v in env.items() if '.mount_putgd' in v or k in ('APPDIR', 'APPIMAGE') or k.startswith('PUTGD_HOST_')}
        execs.append({'pid': int(m.group(1)), 'program': program, 'argv': argv[:200], 'result': result, 'own': bool(own.search(program)), 'imageVariables': sorted(image)})
    host = [e for e in execs if not e['own'] and e['result'] == 0]
    rep = {'execs': execs, 'hostPrograms': [e['program'] for e in host], 'leaks': [e for e in host if e['imageVariables']]}
    rep['pass'] = not rep['leaks']
    out(run.record('envCheck:' + args.label, rep))


def wrap(args):
    """Before `afa.py start`: host-program wrappers first on the resident's PATH. Each logs its
    argv, the image variables it inherited and the real program's exit status (where sudo strace is unavailable)."""
    run = Run(args.name); bindir = run.dir / 'hostbin'; bindir.mkdir(exist_ok=True)
    log = run.dir / 'logs/host-programs.tsv'
    path = run.meta['session'].get('PATH', '/usr/bin:/bin')
    for name in ('gio', 'xdg-open', 'gsettings', 'coredumpctl'):
        real = shutil.which(name, path=path)
        if not real: continue
        w = bindir / name
        # sh, not Python: it must run under whatever library path it inherited.
        w.write_text(f"""#!/bin/sh
{real} "$@"
st=$?
printf '%s\\t%s\\t%s\\t%s\\t%s\\t%s\\t%s\\t%s\\n' "$(date -u +%FT%TZ)" {name} "$*" "${{LD_LIBRARY_PATH-unset}}" "${{QT_PLUGIN_PATH-unset}}" "${{APPDIR-unset}}" "${{PUTGD_HOST_ENVIRONMENT-unset}}" "$st" >> {log}
exit $st
""")
        w.chmod(0o700)
    run.meta['session']['PATH'] = f'{bindir}:{path}'; run.meta['wrapped'] = True; run.save()
    out({'wrapped': sorted(p.name for p in bindir.iterdir()), 'PATH': run.meta['session']['PATH']})


def host_programs(args):
    run = Run(args.name); log = run.dir / 'logs/host-programs.tsv'
    keys = ('at', 'program', 'argv', 'LD_LIBRARY_PATH', 'QT_PLUGIN_PATH', 'APPDIR', 'PUTGD_HOST_ENVIRONMENT', 'exit')
    rows = [dict(zip(keys, l.split('\t'))) for l in log.read_text().splitlines()] if log.exists() else []
    for r in rows:
        r['exit'] = int(r['exit']); r['imageVariables'] = [k for k in keys[3:7] if '.mount_putgd' in r[k] or (k in ('APPDIR', 'PUTGD_HOST_ENVIRONMENT') and r[k] != 'unset')]
    rep = {'calls': rows, 'leaks': [r for r in rows if r['imageVariables']], 'failed': [r for r in rows if r['exit'] != 0]}
    rep['pass'] = not rep['leaks']
    out(run.record('hostPrograms:' + args.label, rep))


def full(args):
    run = Run(args.name); frames, inp, size = afa.tools(run)
    if args.park: afa.park(run, inp, size)
    time.sleep(args.wait); out(run.record('full:' + args.label, {'full': afa.fullshot(run, frames.grab(), args.label), 'at': iso()}))


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    s = p.add_subparsers(dest='cmd', required=True)
    for n in ('handler-on', 'handler-off', 'synth'): s.add_parser(n).add_argument('name')
    a = s.add_parser('open-link'); a.add_argument('name'); a.add_argument('--label', required=True)
    a = s.add_parser('reveal'); a.add_argument('name'); a.add_argument('--label', required=True); a.add_argument('--wait', type=float, default=4); a.add_argument('--close-key', default='ctrl+w')
    a = s.add_parser('trace'); a.add_argument('name'); a.add_argument('action', choices=('on', 'off'))
    a = s.add_parser('env-check'); a.add_argument('name'); a.add_argument('--label', required=True)
    s.add_parser('wrap').add_argument('name')
    a = s.add_parser('full'); a.add_argument('name'); a.add_argument('--label', required=True); a.add_argument('--wait', type=float, default=0); a.add_argument('--park', action='store_true')
    a = s.add_parser('host-programs'); a.add_argument('name'); a.add_argument('--label', required=True)
    args = p.parse_args()
    {'handler-on': handler_on, 'handler-off': handler_off, 'synth': synth, 'open-link': open_link, 'reveal': reveal,
     'trace': trace, 'env-check': env_check, 'wrap': wrap, 'host-programs': host_programs, 'full': full}[args.cmd](args)


if __name__ == '__main__':
    main()
