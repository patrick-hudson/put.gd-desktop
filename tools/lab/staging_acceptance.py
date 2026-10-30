#!/usr/bin/env python3
"""Synthetic #780 guest regression. Run only on the remote QEMU host."""
import hashlib, json, os
from pathlib import Path
import shlex, signal, subprocess, sys, time, uuid
ROOT=next(parent for parent in Path(__file__).resolve().parents
          if (parent/'tools/lab/lab.py').is_file())
sys.path.insert(0, str(ROOT/'tools/lab'))
import lab
RUN=ROOT/'work/lab-runs'/os.environ.get('PUTGD_780_RUN','c1')
OUT=ROOT/'work'/os.environ.get('PUTGD_780_OUTPUT','e780')
OUT.mkdir(parents=True,mode=0o700,exist_ok=True)
lab.verify_owned_guest(RUN)

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
    [ROOT/'tools/lab/lab.py',ROOT/'tools/lab/package_staging.py',ROOT/'tools/lab/package_install.py',Path(__file__).resolve()]},
    'state':lab.state(RUN)})
save('guest-system.json',{'text':guest('cat /etc/os-release; uname -r; systemctl --version | head -1; loginctl show-user lab -p Linger; printf "runtime=%s bus=%s\\n" "$XDG_RUNTIME_DIR" "${DBUS_SESSION_BUS_ADDRESS-unset}"; cat /proc/self/cgroup; cat /proc/mounts | grep cgroup; df -h / /tmp\n')})
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
    wrapper_uid=int(guest('stat -c %u /proc/'+str(identity['pid'])+'\n').strip())
    assert wrapper_uid==1000
    assert identity['cgroup']['path']=='/system.slice/'+identity['cgroup']['unit']
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
        'controllerKilled':controller_identity,'wrapperUid':wrapper_uid,'held':held,'eventsObserved':observed,'recovery':recovery,'visual':visual})
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
    Path('root-scope.json').write_text(json.dumps({'uid':os.getuid(),'cgroup':Path('/proc/self/cgroup').read_text().strip(),'admitted':Path('progress.json').exists(),'requiredScope':'--require-scope' in sys.argv}))
    os.chmod('root-scope.json',0o644)
