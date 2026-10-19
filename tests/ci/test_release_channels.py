"""Hosted workflow boundaries. actionlint in the contracts job parses the syntax."""
import json
from pathlib import Path
import re
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
    def test_exactly_four_workflows_and_no_tag_or_cross_workflow_triggers(self):
        self.assertEqual(sorted(path.name for path in WORKFLOWS.glob('*.yml')),
                         ['ci.yml', 'nightly.yml', 'package.yml', 'release.yml'])
        for path in WORKFLOWS.glob('*.yml'):
            text = path.read_text()
            with self.subTest(workflow=path.name):
                self.assertNotIn('tags:', text)
                self.assertNotIn('/dispatches', text)
                self.assertNotIn('gh workflow run', text)

    def test_every_action_is_pinned_to_a_full_commit(self):
        for path in WORKFLOWS.glob('*.yml'):
            for line in path.read_text().splitlines():
                use = re.match(r'\s+(?:- )?uses: (\S+)', line)
                if use and not use[1].startswith('./'):
                    with self.subTest(workflow=path.name, action=use[1]):
                        self.assertRegex(use[1], r'^[A-Za-z0-9_.-]+/[A-Za-z0-9_./-]+@[0-9a-f]{40}$')

    def test_ci_runs_on_pull_requests_and_nightly_pushes_with_one_required_summary(self):
        ci = workflow('ci.yml')
        self.assertIn('  pull_request:\n', ci)
        self.assertIn('  push:\n    branches: [nightly]\n', ci)
        self.assertRegex(ci, r'workflow_call:\n    inputs:\n      release_tag:\n(?:        .*\n)*?        required: true')
        summary = job(ci, 'ci')
        self.assertIn('name: CI', summary)
        self.assertIn('if: always()', summary)
        self.assertIn('needs: [changes, contracts, linux, portable, packages]', summary)
        self.assertIn("'packages': 'success' if selected.get('packages') == 'true' else 'skipped'", summary)

    def test_path_selection_gates_only_the_heavy_jobs(self):
        ci = workflow('ci.yml')
        self.assertIn('python3 tools/ci/changes.py', job(ci, 'changes'))
        self.assertNotIn('needs:', job(ci, 'contracts'))
        for name in ('linux', 'portable'):
            self.assertIn("if: needs.changes.outputs.build == 'true'", job(ci, name))
        self.assertIn("if: needs.changes.outputs.packages == 'true'", job(ci, 'packages'))

    def test_contracts_run_every_group_and_lint_workflows(self):
        contracts = job(workflow('ci.yml'), 'contracts')
        for group in ('release', 'proton', 'lab', 'observer', 'evidence'):
            self.assertIn('python3 tools/ci/contracts.py ' + group, contracts)
        self.assertEqual(contracts.count('if: always()'), 7)
        self.assertIn('sha256sum --check --strict', contracts)
        self.assertIn('qemu-utils', contracts)

    def test_linux_tests_keep_their_isolation_and_skip_the_duplicate_release_compile(self):
        linux = job(workflow('ci.yml'), 'linux')
        for text in ("distribution: ['24.04', '26.04']", 'cmake --preset dev -DPUTGD_BUNDLE_PROTON=ON',
                     'ctest --preset dev --parallel 8', 'runuser -u putgd-ci',
                     'python3 tools/ci/ctest_preflight.py', '-DCMAKE_CXX_COMPILER_LAUNCHER=ccache',
                     'aws-sdk-source-licenses', 'python3 tools/proton/provision_runtime.py'):
            self.assertIn(text, linux)
        self.assertNotIn('--preset release', linux)
        self.assertNotIn('sdk-source.sh', linux)
        self.assertIn("github.event_name != 'pull_request'", linux.split('Save compiler cache', 1)[1])

    def test_sanitizer_job_tests_against_instrumented_qtcore_outside_the_required_summary(self):
        ci = workflow('ci.yml')
        sanitizers = job(ci, 'sanitizers')
        for text in ("if: needs.changes.outputs.sanitizers == 'true'", 'container: ubuntu:26.04',
                     'clang libclang-rt-dev llvm', 'bash tools/ci/provision-qt-asan.sh build/qt-asan',
                     'cmake --preset clang-asan', 'ctest --preset clang-asan',
                     'LD_LIBRARY_PATH="$PWD/build/qt-asan/lib"', 'runuser -u putgd-ci',
                     'python3 tools/ci/ctest_preflight.py --build build/clang-asan'):
            self.assertIn(text, sanitizers)
        self.assertIn("github.event_name != 'pull_request'", sanitizers.split('Save compiler cache', 1)[1])
        self.assertNotIn('sanitizers', job(ci, 'ci'))
        presets = json.loads((ROOT / 'CMakePresets.json').read_text())
        configure = {preset['name']: preset for preset in presets['configurePresets']}
        self.assertEqual(configure['asan']['cacheVariables'], {'PUTGD_SANITIZER': 'address-undefined'})
        self.assertEqual(configure['clang-asan']['inherits'], 'asan')
        self.assertEqual(configure['clang-asan']['cacheVariables']['CMAKE_CXX_COMPILER'], 'clang++')
        test = {preset['name']: preset for preset in presets['testPresets']}['clang-asan']
        self.assertIn('halt_on_error=1', test['environment']['UBSAN_OPTIONS'])
        for name in ('settings_contract', 'operations_contract', 'history_contract',
                     'storage_locations_integration'):
            self.assertRegex(name, test['filter']['include']['name'])

    def test_linux_tests_reuse_earlier_durations_per_distribution(self):
        linux = job(workflow('ci.yml'), 'linux')
        restore, rest = linux.split('- name: Restore test durations', 1)[1].split('- name: Test\n', 1)
        test, save = rest.split('- name: Save test durations', 1)
        save = save.split('- name: Preserve structured test reports', 1)[0]
        path = 'path: build/dev/Testing/Temporary/CTestCostData.txt'
        key = 'key: ctest-cost-linux-${{ matrix.distribution }}-${{ github.run_id }}-${{ github.run_attempt }}'
        for step, action in ((restore, 'actions/cache/restore@'), (save, 'actions/cache/save@')):
            self.assertIn(action, step)
            self.assertIn(path, step)
            self.assertIn(key, step)
        self.assertIn('restore-keys: ctest-cost-linux-${{ matrix.distribution }}-', restore)
        self.assertIn('ctest --preset dev', test)
        self.assertIn('--resource-spec-file "$3"', test)
        self.assertIn('"$PWD/tools/ci/ctest-resources.json"', test)
        self.assertIn('if: always()', save)
        self.assertIn('github.event.pull_request.head.repo.full_name == github.repository', save)

    def test_cpu_bound_tests_share_a_bounded_cpu_pool(self):
        spec = json.loads((ROOT / 'tools/ci/ctest-resources.json').read_text())
        self.assertEqual(spec['version'], {'major': 1, 'minor': 0})
        self.assertEqual(spec['local'], [{'cpu': [{'id': 'runner', 'slots': 3}]}])
        self.assertIn('RESOURCE_GROUPS "cpu:1"', (ROOT / 'CMakeLists.txt').read_text())

    def test_portable_compiles_windows_only(self):
        portable = job(workflow('ci.yml'), 'portable')
        self.assertIn('os: [windows-2022]', portable)
        self.assertNotIn('macos', portable)
        self.assertIn('cmake --preset release -DBUILD_TESTING=OFF -DCMAKE_PREFIX_PATH=', portable)
        self.assertIn('--contract tools/ci/provision-portable-deps.sh', portable)

    def test_packages_build_every_format_and_collect_only_tagged_runs(self):
        packages = workflow('package.yml')
        self.assertIn('  workflow_call:\n', packages)
        for text in ('name: ubuntu24.04', 'name: ubuntu26.04', 'name: debian13', 'name: fedora44',
                     'python3 tools/releases/native.py', 'bash tools/releases/build-appimage.sh',
                     'python3 tools/releases/check_source_build.py', "if: matrix.name == 'ubuntu24.04'"):
            self.assertIn(text, packages)
        for name in ('native', 'appimage'):
            self.assertIn("if: inputs.release_tag != ''\n        run: python3 tools/releases/verify_tag.py",
                          job(packages, name))
            self.assertIn('bash tools/ci/sdk-source.sh', job(packages, name))
        collect = job(packages, 'collect')
        self.assertIn("if: inputs.release_tag != ''", collect)
        self.assertIn('needs: [native, appimage]', collect)
        self.assertIn('python3 tools/releases/collect.py', collect)
        self.assertIn('name: release-assets', collect)

    def test_appimage_installer_uses_external_logging_configuration(self):
        appimage = job(workflow('package.yml'), 'appimage')
        install = appimage.split('      - uses: jurplel/install-qt-action@', 1)[1].split('      - name:', 1)[0]
        self.assertIn('LOG_CFG: ${{ github.workspace }}/tools/ci/aqt-logging.ini', install)

    def test_nightly_is_scheduled_skips_unchanged_source_and_gates_publication_on_ci(self):
        nightly = workflow('nightly.yml')
        self.assertIn("schedule:\n    - cron: '23 5 * * *'", nightly)
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
        self.assertIn('contents: write', job(nightly, 'plan'))
        self.assertIn('contents: read', ci)

    def test_main_pushes_cut_candidates_and_stable_is_manual(self):
        release = workflow('release.yml')
        self.assertIn('  push:\n    branches: [main]\n', release)
        self.assertIn('options: [candidate, stable]', release)
        self.assertIn('group: release-main\n  cancel-in-progress: false', release)
        self.assertIn("CHANNEL: ${{ inputs.channel || 'candidate' }}", job(release, 'tag'))
        self.assertIn('python3 tools/releases/release_tag.py --channel "$CHANNEL"', job(release, 'tag'))
        self.assertIn('needs: [tag, ci]', job(release, 'draft'))
        self.assertIn('publish.py --tag "$TAG" --directory build/publish\n', job(release, 'draft'))

    def test_no_workflow_publishes_a_stable_or_candidate_release(self):
        # Comments and step-summary text may describe the manual command; nothing may run it.
        for path in WORKFLOWS.glob('*.yml'):
            commands = [line for line in path.read_text().splitlines()
                        if not line.lstrip().startswith(('#', 'echo '))]
            with self.subTest(workflow=path.name):
                self.assertFalse([line for line in commands if '--publish' in line or '--draft=false' in line])

    def test_sdk_cache_keys_follow_recipes_not_workflow_files(self):
        sdk_cache = (ROOT / 'tools/ci/sdk_cache.py').read_text()
        self.assertNotIn('.github/workflows', sdk_cache)
        for recipe in ('tools/releases/provision-sdk.sh', 'tools/ci/sdk-source.sh', 'cmake/dependency-lock.json'):
            self.assertIn(recipe, sdk_cache)


if __name__ == '__main__':
    unittest.main()
