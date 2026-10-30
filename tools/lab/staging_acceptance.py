#!/usr/bin/env python3
"""#780 guest acceptance: scoped staging and supervisor jobs outlive ended logins.

Run only on the QEMU host, against a fresh owned overlay whose first install
has not run yet. Every case uses the real `lab.py install` controller with a
synthetic DEB. Logins end only through plain `systemctl stop session-N.scope`
or `loginctl terminate-user`: SIGTERM/SIGHUP reach the outer sudo, which relays
them; nothing sidesteps that relay with SIGKILL. A root SSH observer exists only
in the overlay. Results go to work/$PUTGD_780_OUTPUT (default e780c).
"""
import json
import os
from pathlib import Path
import shlex
import signal
import subprocess
import sys
import time

ROOT = next(parent for parent in Path(__file__).resolve().parents if (parent / 'tools/lab/lab.py').is_file())
sys.path.insert(0, str(ROOT / 'tools/lab'))
import lab  # noqa: E402

RUN = ROOT / 'work/lab-runs' / os.environ.get('PUTGD_780_RUN', 'c1')
OUT = ROOT / 'work' / os.environ.get('PUTGD_780_OUTPUT', 'e780c')
GATES = '/run/putgd-780'
OUT.mkdir(parents=True, mode=0o700, exist_ok=True)
lab.verify_owned_guest(RUN)


def save(name, value):
    lab.write_json(OUT / name, value)


def guest(script, timeout=30):
    return lab.ssh(RUN, 'set -eu\n' + script, timeout=timeout)


ROOT_ARGS = lab.ssh_args(RUN)
ROOT_ARGS[-1] = 'root@127.0.0.1'


def root(script, timeout=60):
    return lab.command(ROOT_ARGS + ['sh', '-s'], input='set -eu\n' + script, timeout=timeout).stdout


def root_python(source, *args, timeout=60):
    return json.loads(root('python3 -c ' + shlex.quote(source) + ''.join(' ' + shlex.quote(a) for a in args) + '\n',
                           timeout=timeout))


def wait_for(function, seconds, what):
    deadline = time.monotonic() + seconds
    while True:
        value = function()
        if value:
            return value
        if time.monotonic() >= deadline:
            raise TimeoutError(what)
        time.sleep(.5)


def lock_holder():
    return root('cat /home/lab/putgd-lab/install-active/job 2>/dev/null || :\n').strip() or None


# Root observer: the overlay's existing passwordless apt-get Pre-Invoke
# authority adds this run's public key for root and the fixtures below. Empty
# APT sources keep every apt-get offline. The guest-wide Pre-Invoke gate is the
# kind of guest-owned hook #780 is about: it can hold staging, or leave a root
# setsid() daemon behind. Both act only while their /run flag exists.
GATE_HOOK = r'''#!/usr/bin/python3
import json, os, sys, time
from pathlib import Path
gates = Path('/run/putgd-780')
def identity(name):
    fields = Path('/proc/self/stat').read_text().rsplit(')', 1)[1].split()
    (gates / name).write_text(json.dumps({'pid': os.getpid(), 'uid': os.getuid(), 'sessionId': os.getsid(0),
        'startTicks': int(fields[19]), 'cgroup': Path('/proc/self/cgroup').read_text().strip()}))
if (gates / 'staging-gate').exists():
    identity('staging-ready.json')
    deadline = time.monotonic() + 900
    while (gates / 'staging-gate').exists() and time.monotonic() < deadline:
        time.sleep(.1)
if (gates / 'daemon').exists() and not os.fork():
    os.setsid()
    if os.fork():
        os._exit(0)
    for fd in (0, 1, 2):
        os.close(fd)
    identity('daemon.json')
    deadline = time.monotonic() + 900
    while not (gates / 'daemon-finish').exists() and time.monotonic() < deadline:
        time.sleep(.05)
    os._exit(0)
'''

