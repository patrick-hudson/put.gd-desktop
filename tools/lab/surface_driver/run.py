#!/usr/bin/env python3
"""Run portable interaction recipes on a lab.py guest or a physical.py host.

Run this on the QEMU host (including boat), not on the workstation. Target
lifecycle and VNC wall forwarding stay with lab.py and the campaign owner.
"""
import argparse
import dataclasses
import fnmatch
import getopt
import importlib
import inspect
import json
import os
from pathlib import Path
import re
import signal
import shutil
import socket
import stat
import struct
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[3]
if str(ROOT) not in sys.path:
    sys.path.insert(0,str(ROOT))
from tools.lab.surface_driver.api import REGISTRY, ReceiptRow, RecipeBlocked, RecipeFailed, PACKAGES, PLACEMENTS, DESTINATIONS, HOSTS
from tools.lab.surface_driver.context import DriverContext
from tools.lab.surface_driver.profiles import load_profile
from tools.lab.surface_driver.receipts import stamp, write_receipts
from tools.lab.surface_driver.target import Target
from tools.lab import lab

# Live Proton/S3/B2 runs stay off until secret-safe evidence (redaction + leak scan, PR #726) merges.
LIVE_DESTINATIONS_ENABLED = False


def native_endpoint(kind, address):
    """Read-only service proof; also sent over the profile's pinned SSH channel."""
    import os
    from pathlib import Path
    import re
    import stat
    import subprocess
    if kind == 'vnc':
        command = ['ss', '-H', '-ltnp', 'sport = :'+address.rsplit(':', 1)[1]]
    else:
        socket = Path(address)
        if socket.is_symlink() or not stat.S_ISSOCK(socket.stat().st_mode) or socket.stat().st_uid != os.getuid():
            raise ValueError('unowned native socket')
        command = ['ss', '-H', '-lxnp']
    reply = subprocess.run(command, check=True, capture_output=True, text=True, timeout=10)
    pids = {pid for line in reply.stdout.splitlines() if address in line.split()
            for pid in re.findall(r'pid=(\d+)', line)}
    if len(pids) != 1:
        raise ValueError('native service owner unavailable')
    process = Path('/proc') / pids.pop()
    if process.stat().st_uid != os.getuid():
        raise ValueError('native service is not owned')
    executable = (process/'exe').readlink().name.lower()
    arguments = [a.decode() for a in (process/'cmdline').read_bytes().split(b'\0') if a]
    if kind == 'vnc':
        if executable not in ('x11vnc', 'x0vncserver', 'x0tigervnc', 'w0vncserver', 'wayvnc'):
            raise ValueError('listener is not a native desktop server')
    elif not (executable.startswith('python3') and any(Path(a).name == 'host_input.py' for a in arguments)
              and 'serve' in arguments and '--socket' in arguments
              and arguments[arguments.index('--socket')+1] == address):
        raise ValueError('listener is not the native input agent')
    return True


