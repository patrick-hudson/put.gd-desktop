#!/usr/bin/env python3
"""Synthetic #780 guest regression. Run only on the remote QEMU host."""
import hashlib, json, os
from pathlib import Path
import shlex, signal, subprocess, sys, time, uuid
sys.path.insert(0, str(Path(__file__).resolve().parent / 'tools/lab'))
import lab
ROOT=Path(__file__).resolve().parent
RUN=ROOT/'work/lab-runs/c1'
OUT=ROOT/'work/e780'
OUT.mkdir(parents=True,exist_ok=True)

def save(name,value): lab.write_json(OUT/name,value)
def guest(script,timeout=20): return lab.ssh(RUN,'set -eu\n'+script,timeout=timeout)
def wait_for(function,seconds=20):
    deadline=time.monotonic()+seconds
    while True:
        value=function()
        if value:return value
        if time.monotonic()>=deadline:raise TimeoutError('guest handshake')
        time.sleep(.1)
def job():
    token='install-'+uuid.uuid4().hex
    guest(lab.INSTALL_LOCK_PREFIX+'mkdir ~/putgd-lab/install-active\nprintf %s '+token+' > ~/putgd-lab/install-active/job\nmkdir -m 700 ~/putgd-lab/'+token+'\n')
    return token,'putgd-lab/'+token

def read(remote,name):return json.loads(guest('cat ~/'+remote+'/'+name+'\n'))
def exists(remote,name):return guest('test ! -s ~/'+remote+'/'+name+' || echo present\n').strip()=='present'
def recover(token):return lab.recover_staging(RUN,token,abandoned=True)

def witness(name):
    value=lab.screenshot(RUN,name)
    # Convert with the host's distro Pillow, preserving the QMP original.
    from PIL import Image
    source=Path(value['path']);target=OUT/(name+'.png')
    Image.open(source).save(target)
    return {'path':target.name,'sha256':lab.hash_file(target)}

save('source.json',{'placement':'boat','files':{str(p.relative_to(ROOT)):lab.hash_file(p) for p in
    [ROOT/'tools/lab/lab.py',ROOT/'tools/lab/package_staging.py',ROOT/'tools/lab/package_install.py']},
    'state':lab.state(RUN)})