setup_hook = (
    'set -eu; install -d -m 700 /root/.ssh; printf "\\n%s\\n" ' + shlex.quote((RUN / 'identity.pub').read_text().strip())
    + ' >> /root/.ssh/authorized_keys; chmod 600 /root/.ssh/authorized_keys; '
    'printf "PermitRootLogin prohibit-password\\n" > /etc/ssh/sshd_config.d/00-putgd-780-root.conf; '
    '/usr/sbin/sshd -t; systemctl reload ssh.service; exit 1')
guest('sudo -n /usr/bin/apt-get -o Dir::Etc::sourcelist=/dev/null -o Dir::Etc::sourceparts=- '
      '-o APT::Get::List-Cleanup=0 -o ' + shlex.quote('APT::Update::Pre-Invoke::=' + setup_hook) +
      ' update > /tmp/putgd-780-observer-setup.log 2>&1 || :\n')
assert root('id -u\n').strip() == '0'
root('install -d -m 755 /usr/local/lib/putgd-780 ' + GATES + '\n'
     'cat > /usr/local/lib/putgd-780/gate <<\'PYGATE\'\n' + GATE_HOOK + 'PYGATE\n'
     'chmod 755 /usr/local/lib/putgd-780/gate\n'
     "printf 'Dir::Etc::sourcelist \"/dev/null\";\\nDir::Etc::sourceparts \"-\";\\n"
     "APT::Get::List-Cleanup \"0\";\\nAPT::Update::Pre-Invoke {\"/usr/local/lib/putgd-780/gate\";};\\n' "
     '> /etc/apt/apt.conf.d/99-putgd-780-fixture\n')
save('guest-system.json', {'placement': 'boat', 'text': root(
    'cat /etc/os-release; uname -r; systemctl --version | head -1; sudo --version | head -1; '
    'loginctl show-user lab -p Linger; ls /var/lib/systemd/linger; stat -fc %T /sys/fs/cgroup; '
    'cat /etc/sudoers.d/putgd-lab*; ls -l /usr/local/lib/putgd-lab\n')})
assert root('test ! -e /var/lib/systemd/linger/lab && echo no-linger\n').strip() == 'no-linger'

# Synthetic putgd package whose postinst can hold the supervisor phase.
package_root = OUT / 'package'
(package_root / 'DEBIAN').mkdir(parents=True, exist_ok=True)
(package_root / 'usr/bin').mkdir(parents=True, exist_ok=True)
(package_root / 'usr/bin/putgd-synthetic780').write_text('#!/bin/sh\necho synthetic780\n')
(package_root / 'usr/bin/putgd-synthetic780').chmod(0o755)
(package_root / 'DEBIAN/control').write_text(
    'Package: putgd\nVersion: 99.780.3\nArchitecture: all\nMaintainer: put.gd lab <lab@invalid>\n'
    'Description: Synthetic disposable #780 lifetime fixture\n')
(package_root / 'DEBIAN/postinst').write_text(
    '#!/bin/sh\nset -eu\nif [ -e ' + GATES + '/postinst-gate ]; then\n'
    '  echo "$$ $(cat /proc/self/cgroup)" > ' + GATES + '/postinst-ready\n'
    '  i=0; while [ -e ' + GATES + '/postinst-gate ] && [ $i -lt 1800 ]; do sleep .5; i=$((i+1)); done\nfi\n')
(package_root / 'DEBIAN/postinst').chmod(0o755)
DEB = OUT / 'putgd-synthetic780.deb'
lab.command(['dpkg-deb', '--root-owner-group', '--build', package_root, DEB])

