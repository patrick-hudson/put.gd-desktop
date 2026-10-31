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
from concurrent.futures import ThreadPoolExecutor, as_completed
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
import time
import uuid

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
FLOWS = dict(region_upload='Region', window_desktop='Window / desktop',
             recording_upload='Recording → upload', edit_upload='Edit → upload',
             link_browser='Link → browser', history_delete='History delete',
             auto_copy='Auto-copy', upload_clipboard='Upload clipboard',
             copy_image='Copy image', survives_exit='Survives exit',
             s3_region_upload='Live S3', proton_region_upload='Live Proton')
LOCAL = [name for name in FLOWS if not name.startswith(('s3_', 'proton_'))]
TAG = re.compile(r'v\d{2}\.\d{3,4}\.\d+-nightly')
HOSTS = ('m74', 'd69', 'k72', 'aurora-amd', 'f44')


class NotRun(RuntimeError):
    """Infrastructure prevented a valid app-flow observation."""


def cell(status='not_run', assertion='', **proof):
    return dict(status=status, green=status == 'green', assertion=assertion, **proof)


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
        observed = env.get('observed_build', {})
        if observed.get('gitCommit') != env.get('build') or observed.get('dirty') is not False:
            return cell(assertion='installed clean build was not verified')
        passed = bool(rows) and all(r['functional'] == 'passed' for r in rows)
        assertion = next((r.get('note') or r['functional'] for r in rows if r['functional'] != 'passed'), '')
        if not rows:
            assertion = env.get('blocked') or 'flow produced no assertion receipt'
        if env.get('cleanup_errors'):
            assertion = 'private target cleanup failed'
        # The runner retains safe assertion messages for RecipeFailed; unexpected
        # driver errors, blocked fixtures, deadlines and cleanup are infrastructure.
        infrastructure = ('driver error:', 'fixture cleanup failed', 'recipe cleanup',
                          'deadline exceeded', 'shell crashed')
        failed = any(r['functional'] == 'failed' for r in rows)
        status = 'green' if passed else 'red' if failed else 'not_run'
        input_blocked = re.search(r"(?:input target '[^']+'|desktop input) is obscured by "
                                 r"'(?:shell overview|no focused window)'(?: \(window None\))?$", assertion)
        if (env.get('cleanup_errors') or env.get('interrupted') or not rows
                or input_blocked or any(text in assertion.lower() for text in infrastructure)):
            status = 'not_run'
        return cell(status, assertion[-1800:], terminal=terminal, observedBuild=observed['gitCommit'],
                    observations=list(dict.fromkeys((r.get('note') or '')[-1800:] for r in rows)),
                    receiptSha256=digest(Path(out)/'receipt.json'), receipt=str(Path(out)/'receipt.json'))
    except (OSError, ValueError, KeyError, RuntimeError, TypeError):
        return cell(assertion='terminal audited receipt missing or invalid', receipt=str(Path(out)/'receipt.json'))


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


def preflight(config):
    from tools.lab import lab
    row = config['rows'][0]
    if not Path(row['golden']).is_file():
        if Path(row['golden']+'.partial').is_file():
            raise NotRun('golden copy awaiting checksum verification')
        raise NotRun('golden missing')
    if not os.access('/dev/kvm', os.R_OK | os.W_OK):
        raise NotRun('builder KVM unavailable')
    profile = lab.load_profile(row['profile'])
    available = int(next(line.split()[1] for line in Path('/proc/meminfo').read_text().splitlines()
                         if line.startswith('MemAvailable:'))) * 1024
    needed = profile['memory_mib']*2**20 + 4*2**30
    if available < needed:
        raise NotRun('RAM below guest requirement plus 4 GiB headroom')
    if os.getloadavg()[0] >= os.cpu_count():
        raise NotRun('load at or above core count')
    budget = config.get('overlayBudgetGiB', 4)
    if shutil.disk_usage(ROOT).free < (10+budget)*2**30:
        raise NotRun('disk below sparse overlay budget plus 10 GiB reserve')
    for port in (config.get('sshPort', 22446), 5900+config.get('vncDisplay', 46)):
        with socket.socket() as sock:
            try:
                sock.bind(('127.0.0.1', port))
            except OSError:
                raise NotRun('reserved nightly guest port is occupied') from None
    return dict(kvm=True, availableMemory=available, requiredMemory=needed,
                freeBytes=shutil.disk_usage(ROOT).free, overlayBudgetGiB=budget,
                overlay='sparse qcow2; no preallocation', placement='guest')


