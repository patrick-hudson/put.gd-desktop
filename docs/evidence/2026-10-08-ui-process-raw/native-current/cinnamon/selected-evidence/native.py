#!/usr/bin/env python3
"""Visible U11 Cinnamon acceptance steps; never performs a memory measurement.

All candidate phases require the root's committed source receipt. Every command,
normal close, screenshot, operation ID, and unsuccessful attempt is retained.
Only the existing disposable Cinnamon overlay is in scope.
"""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import tempfile
import time

from PIL import Image

ROOT = Path('/home/patrick/work/put.gd')
OUT = Path(__file__).resolve().parent
RUN = ROOT / 'work/lab-runs/current-cinnamon-a3d70dc'
sys.path.insert(0, str(ROOT / 'tools/lab'))
import lab


def now():
    return datetime.datetime.now(datetime.timezone.utc).isoformat()


def save(name, value):
    lab.write_json(OUT / name, value)


def event(kind, **values):
    data = {'timestampUtc': now(), 'monotonicSeconds': time.monotonic(), 'kind': kind, **values}
    with (OUT / 'events.jsonl').open('a') as stream:
        stream.write(json.dumps(data) + '\n')
    return data


def invoke(label, argv):
    if (OUT / (label + '.command.json')).exists():
        raise RuntimeError('Refusing to overwrite a previous attempt: ' + label)
    started = now()
    result = subprocess.run(argv, capture_output=True, text=True)
    (OUT / (label + '.stdout')).write_text(result.stdout)
    (OUT / (label + '.stderr')).write_text(result.stderr)
    save(label + '.command.json', {'argv': argv, 'startedUtc': started,
                                   'finishedUtc': now(), 'exitCode': result.returncode})
    if result.returncode:
        raise RuntimeError(label + ' failed; original command/output retained')
    return json.loads(result.stdout)


def source(path):
    data = json.loads(Path(path).read_text())
    assert len(data['commit']) == 40
    assert lab.hash_file(Path(data['archive'])) == data['archiveSha256']
    return data


def snapshot():
    reply = lab.cli(RUN, 'ui.snapshot')
    expected = json.loads((OUT / 'candidate-source.json').read_text())
    assert reply['build']['gitCommit'] == expected['commit']
    return reply


def windows():
    text = lab.guest_session(RUN, 'wmctrl -lpG\n')
    result = []
    for line in text.splitlines():
        fields = line.split(maxsplit=8)
        if len(fields) == 9:
            result.append({'id': fields[0], 'desktop': int(fields[1]), 'pid': int(fields[2]),
                           'x': int(fields[3]), 'y': int(fields[4]), 'width': int(fields[5]),
                           'height': int(fields[6]), 'title': fields[8]})
    return result


def witness(label):
    shot = lab.screenshot(RUN, 'u11-' + label)
    png = OUT / (label + '.png')
    if png.exists():
        raise RuntimeError('Refusing to overwrite witness: ' + label)
    Image.open(shot['path']).save(png)
    event('witness', label=label, original=shot, png=str(png), sha256=lab.hash_file(png))
    return str(png)


def wait_for(predicate, description, seconds=15):
    deadline = time.monotonic() + seconds
    last = None
    while time.monotonic() < deadline:
        last = predicate()
        if last:
            return last
        time.sleep(.15)
    raise RuntimeError('Timed out: ' + description + '; final observation=' + repr(last))


def helper_pid(data):
    return data['diagnostics']['ui'].get('pid', 0)


def close_helper(label):
    before = snapshot()
    pid = helper_pid(before)
    assert pid > 0
    current = [window for window in windows() if window['pid'] == pid]
    assert current
    for index, window in enumerate(current):
        # Raising targets an already discovered PID-owned window. Closing itself
        # uses the same visible Alt+F4 gesture a user uses, not a process signal.
        lab.guest_session(RUN, 'wmctrl -ia ' + shlex.quote(window['id']) + '\n')
        event('visible-key', keys=['alt', 'f4'], window=window)
        lab.key(RUN, ['alt', 'f4'])
        wait_for(lambda: not any(w['id'] == window['id'] for w in windows()), 'window closes')
    after = wait_for(lambda: (data if helper_pid(data := snapshot()) == 0 else None), 'helper exits normally')
    assert after['diagnostics']['ui']['lastExit']['status'] == 'normal'
    assert after['diagnostics']['ui']['lastExit']['code'] == 0
    still = lab.ssh(RUN, f"test ! -e /proc/{pid} && echo gone\n").strip()
    assert still == 'gone'
    assert after['diagnostics']['trayAvailable'] is True
    witness(label + '-closed')
    event('helper-closed', label=label, helperPid=pid, status=after['diagnostics']['ui'],
          resident=lab.verify_running_app(RUN))
    return {'helperPid': pid, 'diagnostics': after['diagnostics']['ui']}