# Ancestry of the scope wrapper back to the outer sudo in its login scope.
IDENTITIES = r'''
import json, os, sys
from pathlib import Path
def record(pid):
    try:
        fields = Path(f'/proc/{pid}/stat').read_text().rsplit(')', 1)[1].split()
        status = dict(line.split(':\t', 1) for line in Path(f'/proc/{pid}/status').read_text().splitlines() if ':\t' in line)
        return {'pid': pid, 'ppid': int(fields[1]), 'sessionId': int(fields[3]), 'startTicks': int(fields[19]),
                'uid': int(status['Uid'].split()[0]), 'comm': Path(f'/proc/{pid}/comm').read_text().strip(),
                'cgroup': Path(f'/proc/{pid}/cgroup').read_text().strip()}
    except (FileNotFoundError, ProcessLookupError):
        return None
job = Path('/home/lab/putgd-lab') / sys.argv[1]
wrapper = record(json.loads((job / (sys.argv[2] + '-process.json')).read_text())['pid'])
chain = []
pid = wrapper['ppid']
while pid > 1:
    current = record(pid)
    chain.append(current)
    if '/session-' in current['cgroup']:
        break
    pid = current['ppid']
login = chain[-1]['cgroup'].rsplit('/', 1)[1]
assert login.startswith('session-') and login.endswith('.scope'), chain
group = Path('/sys/fs/cgroup' + wrapper['cgroup'][3:])
print(json.dumps({'wrapper': wrapper, 'launchChain': chain, 'loginScope': login,
    'scopeMembers': [record(int(p)) for p in (group / 'cgroup.procs').read_text().split()],
    'sessions': os.popen('loginctl list-sessions --no-legend').read()}))
'''

OBSERVE = r'''
import json, subprocess, sys
from pathlib import Path
ids = json.loads(sys.argv[1])
def alive(entry):
    try:
        fields = Path(f"/proc/{entry['pid']}/stat").read_text().rsplit(')', 1)[1].split()
        return int(fields[19]) == entry['startTicks'] and fields[0] not in ('Z', 'X')
    except (FileNotFoundError, ProcessLookupError):
        return False
state = lambda unit: subprocess.run(['systemctl', 'show', unit, '--property=ActiveState', '--value'],
                                    capture_output=True, text=True).stdout.strip()
scope = ids['wrapper']['cgroup'][3:]
events = Path('/sys/fs/cgroup' + scope + '/cgroup.events')
print(json.dumps({'wrapperAlive': alive(ids['wrapper']), 'launchChainAlive': [alive(e) for e in ids['launchChain']],
    'loginScope': state(ids['loginScope']), 'userManager': state('user@1000.service'),
    'wrapperCgroup': Path(f"/proc/{ids['wrapper']['pid']}/cgroup").read_text().strip() if alive(ids['wrapper']) else None,
    'scopeEvents': events.read_text().split('\n')[0] if events.exists() else 'gone',
    'sessions': subprocess.run(['loginctl', 'list-sessions', '--no-legend'], capture_output=True, text=True).stdout}))
'''


def install(name):
    log = (OUT / (name + '-controller.log')).open('w')
    return subprocess.Popen([sys.executable, str(ROOT / 'tools/lab/lab.py'), 'install', '--run', str(RUN),
                             '--deb', str(DEB), '--install-timeout', '300', '--install-recovery-timeout', '60'],
                            stdin=subprocess.DEVNULL, stdout=log, stderr=log, start_new_session=True), log


def kill_controller(controller):
    # The exact controller started above and its own ssh children (its group).
    os.killpg(controller.pid, signal.SIGKILL)
    controller.wait(timeout=30)


def end_login(mode, ids):
    if mode == 'stop-session':
        command = 'systemctl stop ' + shlex.quote(ids['loginScope'])
    else:
        command = 'loginctl terminate-user lab'
    started = time.monotonic()
    output = root(command + '\necho exit=$?\n', timeout=150)
    return {'mode': mode, 'command': command, 'output': output.strip(), 'seconds': time.monotonic() - started}


def observe(ids, seconds=12):
    samples = []
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        samples.append(root_python(OBSERVE, json.dumps(ids)))
        time.sleep(1)
    return samples


def refused(function):
    try:
        function()
    except lab.LabError as error:
        return str(error)
    raise AssertionError('recovery released a live job')


