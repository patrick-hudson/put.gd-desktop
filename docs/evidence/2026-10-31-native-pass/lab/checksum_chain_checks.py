"""Validate published checksum indexes without running or retiring a lab guest.

Historical followup-* snapshots and explicitly original inventories retain
their recorded identities. Unavailable files are reported, never invented.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import tempfile
import unittest

from tools.lab import overlay_cleanup

HEX = re.compile(r'[0-9a-f]{64}$')
TEXT = re.compile(r'([0-9a-f]{64})\s+\*?(.+)$')
INDEX_NAME = re.compile(r'(.*[-_.])?(sha256sums(\.txt)?|sha256\.json|checksums?\.json|sha256)', re.I)
EVIDENCE = Path(__file__).resolve().parents[2]


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def entries(path):
    """Read flat JSON or conventional SHA256 text membership, independent of name."""
    text = path.read_text()
    if path.suffix.lower() == '.json':
        value = json.loads(text)
        if isinstance(value, dict) and value and all(isinstance(v, str) and HEX.fullmatch(v) for v in value.values()):
            return value
        # Source inventories nest the map under "files" beside their commit.
        files = value.get('files') if isinstance(value, dict) and INDEX_NAME.fullmatch(path.name) else None
        if isinstance(files, dict) and files and all(isinstance(v, str) and HEX.fullmatch(v) for v in files.values()):
            return files
        return None
    matches = [TEXT.fullmatch(line) for line in text.splitlines() if line and not line.startswith('#')]
    if matches and all(matches):
        return {match[2]:match[1] for match in matches}
    return None


def check_index(path, *, require_all=True):
    rows = entries(path)
    if rows is None:
        raise ValueError('unsupported checksum index structure')
    checked, unavailable = 0, 0
    for name, expected in rows.items():
        relative = Path(name)
        # An absolute/private source identity is not a published sibling file.
        if relative.is_absolute() or '..' in relative.parts:
            unavailable += 1
            if require_all:
                raise ValueError('checksum member is outside the publication')
            continue
        member = path.parent/relative
        if not member.is_file():
            unavailable += 1
            if require_all:
                raise ValueError('checksum member is unavailable')
            continue
        if digest(member) != expected:
            raise ValueError('published checksum member mismatch: '+name)
        checked += 1
    return checked, unavailable


def check_phase_b(root):
    cohorts = [root/name for name in ('cinnamon', 'gnome', 'kde')]
    cohorts += [root/'rerun'/name for name in ('cinnamon', 'gnome', 'kde')]
    indexed = 0
    for cohort in cohorts:
        indexed += check_index(cohort/'sha256.json')[0]
        manifest_path = cohort/'completion-manifest.json'
        if not manifest_path.exists():
            manifest_path = cohort/'lifecycle/completion-manifest.json'
        manifest = json.loads(manifest_path.read_text())
        # The first exports relocated lifecycle records into lifecycle/. Rebuild
        # only the file namespace in a temporary directory for the unchanged
        # production validator; no historical run or cleanup action is replayed.
        with tempfile.TemporaryDirectory(prefix='evidence-check-') as temporary:
            staging = Path(temporary)
            for name in manifest['files']:
                relative = Path(name)
                if relative.is_absolute() or '..' in relative.parts:
                    raise ValueError('invalid completion member')
                source = cohort/relative
                if not source.is_file():
                    source = manifest_path.parent/relative
                target = staging/relative
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(source, target)
            copied = staging/'completion-manifest.json'
            copied.write_text(json.dumps(manifest, indent=2)+'\n')
            overlay_cleanup._evidence(Path(manifest['run']), manifest['runId'], copied)
    combined = check_index(root/'sha256.json')[0]
    return dict(cohorts=6, cohortEntries=indexed, productionCompletionValidations=6, combinedEntries=combined)


def audit_formats(root):
    """Discover path-map index formats by content; report private/snapshot scopes."""
    result = dict(formats={}, historicalSnapshots=[], originalInventories=[], emptyIndexes=[], mismatches=[])
    for path in sorted(root.rglob('*')):
        if not path.is_file() or path.suffix.lower() in ('.png', '.ppm', '.jpg', '.webp', '.gz'):
            continue
        # Logs/commands contain historical hash observations, not current indexes.
        if path.suffix.lower() != '.json' and not re.search('sha256|checksum', path.name, re.I):
            continue
        # Conventionally named indexes must parse, or the audit fails. Other files
        # (verify logs, scope notes, nested inventories) count only when their
        # content is a flat digest map.
        candidate = bool(INDEX_NAME.fullmatch(path.name))
        name = str(path.relative_to(root))
        try:
            rows = entries(path)
        except (UnicodeError, ValueError):
            rows = None
        if rows is None and not candidate:
            continue
        if any(part.startswith('followup-') for part in path.relative_to(root).parts):
            result['historicalSnapshots'].append(name)
            continue
        if path.name.endswith('.original'):
            result['originalInventories'].append(name)
            continue
        if rows is None:
            if not path.read_bytes().strip():
                result['emptyIndexes'].append(name)  # e.g. an interrupted run
            else:
                result['mismatches'].append(name)
            continue
        family = 'json-path-to-digest' if path.suffix.lower() == '.json' else 'text-digest-to-path'
        summary = result['formats'].setdefault(family, dict(indexes=[], verifiedEntries=0, unavailableEntries=0))
        summary['indexes'].append(name)
        try:
            checked, unavailable = check_index(path, require_all=False)
            summary['verifiedEntries'] += checked
            summary['unavailableEntries'] += unavailable
        except ValueError:
            result['mismatches'].append(name)
    return result


class FormatRegression(unittest.TestCase):
    def test_production_validator_rejects_changed_index_until_parent_is_refreshed(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            index = root/'sha256.json'; index.write_text('{}')
            manifest = dict(files={'sha256.json':digest(index)})
            overlay_cleanup._members(root/'completion-manifest.json', manifest)
            index.write_text('{"receipt.json":"'+'0'*64+'"}')
            with self.assertRaisesRegex(overlay_cleanup.CleanupError, 'checksum mismatch'):
                overlay_cleanup._members(root/'completion-manifest.json', manifest)
            manifest['files']['sha256.json'] = digest(index)
            overlay_cleanup._members(root/'completion-manifest.json', manifest)

    def test_json_name_variants_reject_redacted_bytes_with_old_digest(self):
        for name in ('sha256.json', 'SHA256.json', 'files.sha256.json', 'checksums.json'):
            with self.subTest(name=name), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                receipt = root/'receipt.json'
                receipt.write_text('{"boot":"synthetic original"}')
                index = root/name
                index.write_text(json.dumps({'receipt.json':digest(receipt)}))
                self.assertEqual(check_index(index), (1, 0))
                receipt.write_text('{"boot":"[REDACTED]"}')
                with self.assertRaisesRegex(ValueError, 'mismatch'):
                    check_index(index)

    def test_alternate_text_name_is_checked_and_missing_scope_is_explicit(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            member = root/'published.json'; member.write_text('{}')
            index = root/'EVIDENCE-SHA256SUMS'
            index.write_text(digest(member)+'  published.json\n'+'0'*64+'  private-omitted.bin\n')
            self.assertEqual(check_index(index, require_all=False), (1, 1))
            with self.assertRaisesRegex(ValueError, 'unavailable'):
                check_index(index)

    def test_malformed_candidate_index_is_a_mismatch_and_unrelated_json_is_ignored(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            member = root/'published.json'; member.write_text('{}')
            (root/'sha256.json').write_text(json.dumps({'published.json':digest(member), 'other.json':'not-a-digest'}))
            (root/'SHA256SUMS').write_text(digest(member)+'  published.json\nmalformed row\n')
            (root/'receipt.json').write_text('{"status":"green"}')
            (root/'sha256-verify.txt').write_text('published.json: OK\n')
            (root/'empty').mkdir(); (root/'empty'/'SHA256SUMS').write_text('\n')
            report = audit_formats(root)
            self.assertEqual(sorted(report['mismatches']), ['SHA256SUMS', 'sha256.json'])
            self.assertEqual(report['emptyIndexes'], ['empty/SHA256SUMS'])
            self.assertEqual(report['formats'], {})


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--test', action='store_true')
    args = parser.parse_args()
    if args.test:
        unittest.main(argv=[__file__])
    else:
        report = audit_formats(EVIDENCE)
        report['phaseBChain'] = check_phase_b(EVIDENCE/'2026-10-30-tier1-phase-b')
        print(json.dumps(report, indent=2))
        raise SystemExit(bool(report['mismatches']))