def install(args):
    frozen = source(args.source)
    deb = Path(args.deb).resolve()
    with tempfile.TemporaryDirectory(prefix='putgd-u11-package-') as directory:
        subprocess.run(['dpkg-deb', '--extract', str(deb), directory], check=True)
        manifest = json.loads((Path(directory) / 'usr/share/putgd/build-info.json').read_text())
        assert manifest['gitCommit'] == frozen['commit']
        for name in ('putgd', 'putgd-ui', 'putgd-cli', 'putgd-desktop-fixture'):
            assert (Path(directory) / 'usr/bin' / name).is_file()
    if args.baseline_stopped:
        probe = '''python3 - <<'PY'
import json,os,pathlib,sqlite3
for p in pathlib.Path('/proc').iterdir():
 if not p.name.isdecimal():continue
 try:
  if p.stat().st_uid==os.getuid() and (p/'comm').read_text().strip() in ('putgd','putgd-ui'):
   raise RuntimeError('A resident/UI process is still running')
 except FileNotFoundError:pass
db=pathlib.Path.home()/'putgd-lab/data/putgd/putgd/history.sqlite'
with sqlite3.connect(db.as_uri()+'?mode=ro',uri=True) as connection:
 rows=[json.loads(row[0]) for row in connection.execute('SELECT data FROM operations')]
 print(json.dumps([{'id':r['id'],'state':r['state'],'action':r['action']} for r in rows]))
PY
'''
        (OUT / 'stopped-baseline-readonly-probe.sh').write_text(probe)
        operations = json.loads(lab.ssh(RUN, probe))
        before = {'stopped': True, 'observation': 'No putgd resident or helper process; synthetic journal read-only'}
    else:
        before = lab.verify_running_app(RUN)
        operations = lab.cli(RUN, 'operation.recent')['operations']
    assert all(op['state'] in ('completed', 'failed', 'cancelled') for op in operations)
    save('before-upgrade.json', {'timestampUtc': now(), 'resident': before, 'operations': operations,
                               'package': lab.read_json(RUN / 'package.json')})
    old = 'putgd-lab-before-u11-' + frozen['commit'][:12]
    stop = '' if args.baseline_stopped else f'''kill -TERM {before['pid']}
for i in $(seq 1 100); do kill -0 {before['pid']} 2>/dev/null || break; sleep .1; done
! kill -0 {before['pid']} 2>/dev/null
'''
    script = f'''set -eu
[ ! -e "$HOME/{old}" ]
{stop}
pkill -x putgd-desktop-f || true
pkill -f '^python3 /home/[^/]+/putgd-lab/gtk_clipboard_fixture.py --output ' || true
mv "$HOME/putgd-lab" "$HOME/{old}"
mkdir -p "$HOME/putgd-lab"
'''
    (OUT / 'preserve-baseline-state.sh').write_text(script)
    (OUT / 'preserve-baseline-state.stdout').write_text(lab.guest_session(RUN, script))
    result = invoke('install', [sys.executable, str(ROOT / 'tools/lab/lab.py'), 'install',
                               '--run', str(RUN), '--deb', str(deb)])
    save('candidate-source.json', frozen)
    probe = '''python3 - <<'PY'
import hashlib,json,pathlib,subprocess
items=[]
for name in ('putgd','putgd-ui','putgd-cli'):
 path=pathlib.Path('/usr/bin')/name
 result=subprocess.run([str(path),'--version'],capture_output=True,text=True,check=True)
 text=result.stdout.strip()
 if not text.startswith(name+' '):raise RuntimeError('Unexpected version envelope')
 items.append({'path':str(path),'sha256':hashlib.sha256(path.read_bytes()).hexdigest(),
               'version':text.split(' ',1)[1],'versionOutput':text,'exitCode':result.returncode})
print(json.dumps(items))
PY
'''
    (OUT / 'installed-executable-probe.sh').write_text(probe)
    executable_identity = json.loads(lab.ssh(RUN, probe))
    expected_binaries = lab.read_json(RUN / 'package.json')['installedBinarySha256']
    for item in executable_identity:
        assert item['sha256'] == expected_binaries[item['path']]
        assert item['version'] == manifest['version']
    save('installed-executables.json', executable_identity)
    save('install.json', {'timestampUtc': now(), 'result': result, 'source': frozen,
                          'package': str(deb), 'packageSha256': lab.hash_file(deb),
                          'manifest': manifest, 'installedBinaries': lab.read_json(RUN / 'package.json'),
                          'baselinePreservedAt': '/home/lab/' + old})


