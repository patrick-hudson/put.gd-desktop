"""Run each supervisor in its own process: it adopts only its package descendants."""
import json
import importlib.util
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]

# The fixture waits for actual supervisor observations, including after atomic
# progress.json replacements. Register before reading so no update is lost.
WAIT_FOR_PROGRESS = '''
import ctypes, json, os, select, time
from pathlib import Path
def wait_for_progress(directory, predicate):
    libc = ctypes.CDLL(None, use_errno=True)
    fd = libc.inotify_init1(os.O_CLOEXEC | os.O_NONBLOCK)
    assert fd >= 0, ctypes.get_errno()
    try:
        assert libc.inotify_add_watch(fd, os.fsencode(directory), 0x80) >= 0, ctypes.get_errno()  # IN_MOVED_TO
        deadline = time.monotonic() + 10
        while True:
            try:
                progress = json.loads((Path(directory) / 'progress.json').read_text())
            except FileNotFoundError:
                progress = {}
            if predicate(progress):
                return
            remaining = deadline - time.monotonic()
            assert remaining > 0 and select.select([fd], [], [], remaining)[0], progress
            os.read(fd, 65536)
    finally:
        os.close(fd)
'''


class PackageInstallContracts(unittest.TestCase):
    def run_supervisor(self, body, timeout, recovery, status='Status: install ok installed\n', query_error=False,
                       setup=''):
        with tempfile.TemporaryDirectory() as directory:
            script = '''import json, sys
from pathlib import Path
from unittest.mock import patch
sys.path.insert(0, sys.argv[1])
import package_install as install
commands = [[sys.executable, '-c', sys.argv[3], sys.argv[2]]]
exec(sys.argv[8])
queries = []
def query(args, **kwargs):
    queries.append(args)
    if sys.argv[7] == 'True':
        raise FileNotFoundError('query unavailable')
    return install.subprocess.CompletedProcess(args, 0, sys.argv[6] if '-s' in args else '', '')
with patch.object(install.subprocess, 'run', side_effect=query):
    result = install.supervise(commands, Path(sys.argv[2]), float(sys.argv[4]), float(sys.argv[5]))
print(json.dumps({'result': result, 'queries': queries}))
'''
            result = subprocess.run([sys.executable, '-c', script, str(ROOT / 'tools/lab'), directory,
                                     body, str(timeout), str(recovery), status, str(query_error), setup],
                                    text=True, capture_output=True, timeout=20, check=True)
            assert (Path(directory) / 'progress.json').is_file()
            assert (Path(directory) / 'result.json').is_file()
            return json.loads(result.stdout), (Path(directory) / 'apt.log').read_text()

    def test_both_apt_commands_have_bounded_lock_wait_inside_shared_budget(self):
        spec = importlib.util.spec_from_file_location('package_install', ROOT/'tools/lab/package_install.py')
        helper = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(helper)
        with patch.object(sys, 'argv', ['package_install.py', '/private/install-'+'a'*32, '900', '600']), \
                patch.object(helper, 'enter_directory'), patch.object(helper, 'become_subreaper'), \
                patch.object(helper, 'verify_scope'), patch.object(helper, 'admit'), \
                patch.object(helper, 'supervise') as supervise:
            helper.main()
        commands, directory, timeout, recovery = supervise.call_args.args
        self.assertEqual((directory, timeout, recovery), (Path('.'), 900, 600))
        self.assertEqual(len(commands), 2)
        for command in commands:
            self.assertEqual(command[0], 'apt-get')
            options = [arg for arg in command if arg.startswith('DPkg::Lock::Timeout=')]
            self.assertEqual(len(options), 1)
            self.assertGreater(int(options[0].split('=')[1]), 0)
            self.assertLess(int(options[0].split('=')[1]), timeout)
        self.assertIn('update', commands[0])
        self.assertEqual(commands[1][-4:], ['install', '--reinstall', '-y', './package.deb'])

    def run_apt_lock_fixture(self, mode, timeout=3):
        # Fake apt is a real supervised subprocess. It never touches system apt state.
        fake = r"""#!/usr/bin/python3
import os, sys
from pathlib import Path
root = Path(os.environ['PUTGD_TEST_APT_DIR'])
calls = root/'apt-calls'
previous = calls.read_text() if calls.exists() else ''
command = sys.argv[-1]
calls.write_text(previous + command + '\n')
mode = os.environ['PUTGD_TEST_APT_MODE']
if (command == 'update' and (mode != 'once' or not previous)) or command == 'install-failure':
    print('E: Could not get lock /var/lib/apt/lists/lock. It is held by process 999 (unattended-upgr)')
    print('E: Unable to lock directory /var/lib/apt/lists/')
    if mode == 'other-error':
        print('E: unrelated repository failure')
    sys.exit(100)
"""
        setup = f"""
root = Path(sys.argv[2])
apt = root/'apt-get'
apt.write_text({fake!r})
apt.chmod(0o700)
install.os.environ['PATH'] = str(root) + install.os.pathsep + install.os.environ['PATH']
install.os.environ['PUTGD_TEST_APT_DIR'] = str(root)
install.os.environ['PUTGD_TEST_APT_MODE'] = {mode!r}
install.APT_LOCK_TIMEOUT = .6
commands = [['apt-get', '-o', 'DPkg::Lock::Timeout=1', 'update'],
            ['apt-get', '-o', 'DPkg::Lock::Timeout=1', 'install']]
if {mode!r} == 'install-failure':
    commands = [['apt-get', 'install-failure']]
"""
        return self.run_supervisor('', timeout, .5, setup=setup)

    def test_update_retries_only_lock_refusal_and_then_installs(self):
        data, log = self.run_apt_lock_fixture('once')
        self.assertTrue(data['result']['ok'], data)
        self.assertEqual(data['result']['exitCodes'], [0, 0])
        self.assertEqual(data['result']['updateLockRetries'], 1)
        self.assertIn('unattended-upgr', log)

    def test_update_lock_wait_is_bounded_and_other_errors_or_installs_never_retry(self):
        for mode in ('busy', 'other-error', 'install-failure'):
            with self.subTest(mode=mode):
                data, _ = self.run_apt_lock_fixture(mode)
                result = data['result']
                self.assertFalse(result['ok'])
                self.assertEqual(result['exitCodes'], [100])
                self.assertFalse(result['aborted'])
                self.assertEqual(result['remainingPids'], [])
                self.assertEqual(result.get('updateLockRetries', 0), 1 if mode == 'busy' else 0)
                self.assertLess(result['elapsedSeconds'], 3)

    def test_update_lock_wait_cannot_extend_shared_supervisor_budget(self):
        data, _ = self.run_apt_lock_fixture('busy', timeout=.1)
        self.assertTrue(data['result']['timedOut'])
        self.assertFalse(data['result']['ok'])
        self.assertEqual(data['result']['remainingPids'], [])

    def test_root_supervisor_scope_check_before_admission(self):
        spec = importlib.util.spec_from_file_location('package_install', ROOT / 'tools/lab/package_install.py')
        helper = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(helper)
        from unittest.mock import patch
        job = 'install-' + 'a' * 32
        unit = 'putgd-supervisor-' + job + '.scope'
        path = '/system.slice/' + unit
        record = {'tracking': 'cgroup-v2', 'cgroup': {'unit': unit, 'path': path}}
        def read(target, *a, **kw):
            return json.dumps(record) if target.name == 'supervisor-process.json' else '0::' + path
        with patch.object(Path, 'cwd', return_value=Path('/private/' + job)), \
                patch.object(Path, 'read_text', read):
            helper.verify_scope()
            helper.verify_scope(required=True)
            record['cgroup']['path'] += '/escaped'
            with self.assertRaisesRegex(ValueError, 'left its dedicated cgroup'):
                helper.verify_scope()
        record['cgroup']['path'] = '/user.slice/user-1000.slice/user@1000.service/app.slice/' + unit
        with patch.object(Path, 'cwd', return_value=Path('/private/' + job)), patch.object(Path, 'read_text',
                lambda target, *a, **kw: json.dumps(record) if target.name == 'supervisor-process.json'
                else '0::' + record['cgroup']['path']):
            helper.verify_scope()  # A historical optional scope record remains readable.
            with self.assertRaisesRegex(ValueError, 'left its dedicated cgroup'):
                helper.verify_scope(required=True)
        with patch.object(Path, 'read_text', side_effect=FileNotFoundError()):
            helper.verify_scope()  # Legacy callers remain supported.
            with self.assertRaisesRegex(ValueError, 'Missing supervisor scope identity'):
                helper.verify_scope(required=True)
        with patch.object(Path, 'read_text', return_value='{"tracking":"session"}'):
            helper.verify_scope()
            with self.assertRaisesRegex(ValueError, 'did not enter its dedicated cgroup'):
                helper.verify_scope(required=True)

    def test_output_rejects_symlinks_and_hardlinks_without_truncating_target(self):
        spec = importlib.util.spec_from_file_location('package_install', ROOT / 'tools/lab/package_install.py')
        helper = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(helper)
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            target = root / 'protected'
            target.write_text('preserved')
            for name, create in (('apt.log', lambda p: p.symlink_to(target)),
                                 ('progress.tmp', lambda p: p.symlink_to(target)),
                                 ('result.tmp', lambda p: os.link(target, p))):
                path = root / name
                create(path)
                with self.subTest(name=name), self.assertRaises((OSError, ValueError)):
                    with helper.output_file(path):
                        pass
                self.assertEqual(target.read_text(), 'preserved')
                path.unlink()

    def test_directory_rejects_symlink_and_wrong_owner_and_pins_inode(self):
        spec = importlib.util.spec_from_file_location('package_install', ROOT / 'tools/lab/package_install.py')
        helper = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(helper)
        old = os.open('.', os.O_RDONLY | os.O_DIRECTORY)
        try:
            with tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                run = root / 'run'
                run.mkdir(mode=0o700)
                link = root / 'link'
                link.symlink_to(run)
                with self.assertRaises(OSError):
                    helper.enter_directory(link, os.getuid())
                with self.assertRaises(ValueError):
                    helper.enter_directory(run, os.getuid() + 1)
                helper.enter_directory(run, os.getuid())
                run.rename(root / 'moved')
                run.symlink_to(root)
                helper.save(Path('result.json'), {'ok': True})
                self.assertTrue((root / 'moved/result.json').is_file())
                self.assertFalse((root / 'result.json').exists())
                os.fchdir(old)
        finally:
            os.fchdir(old)
            os.close(old)

    def test_timeout_waits_for_exact_descendants_then_verifies_package(self):
        child = WAIT_FOR_PROGRESS + '''
import sys
directory, parent = sys.argv[1], int(sys.argv[2])
# Stay alive until the supervisor has timed out, and the original command's
# exit has reparented us to the subreaper. Preserve the .8-second lower bound
# without racing a fixed sleep against startup or scheduler delays.
wait_for_progress(directory, lambda p: p.get('timedOut') and p.get('elapsedSeconds', 0) >= .8
                  and os.getppid() != parent)
print('descendant finished', flush=True)
'''
        output, log = self.run_supervisor(
            WAIT_FOR_PROGRESS + f'''
import subprocess, sys
print('partial apt output', flush=True)
child = subprocess.Popen([sys.executable, '-c', {child!r}, sys.argv[1], str(os.getpid())])
# A short-lived command can finish before the first /proc inventory. Keep both
# identities alive until they are recorded, then exercise orphan adoption.
wait_for_progress(sys.argv[1], lambda p: {{os.getpid(), child.pid}} <=
                  {{identity['pid'] for identity in p.get('ownedProcesses', [])}})
print(json.dumps({{'parent': os.getpid(), 'child': child.pid}}), flush=True)
''', 2, 3)
        result = output['result']
        self.assertTrue(result['timedOut'])
        self.assertTrue(result['ok'], result)
        self.assertGreaterEqual(result['elapsedSeconds'], .8)
        self.assertGreaterEqual(len(result['ownedProcesses']), 2)
        self.assertEqual(result['remainingPids'], [])
        self.assertEqual(output['queries'], [['dpkg', '-s', 'putgd'], ['dpkg', '--audit']])
        self.assertIn('partial apt output', log)
        self.assertIn('descendant finished', log)
        identities = next(json.loads(line) for line in log.splitlines() if line.startswith('{'))
        self.assertEqual({p['pid'] for p in result['ownedProcesses']}, set(identities.values()))

    def test_parent_exiting_during_discovery_cannot_hide_adopted_child(self):
        # Release the parent only after taking a stale /proc children snapshot,
        # then wait on its pidfd before validating that snapshot. This forces the
        # real adoption race without depending on scheduler timing or sleeps.
        setup = '''
import os, select
release_read, release_write = os.pipe()
real_popen = install.subprocess.Popen
def spawn(*args, **kwargs):
    kwargs['pass_fds'] = (release_read,)
    kwargs['env']['PUTGD_TEST_RELEASE_FD'] = str(release_read)
    return real_popen(*args, **kwargs)
install.subprocess.Popen = spawn
real_read_text = Path.read_text
released = False
def read_text(path, *args, **kwargs):
    global released
    text = real_read_text(path, *args, **kwargs)
    if str(path) == f'/proc/{os.getpid()}/task/{os.getpid()}/children' and text and not released:
        released = True
        fd = os.pidfd_open(int(text.split()[0]))
        try:
            os.write(release_write, b'go')
            assert select.select([fd], [], [], 5)[0], 'original command did not exit'
        finally:
            os.close(fd)
    return text
Path.read_text = read_text
'''
        child = WAIT_FOR_PROGRESS + '''
import sys
wait_for_progress(sys.argv[1], lambda p: p.get('timedOut') and p.get('elapsedSeconds', 0) >= .8)
print('adopted child finished', flush=True)
'''
        output, log = self.run_supervisor(f'''
import json, os, subprocess, sys
child = subprocess.Popen([sys.executable, '-c', {child!r}, sys.argv[1]])
print(json.dumps({{'child': child.pid}}), flush=True)
import select
fd = int(os.environ['PUTGD_TEST_RELEASE_FD'])
assert select.select([fd], [], [], 5)[0], 'supervisor did not take snapshot'
os.read(fd, 2)
''', 2, 3, setup=setup)
        result = output['result']
        self.assertTrue(result['timedOut'], result)
        self.assertTrue(result['ok'], result)
        self.assertGreaterEqual(result['elapsedSeconds'], .8)
        self.assertEqual(result['remainingPids'], [])
        child_pid = next(json.loads(line)['child'] for line in log.splitlines() if line.startswith('{'))
        self.assertEqual([p['pid'] for p in result['ownedProcesses']], [child_pid])
        self.assertIn('adopted child finished', log)
        self.assertEqual(output['queries'], [['dpkg', '-s', 'putgd'], ['dpkg', '--audit']])

    def test_abort_kills_only_owned_pids_and_reports_recovery_state(self):
        stranger = subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(30)'])
        try:
            output, _ = self.run_supervisor(
                "import subprocess,sys,time; subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(30)']); time.sleep(30)",
                .3, .3, 'Status: install ok unpacked\n')
            result = output['result']
            self.assertTrue(result['aborted'])
            self.assertFalse(result['ok'])
            self.assertIsNone(stranger.poll())
            self.assertGreaterEqual(len(result['stoppedPids']), 2)
            self.assertEqual(set(result['stoppedPids']), {p['pid'] for p in result['ownedProcesses']})
            self.assertEqual(result['remainingPids'], [])
            self.assertIn('unpacked', result['dpkgStatus']['stdout'])
            for pid in result['stoppedPids']:
                self.assertFalse(Path(f'/proc/{pid}').exists())
        finally:
            stranger.terminate()
            stranger.wait(timeout=5)

    def test_missing_dpkg_still_writes_final_recovery_report(self):
        output, _ = self.run_supervisor('pass', 2, 2, query_error=True)
        result = output['result']
        self.assertFalse(result['ok'])
        self.assertEqual(result['dpkgStatus'], {'error': 'FileNotFoundError'})
        self.assertEqual(result['dpkgAudit'], {'error': 'FileNotFoundError'})
        self.assertEqual(result['remainingPids'], [])

    def test_bad_package_state_cannot_certify_successful_command(self):
        output, _ = self.run_supervisor('pass', 2, 2, 'Status: install ok unpacked\n')
        self.assertFalse(output['result']['ok'])