def await_host(config, out, *, clock=time.monotonic, sleep=time.sleep):
    """Wait outside every desktop/Proton lease; other guests are never touched."""
    from tools.lab.desktop_lock import Desktop
    started = clock()
    deadline = started + config.get('waitSeconds', 1200)
    attempts = []
    while True:
        try:
            for resource in ('desktop', 'proton') if config['rows'][0].get('live') else ('desktop',):
                state = Desktop(Path.home()/'putgd-lab/locks', config['host'], resource=resource).status()
                if state['state'] != 'free':
                    raise NotRun(resource+' lease held by another session')
            if (Path.home()/'putgd-lab/locks/tier1-guest.lock.d').exists():
                raise NotRun('nightly guest slot held by another session')
            ready = preflight(config)
            save(out/'availability.json', dict(attempts=attempts, waitedSeconds=round(clock()-started, 3), ready=ready))
            return clock()-started
        except NotRun as error:
            attempts.append(dict(elapsedSeconds=round(clock()-started, 3), reason=str(error)))
            save(out/'availability.json', dict(attempts=attempts, waitedSeconds=round(clock()-started, 3)))
            if clock() >= deadline or str(error) in ('golden missing', 'builder KVM unavailable'):
                raise NotRun(str(error)+'; not run after '+str(round(clock()-started))+'s') from None
            sleep(min(30, max(0, deadline-clock())))


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


def baton_returned(run):
    locks = Path.home()/'putgd-lab/locks'
    proton = locks/'proton.lock'
    if proton.exists() and 'lent copy outstanding' in proton.read_text():
        raise RuntimeError('Proton carry-back unresolved; overlay retained')
    last = json.loads((locks/'proton-registry.json').read_text()).get('lastLend', {})
    if last.get('run') == str(run) and last.get('carriedBack'):
        return True
    # lend may stop while preparing the guest, before exporting any session.
    return False