def cases(args):
    results = []
    cases = [('qt', s) for s in ['region', 'window', 'desktop', 'clipboard-text',
        'clipboard-image', 'clipboard-files', 'region-cancel-escape',
        'region-cancel-right-click', 'region-cancel-zero-area']]
    cases += [('gtk', s) for s in ['clipboard-text', 'clipboard-image', 'clipboard-files']]
    for client, scenario in cases:
        result = invoke('matrix-' + client + '-' + scenario,
            [sys.executable, str(ROOT / 'tools/lab/lab.py'), 'run', '--run', str(RUN),
             '--keep', '--timeout', '60', '--scenario', scenario, '--clipboard-client', client])
        results.append(result)
        save('matrix.json', {'timestampUtc': now(), 'attempts': results})
        assert result['ok']
        snapshot()


def start(args):
    lab.guest_session(RUN, '''mkdir -p "$HOME/putgd-lab"
nohup putgd-launcher --background > "$HOME/putgd-lab/app.log" 2>&1 < /dev/null &
''')
    deadline = time.monotonic() + 20
    while True:
        try:
            data = snapshot()
            assert data['operationsReady'] is True
            break
        except (lab.LabError, AssertionError):
            if time.monotonic() >= deadline:
                raise
            time.sleep(.2)
    identity = lab.verify_running_app(RUN)
    settings = lab.cli(RUN, 'settings.get')
    patch = {'actions': {action: {'destination': 'local', 'shortcut': f'Ctrl+Shift+{index}'}
                        for index, action in enumerate(('region', 'window', 'desktop', 'clipboard'), 1)},
             'archive': {'root': '/home/lab/putgd-lab/archive', 'keepUploads': True},
             'preferences': {'notifications': False, 'launchAtLogin': False}}
    saved = lab.cli(RUN, 'settings.patch', {'expectedRevision': settings['metadata']['acceptedRevision'], 'patch': patch})
    data = snapshot()
    if helper_pid(data):
        wait_for(lambda: [w for w in windows() if w['pid'] == helper_pid(snapshot())], 'initial setup view')
        witness('initial-setup')
        close_helper('initial-setup')
    assert helper_pid(snapshot()) == 0
    save('start.json', {'timestampUtc': now(), 'resident': identity, 'configurationPatch': patch,
                        'acceptedSettings': saved, 'snapshot': snapshot(), 'witness': witness('resident-tray-only')})


def views(args):
    resident = lab.verify_running_app(RUN)
    for cycle in range(2):
        for view in ('settings', 'history', 'activity', 'about'):
            before = snapshot()
            assert helper_pid(before) == 0
            label = f'views-{cycle}-{view}'
            start = time.monotonic()
            shown = lab.cli(RUN, 'ui.show', {'view': view})
            data = wait_for(lambda: (d if helper_pid(d := snapshot()) else None), 'helper PID')
            pid = helper_pid(data)
            mapped = wait_for(lambda: [w for w in windows() if w['pid'] == pid], 'mapped helper view')
            event('helper-shown', label=label, view=view, acknowledgement=shown,
                  helperPid=pid, mappedWindows=mapped, seconds=time.monotonic() - start,
                  trigger='production CLI; tray trigger tested separately')
            witness(label + '-open')
            if cycle == 0:
                reject_owned_capture(label)
            close_helper(label)
            assert lab.verify_running_app(RUN) == resident
    save('views.json', {'timestampUtc': now(), 'cycles': 2, 'views': 8,
                        'trigger': 'production CLI, normal visible Alt+F4 close',
                        'residentUnchanged': resident, 'visualReview': 'pending'})


