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


def assertCompilerCacheIsTrimmedAndSavedOnlyByNightlyPushes(test, text):
    test.assertRegex(text, r'echo "CCACHE_MAXSIZE=\d+[MG]" >> "\$GITHUB_ENV"')
    build, rest = text.split('        id: build\n', 1)[1].split("- name: Keep only this build's compiler cache entries", 1)
    test.assertIn('echo "CCACHE_BUILD_START=$(date +%s)" >> "$GITHUB_ENV"', build)
    trim, save = rest.split('- name: Save compiler cache', 1)
    test.assertIn('ccache --evict-older-than "$(( $(date +%s) - CCACHE_BUILD_START + 60 ))s"', trim)
    save = save.split('\n      - ', 1)[0]
    test.assertIn("if: github.event_name == 'push' && github.ref == 'refs/heads/nightly'\n", save)
    test.assertIn('actions/cache/save@', save)


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

    def test_ci_runs_on_pull_requests_and_nightly_pushes_with_one_required_summary(self):
        ci = workflow('ci.yml')
        self.assertIn('  pull_request:\n', ci)
        self.assertIn('  push:\n    branches: [nightly]\n', ci)
        self.assertRegex(ci, r'workflow_call:\n    inputs:\n      release_tag:\n(?:        .*\n)*?        required: true')
        summary = job(ci, 'ci')
        self.assertIn('name: CI', summary)
        self.assertIn('if: always()', summary)
        self.assertIn('needs: [changes, contracts, linux, packages, pipewire]', summary)
        self.assertIn("'packages': 'success' if selected.get('packages') == 'true' else 'skipped'", summary)
        self.assertIn("'pipewire': 'success' if selected.get('pipewire') == 'true' and selected.get('packages') != 'true' else 'skipped'", summary)

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

    def test_path_selection_gates_only_the_heavy_jobs(self):
        ci = workflow('ci.yml')
        self.assertIn('python3 tools/ci/changes.py', job(ci, 'changes'))
        self.assertNotIn('needs:', job(ci, 'contracts'))
        self.assertIn("if: needs.changes.outputs.build == 'true'", job(ci, 'linux'))
        self.assertIn("if: needs.changes.outputs.packages == 'true'", job(ci, 'packages'))

    def test_pipewire_check_links_the_appimage_plugin_on_a_pipewire_16_container(self):
        pipewire = job(workflow('ci.yml'), 'pipewire')
        self.assertIn('needs: [changes, packages]', pipewire)
        self.assertIn("needs.changes.outputs.pipewire == 'true'", pipewire)
        # Fresh images are checked inside Packages, concurrently with collection.
        fresh = job(workflow('package.yml'), 'appimage-pipewire')
        self.assertIn('needs: appimage', fresh)
        self.assertIn('name: release-appimage', fresh)
        self.assertIn('bash tests/desktop/pipewire/ci_check.sh --install build/pipewire/squashfs-root', fresh)
        self.assertIn("needs.packages.result == 'skipped'", pipewire)
        self.assertIn('sha256sum --check --strict', pipewire)
        self.assertIn('--appimage-extract', pipewire)
        self.assertRegex(pipewire, r'ubuntu:26\.04@sha256:[0-9a-f]{64}')
        self.assertIn('bash tests/desktop/pipewire/ci_check.sh --install build/pipewire/squashfs-root', pipewire)
        check = (ROOT / 'tests/desktop/pipewire/ci_check.sh').read_text()
        for text in ('1.5.81', 'recorder_properties.cpp', 'src/record/WaylandAcquisition.cpp',
                     'for mode in kwin mutter', 'kwin min-buffers=8'):
            self.assertIn(text, check)

    def test_debug_files_are_kept_per_build_outside_public_downloads(self):
        package = workflow('package.yml')
        for name in ('native', 'appimage'):
            text = job(package, name)
            self.assertIn('tools/releases/debug_symbols.py stage', text)
            self.assertIn('path: build/debug-symbols/', text)
            self.assertIn('retention-days: 90', text)
            self.assertIn('--packages-only', text)
        self.assertNotIn('\n  debug-symbols:\n', package)
        self.assertIn('--installers-only', job(package, 'collect'))
        self.assertIn('name: release-source-assets', job(package, 'collect'))
        self.assertNotIn('proton_sources.py', job(package, 'collect'))
        # Legacy symbol-archive unit tests still exercise DWARF deduplication.
        self.assertIn('qemu-utils dwz', job(workflow('ci.yml'), 'contracts'))

    def test_only_nightly_callers_opt_into_exact_source_ci_reuse(self):
        self.assertIn('reuse_source_ci: true', job(workflow('nightly.yml'), 'ci'))
        self.assertNotIn('reuse_source_ci: true', job(workflow('release.yml'), 'ci'))
        changes = job(workflow('ci.yml'), 'changes')
        self.assertIn('--reuse-source-ci', changes)
        self.assertIn('actions: read', changes)
        self.assertIn('name: source-ci-provenance', changes)

    def test_contracts_run_every_group_and_lint_workflows(self):
        contracts = job(workflow('ci.yml'), 'contracts')
        for group in ('release', 'proton', 'lab', 'observer', 'evidence'):
            self.assertIn('python3 tools/ci/contracts.py ' + group, contracts)
        self.assertEqual(contracts.count('if: always()'), 9)
        self.assertIn('python3-pil python3-pytest', contracts)
        self.assertIn('python3 -m pytest -v tests/desktop/test_surface*.py', contracts)
        self.assertIn('--junitxml=build/contracts/surface-tests.xml', contracts)
        self.assertIn('sha256sum --check --strict', contracts)
        self.assertIn('qemu-utils', contracts)

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

    def test_linux_tests_keep_their_isolation_and_skip_the_duplicate_release_compile(self):
        linux = job(workflow('ci.yml'), 'linux')
        for text in ("distribution: ['24.04', '26.04']", 'cmake --preset dev -DPUTGD_BUNDLE_PROTON=ON',
                     'tools/ci/ctest_retry.py --ledger build/dev/flake-ledger.json', '-- --preset dev --parallel 12',
                     'runuser -u putgd-ci',
                     'python3 tools/ci/ctest_preflight.py', '-DCMAKE_CXX_COMPILER_LAUNCHER=ccache',
                     'aws-sdk-source-licenses', 'python3 tools/proton/provision_runtime.py'):
            self.assertIn(text, linux)
        self.assertNotIn('--preset release', linux)
        self.assertNotIn('sdk-source.sh', linux)
        assertCompilerCacheIsTrimmedAndSavedOnlyByNightlyPushes(self, linux)

    def test_test_jobs_scale_waits_retry_integration_tests_once_and_report_flakes(self):
        ci = workflow('ci.yml')
        for name, build, scale in (('linux', 'build/dev', "'3'"), ('sanitizers', 'build/clang-asan', "'5'")):
            with self.subTest(job=name):
                steps = job(ci, name)
                test = steps.split('- name: Test', 1)[1].split('- name: ', 1)[0]
                self.assertIn('PUTGD_TEST_TIME_SCALE: ' + scale, test)
                self.assertIn('-DPUTGD_TEST_TIMEOUT_SCALE=' + scale.strip("'"), steps)
                self.assertIn(f'python3 tools/ci/ctest_retry.py --ledger {build}/flake-ledger.json', test)
                self.assertNotIn('--repeat', test)
                report = steps.split('- name: Report flaky tests', 1)[1].split('- name: ', 1)[0]
                self.assertIn(f"if: always() && hashFiles('{build}/flake-ledger.json') != ''", report)
                self.assertIn('GITHUB_TOKEN: ${{ github.token }}', report)
                self.assertIn(f'flake_ledger.py summary {build}/flake-ledger.json', report)
                self.assertIn('--output "$GITHUB_STEP_SUMMARY"', report)
                self.assertIn(f'flake_ledger.py issues {build}/flake-ledger.json', report)
                self.assertIn('issues: write', steps)
                for path in ('ctest-results-retry.xml', 'flake-ledger.json'):
                    self.assertIn(f'{build}/{path}', steps)
        # A called workflow's job may not request more than its caller grants.
        for name in ('nightly.yml', 'release.yml'):
            with self.subTest(workflow=name):
                self.assertIn('issues: write', job(workflow(name), 'ci'))

    def test_sanitizer_job_tests_against_instrumented_qtcore_outside_the_required_summary(self):
        ci = workflow('ci.yml')
        sanitizers = job(ci, 'sanitizers')
        for text in ("if: needs.changes.outputs.sanitizers == 'true'", 'container: ubuntu:26.04',
                     'clang libclang-rt-dev llvm', 'bash tools/ci/provision-qt-asan.sh build/qt-asan',
                     'cmake --preset clang-asan', 'tools/ci/ctest_retry.py', '-- --preset clang-asan',
                     'LD_LIBRARY_PATH="$PWD/build/qt-asan/lib"', 'runuser -u putgd-ci',
                     'python3 tools/ci/ctest_preflight.py --build build/clang-asan'):
            self.assertIn(text, sanitizers)
        assertCompilerCacheIsTrimmedAndSavedOnlyByNightlyPushes(self, sanitizers)
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

    def test_nightly_pushes_prune_superseded_caches_outside_the_required_summary(self):
        ci = workflow('ci.yml')
        prune = job(ci, 'caches')
        self.assertIn('needs: [linux, sanitizers]', prune)
        self.assertIn("if: ${{ !cancelled() && github.event_name == 'push' && github.ref == 'refs/heads/nightly' }}", prune)
        self.assertIn('actions: write', prune)
        self.assertIn('pull-requests: read', prune)
        self.assertIn('python3 tools/ci/cache_prune.py --repo "$GITHUB_REPOSITORY"', prune)
        self.assertNotIn('caches', job(ci, 'ci'))
        # A called workflow's job may not request more than its caller grants.
        for name in ('nightly.yml', 'release.yml'):
            caller = job(workflow(name), 'ci')
            with self.subTest(workflow=name):
                self.assertIn('actions: write', caller)
                self.assertIn('pull-requests: read', caller)
        self.assertEqual(ci.count('actions: write'), 1)

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
        self.assertIn('-- --preset dev', test)
        self.assertIn('--resource-spec-file "$3"', test)
        self.assertIn('"$PWD/tools/ci/ctest-resources.json"', test)
        self.assertIn('if: always()', save)
        self.assertIn('github.event.pull_request.head.repo.full_name == github.repository', save)

    def test_cpu_bound_tests_share_a_bounded_cpu_pool(self):
        spec = json.loads((ROOT / 'tools/ci/ctest-resources.json').read_text())
        self.assertEqual(spec['version'], {'major': 1, 'minor': 0})
        self.assertEqual(spec['local'], [{'cpu': [{'id': 'runner', 'slots': 3}]}])
        self.assertIn('RESOURCE_GROUPS "cpu:1"', (ROOT / 'cmake/TestPolicy.cmake').read_text())

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
        self.assertIn('needs: [native, appimage, appimage-sources, sbom]', collect)
        self.assertIn('python3 tools/releases/collect.py', collect)
        self.assertIn('name: release-assets', collect)
        # Every run, tagged or not, makes the SBOMs and their vulnerability report (#420).
        sbom = job(packages, 'sbom')
        self.assertIn('needs: [native, appimage]', sbom)
        self.assertNotIn('if:', sbom.split('steps:', 1)[0])
        for text in ('sbom.py tools', 'sbom.py database', 'sbom.py generate', 'sbom.py scan', '--release-tag "$RELEASE_TAG"',
                     'name: release-sbom', 'name: vulnerability-report'):
            self.assertIn(text, sbom)

    def test_nightly_rescans_the_latest_stable_sboms_without_gating_publication(self):
        nightly = workflow('nightly.yml')
        scan = job(nightly, 'stable-scan')
        self.assertNotIn('needs:', scan)
        self.assertNotIn('stable-scan', job(nightly, 'publish'))
        self.assertIn('sbom.py scan --released', scan)
        self.assertIn("grep -E '[.]cdx[.]json$' SHA256SUMS | sha256sum --check --strict", scan)
        self.assertIn('issues: write', scan)

    def test_renovate_uses_the_workflow_token_and_a_read_only_lookup_on_pull_requests(self):
        renovate = workflow('renovate.yml')
        self.assertIn("schedule:\n    - cron: '17 4 * * 1'", renovate)
        self.assertNotIn('secrets.', renovate)
        self.assertIn('permissions:\n  contents: read\n', renovate)
        lookup = job(renovate, 'lookup')
        self.assertIn("if: github.event_name == 'pull_request'", lookup)
        self.assertNotIn('permissions:', lookup)
        self.assertIn('RENOVATE_PLATFORM=local', lookup)
        run = job(renovate, 'renovate')
        self.assertIn("if: github.event_name != 'pull_request'", run)
        self.assertIn('token: ${{ github.token }}', run)
        self.assertIn('RENOVATE_ALLOWED_COMMANDS: \'["^python3 tools/maintenance/refresh_pins\\\\.py$"]\'', run)
        self.assertRegex(renovate, r'RENOVATE_IMAGE: ghcr\.io/renovatebot/renovate:[0-9.]+@sha256:[0-9a-f]{64}')

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

    def test_sdk_cache_keys_follow_recipes_not_workflow_files(self):
        sdk_cache = (ROOT / 'tools/ci/sdk_cache.py').read_text()
        self.assertNotIn('.github/workflows', sdk_cache)
        for recipe in ('tools/releases/provision-sdk.sh', 'tools/ci/sdk-source.sh', 'cmake/dependency-lock.json'):
            self.assertIn(recipe, sdk_cache)


if __name__ == '__main__':
    unittest.main()
