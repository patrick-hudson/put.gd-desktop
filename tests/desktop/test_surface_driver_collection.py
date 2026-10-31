"""Terminal collection guards; synthetic files only, no desktop required."""
import json
from pathlib import Path
import tempfile
import unittest

from tools.lab.surface_driver.collect import collect, mark_terminal, require_terminal


class TerminalCollection(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.run = self.root / 'run'
        self.run.mkdir()
        self.receipt = {'secret_scan': {'passed': True}, 'environment': {'coverage_export': {
            'published': True, 'scanned_before_copy': True, 'copy_scan_passed': True}}}
        self.save()
        (self.run / 'coverage.json').write_text('{"entries": []}')

    def save(self):
        (self.run / 'receipt.json').write_text(json.dumps(self.receipt))

    def refused(self):
        with self.assertRaisesRegex(RuntimeError, 'collection refused'):
            collect([self.run], self.root / 'evidence.tgz')
        self.assertFalse((self.root / 'evidence.tgz').exists())

    def test_recipe_level_audit_and_export_are_not_terminal(self):
        self.refused()

    def test_missing_final_audit_or_export_refuses_even_with_exit(self):
        for key in ('secret_scan', 'environment'):
            with self.subTest(key=key):
                original = self.receipt.pop(key)
                self.save()
                mark_terminal(self.run, 0)
                self.refused()
                self.receipt[key] = original

    def test_changed_receipt_or_coverage_refuses(self):
        for name in ('receipt.json', 'coverage.json'):
            with self.subTest(name=name):
                self.save()
                mark_terminal(self.run, 0)
                path = self.run / name
                path.write_text(path.read_text() + ' ')
                self.refused()

    def test_terminal_failed_recipe_is_collectible_without_becoming_a_pass(self):
        mark_terminal(self.run, 1)
        self.assertEqual(require_terminal(self.run)['exit_code'], 1)
        collect([self.run], self.root / 'evidence.tgz')
        self.assertTrue((self.root / 'evidence.tgz').is_file())

    def test_one_pending_run_refuses_entire_collection(self):
        mark_terminal(self.run, 0)
        pending = self.root / 'pending'
        pending.mkdir()
        with self.assertRaisesRegex(RuntimeError, 'collection refused'):
            collect([self.run, pending], self.root / 'evidence.tgz')
        self.assertFalse((self.root / 'evidence.tgz').exists())
