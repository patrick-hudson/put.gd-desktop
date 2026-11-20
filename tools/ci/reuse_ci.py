#!/usr/bin/env python3
"""Reuse work only from successful nightly-branch push CI at the exact commit.

A nightly reuses source tests (`reuse`, called by changes.py), or, with
`packages`, the whole run: its tests, and the packages, SBOMs and sources it built
under the nightly's name (nightly.py stamp). Anything else, a missing, cancelled,
failed or expired run included, means the nightly builds them itself.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import time
from urllib.parse import urlencode

SOURCE_JOBS = frozenset({'contracts', 'linux (24.04)', 'linux (26.04)',
                         'sanitizers (clang ASan/UBSan)', 'CI'})
# package.yml's matrix and collect.py's NATIVE_TARGETS.
NATIVE = ('ubuntu24.04', 'ubuntu26.04', 'debian13', 'fedora44', 'fedora45')
PACKAGE_JOBS = frozenset({*(f'packages / native ({name})' for name in NATIVE),
                          *(f'packages / {name}' for name in ('arch', 'arch-install', 'appimage', 'appimage-sources',
                                                              'appimage-pipewire', 'sbom'))})
# What collect.py reads for a nightly (--installers-only).
PACKAGE_ARTIFACTS = (*(f'release-native-{name}' for name in (*NATIVE, 'archlinux')),
                     'release-appimage', 'appimage-sources', 'release-sbom')
WORKFLOW = '.github/workflows/ci.yml'


def github(path):
    return json.loads(subprocess.check_output(['gh', 'api', path], text=True, timeout=60))


def nightly_push(repository, commit, run):
    """A ci.yml push run of this repository's nightly branch at exactly this commit; never a pull request."""
    return (run.get('head_sha') == commit and run.get('head_branch') == 'nightly'
            and run.get('event') == 'push' and run.get('path') == WORKFLOW
            and run.get('repository', {}).get('full_name') == repository
            and run.get('head_repository', {}).get('full_name') == repository)


def succeeded(run):
    return run.get('status') == 'completed' and run.get('conclusion') == 'success'


def run_proof(repository, run, required, *, api=github):
    """The run's identity when every required job of its successful attempt succeeded."""
    # Never mix jobs from earlier attempts with the successful attempt.
    jobs = api(f'repos/{repository}/actions/runs/{run["id"]}/attempts/{run["run_attempt"]}/jobs?per_page=100')
    names = {job['name']: job for job in jobs['jobs']}
    if (jobs['total_count'] != len(jobs['jobs']) or len(names) != len(jobs['jobs'])
            or any(names.get(name, {}).get('conclusion') != 'success' for name in required)):
        return None
    return {'runId': run['id'], 'runAttempt': run['run_attempt'], 'gitCommit': run['head_sha'],
            'url': run['html_url'], 'jobs': {name: names[name]['id'] for name in sorted(required)}}


def find_run(repository, commit, *, api=github):
    query = urlencode({'event': 'push', 'branch': 'nightly', 'head_sha': commit,
                       'status': 'success', 'per_page': 100})
    runs = api(f'repos/{repository}/actions/workflows/ci.yml/runs?' + query)['workflow_runs']
    for run in runs:
        if nightly_push(repository, commit, run) and succeeded(run):
            found = run_proof(repository, run, SOURCE_JOBS, api=api)
            if found:
                return found
    return None


def package_artifacts(repository, run, *, api=github):
    """{name: id} of every artifact a nightly publishes from, each unique, unexpired and from this run."""
    listing = api(f'repos/{repository}/actions/runs/{run["id"]}/artifacts?per_page=100')
    artifacts = listing['artifacts']
    if listing['total_count'] != len(artifacts):
        return None
    found = {}
    for name in PACKAGE_ARTIFACTS:
        matches = [artifact for artifact in artifacts if artifact.get('name') == name]
        if len(matches) != 1:
            return None
        artifact, origin = matches[0], matches[0].get('workflow_run') or {}
        if (artifact.get('expired') is not False or origin.get('id') != run['id']
                or origin.get('head_sha') != run['head_sha']):
            return None
        found[name] = artifact['id']
    return found


def reusable(repository, run, *, api=github):
    found = run_proof(repository, run, SOURCE_JOBS | PACKAGE_JOBS, api=api)
    artifacts = found and package_artifacts(repository, run, api=api)
    return {**found, 'artifacts': artifacts} if artifacts else None