def verify_physical_endpoints(target):
    """Bind local listeners to the pinned SSH host, never trust a port/path label."""
    config = target.physical.config
    try:
        port = config['vnc_port']
        if type(port) is not int or not 1024 <= port <= 65535:
            raise ValueError('invalid VNC port')
        remote_vnc = 5901 if config['host'] == '192.168.68.70' or config['host'] in ('127.0.0.1', '::1') else 5900
        endpoints = [('vnc', '127.0.0.1:'+str(port), '127.0.0.1:'+str(port)+':127.0.0.1:'+str(remote_vnc))]
        socket = target.run / 'qmp.sock'
        if socket.is_symlink():
            raise ValueError('indirect input socket')
        if socket.exists():
            info = socket.stat()
            if not stat.S_ISSOCK(info.st_mode) or info.st_uid != os.getuid():
                raise ValueError('input socket is not owned')
            uid = target.physical.ssh(target.run, 'id -u').strip()
            if not uid.isdecimal():
                raise ValueError('unknown remote account')
            endpoints.append(('input', str(socket), str(socket)+':/run/user/'+uid+'/putgd-lab-input.sock'))
        for kind, address, forward in endpoints:
            command = ['ss', '-H', '-ltnp', 'sport = :'+str(port)] if kind == 'vnc' else ['ss', '-H', '-lxnp']
            reply = subprocess.run(command, check=True, capture_output=True, text=True, timeout=10)
            lines = [line for line in reply.stdout.splitlines() if address in line.split()]
            pids = {pid for line in lines for pid in re.findall(r'pid=(\d+)', line)}
            if len(pids) != 1:
                raise ValueError('listener owner unavailable or ambiguous')
            process = Path('/proc') / pids.pop()
            if process.stat().st_uid != os.getuid():
                raise ValueError('listener is not owned')
            arguments = [value.decode() for value in (process/'cmdline').read_bytes().split(b'\0') if value]
            executable = (process/'exe').readlink().name.lower()
            if executable == 'ssh':
                options, rest = getopt.getopt(arguments[1:], '1246AaCfGgKkMNnqsTtVvXxYyB:b:c:D:E:e:F:I:i:J:L:l:m:O:o:p:Q:R:S:W:w:')
                values = dict(options)
                pins = [value for option, value in options if option == '-o']
                for required in ('StrictHostKeyChecking=yes', 'UserKnownHostsFile='+config['known_hosts'], 'ExitOnForwardFailure=yes'):
                    key = required.split('=', 1)[0].lower()
                    matches = [p for p in pins if re.split(r'[=\s]', p.strip(), maxsplit=1)[0].lower() == key]
                    if matches != [required]:
                        raise ValueError('ambiguous SSH pin options')
                bindings = {}
                for option, value in options:
                    if option != '-L':
                        continue
                    if value.startswith('/'):
                        local, remote = value.split(':', 1)
                        if not remote.startswith('/'):
                            raise ValueError('noncanonical socket forward')
                    else:
                        fields = value.split(':')
                        if len(fields) != 4 or fields[0] != '127.0.0.1':
                            raise ValueError('noncanonical VNC forward')
                        local = ':'.join(fields[:2])
                    if local in bindings:
                        raise ValueError('ambiguous local forwarding endpoint')
                    bindings[local] = value
                overrides = {'hostname', 'hostkeyalias', 'port', 'user', 'proxycommand', 'proxyjump', 'localforward', 'remoteforward', 'dynamicforward'}
                if (rest != [config['user']+'@'+config['host']] or '-N' not in values
                        or sum(option == '-p' for option, _ in options) > 1
                        or any(option in ('-F', '-J', '-W', '-O', '-l', '-R', '-D') for option, _ in options)
                        or any(re.split(r'[=\s]', p.strip(), maxsplit=1)[0].lower() in overrides for p in pins)
                        or int(values.get('-p', 22)) != config.get('ssh_port', 22)
                        or bindings.get(address) != forward):
                    raise ValueError('forward does not bind the authenticated host')
                # A forward to another forward is not a native desktop proof.
                remote_address = forward[len(address)+1:]
                source = inspect.getsource(native_endpoint)
                script = "python3 - <<'SD_ENDPOINT'\n"+source+'\nimport json\ntry:\n'
                script += ' print(json.dumps({"verified":native_endpoint('+repr(kind)+','+repr(remote_address)+')}))\n'
                script += 'except Exception:\n print(json.dumps({"verified":False}))\nSD_ENDPOINT\n'
                if json.loads(target.physical.ssh(target.run, script)).get('verified') is not True:
                    raise ValueError('forwarded native service cannot be verified')
            else:
                # Direct listeners are allowed only when the pinned SSH host
                # is this runner, and the listener is a native desktop server.
                boot = Path('/proc/sys/kernel/random/boot_id').read_text().strip()
                if not boot or target.physical.ssh(target.run, 'cat /proc/sys/kernel/random/boot_id').strip() != boot:
                    raise ValueError('direct listener is on a different host')
                if kind == 'vnc':
                    if executable not in ('x11vnc', 'x0vncserver', 'x0tigervnc', 'w0vncserver', 'wayvnc'):
                        raise ValueError('unverified desktop server')
                elif not (executable.startswith('python3') and any(Path(a).name == 'host_input.py' for a in arguments)
                          and 'serve' in arguments and '--socket' in arguments
                          and arguments[arguments.index('--socket')+1] == address):
                    raise ValueError('unverified host-input server')
    except (OSError, ValueError, KeyError, IndexError, TypeError, AttributeError, UnicodeError, getopt.GetoptError,
            subprocess.SubprocessError, lab.LabError) as error:
        raise RecipeBlocked('physical VNC/input endpoint binding cannot be verified') from error