ns=runpy.run_path(source,run_name='supervisor_fixture')
ns['main'].__globals__['supervise']=synthetic
sys.argv=[source,sys.argv[2],'5','5',*sys.argv[3:]];ns['main']()
'''
# Existing apt privilege installs only a temporary fixture and sudo rule in the
# disposable overlay; never alter the production helper's installed bytes.
fixture=OUT/'root-fixture.py'; fixture.write_text(root_script)
lab.put(RUN,fixture,remote+'/root-fixture.py')
setup='install -m755 /home/lab/'+remote+'/root-fixture.py /usr/local/lib/putgd-lab/root780.py; printf "lab ALL=(root) NOPASSWD: /usr/bin/python3 /usr/local/lib/putgd-lab/root780.py *\\n" > /etc/sudoers.d/putgd-780; chmod 440 /etc/sudoers.d/putgd-780; visudo -cf /etc/sudoers.d/putgd-780; exit 1'
guest('sudo -n /usr/bin/apt-get -o Dir::Etc::sourcelist=/dev/null -o Dir::Etc::sourceparts=- -o APT::Get::List-Cleanup=0 -o '+shlex.quote('APT::Update::Pre-Invoke::='+setup)+' update >/dev/null 2>&1 || :\n')
script='exec sudo -n /usr/bin/python3 /usr/local/lib/putgd-lab/root780.py /usr/local/lib/putgd-lab/package_install.py "$HOME/'+remote+'"'
invocation=lab.staging_invocation(remote,script,supervisor=True)
guest('nohup setsid --wait '+invocation+' > ~/'+remote+'/supervisor.log 2>&1 < /dev/null &\n')
wait_for(lambda:exists(remote,'root-scope.json'))
identity=read(remote,'supervisor-process.json');root=read(remote,'root-scope.json')
assert root['uid']==0 and root['admitted'] and root['requiredScope'] and root['cgroup']=='0::'+identity['cgroup']['path'],root
# N1 remains: once progress exists recovery may not release this token.
try:recover(token);raise AssertionError('admitted supervisor accepted')
except lab.LabError:pass
# Fixture performs no package work. Retire its token under the same admission
# guard only after its wrapper's terminal receipt proves all descendants exit.
wait_for(lambda:exists(remote,'supervisor-result.json'))
guest(lab.INSTALL_LOCK_PREFIX+'test "$(cat ~/putgd-lab/install-active/job)" = '+token+'\nrm ~/putgd-lab/install-active/job\nrmdir ~/putgd-lab/install-active\n')
results.append({'case':'root-supervisor-scope-and-real-admission','identity':identity,'root':root,'recoveryRefusedAfterAdmission':True})
lab.guest_preflight(RUN,stage='after-prior-cases')
save('prior-results.json',{'passed':True,'placement':'boat','results':results,'visual':witness('completed')})



# Separate root SSH is an observer, never a login by the lab user. Its public
# key exists only in this owned overlay; the private per-run key stays on host.
public=(RUN/'identity.pub').read_text().strip()
guest('sudo -n install -d -m 700 /root/.ssh\nprintf "\\n%s\\n" '+shlex.quote(public)+
      ' | sudo -n tee -a /root/.ssh/authorized_keys >/dev/null\n'
      'sudo -n chmod 600 /root/.ssh/authorized_keys\n'
      'printf "PermitRootLogin prohibit-password\\n" | sudo -n tee /etc/ssh/sshd_config.d/00-putgd-780-root.conf >/dev/null\n'
      'sudo -n /usr/sbin/sshd -t\nsudo -n systemctl reload ssh.service\n')
root_args=lab.ssh_args(RUN);root_args[-1]='root@127.0.0.1'
def root_guest(script,timeout=30):
    return lab.command(root_args+['sh','-s'],input='set -eu\n'+script,timeout=timeout).stdout
assert root_guest('id -u\n').strip()=='0'
save('root-observer.json',{'uid':0,'purpose':'Observe lab user-manager stop without opening a lab login',
                          'temporaryRootPublicKeyOnly':True,'placement':'boat'})

# Name collisions must not adopt another scope or retry commands outside it.
token,remote=job()
unit='putgd-staging-'+token+'.scope'
holder_source="import json,os,time;from pathlib import Path;d=Path('/home/lab/"+remote+"');d.joinpath('holder-ready.json').write_text(json.dumps({'pid':os.getpid(),'cgroup':Path('/proc/self/cgroup').read_text().strip()}));\nwhile not d.joinpath('holder-finish').exists():time.sleep(.02)"
root_guest('nohup setsid --wait systemd-run --system --scope --quiet --unit='+unit+
           ' --property=Delegate=yes -- /usr/bin/python3 -c '+shlex.quote(holder_source)+
           ' > /dev/null 2>&1 < /dev/null &\n')
wait_for(lambda:root_guest('test ! -s /home/lab/'+remote+'/holder-ready.json || echo ready\n').strip())
invocation=lab.staging_invocation(remote,'echo unsafe > collision-command-ran')
collision=lab.command(lab.ssh_args(RUN)+['sh','-s'],input='setsid --wait '+invocation+'\n',timeout=30,check=False)
assert collision.returncode!=0
assert not exists(remote,'collision-command-ran') and not exists(remote,'staging-process.json')
holder=json.loads(root_guest('cat /home/lab/'+remote+'/holder-ready.json\n'))
assert root_guest('kill -0 '+str(holder['pid'])+'\necho alive\n').strip()=='alive'
recovered=lab.recover_staging(RUN,token)
assert recovered['state']=='staging-not-started'
root_guest('touch /home/lab/'+remote+'/holder-finish\n')
results.append({'case':'existing-scope-name-collision','exitCode':collision.returncode,
                'existingHolder':holder,'commandDidNotRun':True,'recovery':recovered})
lab.guest_preflight(RUN,stage='before-logout-watchdog')
save('four-prior-cases.json',{'passed':True,'results':results,'visual':witness('before-logout')})

# Run the unmodified fixed-command supervisor against a tiny synthetic package.
# Empty APT sources and the controlled dpkg postinst gate exist only in this VM.
# Root-only SSH after logout cannot keep user@1000.service alive.
root_guest("printf 'Dir::Etc::sourcelist \"/dev/null\";\\nDir::Etc::sourceparts \"-\";\\nAPT::Get::List-Cleanup \"0\";\\n' > /etc/apt/apt.conf.d/99-putgd-780-fixture\n")
package_root=OUT/'watchdog-package';(package_root/'DEBIAN').mkdir(parents=True)
(package_root/'DEBIAN/control').write_text('Package: putgd\nVersion: 99.0.780\nArchitecture: all\nMaintainer: put.gd lab <lab@invalid>\nDescription: Synthetic disposable watchdog lifetime fixture\n')
# The token is deterministic for this child controller, so postinst can address
# exactly its job without a global process/name search.
watchdog_token='install-'+uuid.uuid4().hex
watchdog_remote='putgd-lab/'+watchdog_token
postinst_source="""import json,os,time
from pathlib import Path
root=Path('/home/lab/"""+watchdog_remote+"""')
pid=os.getpid();ancestors=[]
while pid>1:
    fields=Path(f'/proc/{pid}/stat').read_text().rsplit(')',1)[1].split()
    args=Path(f'/proc/{pid}/cmdline').read_bytes().split(bytes([0]))
    if any(value.endswith(b'/package_install.py') for value in args):
        supervisor={'pid':pid,'startTicks':int(fields[19]),'cgroup':Path(f'/proc/{pid}/cgroup').read_text().strip()}
        break
    pid=int(fields[1])
else:raise RuntimeError('Owned supervisor ancestor missing')
root.joinpath('watchdog-ready.json').write_text(json.dumps({'supervisor':supervisor,'postinstPid':os.getpid(),'uid':os.getuid()}))
os.chmod(root/'watchdog-ready.json',0o644)
end=time.monotonic()+120
while not root.joinpath('watchdog-finish').exists():
    if time.monotonic()>end:raise TimeoutError('postinst release gate')
    time.sleep(.02)
"""
postinst=package_root/'DEBIAN/postinst'
postinst.write_text('#!/bin/sh\nset -eu\n/usr/bin/python3 -c '+shlex.quote(postinst_source)+'\n')
postinst.chmod(0o755)
package=OUT/'watchdog.deb'
lab.command(['dpkg-deb','--build',package_root,package])

# Disable autologin in the overlay so logout really leaves no lab login.
# Retain hashes and changed keys, never complete authentication configuration.
configure_autologin="""import hashlib,json,re
from pathlib import Path
files=[Path('/etc/gdm3/custom.conf'),Path('/etc/gdm3/daemon.conf'),Path('/etc/gdm/custom.conf'),Path('/etc/lightdm/lightdm.conf')]
files+=list(Path('/etc/lightdm/lightdm.conf.d').glob('*.conf'))
changes=[]
for p in files:
    if not p.is_file():continue
    old=p.read_text();new=re.sub(r'(?m)^([ \t]*AutomaticLoginEnable[ \t]*=).*$',r'\1false',old)
    new=re.sub(r'(?m)^([ \t]*autologin-user[ \t]*=).*$',r'\1',new)
    if new!=old:
        p.write_text(new);changes.append({'path':str(p),'beforeSha256':hashlib.sha256(old.encode()).hexdigest(),'afterSha256':hashlib.sha256(new.encode()).hexdigest()})
print(json.dumps(changes))
"""
save('autologin-disabled.json',json.loads(root_guest('python3 -c '+shlex.quote(configure_autologin)+'\n')))
assert root_guest('test ! -e /var/lib/systemd/linger/lab && echo no-linger\n').strip()=='no-linger'

# A controller uses the real install flow, then blocks locally only after an
# actual supervisor poll. No orphan lab SSH keeps the user manager running.
controller_code="""import sys,types
from pathlib import Path
sys.path.insert(0,sys.argv[1]);import lab
lab.uuid.uuid4=lambda:types.SimpleNamespace(hex=sys.argv[4])
real=lab.ssh
def ssh(run,script,**kw):
    answer=real(run,script,**kw)
    if 'result.json; then cat' in script and not answer.strip():
        print('supervisor-phase',flush=True);sys.stdin.read()
    return answer
lab.ssh=ssh
lab.install_package(Path(sys.argv[2]),Path(sys.argv[3]),install_timeout=90,install_recovery_timeout=30)
"""
controller_log=(OUT/'logout-controller.log').open('w')
controller=subprocess.Popen([sys.executable,'-c',controller_code,str(ROOT/'tools/lab'),str(RUN),str(package),watchdog_token[8:]],
                            stdin=subprocess.PIPE,stdout=controller_log,stderr=controller_log)
start=time.monotonic();released=False
try:
    wait_for(lambda:'supervisor-phase' in (OUT/'logout-controller.log').read_text(),60)
    ready=wait_for(lambda:(json.loads(root_guest('cat /home/lab/'+watchdog_remote+'/watchdog-ready.json\n'))
                         if root_guest('test ! -s /home/lab/'+watchdog_remote+'/watchdog-ready.json || echo ready\n').strip() else None),30)
    supervisor=ready['supervisor'];assert ready['uid']==0
    assert supervisor['cgroup'].startswith('0::/system.slice/putgd-supervisor-')
    controller_identity={'pid':controller.pid,'startTicks':int(Path(f'/proc/{controller.pid}/stat').read_text().rsplit(')',1)[1].split()[19])}
    desktop=lab.load_profile(lab.state(RUN)['profile'])['session_process']
    logout='cinnamon-session-quit' if desktop=='cinnamon' else 'gnome-session-quit'
    logout_result=lab.guest_session(RUN,'timeout 10 '+logout+' --logout --no-prompt\n',timeout=15)
    controller.kill();controller.wait(timeout=5);killed_at=time.monotonic()
    snapshots=[];inactive_at=None
    deadline=time.monotonic()+55
    while time.monotonic()<deadline:
        probe="""import json,subprocess
from pathlib import Path
pid="""+str(supervisor['pid'])+"""
try:
    fields=Path(f'/proc/{pid}/stat').read_text().rsplit(')',1)[1].split()
    alive=fields[0] not in ('Z','X') and int(fields[19])=="""+str(supervisor['startTicks'])+"""
except FileNotFoundError:alive=False
state=subprocess.run(['systemctl','show','user@1000.service','--property=ActiveState','--value'],text=True,capture_output=True).stdout.strip()
print(json.dumps({'managerState':state,'supervisorAlive':alive,'sessions':subprocess.run(['loginctl','list-sessions','--no-legend'],text=True,capture_output=True).stdout}))
"""
        current=json.loads(root_guest('python3 -c '+shlex.quote(probe)+'\n'))
        current['secondsAfterControllerKill']=time.monotonic()-killed_at;snapshots.append(current)
        assert current['supervisorAlive'],current
        if current['managerState']=='inactive':
            inactive_at=inactive_at or time.monotonic()
            if time.monotonic()-inactive_at>=12:break
        else:inactive_at=None
        time.sleep(1)
    else:raise TimeoutError('Lab user manager did not remain stopped')
    visual=witness('logged-out-watchdog-alive')
    root_guest('touch /home/lab/'+watchdog_remote+'/watchdog-finish\n');released=True
    result=wait_for(lambda:json.loads(root_guest('cat /home/lab/'+watchdog_remote+'/result.json\n'))
                    if root_guest('test ! -s /home/lab/'+watchdog_remote+'/result.json || echo ready\n').strip() else None,30)
    assert result['ok'] and not result['aborted'] and not result['remainingPids'],result
    terminal=wait_for(lambda:json.loads(root_guest('cat /home/lab/'+watchdog_remote+'/supervisor-result.json\n'))
                      if root_guest('test ! -s /home/lab/'+watchdog_remote+'/supervisor-result.json || echo ready\n').strip() else None,10)
    assert terminal['finished'] and terminal['exitCode']==0
    manager_after=root_guest('systemctl show user@1000.service --property=ActiveState --value\n').strip()
    assert manager_after=='inactive'
    # Preserve the normal post-admission guard: staging recovery is not the
    # install-result acknowledgement path. Fixture cleanup follows terminal proof.
    guard=lab.INSTALL_LOCK_PREFIX.replace('~/','/home/lab/')
    root_guest(guard+'test "$(cat /home/lab/putgd-lab/install-active/job)" = '+watchdog_token+
               '\nrm /home/lab/putgd-lab/install-active/job\nrmdir /home/lab/putgd-lab/install-active\n')
    results.append({'case':'logout-controller-sigkill-detached-watchdog','controllerKilled':controller_identity,
                    'rootSupervisor':supervisor,'logoutCommand':logout,'lingerEnabled':False,
                    'userManagerSnapshots':snapshots,'managerAfterResult':manager_after,
                    'result':result,'wrapperTerminal':terminal,'visual':visual,
                    'terminalFixtureLockAcknowledged':True})
    save('results.json',{'passed':True,'placement':'boat','results':results})
    print(json.dumps({'passed':True,'cases':len(results)}),flush=True)
finally:
    if controller.poll() is None:controller.kill();controller.wait(timeout=5)
    if not released:root_guest('touch /home/lab/'+watchdog_remote+'/watchdog-finish\n')
    controller.stdin.close();controller_log.close()
