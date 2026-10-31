"""Controller policy tests; these do not claim graphical desktop acceptance."""
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from tools.lab import nightly_desktops as nightly
from tools.lab.surface_driver.collect import mark_terminal


class NightlyTests(unittest.TestCase):
    def test_timer_drains_new_releases_without_repeating_or_skipping_a_tag(self):
        releases = [dict(tag_name='v26.1031.'+str(i)+'-nightly', draft=False,
                         published_at=f'2026-10-31T0{i}:00:00Z') for i in (1,2,3)]
        self.assertEqual(nightly.select_release(releases)['tag_name'], releases[2]['tag_name'])
        self.assertEqual(nightly.select_release(releases, releases[0]['published_at'])['tag_name'], releases[1]['tag_name'])
        self.assertEqual(nightly.select_release(releases, releases[0]['published_at'],
                         [releases[1]['tag_name']])['tag_name'], releases[2]['tag_name'])
        self.assertIsNone(nightly.select_release(releases, releases[2]['published_at']))

    def receipt(self, root, *, functional='passed', review=True, cleanup=None):
        export = dict(published=True, scanned_before_copy=True, copy_scan_passed=True)
        value = dict(environment=dict(coverage_export=export, cleanup_errors=cleanup),
            secret_scan=dict(passed=True), interactions=[dict(recipe='outcomes.region_upload',
                functional=functional, review=review, visual='frames', note='independent pixel assertion')])
        (root/'receipt.json').write_text(json.dumps(value))
        (root/'coverage.json').write_text('{}')
        mark_terminal(root, 0 if functional == 'passed' else 2)

    def test_visual_review_does_not_gate_automatic_oracles(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            self.receipt(root)
            self.assertTrue(nightly.verdict(root, 'outcomes.region_upload')['green'])

    def test_red_for_failed_missing_changed_or_unclean_proof(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            self.receipt(root, functional='failed')
            self.assertFalse(nightly.verdict(root, 'outcomes.region_upload')['green'])
            self.receipt(root, cleanup=['owned process retained'])
            self.assertFalse(nightly.verdict(root, 'outcomes.region_upload')['green'])
            self.receipt(root)
            self.assertFalse(nightly.verdict(root, 'outcomes.history_delete')['green'])
            (root/'receipt.json').write_text('{}')
            self.assertFalse(nightly.verdict(root, 'outcomes.region_upload')['green'])

    def test_live_disabled_is_explicit_and_red_has_receipt_link(self):
        table = nightly.table(dict(desktops=[dict(desktop='Aurora', cells={
            'region_upload':dict(green=False, receiptUrl='https://example.test/receipt'),
            'history_delete':dict(green=True)})]))
        self.assertIn('[🔴](https://example.test/receipt)', table)
        self.assertIn('🟢 | — | —', table)

    def test_secret_file_permissions_are_enforced(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp)/'token'
            path.write_text('synthetic-only')
            path.chmod(0o644)
            with self.assertRaises(RuntimeError):
                nightly.private_file(path)
            path.chmod(0o600)
            self.assertEqual(nightly.private_file(path), path)
            link = Path(tmp)/'link'
            link.symlink_to(path)
            with self.assertRaises(RuntimeError):
                nightly.private_file(link)

    def test_package_checksum_failure_blocks_before_any_guest(self):
        class GitHub:
            repo = 'owner/repo'
            def gh(self, *args):
                target = Path(args[args.index('--dir')+1])
                name = args[args.index('--pattern')+1]
                (target/name).write_text('0'*64+'  package.deb\n' if name == 'SHA256SUMS' else '{}')
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaisesRegex(RuntimeError, 'checksum mismatch'):
                nightly.release_assets(GitHub(), 'v26.1031.3384-nightly', Path(tmp)/'packages',
                                       [dict(asset='ubuntu26.04-x86_64.deb')])

    def test_failure_issue_is_updated_then_closed_by_passing_cell(self):
        pinned = dict(number=1, title='Nightly desktop results', node_id='PIN', html_url='https://example.test/1')
        failure = dict(number=2, title='nightly: Cinnamon Region failing', state='open')
        class GitHub:
            repo = 'owner/repo'
            writes = []
            def issues(self): return [pinned, failure]
            def issue(self, title, body, existing=None):
                self.writes.append(('issue', title, existing))
                return dict(existing or pinned, html_url='https://example.test/issue')
            def api(self, path, method='GET', body=None):
                self.writes.append((method, path, body))
                if method == 'GET': return dict(node_id='REPO')
                return dict(html_url='https://example.test/receipt')
            def gh(self, *args):
                return json.dumps(dict(data=dict(node=dict(pinnedIssues=dict(nodes=[dict(issue=dict(id='PIN'))])))))
        github = GitHub()
        result = dict(release='v26.1031.3384-nightly', build='a'*40, date='2026-10-31',
            desktops=[dict(desktop='Cinnamon', host='d69', evidence='/owned/run',
                           cells=dict(region_upload=dict(green=False, assertion='pixel mismatch')))])
        with tempfile.TemporaryDirectory() as tmp:
            nightly.publish(github, result, Path(tmp))
            updates = [w for w in github.writes if w[0] == 'issue' and w[1] == failure['title']]
            self.assertEqual(len(updates), 1)
            self.assertIs(updates[0][2], failure)
            github.writes.clear()
            result['desktops'][0]['cells']['region_upload']['green'] = True
            nightly.publish(github, result, Path(tmp))
            self.assertTrue(any(w[0] == 'PATCH' and w[1].endswith('/2')
                                and w[2]['state'] == 'closed' for w in github.writes))


if __name__ == '__main__':
    unittest.main()
