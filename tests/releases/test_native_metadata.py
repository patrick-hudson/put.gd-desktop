import gzip
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools/releases'))
from native_metadata import generate


class NativeMetadataTests(unittest.TestCase):
    def test_copyright_retains_full_notices_and_deterministic_changelog(self):
        with tempfile.TemporaryDirectory() as temp:
            docs = Path(temp)
            (docs / 'LICENSE').write_text('MIT\nCopyright putgd\n')
            (docs / 'aws-sdk-LICENSE').write_text('Apache-2.0\nCopyright AWS\n')
            (docs / 'runtime-notices.txt').write_text('Exact source location\n')
            (docs / 'proton').mkdir()
            (docs / 'proton/BUN-NOTICES.txt.gz').write_bytes(gzip.compress(b'Copyright embedded Bun component\n\nExact grant\n'))
            (docs / 'proton/LICENSE.icu4c-78.3').write_text('Unicode License V3\nCopyright Unicode\n')
            identity = {'packageVersion': '26.1024.1134', 'gitCommit': 'a' * 40}
            generate(docs, identity, 'https://example.invalid/tag/v26.1024.1134', 1792800000)
            copyright = (docs / 'copyright').read_text()
            for notice in ('Copyright AWS', 'Copyright embedded Bun component', 'Unicode License V3', 'Exact source location'):
                self.assertIn(notice, copyright)
            before = (docs / 'changelog.gz').read_bytes()
            generate(docs, identity, 'https://example.invalid/tag/v26.1024.1134', 1792800000)
            self.assertEqual((docs / 'copyright').read_text(), copyright)
            self.assertEqual((docs / 'changelog.gz').read_bytes(), before)
            self.assertIn('putgd (26.1024.1134)', gzip.decompress(before).decode())