def worker(config, tag, identity, artifacts, out):
    from tools.lab import lab
    from tools.lab.nightly_desktop_guest import install_rpm, prepare, prepare_live_keyring
    out.mkdir(mode=0o700, parents=True)
    if command(['git', 'rev-parse', 'HEAD']).strip() != identity['driver']:
        raise NotRun('worker checkout differs from the controller source identity')
    # Live admission reads the classifier/pin from this exact app commit.
    command(['git', 'fetch', '--no-tags', 'origin', 'tag', tag], log=out/'build-source-fetch.txt')
    host = config['host']
    if len(config['rows']) != 1:
        raise NotRun('configure exactly one desktop per builder')
    result = []
    for row in config['rows']:
        started = time.monotonic()
        rowout = out/row['name'].lower().replace(' ', '-')
        rowout.mkdir(mode=0o700)
        session = 'nightly-'+uuid.uuid4().hex[:12]
        cells = {flow: cell(assertion='desktop setup did not complete')
                 for flow in LOCAL + (['s3_region_upload', 'proton_region_upload'] if row.get('live') else [])}
        record = dict(desktop=row['name'], host=host, profile=row['profile'], cells=cells,
                      release=tag, build=identity['gitCommit'], driver=identity['driver'],
                      session=session, evidence=str(rowout))
        run = ROOT/'work/lab-runs'/('n'+uuid.uuid4().hex[:8])
        setup_started = None
        try:
            record['waitSeconds'] = round(await_host(config, rowout), 3)
            with lease(host, session, rowout) as lease_errors:
                try:
                    setup_started = time.monotonic()
                    command(['uptime'], log=rowout/'uptime.txt')
                    save(rowout/'host-preflight.json', preflight(config))
                    command([sys.executable, 'tools/diagnostics/check_socket_paths.py', '--socket',
                             run/'qmp.sock', run/'vnc.sock', '/dev/shm/nd/runtime/putgd/session.sock'],
                            log=rowout/'sockets.json')
                    # Remap an existing authenticated pin, never scan/trust a new key.
                    pins = Path(row['knownHosts']).read_text().splitlines()
                    key = next(' '.join(p.split()[1:3]) for p in pins if ' ssh-ed25519 ' in p)
                    known = rowout/'known_hosts'
                    ssh_port = config.get('sshPort', 22446)
                    vnc_display = config.get('vncDisplay', 46)
                    known.write_text(f'[127.0.0.1]:{ssh_port} '+key+'\n')
                    command([sys.executable, 'tools/lab/lab.py', 'launch', '--profile', row['profile'],
                        '--golden', row['golden'], '--run', run, '--bootstrap-key', row['bootstrapKey'],
                        '--known-hosts', known, '--ssh-port', str(ssh_port), '--vnc-display', str(vnc_display), '--timeout', '420'],
                        log=rowout/'launch.log', timeout=600)
                    package = artifacts/'packages'/f'putgd-{tag[1:]}-{row["asset"]}'
                    if digest(package) != identity['packages'][package.name]:
                        raise RuntimeError('worker package checksum mismatch')
                    if row['package'] == 'rpm':
                        install_rpm(run, package, rowout)
                    else:
                        command([sys.executable, 'tools/lab/lab.py', 'install', '--run', run,
                            '--appimage' if row['package'] == 'appimage' else '--deb', package],
                            log=rowout/'install.log', timeout=1600)
                    profiles = prepare(run, row, rowout)
                    save(rowout/'identity.json', dict(lab.state(run), driver=identity['driver']))
                    record.update(runId=lab.state(run)['id'], goldenSha256=lab.state(run)['golden_sha256'],
                                  packageSha256=digest(package))
                    record['setupSeconds'] = round(time.monotonic()-setup_started, 3)
                    env = dict(os.environ, PUTGD_TIER1_SESSION=session, HOME=str(Path.home().resolve()))
                    for flow in cells:
                        flow_started = time.monotonic()
                        destination = 's3' if flow.startswith('s3_') else 'proton' if flow.startswith('proton_') else 'local'
                        if destination == 's3' and row['desktop'] == 'aurora-kde-wayland':
                            prepare_live_keyring(run, rowout)
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
                                proton_command = config.get('protonCommand', [sys.executable, str(ROOT/'tools/lab/proton_lab.py')])
                                check = [*proton_command, 'check', '--session', session,
                                         '--owner', 'nightly-desktops']
                                command(check, log=rowout/(name+'-check-before.json'), env=env)
                                # The host adapter selects the native check app. Lending
                                # must use this exact driver's guest/bridge implementation,
                                # not the adapter's older installed copies. Canonical HOME
                                # above preserves Atomic's /var/home lab profile identity.
                                args = [sys.executable, str(ROOT/'tools/lab/proton_lab.py'), 'lend', str(run),
                                        '--session', session, '--owner', 'nightly-desktops',
                                        '--purpose', 'automatic nightly outcome', '--']+args
                            # The driver exits nonzero for functional reds. Collect its
                            # terminal result anyway; never infer success from an exit alone.
                            try:
                                command(['nice', '-n', '10', *args], log=rowout/(name+'.log'), env=env, timeout=1800)
                            except RuntimeError:
                                pass
                            observed = verdict(target, recipe)
                            observed['attempt'] = attempt
                            cells[flow].setdefault('attempts', []).append(observed)
                            cells[flow].update(observed)
                            if destination == 'proton':
                                # lend must have returned the baton even when assertions failed.
                                returned = baton_returned(run)
                                command(check, log=rowout/(name+'-check-after.json'), env=env)
                                if not returned:
                                    raise NotRun('Proton guest provisioning did not complete; source check passed')
                            if observed['status'] == 'green':
                                break
                        cells[flow]['seconds'] = round(time.monotonic()-flow_started, 3)
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
            reason = str(error) if isinstance(error, NotRun) or type(error) is RuntimeError else 'controller '+type(error).__name__+'; private evidence retained'
            record['notRunReason'] = reason
            for value in cells.values():
                if value['status'] == 'not_run' and not value.get('attempts'):
                    value.update(assertion=reason)
        if (rowout/'availability.json').exists():
            record['waitSeconds'] = json.loads((rowout/'availability.json').read_text()).get('waitedSeconds', 0)
        record['seconds'] = round(time.monotonic()-started, 3)
        record['finishedAt'] = dt.datetime.now(dt.timezone.utc).isoformat()
        save(rowout/'result.json', record)
        result.append(record)
        print(json.dumps(dict(desktop=row['name'], green=sum(c['green'] for c in cells.values()), total=len(cells))), flush=True)
    save(out/'results.json', result)
    return result


