"""Current SDK build instructions and API references follow the source lock."""
import json
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[2]


def mismatched_versions(text, release):
    return sorted(set(re.findall(r'\b1\.11\.\d+\b', text)) - {release})


class SdkDocumentationContract(unittest.TestCase):
    def test_current_sdk_instructions_match_lock(self):
        release = json.loads((ROOT / 'cmake/dependency-lock.json').read_text())['awsSdk']['release']
        # History and the dated golden preparation record describe past builds.
        for name in ('docs/release/build.md', 'docs/lab/remote-builders.md', 'docs/user/s3.md'):
            with self.subTest(document=name):
                self.assertEqual(mismatched_versions((ROOT / name).read_text(), release), [])
        build = (ROOT / 'docs/release/build.md').read_text()
        self.assertIn('bash tools/ci/provision-linux-sdk.sh', build)
        self.assertNotRegex(build, r'git clone[^\n]*--branch\s+1\.11\.\d+')

    def test_detects_stale_clone_and_api_reference(self):
        self.assertEqual(mismatched_versions('git clone --branch 1.11.907', '1.11.924'), ['1.11.907'])
        self.assertEqual(mismatched_versions('blob/1.11.907/header.h', '1.11.924'), ['1.11.907'])
        self.assertEqual(mismatched_versions('SDK 1.11.924', '1.11.924'), [])


if __name__ == '__main__':
    unittest.main()
