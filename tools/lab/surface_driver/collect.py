"""Collect only terminal, audited driver runs (never recipe-level receipts)."""
import argparse
import hashlib
import json
from pathlib import Path
import tarfile

from .receipts import write_json


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def mark_terminal(out, exit_code):
    """Called after main returns, including target cleanup and the final audit."""
    out = Path(out)
    receipt = json.loads((out / 'receipt.json').read_text())
    proof = {
        'schemaVersion': 1, 'exit_code': exit_code,
        'secret_scan': receipt.get('secret_scan', {}).get('passed') is True,
        'coverage_export': receipt.get('environment', {}).get('coverage_export', {}).get('published') is True,
        'files': {name: digest(out / name) for name in ('receipt.json', 'coverage.json')},
    }
    write_json(out / 'terminal.json', proof)


def require_terminal(out):
    out = Path(out)
    try:
        proof = json.loads((out / 'terminal.json').read_text())
        receipt = json.loads((out / 'receipt.json').read_text())
        export = receipt.get('environment', {}).get('coverage_export', {})
        if (proof.get('schemaVersion') != 1 or type(proof.get('exit_code')) is not int
                or proof['exit_code'] not in (0, 1, 2)
                or proof.get('secret_scan') is not True or proof.get('coverage_export') is not True
                or receipt.get('secret_scan', {}).get('passed') is not True
                or not all(export.get(k) is True for k in ('published', 'scanned_before_copy', 'copy_scan_passed'))
                or proof.get('files') != {name: digest(out / name) for name in ('receipt.json', 'coverage.json')}):
            raise ValueError('incomplete or changed terminal proof')
    except (OSError, ValueError, TypeError, AttributeError) as error:
        raise RuntimeError('collection refused: terminal exit, final secret_scan and coverage_export required') from error
    return proof


def collect(runs, archive):
    runs = [Path(run) for run in runs]
    if len({run.name for run in runs}) != len(runs):
        raise ValueError('collection needs distinct run names')
    for run in runs:
        require_terminal(run)
    # Exclusive creation preserves earlier attempts. Refuse before writing any
    # archive if even one run is still finishing its cleanup/audit/export.
    with tarfile.open(archive, 'x:gz') as stream:
        for run in runs:
            stream.add(run, arcname=run.name)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('runs', type=Path, nargs='+')
    parser.add_argument('--archive', type=Path, required=True)
    args = parser.parse_args()
    collect(args.runs, args.archive)


if __name__ == '__main__':
    main()