def table(result):
    lines = ['| Desktop / host | '+' | '.join(FLOWS.values())+' | Not run |',
             '| --- | '+' | '.join('---' for _ in FLOWS)+' | --- |']
    for row in result['desktops']:
        values = []
        reasons = []
        for flow in FLOWS:
            value = row['cells'].get(flow)
            if value is None:
                values.append('—')
                continue
            color = {'green':'🟢', 'red':'🔴', 'not_run':'⚪'}[value['status']]
            values.append(f'[{color}]({value["receiptUrl"]})' if value.get('receiptUrl') else color)
            if value['status'] == 'not_run' and value.get('assertion'):
                reasons.append(value['assertion'])
        reason = row.get('notRunReason') or '; '.join(dict.fromkeys(reasons))
        reason = reason.replace('|', '/').replace('\n', ' ')[:500]
        lines.append('| '+row['desktop']+' / '+row['host']+' | '+' | '.join(values)+' | '+reason+' |')
    return '\n'.join(lines)+'\n'


def select_release(releases, completed=(), force=False):
    published = sorted((r for r in releases if not r['draft'] and TAG.fullmatch(r['tag_name'])),
                       key=lambda r:r['published_at'])
    latest = published[-1]
    return latest if force or latest['tag_name'] not in completed else None


def tested_release(path):
    try:
        marker = json.loads(Path(path).read_text())
        return marker.get('schemaVersion') == 2 and marker.get('eligibleDesktops', 0) >= 3
    except (OSError, ValueError, TypeError):
        return False


def eligible_desktops(result):
    return sum(any(value['status'] in ('red', 'green') and value.get('terminal')
                   and value.get('observedBuild') == result['build']
                   for value in row['cells'].values()) for row in result['desktops'])


