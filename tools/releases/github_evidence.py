"""Exact-source GitHub producer evidence shared by coordination and publication."""
import json
import re
import subprocess
import time
from urllib.parse import quote

BUILD_WORKFLOW = '.github/workflows/build.yml'
PACKAGE_WORKFLOW = '.github/workflows/release.yml'
BUILD_JOBS = ('linux (24.04)', 'linux (26.04)', 'portable (windows-2022)', 'portable (macos-14)')
PACKAGE_JOBS = ('native (ubuntu24.04)', 'native (ubuntu26.04)', 'native (fedora44)', 'appimage')
_PENDING_STATUSES = ('queued', 'in_progress', 'waiting', 'pending', 'requested')


def full_sha(value):
    if not isinstance(value, str) or not re.fullmatch(r'[0-9a-f]{40}', value):
        raise ValueError('Full lowercase source commit SHA required')
    return value


class GitHub:
    def __init__(self, repository):
        if not re.fullmatch(r'[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+', repository):
            raise ValueError('Exact repository owner/name required')
        self.repository = repository

    def json(self, path, *, paginate=False, method=None, data=None):
        command = ['gh', 'api', 'repos/' + self.repository + '/' + path]
        if paginate:
            command += ['--paginate', '--slurp']
        if method:
            command += ['--method', method]
        if data is not None:
            command += ['--input', '-']
        return json.loads(subprocess.check_output(command, text=True,
                          input=json.dumps(data) if data is not None else None, timeout=60) or 'null')

    def workflow(self, path):
        value = self.json('actions/workflows/' + quote(path.rsplit('/', 1)[-1], safe=''))
        if value.get('path') != path or value.get('state') != 'active' or type(value.get('id')) is not int:
            raise ValueError('Unverified producer workflow path/ID/state')
        return value

    def dispatch(self, path, tag, inputs):
        workflow = self.workflow(path)
        self.json('actions/workflows/' + str(workflow['id']) + '/dispatches', method='POST',
                  data={'ref': tag, 'inputs': inputs})

    def asset_bytes(self, asset_id):
        if type(asset_id) is not int or asset_id <= 0:
            raise ValueError('Invalid release asset ID')
        return subprocess.check_output(['gh', 'api', '-H', 'Accept: application/octet-stream',
                    'repos/' + self.repository + '/releases/assets/' + str(asset_id)], timeout=600)


def remote_tag_commit(api, tag):
    value = api.json('git/ref/tags/' + quote(tag, safe=''))['object']
    for _ in range(8):
        if value.get('type') == 'commit':
            return full_sha(value['sha'])
        if value.get('type') != 'tag':
            break
        value = api.json('git/tags/' + full_sha(value['sha']))['object']
    raise ValueError('Remote tag does not resolve to a bounded commit')


def _run_identity(api, run, run_id, source_sha, workflow):
    if (type(run_id) is not int or run_id <= 0 or run.get('id') != run_id
            or run.get('workflow_id') != workflow['id'] or run.get('path') != workflow['path']
            or run.get('head_sha') != full_sha(source_sha)
            or run.get('repository', {}).get('full_name') != api.repository
            or run.get('head_repository', {}).get('full_name') != api.repository
            or type(run.get('run_attempt')) is not int or run['run_attempt'] <= 0):
        raise ValueError('CI repository, producer, run or full source identity mismatch')


def _verify_producer(api, run_id, source_sha, workflow_path, required_jobs, *, package_jobs=False):
    def successful_state(run):
        return ((run.get('status') == 'completed' and run.get('conclusion') == 'success')
                or (package_jobs and run.get('status') == 'in_progress' and run.get('conclusion') is None))
    workflow = api.workflow(workflow_path)
    run = api.json('actions/runs/' + str(run_id))
    _run_identity(api, run, run_id, source_sha, workflow)
    if not successful_state(run):
        raise ValueError('Required CI producer is not completed successfully: ' + str(run_id))
    pages = api.json('actions/runs/' + str(run_id) + '/jobs?filter=latest&per_page=100', paginate=True)
    jobs = [job for page in pages for job in page['jobs']]
    verified = {}
    for name in required_jobs:
        matching = [job for job in jobs if job.get('name') == name]
        if (len(matching) != 1 or matching[0].get('status') != 'completed'
                or matching[0].get('conclusion') != 'success'
                or type(matching[0].get('id')) is not int):
            raise ValueError('Required CI job missing, ambiguous or unsuccessful: ' + name)
        verified[name] = {'id': matching[0]['id'], 'conclusion': 'success'}
    # Do not accept jobs observed across different rerun attempts.
    after = api.json('actions/runs/' + str(run_id))
    _run_identity(api, after, run_id, source_sha, workflow)
    if after.get('run_attempt') != run['run_attempt'] or not successful_state(after):
        raise ValueError('CI producer changed during evidence verification')
    return {'schemaVersion': 1, 'repository': api.repository, 'sourceSha': source_sha,
            'workflowId': workflow['id'], 'workflowPath': workflow_path,
            'runId': run_id, 'runAttempt': run['run_attempt'], 'jobs': verified}


