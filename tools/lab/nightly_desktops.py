#!/usr/bin/env python3
"""Unattended Tier 1 controller. Run on d69, never on the workstation.

  python3 tools/lab/nightly_desktops.py --now [vYY.MDD.N-nightly]

Private configuration: ~/.config/putgd-nightly/config.json and github-token
(owned, mode 0600). config.json supplies repository, builders (ssh argv, root,
rows), and each row's golden, bootstrapKey, knownHosts, profile and package.
The user service calls this without --now: already published tags are skipped.
The worker uses only existing lab launch/install/recipes/collection/retirement.
No compilation, map updates, per-night commits, or visual approval step.
"""
import argparse
from contextlib import contextmanager
import datetime as dt
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import socket
import stat
import subprocess
import sys
import tarfile
import threading
import uuid

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
FLOWS = dict(region_upload='Region', window_desktop='Window / desktop',
             recording_upload='Recording → upload', edit_upload='Edit → upload',
             link_browser='Link → browser', history_delete='History delete',
             s3_region_upload='Live S3', proton_region_upload='Live Proton')
LOCAL = list(FLOWS)[:6]
TAG = re.compile(r'v\d{2}\.\d{3,4}\.\d+-nightly')


def save(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
    tmp = path.with_suffix(path.suffix + '.tmp')
    tmp.write_text(json.dumps(value, indent=2, ensure_ascii=False) + '\n')
    tmp.chmod(0o600)
    tmp.replace(path)


def digest(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def private_file(path):
    path = Path(path)
    info = path.lstat()
    if (not stat.S_ISREG(info.st_mode) or info.st_uid != os.getuid()
            or info.st_mode & 0o077 or info.st_nlink != 1):
        raise RuntimeError('private configuration must be an owned mode-0600 regular file')
    return path


def command(argv, *, log=None, timeout=900, env=None):
    # Never include a command or its output in exceptions/public receipts.
    if log:
        with Path(log).open('xb') as stream:
            result = subprocess.run([str(a) for a in argv], cwd=ROOT, env=env,
                                    stdout=stream, stderr=stream, timeout=timeout)
    else:
        result = subprocess.run([str(a) for a in argv], cwd=ROOT, env=env,
                                capture_output=True, timeout=timeout)
    if result.returncode:
        step = Path(log).name if log else Path(str(argv[0])).name
        raise RuntimeError(f'{step} failed (exit {result.returncode}); private log retained')
    return result.stdout.decode() if not log else None


class GitHub:
    def __init__(self, repo, token):
        self.repo = repo
        self.env = dict(os.environ, GH_TOKEN=private_file(token).read_text().strip())

    def gh(self, *args):
        return command(['gh', *args], env=self.env)

    def api(self, path, method='GET', body=None):
        result = subprocess.run(['gh', 'api', path, '--method', method,
                                 *(['--input', '-'] if body is not None else [])],
                                input=json.dumps(body) if body is not None else None,
                                text=True, capture_output=True, env=self.env, timeout=120)
        if result.returncode:
            raise RuntimeError('GitHub request failed; response withheld')
        return json.loads(result.stdout) if result.stdout.strip() else {}

    def issues(self):
        issues = []
        for page in range(1, 1001):
            values = self.api(f'repos/{self.repo}/issues?state=all&per_page=100&page={page}')
            issues.extend(i for i in values if 'pull_request' not in i)
            if len(values) < 100:
                return issues
        raise RuntimeError('issue pagination limit exceeded')

    def issue(self, title, body, existing=None):
        base = f'repos/{self.repo}/issues'
        return self.api(base + (f'/{existing["number"]}' if existing else ''),
                        'PATCH' if existing else 'POST', dict(title=title, body=body, state='open'))


def release_assets(github, tag, out, rows):
    """Download the published bytes; every selected package must match SHA256SUMS."""
    out.mkdir(mode=0o700)
    version = tag[1:]
    assets = {f'putgd-{version}-{row["asset"]}' for row in rows}
    for name in sorted(assets | {'SHA256SUMS', 'release-builds.json'}):
        github.gh('release', 'download', tag, '--repo', github.repo, '--pattern', name, '--dir', str(out))
    sums = {}
    for line in (out/'SHA256SUMS').read_text().splitlines():
        sha, name = line.split(maxsplit=1)
        name = name.lstrip('*')
        if name in sums or not re.fullmatch('[0-9a-f]{64}', sha):
            raise RuntimeError('invalid SHA256SUMS')
        sums[name] = sha
    for name in assets | {'release-builds.json'}:
        if sums.get(name) != digest(out/name):
            raise RuntimeError('release asset checksum mismatch')
    identity = json.loads((out/'release-builds.json').read_text())['release']
    if identity['tag'] != tag or not re.fullmatch('[0-9a-f]{40}', identity['gitCommit']):
        raise RuntimeError('release identity mismatch')
    return identity


def catalog(github, build, out):
    """Find a successful producer with the exact build, including reused source CI."""
    runs = github.api(f'repos/{github.repo}/actions/runs?head_sha={build}&status=success&per_page=100')['workflow_runs']
    for run in runs:
        artifacts = github.api(f'repos/{github.repo}/actions/runs/{run["id"]}/artifacts?per_page=100')['artifacts']
        if any(a['name'] == 'ui-catalog' and not a['expired'] for a in artifacts):
            github.gh('run', 'download', str(run['id']), '--repo', github.repo,
                      '--name', 'ui-catalog', '--dir', str(out))
            manifest = json.loads((out/'manifest.json').read_text())
            if manifest['build']['gitCommit'] != build or manifest['build']['dirty']:
                raise RuntimeError('UI catalog identity mismatch')
            return dict(runId=run['id'], manifestSha256=digest(out/'manifest.json'))
    raise RuntimeError('no exact-build UI catalog available')


def verdict(out, recipe):
    """Only terminal audited automatic assertions count; review/visual flags do not."""
    from tools.lab.surface_driver.collect import require_terminal
    try:
        terminal = require_terminal(out)
        receipt = json.loads((Path(out)/'receipt.json').read_text())
        rows = [r for r in receipt['interactions'] if r['recipe'] == recipe]
        env = receipt['environment']
        passed = bool(rows) and all(r['functional'] == 'passed' for r in rows)
        passed = passed and not env.get('cleanup_errors') and not env.get('interrupted')
        assertion = next((r.get('note') or r['functional'] for r in rows if r['functional'] != 'passed'), '')
        if not rows:
            assertion = env.get('blocked') or 'flow produced no assertion receipt'
        if env.get('cleanup_errors'):
            assertion = 'private target cleanup failed'
        return dict(green=bool(passed), assertion=assertion[-1800:], terminal=terminal,
                    receiptSha256=digest(Path(out)/'receipt.json'), receipt=str(Path(out)/'receipt.json'))
    except (OSError, ValueError, KeyError, RuntimeError, TypeError):
        return dict(green=False, assertion='terminal audited receipt missing or invalid', receipt=str(Path(out)/'receipt.json'))


@contextmanager
def lease(host, session, out):
    from tools.lab.desktop_lock import Desktop
    lock = Desktop(Path.home()/'putgd-lab/locks', host)
    save(out/'lock-acquire.json', lock.acquire('nightly-desktops', session, 'automatic Tier 1 nightly; '+str(out), 45))
    stop = threading.Event()
    errors = []
    def beat():
        while not stop.wait(240):
            try:
                lock.extend(session, 45)
            except Exception:
                errors.append('desktop lease renewal failed')
                return
    thread = threading.Thread(target=beat, daemon=True)
    thread.start()
    slot = Path.home()/'putgd-lab/locks/tier1-guest.lock.d'
    acquired = False
    try:
        slot.mkdir(mode=0o700)
        acquired = True
        (slot/'session').write_text(session+'\n')
        yield errors
    finally:
        stop.set()
        thread.join()
        if acquired:
            (slot/'session').unlink()
            slot.rmdir()
        save(out/'lock-release.json', lock.release(session, verified_clean=True))


def preflight(out):
    command(['uptime'], log=out/'uptime.txt')
    if not os.access('/dev/kvm', os.R_OK | os.W_OK):
        raise RuntimeError('builder KVM unavailable')
    for proc in Path('/proc').iterdir():
        try:
            if proc.name.isdigit() and (proc/'comm').read_text().startswith('qemu-system'):
                raise RuntimeError('builder guest slot occupied')
        except OSError:
            pass
    available = int(next(line.split()[1] for line in Path('/proc/meminfo').read_text().splitlines()
                         if line.startswith('MemAvailable:'))) * 1024
    if available < 8*2**30 or os.getloadavg()[0] > os.cpu_count():
        raise RuntimeError('builder memory/load capacity unavailable')
    if shutil.disk_usage(ROOT).free < 20*2**30:
        raise RuntimeError('builder storage reserve unavailable')
    for port in (22446, 5946):
        with socket.socket() as sock:
            sock.bind(('127.0.0.1', port))
    save(out/'host-preflight.json', dict(kvm=True, availableMemory=available,
         freeBytes=shutil.disk_usage(ROOT).free, ports=[22446, 5946], placement='guest'))


def retire(run, out, rows):
    from tools.lab import lab
    info = lab.state(run)
    # Collect only terminal audited run directories. Rehash the archive before
    # writing the existing guarded cleanup manifest; never remove failed evidence.
    from tools.lab.surface_driver.collect import collect
    attempts = [p.parent for p in out.glob('*/receipt.json')]
    archive = out/'attachments.tgz'
    if attempts:
        collect(attempts, archive)
        with tarfile.open(archive) as stream:
            for member in stream.getmembers():
                if member.isfile():
                    content = stream.extractfile(member).read()
                    if hashlib.sha256(content).hexdigest() != digest(out/member.name):
                        raise RuntimeError('exported evidence checksum mismatch')
    completion = dict(schemaVersion=1, kind='vm-overlay-cleanup-evidence', run=str(run),
        runId=info['id'], completed=True, evidenceExportVerified=True,
        outcome='passed' if all(c['green'] for c in rows.values()) else 'failed')
    save(out/'completion.json', completion)
    files = {'completion.json': digest(out/'completion.json')}
    if archive.exists():
        files[archive.name] = digest(archive)
    save(out/'completion-manifest.json', dict(completion, files=files))
    lab.reset(run)
    save(out/'retirement.json', lab.cleanup_overlay(run, evidence_manifest=out/'completion-manifest.json',
         images_root=Path(info['golden']).parents[1]))


def worker(config, tag, identity, artifacts, out):
    from tools.lab import lab
    from tools.lab.nightly_desktop_guest import prepare
    out.mkdir(mode=0o700, parents=True)
    # Live admission reads the classifier/pin from this exact app commit.
    command(['git', 'fetch', '--no-tags', 'origin', 'tag', tag], log=out/'build-source-fetch.txt')
    host = config['host']
    result = []
    for row in config['rows']:
        rowout = out/row['name'].lower().replace(' ', '-')
        rowout.mkdir(mode=0o700)
        session = 'nightly-'+uuid.uuid4().hex[:12]
        cells = {flow: dict(green=False, assertion='desktop setup did not complete')
                 for flow in LOCAL + (['s3_region_upload', 'proton_region_upload'] if row.get('live') else [])}
        record = dict(desktop=row['name'], host=host, profile=row['profile'], cells=cells,
                      release=tag, build=identity['gitCommit'], evidence=str(rowout))
        run = ROOT/'work/lab-runs'/('n'+uuid.uuid4().hex[:8])
        try:
            with lease(host, session, rowout) as lease_errors:
                try:
                    preflight(rowout)
                    command([sys.executable, 'tools/diagnostics/check_socket_paths.py', '--socket',
                             run/'qmp.sock', run/'vnc.sock', '/dev/shm/nd/runtime/putgd/session.sock'],
                            log=rowout/'sockets.json')
                    # Remap an existing authenticated pin, never scan/trust a new key.
                    pins = Path(row['knownHosts']).read_text().splitlines()
                    key = next(' '.join(p.split()[1:3]) for p in pins if ' ssh-ed25519 ' in p)
                    known = rowout/'known_hosts'
                    known.write_text('[127.0.0.1]:22446 '+key+'\n')
                    command([sys.executable, 'tools/lab/lab.py', 'launch', '--profile', row['profile'],
                        '--golden', row['golden'], '--run', run, '--bootstrap-key', row['bootstrapKey'],
                        '--known-hosts', known, '--ssh-port', '22446', '--vnc-display', '46', '--timeout', '420'],
                        log=rowout/'launch.log', timeout=600)
                    package = artifacts/'packages'/f'putgd-{tag[1:]}-{row["asset"]}'
                    if digest(package) != identity['packages'][package.name]:
                        raise RuntimeError('worker package checksum mismatch')
                    if row['package'] == 'rpm':
                        lab.put(run, str(package), 'putgd-nightly.rpm')
                        lab.ssh(run, 'sudo -n rpm -U --oldpackage ~/putgd-nightly.rpm', timeout=300)
                    else:
                        command([sys.executable, 'tools/lab/lab.py', 'install', '--run', run,
                            '--appimage' if row['package'] == 'appimage' else '--deb', package],
                            log=rowout/'install.log', timeout=1600)
                    profiles = prepare(run, row, rowout)
                    save(rowout/'identity.json', dict(lab.state(run), driver=identity['driver']))
                    env = dict(os.environ, PUTGD_TIER1_SESSION=session)
                    for flow in cells:
                        destination = 's3' if flow.startswith('s3_') else 'proton' if flow.startswith('proton_') else 'local'
                        recipe = ('outcomes.' if destination == 'local' else 'outcomes_live.')+flow
                        for attempt in (1, 2):
                            if lease_errors:
                                raise RuntimeError(lease_errors[0])
                            name = flow+'-'+str(attempt)
                            target = rowout/name
                            runtime = '/dev/shm/n'+uuid.uuid4().hex[:8]
                            command([sys.executable, 'tools/diagnostics/check_socket_paths.py', '--socket',
                                     target/'runtime/putgd/session.sock', runtime+'/runtime/putgd/session.sock'],
                                    log=rowout/(name+'-sockets.json'))
                            args = [sys.executable, '-m', 'tools.lab.surface_driver.run', '--desktop', row['desktop'],
                                '--host', host, '--placement', 'guest', '--guest', row['profile'], '--run', str(run),
                                '--package', row['package'], '--destination', destination, '--recipes', recipe,
                                '--catalog', str(artifacts/'catalog'), '--build', identity['gitCommit'],
                                '--profiles', str(profiles), '--session-root', runtime,
                                '--out', str(target), '--campaign', 'nightly-desktops',
                                '--coverage', str(rowout/(name+'-coverage.json'))]
                            if destination == 'proton':
                                check = [sys.executable, 'tools/lab/proton_lab.py', 'check', '--session', session,
                                         '--owner', 'nightly-desktops']
                                command(check, log=rowout/(name+'-check-before.json'), env=env)
                                args = [sys.executable, 'tools/lab/proton_lab.py', 'lend', str(run),
                                        '--session', session, '--owner', 'nightly-desktops',
                                        '--purpose', 'automatic nightly outcome', '--']+args
                            # The driver exits nonzero for functional reds. Collect its
                            # terminal result anyway; never infer success from an exit alone.
                            try:
                                command(['nice', '-n', '10', *args], log=rowout/(name+'.log'), env=env, timeout=1800)
                            except RuntimeError:
                                pass
                            cell = verdict(target, recipe)
                            cell['attempt'] = attempt
                            cells[flow].setdefault('attempts', []).append(cell)
                            cells[flow].update({k: v for k, v in cell.items() if k != 'attempts'})
                            if destination == 'proton':
                                # lend must have returned the baton even when assertions failed.
                                registry = json.loads((Path.home()/'putgd-lab/locks/proton-registry.json').read_text())
                                last = registry.get('lastLend', {})
                                if last.get('run') != str(run) or not last.get('carriedBack'):
                                    raise RuntimeError('Proton baton not confirmed returned; overlay retained')
                                command(check, log=rowout/(name+'-check-after.json'), env=env)
                            if cell['green']:
                                break
                    retire(run, rowout, cells)
                finally:
                    # A failed live cleanup may own the only current baton or a
                    # remote publication. Preserve that running guest for recover.
                    proton = Path.home()/'putgd-lab/locks/proton.lock'
                    outstanding = proton.exists() and 'lent copy outstanding' in proton.read_text()
                    retained = False
                    for receipt_path in rowout.glob('*/receipt.json'):
                        evidence = json.loads(receipt_path.read_text())
                        environment = evidence.get('environment', {})
                        retained |= bool(environment.get('cleanup_errors'))
                        retained |= 'retained' in str(environment.get('private_cleanup', ''))
                    if (run/'state.json').exists() and not outstanding and not retained:
                        try:
                            if (run/'disk.qcow2').exists():
                                retire(run, rowout, cells)
                        finally:
                            lab.reset(run)
        except Exception as error:
            # Only our fixed messages are public. Harness/SSH exception payloads stay private.
            reason = str(error) if type(error) is RuntimeError else 'controller '+type(error).__name__+'; private evidence retained'
            record['controllerFailure'] = reason
            for cell in cells.values():
                cell.update(green=False, assertion=reason)
        save(rowout/'result.json', record)
        result.append(record)
        print(json.dumps(dict(desktop=row['name'], green=sum(c['green'] for c in cells.values()), total=len(cells))), flush=True)
    save(out/'results.json', result)
    return result


def table(result):
    lines = ['| Desktop | '+' | '.join(FLOWS.values())+' |', '| --- | '+' | '.join('---' for _ in FLOWS)+' |']
    for row in result['desktops']:
        values = []
        for flow in FLOWS:
            cell = row['cells'].get(flow)
            if cell is None:
                values.append('—')
            else:
                color = '🟢' if cell['green'] else '🔴'
                values.append(f'[{color}]({cell["receiptUrl"]})' if cell.get('receiptUrl') else color)
        lines.append('| '+row['desktop']+' | '+' | '.join(values)+' |')
    return '\n'.join(lines)+'\n'


def publish(github, result, out):
    issues = github.issues()
    title = 'Nightly desktop results'
    pinned = next((i for i in issues if i['title'] == title), None)
    if not pinned:
        pinned = github.issue(title, 'First automatic nightly run is being published.')
    result['issueUrl'] = pinned['html_url']
    for row in result['desktops']:
        for flow, cell in row['cells'].items():
            # Compact receipt contains only the audited assertion/terminal proof,
            # exact identities and attachment archive path. Never publish raw logs.
            receipt = dict(desktop=row['desktop'], host=row['host'], release=result['release'],
                build=result['build'], flow=flow, green=cell['green'], assertion=cell.get('assertion'),
                attempts=cell.get('attempts', []), evidence=row['evidence'],
                controllerFailure=row.get('controllerFailure'))
            body = f'{result["release"]} · {row["desktop"]} · {FLOWS[flow]}\n\n```json\n'+json.dumps(receipt, indent=2, ensure_ascii=False)+'\n```'
            comment = github.api(f'repos/{github.repo}/issues/{pinned["number"]}/comments', 'POST', dict(body=body))
            cell['receiptUrl'] = comment['html_url']
            failure_title = f'nightly: {row["desktop"]} {FLOWS[flow]} failing'
            existing = next((i for i in issues if i['title'] == failure_title), None)
            if not cell['green']:
                failure = github.issue(failure_title, f'Automatic assertion failed on `{result["release"]}`.\n\n'
                    f'Assertion: {cell.get("assertion", "no terminal proof")}\n\n[Receipt]({cell["receiptUrl"]})', existing)
                cell['issueUrl'] = failure['html_url']
            elif existing and existing['state'] == 'open':
                github.api(f'repos/{github.repo}/issues/{existing["number"]}/comments', 'POST',
                    dict(body=f'Automatically recovered on `{result["release"]}`. [Passing receipt]({cell["receiptUrl"]}).'))
                github.api(f'repos/{github.repo}/issues/{existing["number"]}', 'PATCH', dict(state='closed', state_reason='completed'))
    markdown = f'Release: **{result["release"]}** · {result["date"]}\n\n'+table(result)
    markdown += '\nGreen/red is decided by automatic oracles and terminal proofs. — = live lane disabled. Frames are retained as evidence attachments on the named builder; no manual review is required.\n'
    github.issue(title, markdown, pinned)
    # Pinning is idempotent through the existing pinnedIssues connection.
    query = 'query($id:ID!){node(id:$id){... on Repository{pinnedIssues(first:10){nodes{issue{id}}}}}}'
    repository = github.api(f'repos/{github.repo}')
    pins = json.loads(github.gh('api', 'graphql', '-f', 'query='+query, '-f', 'id='+repository['node_id']))
    if pinned['node_id'] not in [n['issue']['id'] for n in pins['data']['node']['pinnedIssues']['nodes']]:
        github.gh('api', 'graphql', '-f', 'query=mutation($id:ID!){pinIssue(input:{issueId:$id}){issue{id}}}', '-f', 'id='+pinned['node_id'])
    (out/'results.md').write_text(markdown)
    save(out/'results.json', result)


def controller(args, config):
    state = Path(config.get('state', str(Path.home()/'putgd-lab/nightly')))
    state.mkdir(parents=True, mode=0o700, exist_ok=True)
    with (state/'controller.lock').open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        github = GitHub(config['repository'], args.config.parent/'github-token')
        tag = args.tag
        if tag is None:
            releases = github.api(f'repos/{github.repo}/releases?per_page=100')
            tag = next(r['tag_name'] for r in sorted(releases, key=lambda r:r.get('published_at') or '', reverse=True)
                       if not r['draft'] and TAG.fullmatch(r['tag_name']))
        if not TAG.fullmatch(tag):
            raise RuntimeError('expected a published nightly release tag')
        done = state/(tag+'.json')
        if done.exists() and not args.now:
            print('Already published '+tag)
            return
        out = state/(tag+'-'+uuid.uuid4().hex[:8])
        out.mkdir(mode=0o700)
        rows = [row for b in config['builders'].values() for row in b['rows']]
        artifacts = out/'artifacts'
        artifacts.mkdir(mode=0o700)
        identity = release_assets(github, tag, artifacts/'packages', rows)
        identity['packages'] = {p.name:digest(p) for p in (artifacts/'packages').iterdir() if p.suffix in ('.deb', '.rpm', '.AppImage')}
        identity['driver'] = command(['git', 'rev-parse', 'HEAD']).strip()
        identity['catalog'] = catalog(github, identity['gitCommit'], artifacts/'catalog')
        save(artifacts/'identity.json', identity)
        result = dict(release=tag, build=identity['gitCommit'], date=dt.datetime.now(dt.timezone.utc).isoformat(), desktops=[])
        # Each host runs one guest at a time. No detached remote jobs.
        for host, builder in config['builders'].items():
            try:
                if builder.get('ssh'):
                    remote = builder['root']
                    archive = out/(host+'-inputs.tgz')
                    with tarfile.open(archive, 'w:gz') as stream:
                        stream.add(artifacts, arcname='artifacts')
                    command([*builder['ssh'], 'mkdir -p '+shlex.quote(remote+'/inputs')])
                    with archive.open('rb') as data:
                        transfer = subprocess.run([*builder['ssh'], 'tar -xzf - -C '+shlex.quote(remote+'/inputs')], stdin=data, capture_output=True, timeout=600)
                    if transfer.returncode:
                        raise RuntimeError('remote artifact transfer failed')
                    remote_out = '/home/patrick/ne/'+uuid.uuid4().hex[:8]
                    command([*builder['ssh'], shlex.join(['python3', remote+'/tools/lab/nightly_desktops.py',
                        '--worker', host, '--config', builder['config'], '--artifacts', remote+'/inputs/artifacts',
                        '--output', remote_out, tag])], log=out/(host+'.log'), timeout=14400)
                    text = command([*builder['ssh'], 'cat '+shlex.quote(remote_out+'/results.json')])
                    result['desktops'].extend(json.loads(text))
                else:
                    result['desktops'].extend(worker(dict(builder, host=host), tag, identity, artifacts,
                                                     Path.home()/'ne'/uuid.uuid4().hex[:8]))
            except Exception as error:
                for row in builder['rows']:
                    result['desktops'].append(dict(desktop=row['name'], host=host, evidence=str(out/host),
                        cells={f:dict(green=False, assertion='builder controller failed: '+type(error).__name__)
                               for f in LOCAL+(['s3_region_upload','proton_region_upload'] if row.get('live') else [])}))
            save(out/'results.json', result)
        publish(github, result, out)
        save(done, dict(issueUrl=result['issueUrl'], results=str(out/'results.json')))
        print(result['issueUrl'])


def main():
    os.umask(0o077)
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('tag', nargs='?')
    parser.add_argument('--now', action='store_true', help='manual trigger, including rechecking the same release')
    parser.add_argument('--config', type=Path, default=Path.home()/'.config/putgd-nightly/config.json')
    parser.add_argument('--worker', choices=['d69','f44'], help=argparse.SUPPRESS)
    parser.add_argument('--artifacts', type=Path, help=argparse.SUPPRESS)
    parser.add_argument('--output', type=Path, help=argparse.SUPPRESS)
    args = parser.parse_args()
    config = json.loads(private_file(args.config).read_text())
    expected = args.worker or 'd69'
    if socket.gethostname() not in config['builders'][expected]['hostnames']:
        parser.error('controller/worker must run on its configured builder')
    if args.worker:
        os.chdir(ROOT)
        worker(dict(config['builders'][args.worker], host=args.worker), args.tag,
               json.loads((args.artifacts/'identity.json').read_text()), args.artifacts, args.output)
    else:
        controller(args, config)


if __name__ == '__main__':
    main()
