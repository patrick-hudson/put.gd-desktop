"""Hosted workflow boundaries. actionlint in the contracts job parses the syntax."""
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import textwrap
import unittest

ROOT = Path(__file__).resolve().parents[2]
WORKFLOWS = ROOT / '.github/workflows'


def workflow(name):
    return (WORKFLOWS / name).read_text()


def job(text, name):
    """Return one top-level job's block, from its key to the next job key."""
    match = re.search(r'^  ' + re.escape(name) + r':\n(.*?)(?=^  [A-Za-z0-9_-]+:\n|\Z)', text, re.S | re.M)
    if not match:
        raise AssertionError('missing job ' + name)
    return match[1]


class Workflows(unittest.TestCase):
    def test_exactly_five_workflows_and_no_tag_or_cross_workflow_triggers(self):
        self.assertEqual(sorted(path.name for path in WORKFLOWS.glob('*.yml')),
                         ['ci.yml', 'nightly.yml', 'package.yml', 'release.yml', 'renovate.yml'])
        for path in WORKFLOWS.glob('*.yml'):
            text = path.read_text()
            with self.subTest(workflow=path.name):
                self.assertNotIn('tags:', text)
                # Workflows never start each other; the only dispatch goes to the site repository.
                self.assertNotIn('/dispatches', text.replace('repos/patrick-hudson/put.gd-site/dispatches', ''))
                self.assertNotIn('gh workflow run', text)

    def test_every_action_is_pinned_to_a_full_commit(self):
        for path in WORKFLOWS.glob('*.yml'):
            for line in path.read_text().splitlines():
                use = re.match(r'\s+(?:- )?uses: (\S+)', line)
                if use and not use[1].startswith('./'):
                    with self.subTest(workflow=path.name, action=use[1]):
                        self.assertRegex(use[1], r'^[A-Za-z0-9_.-]+/[A-Za-z0-9_./-]+@[0-9a-f]{40}$')

    def test_ci_summary_accepts_only_the_selected_successes(self):
        summary = job(workflow('ci.yml'), 'ci')
        script = textwrap.dedent(summary.split("python3 - <<'PY'\n", 1)[1].rsplit('          PY', 1)[0])
        for build, packages, pipewire in ((False, False, False), (True, False, False),
                                          (False, False, True), (True, True, True)):
            expected = {'changes': 'success', 'contracts': 'success',
                        'linux': 'success' if build else 'skipped',
                        'packages': 'success' if packages else 'skipped',
                        'pipewire': 'success' if pipewire and not packages else 'skipped'}
            needs = {name: {'result': result} for name, result in expected.items()}
            needs['changes']['outputs'] = {name: str(value).lower() for name, value in
                                           (('build', build), ('packages', packages), ('pipewire', pipewire))}

            def run():
                return subprocess.run([sys.executable, '-c', script], capture_output=True, text=True,
                                      env=dict(os.environ, NEEDS=json.dumps(needs)))

            result = run()
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            for name, want in expected.items():
                for result in ('failure', 'cancelled', 'skipped', 'success'):
                    if result == want:
                        continue
                    with self.subTest(build=build, packages=packages, pipewire=pipewire,
                                      job=name, result=result):
                        needs[name]['result'] = result
                        self.assertNotEqual(run().returncode, 0)
                needs[name]['result'] = want

    def test_only_nightly_callers_opt_into_exact_source_ci_reuse(self):
        self.assertIn('reuse_source_ci: true', job(workflow('nightly.yml'), 'ci'))
        self.assertNotIn('reuse_source_ci: true', job(workflow('release.yml'), 'ci'))
        changes = job(workflow('ci.yml'), 'changes')
        self.assertIn('--reuse-source-ci', changes)
        self.assertIn('actions: read', changes)
        self.assertIn('name: source-ci-provenance', changes)

    def test_contracts_scan_every_ref_for_leaked_secrets(self):
        # A fork receives every public ref, so the scan reads all of them, with
        # the pinned gitleaks release and the repository's narrow allowlist.
        contracts = job(workflow('ci.yml'), 'contracts')
        self.assertIn('fetch-depth: 0', contracts.split('- name:', 1)[0])
        scan = contracts.split('- name: Scan every branch and tag for leaked secrets', 1)[1].split('- name:', 1)[0]
        self.assertIn('if: always()', scan)
        self.assertRegex(scan, r'gitleaks/releases/download/v8\.\d+\.\d+/gitleaks_8\.\d+\.\d+_linux_x64\.tar\.gz')
        self.assertRegex(scan, r'echo "[0-9a-f]{64}  \$archive" \| sha256sum --check --strict')
        for text in ('--config .gitleaks.toml', '--log-opts=--all', '--redact', '--exit-code 1',
                     '--report-path "$report_dir/raw.sarif"', '|| status=$?',
                     'python3 tools/ci/sanitize_gitleaks_sarif.py "$report_dir/raw.sarif" build/contracts/gitleaks.sarif',
                     'exit "$status"'):
            self.assertIn(text, scan)
        config = (ROOT / '.gitleaks.toml').read_text()
        self.assertIn('useDefault = true', config)
        self.assertNotIn('regexTarget = "line"', config)

    def test_packages_build_every_format_and_collect_only_tagged_runs(self):
        packages = workflow('package.yml')
        self.assertIn('  workflow_call:\n', packages)
        for text in ('name: ubuntu24.04', 'name: ubuntu26.04', 'name: debian13', 'name: fedora44', 'name: fedora45',
                     'python3 tools/releases/native.py', 'bash tools/releases/build-appimage.sh',
                     'python3 tools/releases/check_source_build.py', "if: matrix.name == 'ubuntu24.04'",
                     'check_source_build.py --inputs-of build/release'):
            self.assertIn(text, packages)
        for name in ('native', 'appimage'):
            self.assertIn("if: inputs.release_tag != ''\n        run: python3 tools/releases/verify_tag.py",
                          job(packages, name))
            self.assertIn('bash tools/ci/sdk-source.sh', job(packages, name))
        self.assertIn('bash tools/releases/build-arch.sh', job(packages, 'arch'))
        install = job(packages, 'arch-install')
        self.assertIn('needs: arch', install)
        self.assertIn('name: release-native-archlinux', install)
        self.assertIn('bash tools/releases/smoke-arch.sh', install)
        collect = job(packages, 'collect')
        self.assertIn("if: inputs.release_tag != ''", collect)
        self.assertIn('needs: [native, arch, arch-install, appimage, appimage-sources, sbom]', collect)
        self.assertIn('python3 tools/releases/collect.py', collect)
        self.assertIn('name: release-assets', collect)
        # Every run, tagged or not, makes the SBOMs and their vulnerability report (#420).
        sbom = job(packages, 'sbom')
        self.assertIn('needs: [native, arch, arch-install, appimage]', sbom)
        self.assertNotIn('if:', sbom.split('steps:', 1)[0])
        for text in ('sbom.py tools', 'sbom.py database', 'sbom.py generate', 'sbom.py scan', '--release-tag "$RELEASE_TAG"',
                     'name: release-sbom', 'name: vulnerability-report'):
            self.assertIn(text, sbom)

    def test_nightly_is_scheduled_skips_unchanged_source_and_gates_publication_on_ci(self):
        nightly = workflow('nightly.yml')
        self.assertIn("schedule:\n    - cron: '23 10 * * *'", nightly)
        self.assertIn('  workflow_dispatch:\n', nightly)
        self.assertNotIn('  push:', nightly)
        self.assertIn('group: nightly-publication\n  cancel-in-progress: false', nightly)
        self.assertIn('python3 tools/releases/nightly.py plan', job(nightly, 'plan'))
        ci = job(nightly, 'ci')
        self.assertIn("if: needs.plan.outputs.skip == 'false'", ci)
        self.assertIn('uses: ./.github/workflows/ci.yml', ci)
        publish = job(nightly, 'publish')
        self.assertIn('needs: [plan, ci]', publish)
        self.assertIn("if: needs.plan.outputs.skip == 'false'", publish)
        self.assertIn('publish.py --tag "$TAG" --directory build/publish --nightly', publish)
        notify = publish.split('- name: Notify put.gd-site\n', 1)[1].split('      - name:', 1)[0]
        self.assertIn('SITE_DISPATCH_TOKEN: ${{ secrets.SITE_DISPATCH_TOKEN }}', notify)
        self.assertIn('if [ -z "$SITE_DISPATCH_TOKEN" ]', notify)
        self.assertIn('gh api repos/patrick-hudson/put.gd-site/dispatches', notify)
        self.assertIn('-f event_type=release-published', notify)
        self.assertIn('-f "client_payload[tag]=$TAG"', notify)
        self.assertIn('-F "client_payload[prerelease]=true"', notify)
        self.assertNotIn('/dispatches', workflow('release.yml'))
        self.assertIn('contents: write', job(nightly, 'plan'))
        self.assertIn('contents: read', ci)

    def test_main_pushes_tag_and_stage_the_stable_release(self):
        # #373: no candidate channel; every push to main tags HEAD's calendar version.
        release = workflow('release.yml')
        self.assertIn('  push:\n    branches: [main]\n  workflow_dispatch:\npermissions:', release)
        self.assertNotIn('candidate', release)
        self.assertIn('group: release-main\n  cancel-in-progress: false', release)
        self.assertIn('python3 tools/releases/release_tag.py --output "$RUNNER_TEMP/release-tag.json"', job(release, 'tag'))
        self.assertIn('needs: [tag, ci]', job(release, 'draft'))
        self.assertIn('publish.py --tag "$TAG" --directory build/publish\n', job(release, 'draft'))
        self.assertIn('promote.py --finalize $TAG --open', job(release, 'draft'))

    def test_no_workflow_publishes_a_stable_or_candidate_release(self):
        # Comments and step-summary text may describe the manual command; nothing may run it.
        for path in WORKFLOWS.glob('*.yml'):
            commands = [line for line in path.read_text().splitlines()
                        if not line.lstrip().startswith(('#', 'echo '))]
            with self.subTest(workflow=path.name):
                self.assertFalse([line for line in commands if '--publish' in line or '--draft=false' in line])

if __name__ == '__main__':
    unittest.main()