def guard_physical_endpoints(target):
    verify_physical_endpoints(target)
    target._verify_endpoints = lambda: verify_physical_endpoints(target)


def resolve_boat_cli():
    """Pin the executable once, before any authenticated boat identity proof."""
    path = shutil.which('boat')
    if not path:
        raise RecipeBlocked('boat CLI is unavailable')
    return str(Path(path).resolve(strict=True))


def verify_guest_endpoint(target, pid, arguments):
    """Recheck the cached owned QEMU identity and the current socket peer."""
    try:
        path = target.run / 'qmp.sock'
        metadata = path.lstat()
        if not stat.S_ISSOCK(metadata.st_mode) or metadata.st_uid != os.getuid():
            raise ValueError('QMP socket is not owned')
        process = Path('/proc') / str(pid)
        if (process.stat().st_uid != os.getuid()
                or (process / 'cmdline').read_bytes() != arguments):
            raise ValueError('owned QEMU identity changed')
        with lab.connect_unix(path, timeout=2) as peer:
            peer_pid, peer_uid, _ = struct.unpack('3i', peer.getsockopt(
                socket.SOL_SOCKET, socket.SO_PEERCRED, struct.calcsize('3i')))
            if peer_pid != pid or peer_uid != os.getuid():
                raise ValueError('QMP listener is not the owned QEMU')
    except (OSError, ValueError) as error:
        raise RecipeBlocked('guest QMP endpoint binding cannot be verified') from error


