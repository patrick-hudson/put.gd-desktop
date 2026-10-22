"""tools/ci/install-packages.sh retries mirror failures, reports them and falls back to the snapshot (#368)."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / 'tools/ci/install-packages.sh'
TOOLS = ('bash', 'cat', 'comm', 'cp', 'cut', 'date', 'dirname', 'find', 'grep', 'mkdir', 'mktemp', 'mv', 'rm',
         'sleep', 'sort', 'tail', 'tee', 'touch')
MIRROR_ERROR = 'Librepo error: Cannot download Packages/m/mesa-vulkan-drivers-26.2.4-1.fc44.x86_64.rpm: All mirrors were tried'

# A package manager that fails its first $FAKE_FAILURES online calls and logs every call.
FAKE_DNF = r'''#!/bin/bash
echo "dnf $*" >> "$FAKE_CALLS"
cachedir=
for arg; do case "$arg" in --setopt=cachedir=*) cachedir=${arg#--setopt=cachedir=} ;; esac; done
case " $* " in
  *' --cacheonly '*) [ "${FAKE_OFFLINE:-ok}" = ok ] && exit 0; echo 'Cannot download the "mesa" package, cacheonly option is activated.'; exit 1 ;;
  *' no-such-package '*) echo 'No match for argument: no-such-package'; exit 1 ;;
esac
count=$(($(cat "$FAKE_COUNT" 2>/dev/null || echo 0) + 1)); echo "$count" > "$FAKE_COUNT"
if [ "$count" -le "${FAKE_FAILURES:-0}" ]; then echo "$FAKE_ERROR"; exit 1; fi
if [ -n "$cachedir" ]; then
  mkdir -p "$cachedir/updates-0123/packages"
  touch "$cachedir/updates-0123/packages/mesa-26.2.4-1.fc44.x86_64.rpm" "$cachedir/updates-0123/repomd.xml"
fi
'''
FAKE_APT = r'''#!/bin/bash
echo "apt-get $*" >> "$FAKE_CALLS"
case "$1" in
  update)
    count=$(($(cat "$FAKE_COUNT" 2>/dev/null || echo 0) + 1)); echo "$count" > "$FAKE_COUNT"
    if [ "$count" -le "${FAKE_FAILURES:-0}" ]; then echo 'E: Failed to fetch http://archive.ubuntu.com/ubuntu/dists/noble/InRelease  Hash Sum mismatch'; exit 100; fi ;;
  install) case " $* " in *' --no-download '*) [ "${FAKE_OFFLINE:-ok}" = ok ] || exit 100 ;; esac ;;
esac
exit 0
'''
FAKE_RPM = '#!/bin/bash\necho mesa-26.2.4-1.fc44.x86_64.rpm\n'


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
        for name, body in ({'dnf': FAKE_DNF, 'rpm': FAKE_RPM} if manager == 'dnf' else {'apt-get': FAKE_APT}).items():
            (self.bin / name).write_text(body)
            (self.bin / name).chmod(0o755)
        env = {'PATH': str(self.bin), 'PUTGD_PKG_BACKOFF': '0', 'PUTGD_PKG_LABEL': 'native (test)',
               'GITHUB_STEP_SUMMARY': str(self.summary), 'GITHUB_ENV': str(self.github_env),
               'FAKE_CALLS': str(self.calls), 'FAKE_COUNT': str(self.dir / 'count'), 'FAKE_ERROR': MIRROR_ERROR, **env}
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

    def test_metadata_only_refresh_has_no_snapshot_fallback(self):
        result, calls, summary = self.run_script('apt-get', FAKE_FAILURES='9', PUTGD_PKG_ATTEMPTS='2')
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertEqual([call.split()[1] for call in calls], ['update', 'update'])
        self.assertIn('**failed**: package metadata could not be refreshed', summary)


if __name__ == '__main__':
    unittest.main()