def publish(github, result, out, *, previous=None):
    issues = github.issues()
    title = 'Nightly desktop results'
    pinned = next((i for i in issues if i['title'] == title), None)
    if not pinned:
        pinned = github.issue(title, 'Automatic nightly results are being published.')
    result['issueUrl'] = pinned['html_url']
    for row in result['desktops']:
        compact = {}
        for flow, value in row['cells'].items():
            compact[flow] = {k:value[k] for k in ('status','assertion','seconds','observedBuild','terminal','receipt','receiptSha256') if k in value}
            compact[flow]['attempts'] = [{k:a[k] for k in ('status','assertion','terminal','receipt','receiptSha256','attempt') if k in a}
                                         for a in value.get('attempts', [])]
        receipt = dict(desktop=row['desktop'], host=row['host'], release=result['release'],
            build=result['build'], cells=compact, evidence=row['evidence'],
            identity={k:row.get(k) for k in ('profile','driver','session','runId','goldenSha256','packageSha256')},
            seconds=row.get('totalSeconds', row.get('seconds')), waitSeconds=row.get('waitSeconds', 0),
            attachments=row['evidence']+'/attachments.tgz', notRunReason=row.get('notRunReason'))
        body = f'{result["release"]} · {row["desktop"]} / {row["host"]}\n\n```json\n'+json.dumps(receipt, indent=2, ensure_ascii=False)+'\n```'
        comment = github.api(f'repos/{github.repo}/issues/{pinned["number"]}/comments', 'POST', dict(body=body))
        for value in row['cells'].values():
            value['receiptUrl'] = comment['html_url']
        # Exactly one stable tracking issue per desktop; grey never creates,
        # reopens or closes an issue, and legacy per-flow issues stay untouched.
        failure_title = f'nightly: {row["desktop"]} failing'
        existing = next((i for i in issues if i['title'] == failure_title), None)
        failed = [(flow,value) for flow,value in row['cells'].items() if value['status'] == 'red']
        if failed:
            body = f'Automatic app-flow assertions failed on `{result["release"]}`.\n\n'
            body += '\n'.join(f'- **{FLOWS[flow]}**: '+value['assertion'] for flow,value in failed)
            body += f'\n\n[Desktop receipt]({comment["html_url"]})'
            issue = github.issue(failure_title, body, existing)
            github.api(f'repos/{github.repo}/issues/{issue["number"]}/labels', 'POST',
                       dict(labels=['release-blocker']))
            row['issueUrl'] = issue['html_url']
        elif existing and existing['state'] == 'open' and all(v['status'] == 'green' for v in row['cells'].values()):
            github.api(f'repos/{github.repo}/issues/{existing["number"]}/comments', 'POST',
                dict(body=f'All enabled flows recovered on `{result["release"]}`. [Receipt]({comment["html_url"]}).'))
    if previous is not None:
        result['desktops'] = merge_desktops(previous, result)
    markdown = f'Release: **{result["release"]}** · {result["date"]}\n\n'+table(result)
    markdown += '\n🟢 automatic oracles passed · 🔴 app-flow oracle failed on the verified installed build · ⚪ infrastructure or documented manager-free X11 loss (no issue) · — disabled.\n'
    markdown += '\n| Host | Total seconds | Capacity/lease wait seconds |\n| --- | ---: | ---: |\n'
    for row in result['desktops']:
        markdown += f'| {row["host"]} | {row.get("totalSeconds", row.get("seconds", 0)):.1f} | {row.get("waitSeconds", 0):.1f} |\n'
    markdown += '\nFrames remain evidence attachments; no manual visual review is required.\n'
    github.issue(title, markdown, pinned)
    query = 'query($id:ID!){node(id:$id){... on Repository{pinnedIssues(first:10){nodes{issue{id}}}}}}'
    repository = github.api(f'repos/{github.repo}')
    pins = json.loads(github.gh('api', 'graphql', '-f', 'query='+query, '-f', 'id='+repository['node_id']))
    if pinned['node_id'] not in [n['issue']['id'] for n in pins['data']['node']['pinnedIssues']['nodes']]:
        github.gh('api', 'graphql', '-f', 'query=mutation($id:ID!){pinIssue(input:{issueId:$id}){issue{id}}}', '-f', 'id='+pinned['node_id'])
    (out/'results.md').write_text(markdown)
    save(out/'results.json', result)


def not_run_rows(host, builder, reason, out):
    return [dict(desktop=row['name'], host=host, evidence=str(out), notRunReason=reason,
                 cells={flow:cell(assertion=reason) for flow in LOCAL+
                        (['s3_region_upload','proton_region_upload'] if row.get('live') else [])})
            for row in builder['rows']]


def selected_builders(builders, desktops):
    if not desktops:
        return builders
    selected = {}
    for name in desktops:
        matches = {host: builder for host, builder in builders.items() if
                   name.casefold() in {host.casefold(), *(r['name'].casefold() for r in builder['rows']),
                                       *(r['desktop'].casefold() for r in builder['rows'])}}
        if len(matches) != 1:
            raise RuntimeError('desktop filter must identify exactly one configured builder: '+name)
        selected.update(matches)
    return {host: builder for host, builder in builders.items() if host in selected}


def merge_desktops(previous, result):
    if (previous['release'], previous['build']) != (result['release'], result['build']):
        raise RuntimeError('filtered publication requires the same release and exact build')
    replacements = {r['host']: r for r in result['desktops']}
    if not replacements.keys() <= {r['host'] for r in previous['desktops']}:
        raise RuntimeError('filtered publication cannot introduce an unknown desktop')
    return [replacements.get(r['host'], r) for r in previous['desktops']]


