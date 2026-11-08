"""Current SDK build instructions and API references follow the source lock."""
import json
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[2]


# An SDK release named by context (any minor or major), plus any bare 1.11.x.
SDK_RELEASE = re.compile(
    r'(?:\bSDK(?: for C\+\+)?|aws-sdk-cpp\S*?|--branch)[\s/=]+v?(\d+\.\d+\.\d+)\b'
    r'|\b(1\.11\.\d+)\b')
# `git clone` of the SDK, following shell line continuations.
MANUAL_CLONE = re.compile(r'git clone(?:[^\n]|\\\n)*aws-sdk-cpp')


def mismatched_versions(text, release):
    found = {first or second for first, second in SDK_RELEASE.findall(text)}
    return sorted(found - {release})


class SdkDocumentationContract(unittest.TestCase):
    def test_current_sdk_instructions_match_lock(self):
        release = json.loads((ROOT / 'cmake/dependency-lock.json').read_text())['awsSdk']['release']
        # History and the dated golden preparation record describe past builds.
        for name in ('docs/release/build.md', 'docs/lab/remote-builders.md', 'docs/user/s3.md'):
            with self.subTest(document=name):
                self.assertEqual(mismatched_versions((ROOT / name).read_text(), release), [])
        build = (ROOT / 'docs/release/build.md').read_text()
        self.assertIn('bash tools/ci/provision-linux-sdk.sh', build)
        self.assertNotRegex(build, MANUAL_CLONE)

    def test_detects_stale_clone_and_api_reference(self):
        self.assertEqual(mismatched_versions('git clone --branch 1.11.907', '1.11.924'), ['1.11.907'])
        self.assertEqual(mismatched_versions('blob/1.11.907/header.h', '1.11.924'), ['1.11.907'])
        self.assertEqual(mismatched_versions('SDK 1.11.924', '1.11.924'), [])
        self.assertEqual(mismatched_versions('AWS SDK 1.12.0', '1.11.924'), ['1.12.0'])
        self.assertEqual(mismatched_versions('aws-sdk-cpp/blob/2.0.1/x.h', '1.11.924'), ['2.0.1'])
        self.assertEqual(mismatched_versions('TigerVNC 1.16.2, Qt 6.11.2', '1.11.924'), [])

    def test_detects_manual_clone_across_line_continuations(self):
        text = 'git clone --depth 1 \\\n  --branch 1.11.924 https://github.com/aws/aws-sdk-cpp.git dir'
        self.assertRegex(text, MANUAL_CLONE)
        self.assertNotRegex('git clone https://github.com/other/repo.git', MANUAL_CLONE)


if __name__ == '__main__':
    unittest.main()