def held_case(name, phase, mode):
    """Hold a real install at a gate, kill its controller, end the login, then finish and recover."""
    gate = 'staging-gate' if phase == 'staging' else 'postinst-gate'
    ready = 'staging-ready.json' if phase == 'staging' else 'postinst-ready'
    root(f'rm -f {GATES}/{ready}; touch {GATES}/{gate}\n')
    controller, log = install(name)
    try:
        wait_for(lambda: root(f'test -e {GATES}/{ready} && echo ready || :\n').strip(), 240, name + ' gate')
        job = lock_holder()
        ids = root_python(IDENTITIES, job, phase)
        assert ids['wrapper']['uid'] == 1000 and ids['wrapper']['cgroup'] == f'0::/system.slice/putgd-{phase}-{job}.scope', ids
        kill_controller(controller)
        ended = end_login(mode, ids)
        samples = observe(ids)
        last = samples[-1]
        # The login and its launch chain are gone; the wrapper and scope are not.
        assert last['wrapperAlive'] and not any(last['launchChainAlive']), last
        assert last['loginScope'] in ('inactive', ''), last
        assert last['wrapperCgroup'] == ids['wrapper']['cgroup'], last
        held = refused(lambda: lab.recover_staging(RUN, job))
        assert lock_holder() == job
        root(f'rm -f {GATES}/{gate}\n')
        terminal = 'result.json' if phase == 'supervisor' else 'staging-result.json'
        wait_for(lambda: root(f'test -e /home/lab/putgd-lab/{job}/{terminal} && echo done || :\n').strip(), 240,
                 name + ' terminal result')
        results = {}
        for file in (('result.json', 'supervisor-result.json') if phase == 'supervisor' else ('staging-result.json',)):
            results[file] = wait_for(lambda: json.loads(root(f'cat /home/lab/putgd-lab/{job}/{file} 2>/dev/null || :\n')
                                                        or 'null'), 60, name + ' ' + file)
        if phase == 'supervisor':
            assert results['result.json']['ok'] and not results['result.json']['remainingPids'], results
            assert results['supervisor-result.json']['exitCode'] == 0, results
        else:
            assert results['staging-result.json']['exitCode'] == 0, results
        assert results[('supervisor' if phase == 'supervisor' else 'staging') + '-result.json']['pid'] == ids['wrapper']['pid']
        recovery = lab.recover_staging(RUN, job)
        assert recovery['state'] == ('supervisor-finished' if phase == 'supervisor' else 'staging-finished'), recovery
        assert lock_holder() is None
        return {'case': name, 'phase': phase, 'loginEnd': ended, 'job': job, 'identities': ids,
                'afterLoginEnd': samples, 'refusedWhileRunning': held, 'terminal': results, 'recovery': recovery,
                'lockAfter': None}
    finally:
        root(f'rm -f {GATES}/{gate}\n')
        if controller.poll() is None:
            kill_controller(controller)
        log.close()


