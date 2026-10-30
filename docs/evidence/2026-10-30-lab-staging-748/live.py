import concurrent.futures
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import time

ROOT = Path('/home/user/work/put.gd/issue748')
sys.path.insert(0, str(ROOT / 'tools/lab'))
import lab
RUN = ROOT / 'work/lab-runs/c1'
OUT = ROOT / 'work/issue748'
DEB = Path('/home/user/work/put.gd/src/build/packages-lab-ItJsYnjz/putgd_0.1.1~dev.907+g0071acafc975_amd64.deb')
GOLDEN = Path('/home/user/work/put.gd/work/lab-images/cinnamon-x11/image.qcow2')
original_ssh = lab.ssh
receipt = {'placement': 'boat', 'sandbox': 'bx_2y86c4ne', 'sourceCommit': '5fd31c19',
           'goldenBefore': lab.hash_file(GOLDEN), 'run': str(RUN), 'runId': lab.state(RUN)['id'],
           'packageSha256': lab.hash_file(DEB), 'packageNote': 'Unchanged historical snapshot DEB; installer lifecycle proof only',
           'sourceFiles': {str(p.relative_to(ROOT)): lab.hash_file(p) for p in
               [ROOT / 'tools/lab/lab.py', ROOT / 'tools/lab/package_staging.py', ROOT / 'tools/lab/package_install.py']}}

def save():
    lab.write_json(OUT / 'live-receipt.json', receipt)

def event(name, **fields):
    receipt.setdefault('events', []).append({'event': name, 'monotonic': time.monotonic(), **fields})
    save()
    print(name, flush=True)

fault = {}
def instrumented_ssh(run, script, **kwargs):
    if 'APT::Update::Pre-Invoke' not in script:
        return original_ssh(run, script, **kwargs)
    assert 'set -eu; install -d' in script
    script = script.replace('set -eu; install -d',
        'set -eu; echo $$ > /home/lab/putgd-lab/staging-hook.pid; '
        'touch /home/lab/putgd-lab/staging-hook-ready; '
        'while [ ! -e /home/lab/putgd-lab/staging-release ]; do sleep 0.1; done; install -d', 1)
    process = subprocess.Popen(lab.ssh_args(run) + ['sh', '-s'], stdin=subprocess.PIPE,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    fault['ssh'] = process
    fault['sshIdentity'] = {'pid': process.pid, 'startTicks': int(Path(f'/proc/{process.pid}/stat').read_text().rsplit(')', 1)[1].split()[19])}
    stdout, stderr = process.communicate(script, timeout=60)
    if process.returncode:
        raise lab.LabError('Injected cancellation of exact controller staging SSH PID')
    return stdout

try:
    receipt['desktop'] = original_ssh(RUN, 'cat /etc/os-release; cinnamon --version; loginctl list-sessions --no-legend\n')
    receipt['guestInitial'] = original_ssh(RUN, 'sha256sum ~/.ssh/authorized_keys; test ! -e /usr/local/lib/putgd-lab/package_install.py\n')
    lab.screenshot(RUN, 'before-staging')
    lab.ssh = instrumented_ssh
    with concurrent.futures.ThreadPoolExecutor(max_workers=1) as executor:
        attempt = executor.submit(lab.install_package, RUN, DEB)
        deadline = time.monotonic() + 90
        while True:
            try:
                original_ssh(RUN, 'test -f ~/putgd-lab/staging-hook-ready\n', timeout=5)
                break
            except (lab.LabError, subprocess.TimeoutExpired):
                if attempt.done():
                    attempt.result()
                if time.monotonic() >= deadline:
                    raise RuntimeError('Hook readiness deadline')
                time.sleep(.1)
        job = original_ssh(RUN, 'cat ~/putgd-lab/install-active/job\n').strip()
        receipt['job'] = job
        receipt['guestProcessBeforeCancel'] = json.loads(original_ssh(RUN, f'cat ~/putgd-lab/{job}/staging-process.json\n'))
        receipt['hookPid'] = int(original_ssh(RUN, 'cat ~/putgd-lab/staging-hook.pid\n').strip())
        process = fault['ssh']
        assert process.poll() is None
        current_ticks = int(Path(f'/proc/{process.pid}/stat').read_text().rsplit(')', 1)[1].split()[19])
        assert current_ticks == fault['sshIdentity']['startTicks']
        event('cancel-exact-controller-ssh', identity=fault['sshIdentity'])
        os.kill(process.pid, signal.SIGTERM)
        # A concurrent retry must be refused before it creates any new evidence.
        directories = set(RUN.glob('install-*'))
        try:
            lab.install_package(RUN, DEB)
            raise AssertionError('Overlapping retry was accepted')
        except lab.LabError as error:
            assert 'retry refused' in str(error)
            receipt['refusedRetry'] = str(error)
        assert set(RUN.glob('install-*')) == directories
        event('retry-refused-while-staging-alive')
        lab.guest_session(RUN, 'xmessage -center "Issue 748: staging SSH cancelled. install-active retained. Overlapping retry refused." > /tmp/issue748-xmessage.log 2>&1 < /dev/null &\n')
        lab.screenshot(RUN, 'staging-alive-retry-refused')
        try:
            attempt.result(timeout=50)
            raise AssertionError('Cancelled staging unexpectedly passed')
        except lab.LabError as error:
            assert 'retains install-active' in str(error)
            receipt['heldFailure'] = str(error)
        report = lab.read_json(RUN / job / 'staging-reconciliation.json')
        assert report['finished'] is False and report['exactProcessAlive'] is True
        assert report['process'] == receipt['guestProcessBeforeCancel']
        assert original_ssh(RUN, 'cat ~/putgd-lab/install-active/job\n').strip() == job
        event('bounded-reconciliation-retains-live-job', reconciliation=report)
        # Recheck after the full bounded wait; this is an actual held job.
        try:
            lab.install_package(RUN, DEB)
            raise AssertionError('Held-job retry was accepted')
        except lab.LabError as error:
            assert 'retry refused' in str(error)
        original_ssh(RUN, 'touch ~/putgd-lab/staging-release\n')
        report = lab.recover_staging(RUN, job)
        assert report['finished'] is True and report['state'] == 'staging-finished'
        assert report['pid'] == receipt['guestProcessBeforeCancel']['pid']
        original_ssh(RUN, 'test ! -e ~/putgd-lab/install-active\n')
        event('guest-staging-exited-lock-recovered', reconciliation=report)
    lab.ssh = original_ssh
    result = lab.install_package(RUN, DEB, install_timeout=900, install_recovery_timeout=600)
    receipt['retrySuccess'] = result
    original_ssh(RUN, 'test ! -e ~/putgd-lab/install-active\n')
    event('later-install-retry-succeeded')
    receipt['guestFinal'] = original_ssh(RUN, 'sha256sum ~/.ssh/authorized_keys; dpkg --audit\n')
    assert receipt['guestInitial'] == receipt['guestFinal']
    receipt['goldenAfter'] = lab.hash_file(GOLDEN)
    assert receipt['goldenAfter'] == receipt['goldenBefore']
    lab.screenshot(RUN, 'retry-succeeded')
    receipt['ok'] = True
    event('golden-unchanged')
finally:
    lab.ssh = original_ssh
    save()
