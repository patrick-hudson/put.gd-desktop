#!/usr/bin/env python3
"""Reuse source tests only from successful nightly-branch CI at the exact commit."""
import json
import os
from pathlib import Path
import subprocess
from urllib.parse import urlencode


def github(path):
    return json.loads(subprocess.check_output(['gh', 'api', path], text=True, timeout=60))


def find_run(repository, commit, *, api=github):
    query = urlencode({'event': 'push', 'branch': 'nightly', 'head_sha': commit,
                       'status': 'success', 'per_page': 100})
    prefix = f'repos/{repository}/actions'
    runs = api(prefix + '/workflows/ci.yml/runs?' + query)['workflow_runs']
    required = {'contracts', 'linux (24.04)', 'linux (26.04)', 'portable (windows-2022)',
                'sanitizers (clang ASan/UBSan)', 'CI'}
    for run in runs:
        if (run.get('head_sha') != commit or run.get('head_branch') != 'nightly'
                or run.get('event') != 'push' or run.get('status') != 'completed'
                or run.get('conclusion') != 'success' or run.get('path') != '.github/workflows/ci.yml'
                or run.get('repository', {}).get('full_name') != repository):
            continue
        # Never mix jobs from earlier attempts with the successful attempt.
        jobs = api(prefix + f'/runs/{run["id"]}/attempts/{run["run_attempt"]}/jobs?per_page=100')
        names = {job['name']: job for job in jobs['jobs']}
        if (jobs['total_count'] != len(jobs['jobs']) or len(names) != len(jobs['jobs'])
                or any(names.get(name, {}).get('conclusion') != 'success' for name in required)):
            continue
        return {'runId': run['id'], 'runAttempt': run['run_attempt'], 'gitCommit': commit,
                'url': run['html_url'], 'jobs': {name: names[name]['id'] for name in sorted(required)}}
    return None


def reuse(root):
    commit = subprocess.check_output(['git', '-C', str(root), 'rev-parse', 'HEAD'], text=True).strip()
    try:
        proof = find_run(os.environ['GITHUB_REPOSITORY'], commit)
    except (subprocess.SubprocessError, KeyError, ValueError, OSError) as error:
        # A lookup failure only costs time: it can never skip a test.
        print('::notice::Cannot reuse prior CI; running source tests (' + type(error).__name__ + ')')
        proof = None
    output = Path(root) / 'build/reused-ci.json'
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps({'gitCommit': commit, 'reused': proof is not None, 'producer': proof}, indent=2) + '\n')
    if proof and os.environ.get('GITHUB_STEP_SUMMARY'):
        with open(os.environ['GITHUB_STEP_SUMMARY'], 'a') as stream:
            stream.write(f'Source tests reuse [CI run {proof["runId"]}]({proof["url"]}), '
                         f'attempt {proof["runAttempt"]}, for exact commit `{commit}`. '
                         'Tagged package checks still run here.\n\n')
    return proof