def run_host(host, builder, tag, artifacts, archive, out):
    """One foreground worker process per host, including d69 itself."""
    started = time.monotonic()
    started_at = dt.datetime.now(dt.timezone.utc).isoformat()
    ssh = builder.get('ssh', [])
    root = builder['root']
    output = str(Path(root).parent/'ne'/uuid.uuid4().hex[:8])
    waited = 0
    try:
        if ssh:
            deadline = time.monotonic()+builder.get('waitSeconds', 1200)
            while True:
                try:
                    command([*ssh, 'true'], timeout=25)
                    break
                except (RuntimeError, subprocess.TimeoutExpired):
                    if time.monotonic() >= deadline:
                        raise NotRun('SSH unreachable after 20-minute wait') from None
                    time.sleep(min(30, max(0, deadline-time.monotonic())))
            waited = time.monotonic()-started
            inputs = root+'/inputs/'+out.name
            command([*ssh, 'mkdir -p '+shlex.quote(inputs)])
            with archive.open('rb') as data:
                transfer = subprocess.run([*ssh, 'tar -xf - -C '+shlex.quote(inputs)], stdin=data, capture_output=True, timeout=600)
            if transfer.returncode:
                raise NotRun('artifact transfer failed')
            selected = inputs+'/artifacts'
        else:
            selected = str(artifacts)
        args = ['nice','-n','10','python3',root+'/tools/lab/nightly_desktops.py', '--worker',host,
                '--config',builder['config'], '--artifacts',selected, '--output',output, tag]
        command([*ssh, shlex.join(args)] if ssh else args, log=out/(host+'.log'), timeout=18000)
        text = command([*ssh, 'cat '+shlex.quote(output+'/results.json')]) if ssh else Path(output+'/results.json').read_text()
        rows = json.loads(text)
    except Exception as error:
        reason = str(error) if isinstance(error, NotRun) else 'worker infrastructure failed: '+type(error).__name__
        rows = not_run_rows(host, builder, reason, out/host)
    for row in rows:
        row.update(totalSeconds=round(time.monotonic()-started,3), startedAt=started_at)
        row['waitSeconds'] = round(row.get('waitSeconds', 0)+waited, 3)
    return rows


def run_hosts(builders, tag, artifacts, out, result, dispatch=run_host):
    archive = out/'inputs.tar'
    with tarfile.open(archive, 'w') as stream:
        stream.add(artifacts, arcname='artifacts')
    by_host = {}
    with ThreadPoolExecutor(max_workers=len(builders)) as pool:
        futures = {pool.submit(dispatch, host, builder, tag, artifacts, archive, out):host
                   for host,builder in builders.items()}
        for future in as_completed(futures):
            host = futures[future]
            by_host[host] = future.result()
            result['desktops'] = [row for name in builders for row in by_host.get(name, [])]
            save(out/'results.json', result)
            print(json.dumps(dict(host=host, completed=True, seconds=by_host[host][0].get('totalSeconds'))),flush=True)
    return result