def reject_owned_capture(label):
    data = snapshot()
    pid = helper_pid(data)
    assert pid > 0
    before = {o['id'] for o in lab.cli(RUN, 'operation.recent')['operations']}
    def owned_active():
        raw = lab.guest_session(RUN, 'xprop -root _NET_ACTIVE_WINDOW\n').strip()
        match = re.search(r'0x[0-9a-fA-F]+', raw)
        owned = [w for w in windows() if w['pid'] == pid]
        return raw if match and any(int(w['id'], 16) == int(match.group(), 16) for w in owned) else None
    active = wait_for(owned_active, 'helper-owned active window before capture hotkey')
    event('native-owned-window-rejection-start', label=label, helperPid=pid,
          activeWindow=active, windows=windows(), key=['ctrl', 'shift', '2'])
    lab.key(RUN, ['ctrl', 'shift', '2'])
    deadline = time.monotonic() + 10
    observed = None
    while time.monotonic() < deadline:
        current = lab.cli(RUN, 'operation.recent')['operations']
        new = [o for o in current if o['id'] not in before]
        assert len(new) <= 1
        if new:
            observed = lab.cli(RUN, 'operation.inspect', {'id': new[0]['id']})
            if observed['state'] in ('failed', 'completed', 'cancelled', 'partial'):
                break
        time.sleep(.1)
    save(label + '-owned-capture.json', {'timestampUtc': now(), 'operation': observed,
                                       'helperPid': pid, 'activeWindow': active})
    assert observed and observed['state'] == 'failed'
    assert observed['error']['code'] == 'capture_target'
    assert observed['items'] == []
    assert helper_pid(snapshot()) == pid
    return observed


def traycase(args):
    """Reuse independent pixel/paste oracle; replace only its QMP trigger gesture.

    This wrapper changes no product code or application adapter. The native tray
    menu is clicked visibly, using coordinates inspected in a candidate witness.
    The inherited lab parameters still describe configured shortcuts; this receipt
    records explicitly that the actual action trigger was the tray instead.
    """
    action = args.action
    scenario = 'clipboard-text' if action == 'clipboard' else action
    expected_key = ['ctrl', 'shift', str(('region', 'window', 'desktop', 'clipboard').index(action) + 1)]
    original_key = lab.key
    trigger = []

    def replacement_key(run, keys):
        if keys != expected_key:
            return original_key(run, keys)
        assert not trigger
        before = snapshot()
        assert helper_pid(before) == 0
        lab.pointer(RUN, args.icon_x, args.icon_y, 1280, 800)
        lab.input_events(RUN, [{'type': 'btn', 'data': {'button': 'right', 'down': True}}])
        time.sleep(.1)
        lab.input_events(RUN, [{'type': 'btn', 'data': {'button': 'right', 'down': False}}])
        time.sleep(.4)
        menu = witness('tray-' + action + '-menu')
        lab.pointer(RUN, args.menu_x, args.row_y, 1280, 800, True)
        lab.pointer(RUN, args.menu_x, args.row_y, 1280, 800, False)
        trigger.append(event('native-tray-action', action=action,
            helperAbsent=True, menuWitness=menu, icon=[args.icon_x, args.icon_y],
            menuRow=[args.menu_x, args.row_y], originalConfiguredHotkey=keys))

    lab.key = replacement_key
    result = None
    try:
        result = lab.fixture_run(argparse.Namespace(run=str(RUN), scenario=scenario,
            clipboard_client='qt', settings=None, history=False, keep=True, timeout=60))
        assert len(trigger) == 1
        assert result['ok']
        assert helper_pid(snapshot()) == 0
    finally:
        lab.key = original_key
        save('tray-' + action + '.json', {'timestampUtc': now(), 'action': action,
            'trigger': trigger, 'result': result,
            'scope': 'Real native tray QMP gesture using unchanged independent lab artifact/paste oracle; no product mock.'})
    print(json.dumps(result))


