"""Atomic schema-v2 receipt and coverage output; rows come only from ReceiptRow."""
import datetime as dt
import fcntl
import json
import os
from pathlib import Path
import tempfile
from .api import SCHEMA_VERSION
from .redact import Redactor, Sensitive, scan_run, save_frame


class Evidence:
    """One run's in-memory salt, masking provenance and safe persistence sinks."""
    def __init__(self, out, *, require_ocr=False, local=False):
        self.out = Path(out)
        self.redactor = Redactor()
        self.frames = {}
        self.require_ocr = require_ocr
        self.audit_images = not local
        self.failed_scan = None

    def command(self, kind, result):
        # Scripts/argv/stdout/stderr are never included, including on failure.
        self.redactor.remember(result)
        self.redactor.remember(result.get('stderr', ''), opaque=True)
        event = {'kind': kind, 'returncode': result.get('returncode'),
                 'stdout': {'redacted': True, 'length': len(result.get('stdout', '')), 'present': bool(result.get('stdout', ''))},
                 'stderr': {'redacted': True, 'length': len(result.get('stderr', '')), 'present': bool(result.get('stderr', ''))}}
        self.out.mkdir(mode=0o700, parents=True, exist_ok=True)
        with (self.out/'commands.jsonl').open('a') as stream:
            stream.write(json.dumps(self.redactor.safe(event))+'\n')

    def frame(self, image, path, regions, *, crop=None, fallback=False, annotations=None):
        path = Path(path)
        # A frame is registered only after the first (already redacted) write.
        self.out.mkdir(mode=0o700, parents=True, exist_ok=True)
        self.out.chmod(0o700)
        metadata = save_frame(image, path, regions, crop=crop, fallback=fallback, literals=self.redactor.audit_literals,
                              hosts=self.redactor.link_hosts, audit_images=self.audit_images)
        metadata.update(annotations or {})
        self.frames[str(path.relative_to(self.out))] = metadata
        write_json(self.out/'frame-masks.json', {'frames': self.frames}, redactor=self.redactor)
        return metadata

    def scan(self):
        result = scan_run(self.out, frames=self.frames, literals=self.redactor.audit_literals,
                          require_ocr=self.require_ocr, quarantine=True, hosts=self.redactor.link_hosts, audit_images=self.audit_images)
        if any(v.get('guard') == 'ocr-unavailable-fully-masked' for v in self.frames.values()):
            result['passed'] = False
            result['findings'].append({'kind': 'frame-ocr-unavailable', 'blocked': 'tesseract is required before saving visible pixels'})
        if not result['passed']:
            if self.failed_scan:
                result['findings'] = self.failed_scan['findings'] + result['findings']
                result['quarantine'] = self.failed_scan.get('quarantine', []) + result.get('quarantine', [])
            self.failed_scan = result
        return self.failed_scan or result


Evidence.frame.prewrite_gate = save_frame
Evidence.scan.run_audit = scan_run


def fail_coverage(path, rows, *, redactor=None):
    """Invalidate this receipt's rows, preserving unrelated campaign evidence."""
    path = Path(path)
    if not path.exists():
        return 0
    fields = ('interaction','desktop','package','host','placement','guest','destination','recipe','build')
    identity = lambda row: tuple(row.get(key) for key in fields)
    failed = {identity(row) for row in rows}
    with open(str(path)+'.lock', 'a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        value = json.loads(path.read_text())
        invalidated = 0
        for row in value.get('entries', []):
            if identity(row) in failed and row.get('functional') != 'failed':
                row['functional'] = 'failed'
                invalidated += 1
        # Other runs' rows are never rewritten unless this run's rows changed.
        if invalidated:
            write_json(path, value, redactor=redactor)
        return invalidated


def stamp():
    return dt.datetime.now(dt.timezone.utc).isoformat(timespec='seconds').replace('+00:00', 'Z')


def write_json(path, value, *, redactor=None):
    value = (redactor or Redactor()).safe(value)
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary = tempfile.mkstemp(prefix='.' + path.name, dir=path.parent)
    try:
        with os.fdopen(descriptor, 'w') as stream:
            json.dump(value, stream, indent=2, ensure_ascii=False, allow_nan=False)
            stream.write('\n'); stream.flush(); os.fsync(stream.fileno())
        os.replace(temporary, path)
    finally:
        Path(temporary).unlink(missing_ok=True)


def write_receipts(out, environment, rows, campaign, coverage, *, evidence=None, scan=None):
    redactor = evidence.redactor if evidence else Redactor()
    entries = [redactor.safe(row.to_json()) for row in rows]
    write_json(Path(out) / 'receipt.json', {'schemaVersion': SCHEMA_VERSION, 'environment': environment,
                                          'interactions': entries, **({'secret_scan': scan} if scan is not None else {})}, redactor=redactor)
    coverage = Path(coverage)
    coverage.parent.mkdir(parents=True, exist_ok=True)
    # One campaign is fed by many desktops/workers. Keep their rows under an
    # advisory lock; a rerun replaces only the same evidence identity.
    with open(str(coverage)+'.lock', 'a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        previous = json.loads(coverage.read_text()).get('entries', []) if coverage.exists() else []
        fields = ('interaction','desktop','package','host','placement','guest','destination','recipe','build')
        identity = lambda row: tuple(row.get(k) for k in fields)
        combined = {identity(row): row for row in previous}
        combined.update((identity(row), row) for row in entries)
        write_json(coverage, {'schemaVersion': SCHEMA_VERSION, 'campaign': campaign, 'entries': list(combined.values())}, redactor=redactor)