def controller(args, config):
    state = Path(config.get('state', str(Path.home()/'putgd-lab/nightly')))
    state.mkdir(parents=True, mode=0o700, exist_ok=True)
    with (state/'controller.lock').open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        github = GitHub(config['repository'], args.config.parent/'github-token')
        if args.tag:
            if not TAG.fullmatch(args.tag):
                raise RuntimeError('expected a published nightly release tag')
            release = github.api(f'repos/{github.repo}/releases/tags/{args.tag}')
        else:
            release = select_release(github.api(f'repos/{github.repo}/releases?per_page=100'),
                                     {p.stem for p in state.glob('v*-nightly.json') if tested_release(p)}, args.now)
            if release is None:
                print('No new published nightly')
                return
        tag = release['tag_name']
        if release['draft']:
            raise RuntimeError('draft releases cannot enter nightly checks')
        done = state/(tag+'.json')
        if tested_release(done) and not args.now:
            print('Already tested '+tag)
            return
        if set(config['builders']) != set(HOSTS) or any(len(b['rows']) != 1 for b in config['builders'].values()):
            raise RuntimeError('configure exactly one desktop on each of the five builders')
        builders = selected_builders(config['builders'], args.desktops)
        previous = None
        if args.desktops:
            marker = json.loads((state/'last-run.json').read_text())
            previous = json.loads(Path(marker['results']).read_text())
            if previous['release'] != tag:
                raise RuntimeError('filtered run requires the currently published table release')
        out = state/(tag+'-'+uuid.uuid4().hex[:8])
        out.mkdir(mode=0o700)
        artifacts = out/'artifacts'
        artifacts.mkdir(mode=0o700)
        result = dict(release=tag,build='unverified',date=dt.datetime.now(dt.timezone.utc).isoformat(),desktops=[])
        try:
            rows = [row for b in builders.values() for row in b['rows']]
            identity = release_assets(github, tag, artifacts/'packages', rows)
            identity['packages'] = {p.name:digest(p) for p in (artifacts/'packages').iterdir() if p.suffix in ('.deb','.rpm','.AppImage')}
            identity['driver'] = command(['git','rev-parse','HEAD']).strip()
            identity['catalog'] = catalog(github, identity['gitCommit'], artifacts/'catalog')
            save(artifacts/'identity.json', identity)
            result['build'] = identity['gitCommit']
            if previous is not None:
                merge_desktops(previous, result)  # Fail before launching any guest on identity mismatch.
        except Exception as error:
            reason = str(error) if type(error) is RuntimeError else 'release preparation failed: '+type(error).__name__
            if previous is not None:
                raise  # Preserve the published table if release preparation was not verified.
            result['desktops'] = [row for host,builder in builders.items()
                                  for row in not_run_rows(host,builder,reason,out)]
        else:
            run_hosts(builders,tag,artifacts,out,result)
        result['eligibleDesktops'] = eligible_desktops(result)
        publish(github,result,out,previous=previous)
        save(state/'last-run.json',dict(results=str(out/'results.json'),eligibleDesktops=result['eligibleDesktops']))
        # Grey is not a tested release. A later daily check may try it again.
        if not args.desktops and result['eligibleDesktops'] >= 3:
            save(done,dict(schemaVersion=2,eligibleDesktops=result['eligibleDesktops'],
                           issueUrl=result['issueUrl'],results=str(out/'results.json')))
        print(result['issueUrl'])

def main():
    os.umask(0o077)
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('tag', nargs='?')
    parser.add_argument('--now', action='store_true', help='manual trigger, including rechecking the same release')
    parser.add_argument('--desktops', nargs='+', help='recheck only these desktop names, profile names or builder IDs; preserve other rows')
    parser.add_argument('--config', type=Path, default=Path.home()/'.config/putgd-nightly/config.json')
    parser.add_argument('--worker', choices=HOSTS, help=argparse.SUPPRESS)
    parser.add_argument('--artifacts', type=Path, help=argparse.SUPPRESS)
    parser.add_argument('--output', type=Path, help=argparse.SUPPRESS)
    args = parser.parse_args()
    if args.desktops and (not args.now or args.worker):
        parser.error('--desktops requires --now on the controller')
    config = json.loads(private_file(args.config).read_text())
    expected = args.worker or 'd69'
    if socket.gethostname() not in config['builders'][expected]['hostnames']:
        parser.error('controller/worker must run on its configured builder')
    if args.worker:
        os.chdir(ROOT)
        tools_bin = config['builders'][args.worker].get('toolsBin')
        if tools_bin:
            os.environ['PATH'] = tools_bin+os.pathsep+os.environ.get('PATH', '/usr/bin:/bin')
        worker(dict(config['builders'][args.worker], host=args.worker), args.tag,
               json.loads((args.artifacts/'identity.json').read_text()), args.artifacts, args.output)
    else:
        controller(args, config)


if __name__ == '__main__':
    main()
