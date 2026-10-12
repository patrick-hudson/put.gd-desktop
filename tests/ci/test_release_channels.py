"""Hosted workflow boundaries; actionlint separately parses the workflow syntax."""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[2]


class ReleaseChannels(unittest.TestCase):
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
