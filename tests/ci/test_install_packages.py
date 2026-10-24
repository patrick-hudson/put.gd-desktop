"""tools/ci/install-packages.sh retries mirror failures and stalls, reports them and falls back to the snapshot (#368, #467)."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / 'tools/ci/install-packages.sh'
TOOLS = ('bash', 'cat', 'comm', 'cp', 'cut', 'date', 'dirname', 'find', 'grep', 'mkdir', 'mktemp', 'mv', 'rm',
         'sleep', 'sort', 'tail', 'tee', 'timeout', 'touch', 'tr')
MIRROR_ERROR = 'Librepo error: Cannot download Packages/m/mesa-vulkan-drivers-26.2.4-1.fc44.x86_64.rpm: All mirrors were tried'
# The last line nightly run 39851369595 printed before it hung for 93 minutes (#467).
STALL_LINE = 'Get:5 https://archive.ubuntu.com/ubuntu noble-security InRelease [126 kB]'
# The first $FAKE_STALLS online calls hang: silent prints one line and then nothing,
# trickle keeps printing and never finishes. Each records its PID in $FAKE_PIDS.
STALL = r'''stall() {
  echo $$ >> "$FAKE_PIDS"
  if [ "${FAKE_STALL_MODE:-silent}" = silent ]; then echo "$STALL_LINE"; exec sleep 60; fi
  while :; do echo 'Get:658 http://azure.archive.ubuntu.com/ubuntu noble/universe amd64 pocketsphinx-en-us all [27.4 MB]'; sleep 0.2; done
}
'''

# A package manager that fails its first $FAKE_FAILURES online calls and logs every call.
FAKE_DNF = r'''#!/bin/bash
echo "dnf $*" >> "$FAKE_CALLS"
''' + STALL + r'''
cachedir=
for arg; do case "$arg" in --setopt=cachedir=*) cachedir=${arg#--setopt=cachedir=} ;; esac; done
case " $* " in
  *' --cacheonly '*)
    case "${FAKE_OFFLINE:-ok}" in
      ok) exit 0 ;;
      missing) echo 'Cannot download the "mesa" package, cacheonly option is activated.' ;;
      *) echo 'Cache-only enabled but no cache for repository "updates"' ;;
    esac
    exit 1 ;;
  *' no-such-package '*) echo 'No match for argument: no-such-package'; exit 1 ;;
esac
count=$(($(cat "$FAKE_COUNT" 2>/dev/null || echo 0) + 1)); echo "$count" > "$FAKE_COUNT"
if [ "$count" -le "${FAKE_STALLS:-0}" ]; then stall; fi
if [ "$count" -le "${FAKE_FAILURES:-0}" ]; then echo "$FAKE_ERROR"; exit 1; fi
if [ -n "$cachedir" ]; then
  mkdir -p "$cachedir/updates-0123/packages"
  touch "$cachedir/updates-0123/packages/mesa-26.2.4-1.fc44.x86_64.rpm" "$cachedir/updates-0123/repomd.xml"
fi
'''
FAKE_APT = r'''#!/bin/bash
echo "apt-get $*" >> "$FAKE_CALLS"
''' + STALL + r'''
case "$1" in
  update)
    # Model the hosted runner's mirror choice: Azure keeps making progress but
    # never completes; either official Ubuntu fallback responds normally.
    if [ -n "${FAKE_MIRRORS:-}" ]; then
      read -r mirror rest < "$FAKE_MIRRORS"
      case "$mirror" in http*://azure.archive.ubuntu.com/ubuntu/*) stall ;; esac
      echo "Get:1 $mirror noble InRelease [256 kB]"
    fi
    count=$(($(cat "$FAKE_COUNT" 2>/dev/null || echo 0) + 1)); echo "$count" > "$FAKE_COUNT"
    if [ "$count" -le "${FAKE_STALLS:-0}" ]; then stall; fi
    if [ "$count" -le "${FAKE_FAILURES:-0}" ]; then echo 'E: Failed to fetch http://archive.ubuntu.com/ubuntu/dists/noble/InRelease  Hash Sum mismatch'; exit 100; fi ;;
  install) case " $* " in *' --no-download '*) [ "${FAKE_OFFLINE:-ok}" = ok ] || exit 100 ;; esac ;;
esac
exit 0
'''
FAKE_RPM = '#!/bin/bash\necho mesa-26.2.4-1.fc44.x86_64.rpm\n'
FAKE_DPKG = '#!/bin/bash\necho "dpkg $*" >> "$FAKE_CALLS"\n'


class InstallPackages(unittest.TestCase):
    def setUp(self):
        self.dir = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.dir)
        self.bin = self.dir / 'bin'
        self.bin.mkdir()
        for tool in TOOLS:
            (self.bin / tool).symlink_to(shutil.which(tool))
        self.summary = self.dir / 'summary.md'
        self.github_env = self.dir / 'github-env'
        self.calls = self.dir / 'calls'
        self.snapshot = self.dir / 'packages'

    def run_script(self, manager, *args, **env):
        fakes = {'dnf': FAKE_DNF, 'rpm': FAKE_RPM} if manager == 'dnf' else {'apt-get': FAKE_APT, 'dpkg': FAKE_DPKG}
        for name, body in fakes.items():
            (self.bin / name).write_text(body)
            (self.bin / name).chmod(0o755)
        env = {'PATH': str(self.bin), 'PUTGD_PKG_BACKOFF': '0', 'PUTGD_PKG_LABEL': 'native (test)',
               'GITHUB_STEP_SUMMARY': str(self.summary), 'GITHUB_ENV': str(self.github_env),
               'FAKE_CALLS': str(self.calls), 'FAKE_COUNT': str(self.dir / 'count'), 'FAKE_ERROR': MIRROR_ERROR,
               'FAKE_PIDS': str(self.dir / 'pids'), 'STALL_LINE': STALL_LINE, **env}
        result = subprocess.run([str(self.bin / 'bash'), str(SCRIPT), *args], env=env, text=True,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        calls = self.calls.read_text().splitlines() if self.calls.exists() else []
        summary = self.summary.read_text() if self.summary.exists() else ''
        return result, calls, summary

    def test_a_clean_install_reports_nothing(self):
        result, calls, summary = self.run_script('dnf', 'mesa')
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertEqual(len(calls), 1)
        self.assertNotIn('--refresh', calls[0])
        self.assertEqual(summary, '')

    def test_a_retry_refreshes_metadata_and_is_reported(self):
        result, calls, summary = self.run_script('dnf', 'mesa', FAKE_FAILURES='1')
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertEqual(len(calls), 2)
        self.assertNotIn('--refresh', calls[0])
        self.assertIn('--refresh', calls[1])
        self.assertIn('--setopt=retries=10', calls[1])
        self.assertIn('### Package install retries (native (test), dnf)', summary)
        self.assertIn(f'| 1 of 4 | failed, retrying in 0 s with refreshed metadata | `{MIRROR_ERROR}` |', summary)
        self.assertIn('| 2 of 4 | **passed on retry** |', summary)
        self.assertIn('::warning::Package install (native (test), dnf): attempt 1 of 4', result.stdout)

    def test_a_successful_install_refreshes_the_snapshot_with_installed_packages_only(self):
        stale = self.snapshot / 'updates-0123/packages/mesa-26.2.3-1.fc44.x86_64.rpm'
        stale.parent.mkdir(parents=True)
        stale.touch()
        (self.snapshot / 'putgd-snapshot').write_text('2026-10-15T00:00Z\n')
        result, calls, _ = self.run_script('dnf', 'mesa', PUTGD_PKG_CACHE=str(self.snapshot))
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn(f'--setopt=cachedir={self.snapshot}.live', calls[0])
        self.assertIn('--setopt=keepcache=True', calls[0])
        self.assertEqual(sorted(p.name for p in (self.snapshot / 'updates-0123/packages').iterdir()),
                         ['mesa-26.2.4-1.fc44.x86_64.rpm'])
        self.assertNotEqual((self.snapshot / 'putgd-snapshot').read_text(), '2026-10-15T00:00Z\n')
        self.assertEqual(self.github_env.read_text(), 'PUTGD_PKG_CACHE_REFRESHED=true\n')

    def test_when_every_mirror_attempt_fails_the_snapshot_installs_offline(self):
        self.snapshot.mkdir()
        (self.snapshot / 'putgd-snapshot').write_text('2026-10-20T04:00Z https://example.invalid/runs/1\n')
        result, calls, summary = self.run_script('dnf', 'mesa', FAKE_FAILURES='9', PUTGD_PKG_CACHE=str(self.snapshot))
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertEqual(len(calls), 5)
        self.assertIn('--cacheonly', calls[4])
        self.assertIn(f'--setopt=cachedir={self.snapshot}', calls[4].split())
        self.assertIn('| 4 of 4 | failed |', summary)
        self.assertIn('| snapshot | **installed offline from the package snapshot** saved '
                      '2026-10-20T04:00Z https://example.invalid/runs/1 |', summary)
        self.assertFalse(self.github_env.exists())

    def test_the_job_fails_when_the_snapshot_cannot_install_either(self):
        self.snapshot.mkdir()
        (self.snapshot / 'putgd-snapshot').write_text('2026-10-20T04:00Z\n')
        result, _, summary = self.run_script('dnf', 'mesa', FAKE_FAILURES='9', FAKE_OFFLINE='missing',
                                             PUTGD_PKG_CACHE=str(self.snapshot))
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertIn('| snapshot | **failed** | `Cannot download the "mesa" package, cacheonly option is activated.` |',
                      summary)
        self.summary.unlink()
        (self.dir / 'count').unlink()
        result, _, summary = self.run_script('dnf', 'mesa', FAKE_FAILURES='9', FAKE_OFFLINE='nocache',
                                             PUTGD_PKG_CACHE=str(self.snapshot))
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertIn('| snapshot | **failed** | `Cache-only enabled but no cache for repository "updates"` |', summary)

    def test_the_job_fails_without_a_snapshot(self):
        result, calls, summary = self.run_script('dnf', 'mesa', FAKE_FAILURES='9')
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertEqual(len(calls), 4)
        self.assertIn('| snapshot | **failed**: no package snapshot to install from |', summary)

    def test_an_unknown_package_fails_at_once(self):
        result, calls, summary = self.run_script('dnf', 'no-such-package')
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertEqual(len(calls), 1)
        self.assertIn('not retried: a package does not exist', summary)

    def test_apt_retries_update_and_install_and_uses_the_snapshot_offline(self):
        result, calls, summary = self.run_script('apt-get', '--no-install-recommends', 'dwz', FAKE_FAILURES='1')
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertEqual([call.split()[1] for call in calls], ['update', 'update', 'install'])
        self.assertIn('--no-install-recommends dwz', calls[2])
        self.assertIn('Acquire::Retries=3', calls[0])
        self.assertIn('Hash Sum mismatch', summary)
        self.assertIn('| 2 of 4 | **passed on retry** |', summary)

        self.calls.unlink()
        (self.dir / 'count').unlink()
        self.snapshot.mkdir()
        (self.snapshot / 'putgd-snapshot').write_text('2026-10-20T04:00Z\n')
        result, calls, _ = self.run_script('apt-get', 'dwz', FAKE_FAILURES='9', PUTGD_PKG_CACHE=str(self.snapshot))
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn('--no-download', calls[-1])
        self.assertIn(f'Dir::State::Lists={self.snapshot}/lists/', calls[-1])

    def assert_stalled_calls_were_stopped(self):
        for pid in (self.dir / 'pids').read_text().split():
            self.assertFalse(Path(f'/proc/{pid}').exists(), f'stalled package manager {pid} still runs')

    def test_networked_calls_drop_dead_connections(self):
        _, calls, _ = self.run_script('apt-get', 'dwz')
        for call in calls:
            self.assertIn('-o Acquire::Retries=3 -o Acquire::http::Timeout=60 -o Acquire::https::Timeout=60', call)
        self.calls.unlink()
        _, calls, _ = self.run_script('dnf', 'mesa')
        self.assertIn('--setopt=retries=10 --setopt=timeout=60 --setopt=minrate=1000', calls[0])

    def test_a_silent_stall_is_stopped_and_retried(self):
        started = time.monotonic()
        result, calls, summary = self.run_script('apt-get', 'dwz', FAKE_STALLS='1', PUTGD_PKG_STALL='1')
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertLess(time.monotonic() - started, 20)
        self.assert_stalled_calls_were_stopped()
        # The retry first finishes whatever dpkg work the stopped attempt left.
        self.assertEqual([call.split()[1] for call in calls], ['update', '--configure', 'update', 'install'])
        self.assertIn('| 1 of 4 | **stopped**, retrying in 0 s with refreshed metadata | '
                      f'`printed nothing for 1 s after: {STALL_LINE}` |', summary)
        self.assertIn('| 2 of 4 | **passed on retry** |', summary)
        self.assertIn('::warning::Package install (native (test), apt-get): attempt 1 of 4: **stopped**', result.stdout)

    def test_an_attempt_over_its_time_limit_is_stopped_and_the_snapshot_installs(self):
        self.snapshot.mkdir()
        (self.snapshot / 'putgd-snapshot').write_text('2026-10-20T04:00Z\n')
        started = time.monotonic()
        result, calls, summary = self.run_script('dnf', 'mesa', FAKE_STALLS='9', FAKE_STALL_MODE='trickle',
                                                 PUTGD_PKG_ATTEMPTS='2', PUTGD_PKG_TIMEOUT='2',
                                                 PUTGD_PKG_CACHE=str(self.snapshot))
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertLess(time.monotonic() - started, 20)
        self.assert_stalled_calls_were_stopped()
        self.assertEqual(len(calls), 3)
        self.assertIn('--cacheonly', calls[2])
        self.assertIn('| 1 of 2 | **stopped**, retrying in 0 s with refreshed metadata | `ran over the 2 s attempt limit '
                      'after: Get:658 http://azure.archive.ubuntu.com/ubuntu noble/universe amd64 pocketsphinx-en-us all '
                      '[27.4 MB]` |', summary)
        self.assertIn('| 2 of 2 | **stopped** |', summary)
        self.assertIn('| snapshot | **installed offline from the package snapshot** saved 2026-10-20T04:00Z |', summary)

    def test_when_every_attempt_stalls_without_a_snapshot_the_install_fails_in_bounded_time(self):
        started = time.monotonic()
        result, calls, summary = self.run_script('apt-get', 'dwz', FAKE_STALLS='9', PUTGD_PKG_ATTEMPTS='2',
                                                 PUTGD_PKG_STALL='1')
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertLess(time.monotonic() - started, 20)
        self.assert_stalled_calls_were_stopped()
        self.assertEqual([call.split()[1] for call in calls], ['update', '--configure', 'update'])
        self.assertIn(f'| 2 of 2 | **stopped** | `printed nothing for 1 s after: {STALL_LINE}` |', summary)
        self.assertIn('| snapshot | **failed**: no package snapshot to install from |', summary)

    def test_metadata_only_refresh_has_no_snapshot_fallback(self):
        result, calls, summary = self.run_script('apt-get', FAKE_FAILURES='9', PUTGD_PKG_ATTEMPTS='2')
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertEqual([call.split()[1] for call in calls], ['update', 'update'])
        self.assertIn('**failed**: package metadata could not be refreshed', summary)

    def test_hosted_azure_trickle_is_bounded_and_bypassed_without_a_snapshot(self):
        from test_hosted_apt_mirrors import mirrors, RUNNER_MIRRORS
        apt = self.dir / 'apt'
        apt.mkdir()
        mirror_list = apt / 'apt-mirrors.txt'
        mirror_list.write_text(RUNNER_MIRRORS)
        env = dict(FAKE_MIRRORS=str(mirror_list), FAKE_STALL_MODE='trickle',
                   PUTGD_PKG_ATTEMPTS='1', PUTGD_PKG_TIMEOUT='2')
        result, _, summary = self.run_script('apt-get', 'dwz', **env)
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertIn('ran over the 2 s attempt limit', summary)
        self.assert_stalled_calls_were_stopped()
        self.calls.unlink()
        self.summary.unlink()
        mirrors.configure(apt)
        result, calls, summary = self.run_script('apt-get', 'dwz', **env)
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn('https://archive.ubuntu.com/ubuntu/ noble InRelease', result.stdout)
        self.assertEqual([call.split()[1] for call in calls], ['update', 'install'])
        self.assertEqual(summary, '')
        self.assertFalse(self.github_env.exists())


if __name__ == '__main__':
    unittest.main()