def find_package_run(repository, commit, *, wait, api=github, sleep=time.sleep, clock=time.monotonic, log=print):
    """The push-CI run that tested and packaged this commit, waiting up to `wait` seconds while it runs.

    Returns (proof or None, reason).
    """
    query = urlencode({'event': 'push', 'branch': 'nightly', 'head_sha': commit, 'per_page': 100})
    deadline = clock() + wait
    while True:
        runs = [run for run in api(f'repos/{repository}/actions/workflows/ci.yml/runs?' + query)['workflow_runs']
                if nightly_push(repository, commit, run)]
        for run in runs:
            if succeeded(run):
                found = reusable(repository, run, api=api)
                if found:
                    return found, f'run {run["id"]} tested and packaged this commit'
        pending = [run for run in runs if run.get('status') != 'completed']
        if not pending:
            if not runs:
                return None, 'no push-CI run of this commit'
            outcomes = ', '.join(f'run {run["id"]} {run.get("conclusion")}' for run in runs)
            return None, f'no successful push-CI run with every job and unexpired artifacts ({outcomes})'
        if clock() >= deadline:
            return None, f'push-CI run {pending[0]["id"]} still {pending[0].get("status")} after {wait // 60} minutes'
        log(f'Waiting for push-CI run {pending[0]["id"]} ({pending[0].get("status")}) of {commit[:12]}', flush=True)
        sleep(max(1, min(60, deadline - clock())))


def rehearse_run(repository, commit, run_id, *, api=github):
    """Rehearsals only: every check of `find_package_run` except that it was a nightly push."""
    run = api(f'repos/{repository}/actions/runs/{run_id}')
    if (run.get('head_sha') != commit or run.get('path') != WORKFLOW or not succeeded(run)
            or run.get('repository', {}).get('full_name') != repository
            or run.get('head_repository', {}).get('full_name') != repository):
        return None, f'run {run_id} is not a successful ci.yml run of this commit'
    found = reusable(repository, run, api=api)
    if not found:
        return None, f'run {run_id} lacks a successful job or an unexpired artifact'
    return {**found, 'rehearsal': True, 'event': run.get('event'), 'headBranch': run.get('head_branch')}, \
        f'rehearsal: run {run_id} ({run.get("event")} on {run.get("head_branch")})'


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


def packages(repository, commit, output, *, wait, rehearse=None):
    """Record which push-CI run's packages a nightly publishes, or none, so it builds them."""
    try:
        if rehearse:
            proof, reason = rehearse_run(repository, commit, rehearse, api=github)
        else:
            proof, reason = find_package_run(repository, commit, wait=wait, api=github)
    except (subprocess.SubprocessError, KeyError, TypeError, ValueError, OSError) as error:
        # A lookup failure only costs time: the nightly then builds and tests everything.
        proof, reason = None, 'cannot look up push CI (' + type(error).__name__ + ')'
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps({'gitCommit': commit, 'reused': proof is not None, 'packages': proof is not None,
                                  'reason': reason, 'producer': proof}, indent=2) + '\n')
    if os.environ.get('GITHUB_OUTPUT'):
        with open(os.environ['GITHUB_OUTPUT'], 'a') as stream:
            stream.write(f'run_id={proof["runId"] if proof else ""}\n')
            # The exact artifacts checked here, so a later rerun's same-named uploads are never published.
            ids = proof['artifacts'] if proof else {}
            stream.write('package_artifact_ids=' + ','.join(str(ids[name]) for name in PACKAGE_ARTIFACTS
                                                            if name in ids and name != 'appimage-sources') + '\n')
            stream.write(f'sources_artifact_id={ids.get("appimage-sources", "")}\n')
    if os.environ.get('GITHUB_STEP_SUMMARY'):
        with open(os.environ['GITHUB_STEP_SUMMARY'], 'a') as stream:
            if proof:
                stream.write(f'Publishing the packages of [push-CI run {proof["runId"]}]({proof["url"]}), '
                             f'attempt {proof["runAttempt"]}, which tested and packaged `{commit}`. '
                             'The publish job verifies every one against the tag before publishing.\n\n')
            else:
                stream.write(f'Building and testing here: {reason}.\n\n')
    print(json.dumps({'reason': reason, 'producer': proof}))
    return proof


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='command', required=True)
    found = commands.add_parser('packages', help='Find the push-CI run whose packages a nightly can publish')
    found.add_argument('--commit', required=True)
    found.add_argument('--output', type=Path, required=True)
    found.add_argument('--wait-minutes', type=int, default=0, help='How long to wait for a run still in progress')
    found.add_argument('--rehearse-run', type=int, help='Rehearsals only: accept this ci.yml run of the commit '
                       'whatever its event or branch; publish.py refuses the record')
    args = parser.parse_args()
    packages(os.environ['GITHUB_REPOSITORY'], args.commit, args.output, wait=args.wait_minutes * 60,
             rehearse=args.rehearse_run)


if __name__ == '__main__':
    main()