def trayviews(args):
    results = []
    for view, row in [('settings', 631), ('history', 656), ('activity', 682), ('about', 713)]:
        assert helper_pid(snapshot()) == 0
        lab.pointer(RUN, 1049, 780, 1280, 800)
        lab.input_events(RUN, [{'type': 'btn', 'data': {'button': 'right', 'down': True}}])
        time.sleep(.1)
        lab.input_events(RUN, [{'type': 'btn', 'data': {'button': 'right', 'down': False}}])
        time.sleep(.3)
        lab.pointer(RUN, 1125, row, 1280, 800, True)
        lab.pointer(RUN, 1125, row, 1280, 800, False)
        shown = wait_for(lambda: d if helper_pid(d := snapshot()) > 0 else None, 'tray opens helper')
        pid = helper_pid(shown)
        mapped = wait_for(lambda: [w for w in windows() if w['pid'] == pid], 'tray maps helper view')
        open_shot = witness('tray-view-' + view + '-open')
        event('native-tray-view', view=view, helperPid=pid, mappedWindows=mapped,
              input={'icon': [1049, 780], 'menuRow': [1125, row]}, witness=open_shot)
        closed = close_helper('tray-view-' + view)
        results.append({'view': view, 'openWitness': open_shot, 'windows': mapped, 'close': closed})
        save('tray-views.json', {'timestampUtc': now(), 'results': results, 'visualReview': 'pending'})


def step(args):
    if args.kind == 'click':
        x, y = map(int, args.values[:2])
        button = args.values[2] if len(args.values) > 2 else 'left'
        lab.pointer(RUN, x, y, 1280, 800)
        lab.input_events(RUN, [{'type': 'btn', 'data': {'button': button, 'down': True}}])
        time.sleep(.1)
        lab.input_events(RUN, [{'type': 'btn', 'data': {'button': button, 'down': False}}])
    elif args.kind == 'key':
        lab.key(RUN, args.values)
    elif args.kind != 'shot':
        raise ValueError(args.kind)
    event('visible-step', label=args.label, action=args.kind, values=args.values)
    time.sleep(.5)
    result = {'timestampUtc': now(), 'witness': witness(args.label), 'windows': windows(),
              'resident': lab.verify_running_app(RUN)}
    if (OUT / 'candidate-source.json').exists():
        data = snapshot()
        result['ui'] = data['diagnostics']['ui']
        result['build'] = data['build']
        result['trayAvailable'] = data['diagnostics']['trayAvailable']
    save(args.label + '.json', result)
    print(json.dumps({k: v for k, v in result.items() if k != 'snapshot'}))


def main():
    p = argparse.ArgumentParser(description=__doc__)
    sub = p.add_subparsers(dest='phase', required=True)
    q = sub.add_parser('install');q.add_argument('--source', required=True);q.add_argument('--deb', required=True)
    q.add_argument('--baseline-stopped', action='store_true', help='Require no resident/helper and inspect the preserved synthetic journal read-only')
    sub.add_parser('start');sub.add_parser('cases');sub.add_parser('views');sub.add_parser('trayviews')
    q = sub.add_parser('traycase');q.add_argument('action', choices=['region', 'window', 'desktop', 'clipboard'])
    for field in ('icon-x', 'icon-y', 'menu-x', 'row-y'):
        q.add_argument('--' + field, type=int, required=True)
    q = sub.add_parser('step');q.add_argument('label');q.add_argument('kind', choices=['click', 'key', 'shot']);q.add_argument('values', nargs='*')
    args = p.parse_args()
    event('phase-start', phase=args.phase, argv=sys.argv, labRunnerSha256=lab.hash_file(ROOT / 'tools/lab/lab.py'))
    try:
        globals()[args.phase](args)
    except Exception as error:
        event('phase-failed', phase=args.phase, error=str(error))
        raise
    event('phase-complete', phase=args.phase)


if __name__ == '__main__':
    main()