def verify_ci(api, run_id, source_sha, workflow_path, required_jobs):
    """Require the whole producer and every specified job to have succeeded."""
    return _verify_producer(api, run_id, source_sha, workflow_path, required_jobs)


def verify_package_jobs(api, run_id, source_sha):
    """Verify four completed package jobs while this run's publisher is running."""
    return _verify_producer(api, run_id, source_sha, PACKAGE_WORKFLOW, PACKAGE_JOBS, package_jobs=True)


def coordinate_build(api, source_sha, tag, *, timeout=7200, poll_seconds=15):
    """Reuse success, observe existing work, dispatch only when none exists."""
    full_sha(source_sha)
    workflow = api.workflow(BUILD_WORKFLOW)
    deadline = time.monotonic() + timeout
    selected = None
    dispatched = False
    while True:
        if selected is None:
            pages = api.json('actions/workflows/' + str(workflow['id']) +
                             '/runs?head_sha=' + source_sha + '&per_page=100', paginate=True)
            runs = [run for page in pages for run in page['workflow_runs'] if run.get('head_sha') == source_sha]
            for run in runs:
                _run_identity(api, run, run.get('id'), source_sha, workflow)
            successful = [run for run in runs if run.get('status') == 'completed' and run.get('conclusion') == 'success']
            pending = [run for run in runs if run.get('status') in _PENDING_STATUSES]
            candidates = successful or pending or runs
            if candidates:
                selected = max(candidates, key=lambda run: run['id'])['id']
            elif not dispatched:
                api.dispatch(BUILD_WORKFLOW, tag, {'package_tag': tag})
                dispatched = True
                continue
        if selected is not None:
            run = api.json('actions/runs/' + str(selected))
            _run_identity(api, run, selected, source_sha, workflow)
            if run.get('status') == 'completed':
                return verify_ci(api, selected, source_sha, BUILD_WORKFLOW, BUILD_JOBS)
            if run.get('status') not in _PENDING_STATUSES:
                raise ValueError('Unknown required Build status')
        if time.monotonic() >= deadline:
            raise TimeoutError('Exact-source Build verification deadline exceeded; producer=' + str(selected))
        time.sleep(min(poll_seconds, max(0, deadline - time.monotonic())))


def coordinate_packages(api, source_sha, tag, coordinator_run_id, build, *, timeout=120, poll_seconds=5):
    """Retain the original exact-tag package handoff instead of rebuilding on retry."""
    workflow = api.workflow(PACKAGE_WORKFLOW)
    deadline = time.monotonic() + timeout
    dispatched = False
    while True:
        pages = api.json('actions/workflows/' + str(workflow['id']) +
                         '/runs?head_sha=' + source_sha + '&per_page=100', paginate=True)
        runs = [run for page in pages for run in page['workflow_runs']
                if run.get('head_sha') == source_sha and run.get('head_branch') == tag
                and run.get('event') == 'workflow_dispatch']
        if runs:
            run = min(runs, key=lambda value: value['id'])
            _run_identity(api, run, run['id'], source_sha, workflow)
            if run.get('status') == 'completed' and run.get('conclusion') != 'success':
                raise ValueError('Original exact-tag package run requires recovery: ' + str(run['id']))
            if run.get('status') not in ('completed',) + _PENDING_STATUSES:
                raise ValueError('Unknown original package run status')
            return {'workflowId': workflow['id'], 'workflowPath': PACKAGE_WORKFLOW,
                    'runId': run['id'], 'runAttempt': run['run_attempt'], 'ref': tag,
                    'sourceSha': source_sha, 'coordinatorRunId': coordinator_run_id,
                    'reused': not dispatched}
        if not dispatched:
            api.dispatch(PACKAGE_WORKFLOW, tag, {'tag': tag, 'publish': True, 'nightly': True,
                'source_sha': source_sha, 'build_run_id': str(build.get('runId', '')),
                'build_workflow_id': str(build.get('workflowId', '')), 'coordinator_run_id': str(coordinator_run_id)})
            dispatched = True
            continue
        if time.monotonic() >= deadline:
            raise TimeoutError('Exact-tag package dispatch receipt deadline exceeded')
        time.sleep(min(poll_seconds, max(0, deadline - time.monotonic())))