def guard_target(args, target):
    """Authorize the actual transport before any desktop command or capture."""
    if target.physical:
        if args.placement == 'guest':
            raise RecipeBlocked('--host-profile cannot use placement=guest')
        from tools.lab import desktop_lock
        config = target.physical.config
        builder = next((host for host, (address, _) in desktop_lock.HOSTS.items()
                        if config.get('host') == address), None)
        if builder:
            if args.host != builder or args.placement != 'native':
                raise RecipeBlocked('builder host-profile requires its actual --host '+builder+' and placement=native')
            if config.get('user') != desktop_lock.USER or config.get('ssh_port', 22) != 22:
                raise RecipeBlocked('builder host-profile must use the desktop lock account and pinned SSH endpoint')
            if not args.lock_session:
                raise RecipeBlocked('physical desktop input requires its held --lock-session')
            target.require_lock(builder, args.lock_session)
            guard_physical_endpoints(target)
            return
        # A label, profile name or supplied sandbox id is not identity evidence.
        # Permit a boat head only on that sandbox's own runner, over local SSH.
        if (args.host == 'boat' and args.placement == 'boat' and args.boat_id
                and re.fullmatch(r'bx_[a-z0-9]{1,64}', args.boat_id)
                and re.fullmatch(r'[A-Za-z_][A-Za-z0-9_.-]*', config.get('user', ''))):
            if config.get('host') in ('127.0.0.1', '::1') and config.get('ssh_port', 22) == 22:
                try:
                    reply = subprocess.run([args.boat_cli, 'info', args.boat_id, '--json'], check=True,
                                           capture_output=True, text=True, timeout=15)
                    info = json.loads(reply.stdout)['sandbox']
                    if info.get('id') == args.boat_id and info.get('hydrated') is True:
                        witness = subprocess.run([args.boat_cli, 'exec', args.boat_id, '--json', '--',
                                                  'cat', '/proc/sys/kernel/random/boot_id'], check=True,
                                                 capture_output=True, text=True, timeout=20)
                        proof = json.loads(witness.stdout)
                        boot = Path('/proc/sys/kernel/random/boot_id').read_text().strip()
                        observed = target.physical.ssh(target.run, 'cat /proc/sys/kernel/random/boot_id').strip()
                        if boot and proof.get('exitCode') == 0 and proof.get('stdout', '').strip() == boot == observed:
                            guard_physical_endpoints(target)
                            return
                except (OSError, ValueError, KeyError, TypeError, AttributeError, subprocess.SubprocessError, lab.LabError):
                    pass
        raise RecipeBlocked('unverified host-profile is a physical target; no matching builder desktop lock')
    if args.placement == 'native':
        raise RecipeBlocked('native input requires a builder --host-profile and held --lock-session')
    # physical.py also writes state.json and uses a QMP-compatible host-input
    # socket. Prove this is an owned QEMU overlay, including its exact QMP path.
    try:
        info = lab.verify_owned_guest(target.run)
        command = (Path('/proc') / str(info['pid']) / 'cmdline').read_bytes()
        arguments = command.split(b'\0')
        endpoint = ('unix:'+str(target.run / 'qmp.sock')+',server=on,wait=off').encode()
        if not any(a == b'-qmp' and b == endpoint for a, b in zip(arguments, arguments[1:])):
            raise ValueError('QMP endpoint does not belong to the owned guest')
        verify_guest_endpoint(target, info['pid'], command)
        target._verify_endpoints = lambda: verify_guest_endpoint(target, info['pid'], command)
    except (lab.LabError, OSError, ValueError, KeyError, RuntimeError) as error:
        raise RecipeBlocked('unverified input target is physical; an owned QMP guest or builder desktop lock is required') from error


def discover(pattern, recipe_dir=None):
    directory = Path(recipe_dir or Path(__file__).with_name('recipes'))
    for path in sorted(directory.glob('*.py')):
        if path.name != '__init__.py':
            importlib.import_module('tools.lab.surface_driver.recipes.'+path.stem)
    patterns = [p for p in pattern.split(',') if p]
    selected = [r for r in REGISTRY if any(fnmatch.fnmatchcase(r.name,p) or fnmatch.fnmatchcase(r.name.split('.')[0],p)
                   or any(fnmatch.fnmatchcase(i,p) for i in r.interactions) for p in patterns)]
    if not selected:
        raise RecipeBlocked('no recipes match '+pattern)
    return selected


def validate_recipes(recipes):
    model = json.loads((ROOT/'tools/lab/surface_map/model.json').read_text())
    known = {i['id'] for i in model['interactions']}
    for recipe in recipes:
        if not recipe.interactions or any(i not in known for i in recipe.interactions):
            raise ValueError(recipe.name+': recipe names an unknown interaction')
        if recipe.timeout <= 0:
            raise ValueError(recipe.name+': timeout must be positive')


def run_recipes(context, recipes, environment, out, campaign, coverage):
    interrupted=[]
    def terminate(signum, _frame):
        interrupted.append(signum)
        raise RecipeFailed('runner interrupted by '+signal.Signals(signum).name)
    previous={sig:signal.signal(sig,terminate) for sig in (signal.SIGTERM,signal.SIGHUP)}
    try:
        return _run_recipes(context,recipes,environment,out,campaign,coverage,interrupted)
    finally:
        for sig,handler in previous.items():
            signal.signal(sig,handler)


def shell_check(context):
    """Why the desktop shell can no longer be trusted, or None (#814).

    Runs between recipes, so it has its own bounded budget rather than the
    previous recipe's (possibly expired) deadline.
    """
    if not hasattr(context,'shell_failure'):
        return None
    if hasattr(context,'_deadline'):
        context._deadline=time.monotonic()+15
    try:
        return context.shell_failure()
    except (RecipeBlocked,RecipeFailed) as error:
        return 'desktop shell state unavailable: '+str(error)


