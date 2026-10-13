"""Hosted workflow boundaries; actionlint separately parses the workflow syntax."""
from pathlib import Path
import unittest
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


class ReleaseChannels(unittest.TestCase):
    def test_appimage_installer_uses_external_logging_configuration(self):
        release = (ROOT / '.github/workflows/release.yml').read_text()
        appimage = release.split('  appimage:\n', 1)[1].split('  publish:\n', 1)[0]
        install = appimage.split('      - uses: jurplel/install-qt-action@', 1)[1]
        install = install.split('      - name:', 1)[0]
        self.assertIn('LOG_CFG: ${{ github.workspace }}/tools/ci/aqt-logging.ini', install)

    def test_existing_build_and_package_proofs_remain(self):
        build = (ROOT / '.github/workflows/build.yml').read_text()
        release = (ROOT / '.github/workflows/release.yml').read_text()
        for text in ('cmake --preset dev', 'ctest --preset dev --parallel 2',
                     "distribution: ['24.04', '26.04']", 'os: [windows-2022, macos-14]',
                     'cmake --preset release -DBUILD_TESTING=OFF -DCMAKE_PREFIX_PATH='):
            self.assertIn(text, build)
        for text in ('name: ubuntu24.04', 'name: ubuntu26.04', 'name: fedora44',
                     'python3 tools/releases/native.py', 'bash tools/releases/build-appimage.sh',
                     'python3 tools/releases/check_source_build.py', "if: matrix.name == 'ubuntu24.04'"):
            self.assertIn(text, release)
        self.assertIn('cancel-in-progress: true', build)
        self.assertIn("cancel-in-progress: ${{ github.event_name == 'pull_request' }}", release)

    def test_coordinated_build_omits_only_duplicate_linux_production_compile(self):
        build = (ROOT / '.github/workflows/build.yml').read_text()
        self.assertIn('branches: [main, nightly]', build)
        self.assertIn('package_tag:', build)
        self.assertIn('if: steps.package-gate.outputs.coordinated != \'true\'', build)
        self.assertIn('python3 tools/releases/nightly.py build-mode', build)

    def test_dispatcher_retains_preparation_before_exact_tag_fanout(self):
        nightly = (ROOT / '.github/workflows/nightly.yml').read_text()
        self.assertIn('branches: [nightly]', nightly)
        self.assertIn('schedule:', nightly)
        self.assertIn('cancel-in-progress: false', nightly)
        self.assertIn('nightly-preparation-${{ github.run_id }}', nightly)
        self.assertLess(nightly.index('actions/upload-artifact@'), nightly.index('nightly.py execute'))
        self.assertIn('ref: ${{ steps.pin.outputs.source_sha }}', nightly)
        self.assertIn('actions: write', nightly)
        self.assertIn('contents: write', nightly)

    def test_failed_dispatch_remains_failed_while_its_log_is_retained(self):
        lines = (ROOT / '.github/workflows/nightly.yml').read_text().splitlines()
        start = next(i for i, line in enumerate(lines) if 'name: Verify public no-op' in line)
        end = next(i for i in range(start + 1, len(lines)) if lines[i].startswith('      - '))
        step = lines[start:end]
        run = next(line.split('run: ', 1)[1] for line in step if 'run: ' in line)
        explicit_bash = any(line.strip() == 'shell: bash' for line in step)
        # Execute the actual workflow pipeline with a synthetic failing dispatcher.
        # These are GitHub's documented default/explicit Bash invocation options.
        shell = ['/bin/bash', '--noprofile', '--norc', '-e']
        if explicit_bash:
            shell += ['-o', 'pipefail']
        with tempfile.TemporaryDirectory(prefix='pgsh-') as temporary:
            directory = Path(temporary)
            dispatcher = directory / 'python3'
            dispatcher.write_text('#!/bin/sh\nprintf "synthetic dispatch failure\\n" >&2\nexit 17\n')
            dispatcher.chmod(0o755)
            result = subprocess.run([*shell, '-c', run], capture_output=True, text=True,
                env={**os.environ, 'PATH': temporary + os.pathsep + os.environ['PATH'],
                     'RUNNER_TEMP': temporary})
            self.assertEqual(result.returncode, 17, result.stdout + result.stderr)
            self.assertEqual((directory / 'nightly-execution.log').read_text(),
                             'synthetic dispatch failure\n')

    def test_final_publication_receipt_and_owned_mode_are_retained(self):
        release = (ROOT / '.github/workflows/release.yml').read_text()
        self.assertIn("inputs.nightly && inputs.publish && inputs.coordinator_run_id && 'Nightly'", release)
        step = release.split('      - name: Retain final publication evidence', 1)[1]
        for text in ('if: always()', 'publication-${{ github.run_id }}-${{ github.run_attempt }}',
                     'build/publish/publication.json', 'if-no-files-found: warn', 'retention-days: 90'):
            self.assertIn(text, step)

    def test_package_recipe_and_nightly_publisher_inputs_are_bound(self):
        release = (ROOT / '.github/workflows/release.yml').read_text()
        for text in ('nightly:', 'build_run_id:', 'build_workflow_id:', 'source_sha:',
                     'coordinator_run_id:', '--nightly', '--package-run-id', '--build-run-id',
                     'python3 tests/releases/test_verify_tag.py', 'nightly.py release-ci'):
            self.assertIn(text, release)
        self.assertIn("if: github.event_name != 'pull_request'", release)
        self.assertIn('actions: write', release)
        self.assertIn('needs: [ci, native, appimage]', release)
        self.assertNotIn('needs: [ci]\n', release)
        self.assertIn('test "$GITHUB_REF" = "refs/tags/$RELEASE_TAG"', release)


if __name__ == '__main__':
    unittest.main()