class ScopeLauncherContracts(unittest.TestCase):
    """F5: the only scope authority granted to the lab user is this fixed launcher."""
    def setUp(self):
        spec = importlib.util.spec_from_file_location('package_scope', ROOT / 'tools/lab/package_scope.py')
        self.scope = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(self.scope)
        self.user = self.scope.pwd.struct_passwd(('lab', 'x', 1000, 1000, '', '/home/lab', '/bin/sh'))
        self.job = 'install-' + 'e' * 32

    def launch(self, argv, uid='1000', euid=0):
        environment = {} if uid is None else {'SUDO_UID': uid}
        with patch.dict(self.scope.os.environ, environment, clear=True), \
                patch.object(self.scope.os, 'geteuid', return_value=euid), \
                patch.object(self.scope.pwd, 'getpwuid', return_value=self.user), \
                patch.object(self.scope.os, 'execv', side_effect=SystemExit('exec')) as execute:
            try:
                self.scope.main(['package_scope.py', *argv])
            except SystemExit as stop:
                if str(stop) != 'exec':
                    return str(stop), None
            return None, execute.call_args.args

    def test_every_systemd_run_argument_is_fixed_and_the_command_runs_as_the_caller(self):
        for kind in ('staging', 'supervisor'):
            refusal, (path, arguments) = self.launch([kind, self.job, '--property=User=root', '--', 'sh'])
            self.assertIsNone(refusal)
            self.assertEqual(path, '/usr/bin/systemd-run')
            separator = arguments.index('/usr/bin/env')
            self.assertEqual(arguments[:separator], [
                '/usr/bin/systemd-run', '--system', '--scope', '--quiet', '--slice=system.slice',
                '--property=Delegate=yes', f'--unit=putgd-{kind}-{self.job}.scope', '--expand-environment=no', '--',
                '/usr/bin/setpriv', '--reuid=1000', '--regid=1000', '--init-groups', '--'])
            # Caller-chosen words reach only the unprivileged command, after a clean env.
            self.assertEqual(arguments[separator:], ['/usr/bin/env', '-i', 'HOME=/home/lab', 'USER=lab',
                'LOGNAME=lab', 'PATH=' + self.scope.PATH, '--property=User=root', '--', 'sh'])

    def test_refuses_other_units_root_callers_and_direct_use(self):
        for argv in (['other', self.job, 'sh'], ['staging', 'install-' + 'E' * 32, 'sh'],
                     ['staging', self.job + '.scope', 'sh'], ['staging', '../' + self.job, 'sh'],
                     ['staging', self.job], ['staging'], []):
            with self.subTest(argv=argv):
                refusal, call = self.launch(argv)
                self.assertIn('refused', refusal)
                self.assertIsNone(call)
        for uid, euid in (('0', 0), (None, 0), ('1000', 1000)):
            with self.subTest(uid=uid, euid=euid):
                refusal, call = self.launch(['staging', self.job, 'sh'], uid=uid, euid=euid)
                self.assertIn('refused', refusal)
                self.assertIsNone(call)

    def test_probe_proves_root_execution_without_creating_a_unit(self):
        import contextlib, io
        with patch.dict(self.scope.os.environ, {'SUDO_UID': '1000'}, clear=True), \
                patch.object(self.scope.os, 'geteuid', return_value=0), \
                patch.object(self.scope.pwd, 'getpwuid', return_value=self.user), \
                patch.object(self.scope.os, 'access', return_value=True), \
                patch.object(self.scope.os, 'execv') as execute, \
                contextlib.redirect_stdout(io.StringIO()) as output:
            self.assertEqual(self.scope.main(['package_scope.py', '--probe']), 0)
        execute.assert_not_called()
        self.assertEqual(output.getvalue(), 'putgd-lab-scope-ready\n')
        refusal, call = self.launch(['--probe'], euid=1000)
        self.assertIn('refused', refusal)

    def test_bootstrap_grants_the_launcher_never_systemd_run(self):
        script = (ROOT / 'tools/lab/bootstrap-guest.sh').read_text()
        grant = next(line for line in script.splitlines() if 'NOPASSWD' in line)
        self.assertNotIn('systemd-run', grant)
        self.assertIn('/usr/local/lib/putgd-lab/package_scope.py', grant)
        self.assertIn('install -m 755 "$(dirname "$0")/package_scope.py" /usr/local/lib/putgd-lab/package_scope.py',
                      script)
        self.assertTrue(os.access(ROOT / 'tools/lab/package_scope.py', os.X_OK))
        self.assertEqual((ROOT / 'tools/lab/package_scope.py').read_text().splitlines()[0], '#!/usr/bin/python3 -I')


if __name__ == '__main__':
    unittest.main()