def _run_recipes(context, recipes, environment, out, campaign, coverage, interrupted):
    rows=[]
    cleanup_block=None
    shell_block=None
    receipt=Path(out)/'receipt.json'
    try:
        receipt_name=str(receipt.resolve().relative_to(ROOT))
    except ValueError:
        receipt_name=str(receipt.resolve())
    for recipe in recipes:
        shell_block=shell_block or shell_check(context)
        if shell_block:
            # A crashed shell ends the run: no frames, fixtures or input on it.
            for identity in recipe.interactions:
                rows.append(ReceiptRow(identity,environment['desktop'],environment['package'],'blocked',None,
                    receipt_name,814,shell_block,environment['host'],environment['placement'],environment.get('guest'),
                    environment['destination'],False,None,[],recipe.name,environment['build'],stamp()))
            write_receipts(out,environment,rows,campaign,coverage)
            print(json.dumps({'recipe':recipe.name,'functional':'blocked','shell':'crashed'}),flush=True)
            continue
        context.begin(recipe)
        functional, issue, failure = 'passed',None,None
        def deadline(_signum,_frame):
            raise RecipeFailed('recipe deadline exceeded: '+recipe.name)
        previous = signal.signal(signal.SIGALRM,deadline)
        signal.setitimer(signal.ITIMER_REAL,recipe.timeout)
        try:
            if cleanup_block:
                raise RecipeBlocked(cleanup_block)
            if environment['destination'] not in recipe.destinations:
                raise RecipeBlocked('recipe does not support destination '+environment['destination'])
            context.frame('before')
            for need in recipe.needs:
                context.fixture(need)
            recipe.function(context)
        except RecipeBlocked as error:
            functional,issue,failure='blocked',error.issue,error.reason
        except Exception as error:
            functional='failed'
            # Driver exceptions may contain command payloads. Only declared
            # RecipeFailed messages are safe to put in coverage by default.
            failure=str(error) if isinstance(error,RecipeFailed) else 'driver error: '+type(error).__name__
        finally:
            signal.setitimer(signal.ITIMER_REAL,0)
            try:
                # Teardown owns a separate bounded budget. An action deadline must
                # not strand a window and contaminate the next recipe's before frame.
                cleanup=getattr(recipe.function,'cleanup',None)
                if cleanup:
                    try:
                        if hasattr(context,'_deadline'):
                            context._deadline=time.monotonic()+10
                        def cleanup_deadline(_signum,_frame):
                            raise RecipeFailed('recipe cleanup deadline exceeded: '+recipe.name)
                        signal.signal(signal.SIGALRM,cleanup_deadline)
                        signal.setitimer(signal.ITIMER_REAL,10)
                        cleanup(context)
                    except Exception as error:
                        reason=str(error) if isinstance(error,RecipeFailed) else type(error).__name__
                        cleanup_block='previous recipe cleanup did not finish; further actions blocked'
                        context.review('recipe cleanup failed: '+reason)
                        failure='; '.join(filter(None,(failure,'recipe cleanup failed: '+reason)))
                        if functional=='passed':
                            functional='failed'
                    finally:
                        signal.setitimer(signal.ITIMER_REAL,0)
                signal.signal(signal.SIGALRM,previous)
                try:
                    if hasattr(context,'_deadline'):
                        context._deadline=time.monotonic()+10
                    context.frame('after')
                except Exception as error:
                    context.review('after-frame unavailable: '+type(error).__name__)
            finally:
                signal.setitimer(signal.ITIMER_REAL,0)
                signal.signal(signal.SIGALRM,previous)
                # A second termination must not interrupt mount/nft/keyring cleanup.
                handlers={sig:signal.signal(sig,signal.SIG_IGN) for sig in (signal.SIGTERM,signal.SIGHUP)}
                try:
                    cleanup = context._cleanup_fixtures() if hasattr(context, '_cleanup_fixtures') else []
                finally:
                    for sig,handler in handlers.items():signal.signal(sig,handler)
        if cleanup:
            cleanup_failure = 'fixture cleanup failed: ' + '; '.join(cleanup)
            functional = 'failed'
            failure = failure + '; ' + cleanup_failure if failure else cleanup_failure
        shell_block = shell_check(context)
        if shell_block:
            # The shell died while this recipe ran: its own verdict is not evidence either.
            if failure != shell_block:
                failure = shell_block + '; recipe result on the crashed desktop: ' + functional + (' (' + failure + ')' if failure else '')
            functional, issue = 'blocked', 814
        if failure:
            context.note(failure)
        if recipe.desktop_drawn and context.frames:
            context.review('desktop-drawn frames need visual judgement')
        frames = [dataclasses.asdict(f) for f in context.frames]
        compared = [f for f in context.frames if f.distance is not None]
        visual = 'reviewed-ok' if compared and not context.reviews and all(f.distance<=f.tolerance for f in compared) else ('frames' if frames else None)
        for identity in recipe.interactions:
            rows.append(ReceiptRow(identity,environment['desktop'],environment['package'],functional,visual,
                receipt_name,issue,'; '.join(context.notes) or None,environment['host'],environment['placement'],
                environment.get('guest'),environment['destination'],bool(context.reviews),'; '.join(context.reviews) or None,
                frames,recipe.name,environment['build'],stamp()))
        write_receipts(out,environment,rows,campaign,coverage)
        print(json.dumps({'recipe':recipe.name,'functional':functional,'visual':visual,'review':bool(context.reviews)}),flush=True)
        if interrupted:
            break
    return rows


