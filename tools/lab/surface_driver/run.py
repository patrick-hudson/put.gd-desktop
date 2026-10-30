#!/usr/bin/env python3
"""Run portable interaction recipes on a lab.py guest or a physical.py host.

Run this on the QEMU host (including boat), not on the workstation. Target
lifecycle and VNC wall forwarding stay with lab.py and the campaign owner.
"""
import argparse
import dataclasses
import fnmatch
import importlib
import json
import os
from pathlib import Path
import re
import signal
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
    rows=[]
    receipt=Path(out)/'receipt.json'
    try:
        receipt_name=str(receipt.resolve().relative_to(ROOT))
    except ValueError:
        receipt_name=str(receipt.resolve())
    for recipe in recipes:
        context.begin(recipe)
        functional, issue, failure = 'passed',None,None
        def deadline(_signum,_frame):
            raise RecipeFailed('recipe deadline exceeded: '+recipe.name)
        previous = signal.signal(signal.SIGALRM,deadline)
        signal.setitimer(signal.ITIMER_REAL,recipe.timeout)
        try:
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
            signal.signal(signal.SIGALRM,previous)
            try:
                if hasattr(context,'_deadline'):
                    context._deadline=time.monotonic()+10
                context.frame('after')
            except Exception as error:
                context.review('after-frame unavailable: '+type(error).__name__)
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
    if args.host_profile and args.host != 'boat' and (args.placement != 'native' or not args.lock_session):
        raise SystemExit('builder host-profile requires placement=native and its held --lock-session')
    if args.placement=='native' and not (args.host_profile and args.lock_session):
        raise SystemExit('native input requires --host-profile and --lock-session held by this campaign')
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
    rows=[]
    recipes=[]
    exit_code=2
    try:
        profile=load_profile(args.desktop,args.profiles)
        recipes=discover(args.recipes);validate_recipes(recipes)
        target=Target(run=run,host_profile=args.host_profile,bin_dir=args.bin_dir,
                      app_executable='putgd-launcher' if args.package=='appimage' else 'putgd')
        environment['placement_details'].update(run_id=target.info['id'],lab_profile=target.info['profile'],
                                                 run_directory=str(target.run),session_root=target.root,socket=target.socket,
                                                 input_backend=('host-input' if (target.run/'qmp.sock').exists() else 'vnc') if target.physical else 'qmp',
                                                 frame_backend='vnc' if target.physical else 'qmp')
        if args.placement=='native':
            target.require_lock(args.host,args.lock_session)
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
        rows=run_recipes(context,recipes,environment,args.out,args.campaign,coverage)
        exit_code=0 if all(r.functional=='passed' for r in rows) else 1
    except (RecipeBlocked,RecipeFailed,lab.LabError,ValueError,FileNotFoundError) as error:
        environment['blocked']=str(error)
        if target:
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
