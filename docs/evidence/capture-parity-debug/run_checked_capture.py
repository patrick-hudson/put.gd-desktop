#!/usr/bin/env python3
"""Validate the actual harness-created socket before its first app launch."""
import argparse
import hashlib
import json
from pathlib import Path
import runpy
import subprocess
import sys
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--harness', type=Path, required=True)
parser.add_argument('--harness-sha256', required=True)
parser.add_argument('--checker', type=Path, required=True)
args, forwarded = parser.parse_known_args()
if not forwarded or forwarded[0] != '--':
    parser.error('Put frozen harness arguments after --')
forwarded = forwarded[1:]
harness = args.harness.resolve(strict=True)
if hashlib.sha256(harness.read_bytes()).hexdigest() != args.harness_sha256:
    raise SystemExit('Harness fingerprint mismatch; choose the intended frozen source explicitly')
if '--output' not in forwarded:
    parser.error('The harness requires a fresh --output directory')
output = Path(forwarded[forwarded.index('--output') + 1]).resolve()
original = tempfile.TemporaryDirectory

class CheckedTemporaryDirectory(original):
    def __enter__(self):
        private = super().__enter__()
        socket = str(Path(private).resolve() / 'runtime' / 'putgd' / 'session.sock')
        checked = subprocess.run([sys.executable, str(args.checker.resolve(strict=True)),
                                  '--socket', socket], check=True, capture_output=True, text=True)
        evidence = json.loads(checked.stdout)
        if not evidence.get('passed'):
            raise RuntimeError('Actual runtime socket preflight failed')
        # The frozen harness creates output before entering its runtime context.
        with (output / 'socket-preflight.json').open('x') as receipt:
            json.dump(evidence, receipt, indent=2)
            receipt.write('\n')
        return private

tempfile.TemporaryDirectory = CheckedTemporaryDirectory
try:
    sys.argv = [str(harness), *forwarded]
    runpy.run_path(str(harness), run_name='__main__')
finally:
    tempfile.TemporaryDirectory = original