def parser():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--desktop',required=True)
    p.add_argument('--host',required=True,choices=HOSTS)
    p.add_argument('--placement',required=True,choices=PLACEMENTS)
    p.add_argument('--guest')
    p.add_argument('--run',help='existing lab.py run directory or id under work/lab-runs')
    p.add_argument('--host-profile',type=Path,help='physical.py pinned host JSON')
    p.add_argument('--lock-session',help='held physical desktop lock session id')
    p.add_argument('--bin-dir',help='target directory holding source/AppImage executables')
    p.add_argument('--boat-id',help='required boat sandbox identity for placement=boat')
    p.add_argument('--package',required=True,choices=PACKAGES)
    p.add_argument('--destination',required=True,choices=DESTINATIONS)
    p.add_argument('--recipes',required=True,help='comma-separated recipe/interactions globs or lane names')
    p.add_argument('--catalog',required=True,type=Path)
    p.add_argument('--build',required=True)
    p.add_argument('--out',required=True,type=Path)
    p.add_argument('--campaign',default='surface-driver')
    p.add_argument('--coverage',type=Path)
    p.add_argument('--profiles',type=Path)
    return p


def main(argv=None):
    args=parser().parse_args(argv)
    if not re.fullmatch(r'[A-Za-z0-9_.-]+',args.campaign):
        raise SystemExit('campaign must be a filename-safe identifier')
    if args.placement=='native' and args.guest:
        raise SystemExit('native placement cannot name a guest')
    if args.placement=='boat' and not args.boat_id:
        raise SystemExit('--placement boat requires --boat-id')
    if args.destination!='local' and not LIVE_DESTINATIONS_ENABLED:
        raise SystemExit('live destination '+args.destination+' is disabled until secret-safe evidence '
                         '(PR #726) merges; use --destination local')
    run=args.run or args.guest
    if not args.host_profile:
        if not run:
            raise SystemExit('guest input requires --run (existing lab.py run)')
        run=Path(run) if Path(run).is_dir() else ROOT/'work/lab-runs'/run
    args.out=args.out.resolve()
    args.out.mkdir(mode=0o700,parents=True,exist_ok=False)
    environment={k:getattr(args,k) for k in ('desktop','host','placement','guest','package','destination','build')}
    environment['placement_details']={'boat_sandbox_id':args.boat_id} if args.boat_id else {}
    coverage=args.coverage or ROOT/'tools/lab/surface_map/coverage'/(stamp()[:10]+'-'+args.campaign+'.json')
    target=None
    authorized=False
    rows=[]
    recipes=[]
    exit_code=2
    try:
        profile=load_profile(args.desktop,args.profiles)
        recipes=discover(args.recipes);validate_recipes(recipes)
        target=Target(run=run,host_profile=args.host_profile,bin_dir=args.bin_dir,
                      app_executable='putgd-launcher' if args.package=='appimage' else 'putgd')
        # Only the boat native-head proof invokes this CLI. Owned guests do
        # not acquire a new host-tool dependency merely by naming their boat id.
        args.boat_cli = resolve_boat_cli() if target.physical and args.placement == 'boat' else None
        if args.boat_cli:
            environment['placement_details']['boat_cli'] = args.boat_cli
        guard_target(args,target)
        authorized=True
        environment['placement_details'].update(run_id=target.info['id'],lab_profile=target.info['profile'],
                                                 run_directory=str(target.run),session_root=target.root,socket=target.socket,
                                                 input_backend=('host-input' if (target.run/'qmp.sock').exists() else 'vnc') if target.physical else 'qmp',
                                                 frame_backend='vnc' if target.physical else 'qmp')
        environment['target']=target.identity()
        if environment['target'].get('session_type')!=profile['session']:
            raise RecipeBlocked('native session type does not match the selected desktop profile')
        target.start()
        context=DriverContext(target,environment,profile,args.catalog,args.out)
        snapshot=context.call('ui.snapshot')
        observed=snapshot.get('build',{})
        environment['observed_build']=observed
        actual=observed.get('gitCommit') or observed.get('commit')
        if not actual or not (actual.startswith(args.build) or observed.get('version')==args.build):
            raise RecipeBlocked('resident build does not match --build; observed identity retained in receipt')
        if observed.get('dirty'):
            raise RecipeBlocked('resident reports a dirty build; use an identified clean artifact')
        settings=context.call('settings.get')
        patch={'archive':{'root':target.root+'/files'},'preferences':{'launchAtLogin':False}}
        if args.destination=='local':
            patch['archive']['keepUploads']=True
            patch['actions']={action:{'destination':'local','profileId':None}
                              for action in ('region','window','desktop','clipboard','edit')}
        context.call('settings.patch',{'expectedRevision':settings['metadata']['observedRevision'],'patch':patch})
        context.start_notifications()
        shell=context.shell_baseline()
        if shell:
            environment['shell_health']=shell
        rows=run_recipes(context,recipes,environment,args.out,args.campaign,coverage)
        exit_code=0 if all(r.functional=='passed' for r in rows) else 1
    except (RecipeBlocked,RecipeFailed,lab.LabError,ValueError,FileNotFoundError) as error:
        environment['blocked']=str(error)
        if target and authorized:
            try:
                target.screenshot(args.out/'startup-failed.png')
                environment['failure_frames']=['startup-failed.png']
            except Exception:
                environment['failure_frames_unavailable']=True
        if not rows:
            rows=[ReceiptRow(i,args.desktop,args.package,'blocked',receipt=str(args.out/'receipt.json'),
                  note=str(error),host=args.host,placement=args.placement,guest=args.guest,
                  destination=args.destination,recipe=r.name,build=args.build,at=stamp()) for r in recipes for i in r.interactions]
        write_receipts(args.out,environment,rows,args.campaign,coverage)
        print(json.dumps({'blocked':str(error)}),file=sys.stderr)
        exit_code=2
    finally:
        if target:
            cleanup=target.close()
            if cleanup:
                environment['cleanup_errors']=cleanup
                write_receipts(args.out,environment,rows,args.campaign,coverage)
                if exit_code==0:
                    exit_code=2
    return exit_code


if __name__=='__main__':
    raise SystemExit(main())
