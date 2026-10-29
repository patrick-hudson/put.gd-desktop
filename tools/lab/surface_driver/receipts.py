"""Atomic schema-v2 receipt and coverage output; rows come only from ReceiptRow."""
import datetime as dt
import fcntl
import json
import os
from pathlib import Path
import tempfile
from .api import SCHEMA_VERSION


def stamp():
    return dt.datetime.now(dt.timezone.utc).isoformat(timespec='seconds').replace('+00:00', 'Z')


def write_json(path, value):
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


def write_receipts(out, environment, rows, campaign, coverage):
    entries = [row.to_json() for row in rows]
    write_json(Path(out) / 'receipt.json', {'schemaVersion': SCHEMA_VERSION, 'environment': environment,
                                          'interactions': entries})
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
        write_json(coverage, {'schemaVersion': SCHEMA_VERSION, 'campaign': campaign, 'entries': list(combined.values())})
