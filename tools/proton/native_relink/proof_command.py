"""Deadline-bounded commands for the offline native helper proof."""
import json
import math
import subprocess


def run(args, name, *, logs, proof, timeout_seconds, cwd=None, env=None):
    if not math.isfinite(timeout_seconds) or timeout_seconds <= 0:
        raise ValueError('Proof command timeout must be finite and positive')
    path = logs / name
    try:
        with path.open('w') as stream:
            subprocess.run([str(value) for value in args], cwd=cwd, env=env,
                           stdout=stream, stderr=subprocess.STDOUT, check=True,
                           timeout=timeout_seconds)
    except subprocess.TimeoutExpired:
        (proof / 'command-timeout.json').write_text(json.dumps(
            {'stage': name, 'reason': 'command_timeout', 'timeoutSeconds': timeout_seconds},
            indent=2) + '\n')
        raise SystemExit(f'Native proof command timed out: {name} ({timeout_seconds}s)') from None
    return path.read_text()