results = []
try:
    # 1. Fresh overlay: no scope launcher yet. Staging falls back to the session
    # scan and grants the launcher; the supervisor then uses its system scope.
    controller, log = install('fallback-first-install')
    code = controller.wait(timeout=600)
    log.close()
    assert code == 0, (OUT / 'fallback-first-install-controller.log').read_text()
    evidence = Path(json.loads((OUT / 'fallback-first-install-controller.log').read_text().splitlines()[-1])['installEvidence'])
    tracking = {name: lab.read_json(evidence / name) for name in ('staging-tracking.json', 'supervisor-tracking.json')}
    assert tracking['staging-tracking.json']['tracking'] == 'session', tracking
    assert 'scope launcher' in tracking['staging-tracking.json']['fallbackReason'], tracking
    assert tracking['supervisor-tracking.json']['tracking'] == 'cgroup-v2', tracking
    grant = root('cat /etc/sudoers.d/putgd-lab-install; stat -c "%U %a" /usr/local/lib/putgd-lab/package_scope.py; '
                 'sha256sum /usr/local/lib/putgd-lab/package_scope.py\n')
    assert 'systemd-run' not in grant.splitlines()[0] and 'root 755' in grant, grant
    assert lab.hash_file(ROOT / 'tools/lab/package_scope.py') in grant
    results.append({'case': 'fallback-first-install', 'tracking': tracking, 'grant': grant,
                    'result': lab.read_json(evidence / 'result.json'), 'controllerWarning': [
                        line for line in (OUT / 'fallback-first-install-controller.log').read_text().splitlines()
                        if 'session-only tracking' in line], 'lockAfter': lock_holder()})
    assert results[-1]['lockAfter'] is None
    save('results-progress.json', results)

    for phase in ('supervisor', 'staging'):
        for mode in ('stop-session', 'terminate-user'):
            results.append(held_case(f'{phase}-{mode}', phase, mode))
            save('results-progress.json', results)

    # A root setsid() daemon from the guest's own Pre-Invoke hook outlives the
    # wrapper. Recovery refuses while populated, and releases once it exits.
    root(f'rm -f {GATES}/daemon.json {GATES}/daemon-finish; touch {GATES}/daemon\n')
    controller, log = install('setsid-survivor')
    try:
        daemon = wait_for(lambda: json.loads(root(f'cat {GATES}/daemon.json 2>/dev/null || :\n') or 'null'), 240,
                          'daemon')
        job = lock_holder()
        ids = root_python(IDENTITIES, job, 'staging')
        assert daemon['uid'] == 0 and daemon['sessionId'] != ids['wrapper']['sessionId'], (daemon, ids)
        assert daemon['cgroup'] == ids['wrapper']['cgroup'], (daemon, ids)
        kill_controller(controller)
        ended = end_login('stop-session', ids)
        samples = observe(ids, 6)
        assert samples[-1]['wrapperAlive'], samples[-1]  # As subreaper it waits for the daemon.
        root(f'kill -KILL {ids["wrapper"]["pid"]}\n')  # The exact wrapper identity recorded above.
        time.sleep(1)
        holding = [refused(lambda: lab.recover_staging(RUN, job, abandoned=abandoned)) for abandoned in (False, True)]
        assert all('cgroup remains populated' in message for message in holding), holding
        assert lock_holder() == job
        watch = subprocess.Popen(ROOT_ARGS + ['python3', '-c', shlex.quote(
            'import json,sys,time\nfrom pathlib import Path\np=Path(sys.argv[1]);seen=[]\n'
            'deadline=time.monotonic()+60\n'
            'while time.monotonic()<deadline:\n'
            '    try:value=(p/"cgroup.events").read_text().split("\\n")[0]\n'
            '    except OSError:value="gone"\n'
            '    if not seen or seen[-1]!=value:seen.append(value)\n'
            '    if value in ("populated 0","gone"):break\n'
            '    time.sleep(.01)\n'
            'print(json.dumps(seen))'), shlex.quote('/sys/fs/cgroup' + ids['wrapper']['cgroup'][3:])],
            stdout=subprocess.PIPE, text=True)
        time.sleep(2)
        root(f'touch {GATES}/daemon-finish\n')
        events, _ = watch.communicate(timeout=90)
        def released():
            if root_python(OBSERVE, json.dumps(ids))['scopeEvents'] == 'populated 1':
                return None
            return lab.recover_staging(RUN, job, abandoned=True)
        recovery = wait_for(released, 60, 'survivor exit')
        assert recovery['finished'] and recovery['cgroupPopulated'] is False and 'limit' not in recovery, recovery
        assert lock_holder() is None
        results.append({'case': 'setsid-root-survivor', 'job': job, 'daemon': daemon, 'identities': ids,
                        'loginEnd': ended, 'afterLoginEnd': samples, 'refusedWhilePopulated': holding,
                        'eventsObserved': json.loads(events), 'recovery': recovery})
    finally:
        root(f'touch {GATES}/daemon-finish; rm -f {GATES}/daemon\n')
        if controller.poll() is None:
            kill_controller(controller)
        log.close()
    save('results.json', {'passed': True, 'placement': 'boat', 'results': results,
                          'linger': root('loginctl show-user lab -p Linger 2>&1 || :\n').strip(),
                          'source': {str(p.relative_to(ROOT)): lab.hash_file(p) for p in sorted(
                              (ROOT / 'tools/lab').glob('package_*.py')) + [ROOT / 'tools/lab/lab.py', Path(__file__)]},
                          'state': lab.state(RUN)})
    print(json.dumps({'passed': True, 'cases': len(results)}), flush=True)
except BaseException as error:
    save('failure.json', {'errorType': type(error).__name__, 'message': str(error), 'completed': results})
    raise