save('guest-system.json',{'text':guest('cat /etc/os-release; uname -r; systemd --version | head -1; loginctl show-user lab -p Linger; printf "runtime=%s bus=%s\\n" "$XDG_RUNTIME_DIR" "${DBUS_SESSION_BUS_ADDRESS-unset}"; cat /proc/self/cgroup; cat /proc/mounts | grep cgroup; df -h / /tmp\n')})
lab.guest_preflight(RUN,stage='before-780')
results=[]
# First exercise the unmodified production helper staging hook in the overlay.
token,remote=job()
lab.put(RUN,ROOT/'tools/lab/package_install.py',remote+'/package_install.py')
lab.stage_install_helper(RUN,remote,'/usr/local/lib/putgd-lab/package_install.py',lab.hash_file(ROOT/'tools/lab/package_install.py'))
normal=lab.reconcile_staging(RUN,remote,wait=5)
assert normal['finished'] and normal['tracking']=='cgroup-v2',normal
released=lab.recover_staging(RUN,token)
results.append({'case':'production-staging','identity':read(remote,'staging-process.json'),'recovery':released})
save('results-progress.json',results)
# A real APT Pre-Invoke root child calls setsid(), exits its hook parent, and
# waits for a synthetic file. No package or sources are installed/changed.
token,remote=job()
child_source='''import json,os,time
from pathlib import Path
os.setsid()
pid=os.getpid()
fields=Path('/proc/self/stat').read_text().rsplit(')',1)[1].split()
Path('escaped.json').write_text(json.dumps({'pid':pid,'uid':os.getuid(),'sessionId':os.getsid(0),'startTicks':int(fields[19]),'cgroup':Path('/proc/self/cgroup').read_text().strip()}))
os.chmod('escaped.json',0o644)
while not Path('finish').exists():time.sleep(.02)
'''
hook_source='import os,subprocess,sys; os.chdir(sys.argv[1]); subprocess.Popen([sys.executable,"-c",'+repr(child_source)+'],stdin=subprocess.DEVNULL,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL); sys.exit(1)'
hook='python3 -c '+shlex.quote(hook_source)+' "$HOME/'+remote+'"'
# HOME is expanded as lab before sudo resets it.
script='cd "$HOME/'+remote+'"\nhook='+shlex.quote('python3 -c '+shlex.quote(hook_source)+' ')+ '"$HOME/'+remote+'"\n'
script+='sudo -n /usr/bin/apt-get -o Dir::Etc::sourcelist=/dev/null -o Dir::Etc::sourceparts=- -o APT::Get::List-Cleanup=0 -o "APT::Update::Pre-Invoke::=$hook" update > apt-synthetic.log 2>&1 || :\necho shell-finished > shell-finished\n'
invocation=lab.staging_invocation(remote,script)
controller_source='''import sys
from pathlib import Path
sys.path.insert(0,sys.argv[1]);import lab
with lab.install_controller_fence(Path(sys.argv[2]),'busy'):
    lab.ssh(Path(sys.argv[2]),sys.argv[3],timeout=120)
'''
controller=subprocess.Popen([sys.executable,'-c',controller_source,str(ROOT/'tools/lab'),str(RUN),
    'nohup setsid --wait '+invocation+' > ~/'+remote+'/staging-output.log 2>&1 < /dev/null &\nwait $!\n'],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
try:
    wait_for(lambda:exists(remote,'escaped.json'))
    wait_for(lambda:exists(remote,'shell-finished'))
    identity=read(remote,'staging-process.json'); escaped=read(remote,'escaped.json')
    assert identity['tracking']=='cgroup-v2'
    assert escaped['uid']==0 and escaped['sessionId']!=identity['sessionId'],escaped
    assert escaped['cgroup']=='0::'+identity['cgroup']['path'],(identity,escaped)
    # The live controller fence still refuses even before killing the wrapper.
    try:recover(token);raise AssertionError('live controller accepted')
    except lab.LabError as error:assert 'controller' in str(error)
    controller_identity={'pid':controller.pid,'startTicks':int(Path(f'/proc/{controller.pid}/stat').read_text().rsplit(')',1)[1].split()[19])}
    controller.kill();controller.wait(timeout=5)
    guest('kill -KILL '+str(identity['pid'])+'\n')
    # No member of the original session remains; only the cgroup sees root.
    held=lab.reconcile_staging(RUN,remote,wait=0,abandoned=True)
    assert not held['finished'] and held['exactProcessAlive'] is False and held['sessionAlive'] is False,held
    assert held['cgroupPopulated'] is True and 'limit' not in held,held
    try:recover(token);raise AssertionError('setsid survivor accepted')
    except lab.LabError as error:assert 'remains held' in str(error)
    lock=guest('cat ~/putgd-lab/install-active/job\n').strip();assert lock==token
    visual=witness('setsid-survivor')
    # Watch the events fd from another SSH scope, outside the staging scope.
    watch_source='''import json,os,sys,time
from pathlib import Path
p=Path(sys.argv[1]);seen=[]
deadline=time.monotonic()+10
with open(p/'cgroup.events') as f:
    Path(sys.argv[2]).write_text('ready')
    while time.monotonic()<deadline:
        try:f.seek(0);value=f.read()
        except OSError as e:
            seen.append({'error':type(e).__name__,'gone':not p.exists()});break
        if not seen or seen[-1]!=value:seen.append(value)
        if 'populated 0' in value:break
        # Busy observation is bounded to ten seconds on the guest.
print(json.dumps(seen))
'''
    watch=subprocess.Popen(lab.ssh_args(RUN)+['python3 -c '+shlex.quote(watch_source)+' '+shlex.quote('/sys/fs/cgroup'+identity['cgroup']['path'])+' '+shlex.quote('/home/lab/'+remote+'/watch-ready')],stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
    wait_for(lambda:exists(remote,'watch-ready'))
    guest('touch ~/'+remote+'/finish\n')
    stdout,stderr=watch.communicate(timeout=15);assert watch.returncode==0,stderr
    observed=json.loads(stdout)
    released=wait_for(lambda:(lambda r:r if r['finished'] else None)(lab.reconcile_staging(RUN,remote,wait=0,abandoned=True)))
    recovery=recover(token)
    assert recovery['finished'] and recovery['cgroupPopulated'] is False and 'limit' not in recovery,recovery
    assert guest('test -e ~/putgd-lab/install-active && echo exists || :\n').strip()==''
    results.append({'case':'setsid-root-grandchild-wrapper-and-controller-sigkill','identity':identity,'escaped':escaped,
        'controllerKilled':controller_identity,'held':held,'eventsObserved':observed,'recovery':recovery,'visual':visual})
    save('results-progress.json',results)
finally:
    if controller.poll() is None:controller.kill();controller.wait(timeout=5)
    guest('touch ~/'+remote+'/finish\n')
# Root supervisor, launched through the production scope launcher. The helper's
# normal admission runs unchanged, with a synthetic supervise() to avoid apt.
token,remote=job()
root_script='''import json,os,sys
from pathlib import Path
from unittest.mock import patch
import runpy
source=sys.argv[1]
def synthetic(commands,directory,timeout,recovery,package='putgd'):
    Path('root-scope.json').write_text(json.dumps({'uid':os.getuid(),'cgroup':Path('/proc/self/cgroup').read_text().strip(),'admitted':Path('progress.json').exists()}))
    os.chmod('root-scope.json',0o644)
ns=runpy.run_path(source,run_name='supervisor_fixture')
ns['main'].__globals__['supervise']=synthetic
sys.argv=[source,sys.argv[2],'5','5'];ns['main']()
'''
# Existing apt privilege installs only a temporary fixture and sudo rule in the
# disposable overlay; never alter the production helper's installed bytes.
fixture=ROOT/'work/e780/root-fixture.py'; fixture.write_text(root_script)
lab.put(RUN,fixture,remote+'/root-fixture.py')
# Instead use the production helper with a synthetic apt-get at a root-owned
# PATH? That would weaken its command contract. The fixture above calls main()
# from the exact helper and replaces only supervise(), after real admission.
setup='install -m755 /home/lab/'+remote+'/root-fixture.py /usr/local/lib/putgd-lab/root780.py; printf "lab ALL=(root) NOPASSWD: /usr/bin/python3 /usr/local/lib/putgd-lab/root780.py *\\n" > /etc/sudoers.d/putgd-780; chmod 440 /etc/sudoers.d/putgd-780; visudo -cf /etc/sudoers.d/putgd-780; exit 1'
guest('sudo -n /usr/bin/apt-get -o Dir::Etc::sourcelist=/dev/null -o Dir::Etc::sourceparts=- -o APT::Get::List-Cleanup=0 -o '+shlex.quote('APT::Update::Pre-Invoke::='+setup)+' update >/dev/null 2>&1 || :\n')
script='exec sudo -n /usr/bin/python3 /usr/local/lib/putgd-lab/root780.py /usr/local/lib/putgd-lab/package_install.py "$HOME/'+remote+'"'
invocation=lab.staging_invocation(remote,script,supervisor=True)
guest('nohup setsid --wait '+invocation+' > ~/'+remote+'/supervisor.log 2>&1 < /dev/null &\n')
wait_for(lambda:exists(remote,'root-scope.json'))
identity=read(remote,'supervisor-process.json');root=read(remote,'root-scope.json')
assert root['uid']==0 and root['admitted'] and root['cgroup']=='0::'+identity['cgroup']['path'],root
# N1 remains: once progress exists recovery may not release this token.
try:recover(token);raise AssertionError('admitted supervisor accepted')
except lab.LabError:pass
# Fixture performs no package work. Retire its token under the same admission
# guard only after its wrapper's terminal receipt proves all descendants exit.
wait_for(lambda:exists(remote,'supervisor-result.json'))
guest(lab.INSTALL_LOCK_PREFIX+'test "$(cat ~/putgd-lab/install-active/job)" = '+token+'\nrm ~/putgd-lab/install-active/job\nrmdir ~/putgd-lab/install-active\n')
results.append({'case':'root-supervisor-scope-and-real-admission','identity':identity,'root':root,'recoveryRefusedAfterAdmission':True})
lab.guest_preflight(RUN,stage='after-780')
save('results.json',{'passed':True,'placement':'boat','results':results,'visual':witness('completed')})
print(json.dumps({'passed':True,'cases':len(results)}),flush=True)
