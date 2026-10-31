"""Deadline-bounded commands for the offline native helper proof."""
import json
import math
import os
import signal
import subprocess


def run(args, name, *, logs, proof, timeout_seconds, cwd=None, env=None, capture_stdout=False):
    if not math.isfinite(timeout_seconds) or timeout_seconds <= 0:
        raise ValueError('Proof command timeout must be finite and positive')
    path = logs / name
    command = [str(value) for value in args]
    try:
        with path.open('w') as stream:
            process = subprocess.Popen(command, cwd=cwd, env=env, start_new_session=True,
                                       stdout=stream,
                                       stderr=subprocess.PIPE if capture_stdout else subprocess.STDOUT)
            try:
                _, stderr = process.communicate(timeout=timeout_seconds)
            except subprocess.TimeoutExpired:
                # The proof owns this new process group, including the smoke driver's helper.
                try:
                    os.killpg(process.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                process.communicate()
                raise
            if process.returncode:
                raise subprocess.CalledProcessError(process.returncode, command, stderr=stderr)
    except subprocess.TimeoutExpired:
        (proof / 'command-timeout.json').write_text(json.dumps(
            {'stage': name, 'reason': 'command_timeout', 'timeoutSeconds': timeout_seconds},
            indent=2) + '\n')
        raise SystemExit(f'Native proof command timed out: {name} ({timeout_seconds}s)') from None
    if capture_stdout:
        return subprocess.CompletedProcess(command, process.returncode, path.read_bytes(), stderr)
    return path.read_text()
