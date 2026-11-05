import json, os, shlex, signal, subprocess, sys, time
from pathlib import Path
sys.path.insert(0, '/tmp/sp/tools/lab')
import lab
run=Path('/tmp/sp/work/lab-runs/c1')
out=Path('/tmp/sp/work/evidence')
source='d22d53f76bd884108faa17d1e35e273514d2ec23'
remote='putgd-lab/staging-polish'
lab.ssh(run, f'mkdir -m 700 -p ~/{remote}\n')
holder_source='''import fcntl,json,os,pwd,time
from pathlib import Path
fd=os.open('/var/lib/apt/lists/lock',os.O_RDWR)
deadline=time.monotonic()+30
while True:
    try: fcntl.lockf(fd,fcntl.LOCK_EX|fcntl.LOCK_NB); break
    except BlockingIOError:
        if time.monotonic()>=deadline: raise
        time.sleep(.1)
user=pwd.getpwnam('lab')
os.setgroups([]); os.setgid(user.pw_gid); os.setuid(user.pw_uid)
identity={'pid':os.getpid(),'startTicks':int(Path('/proc/self/stat').read_text().rsplit(')',1)[1].split()[19]),'uid':os.getuid()}
Path('/home/lab/putgd-lab/staging-polish/holder.json').write_text(json.dumps(identity))
time.sleep(240)
'''
holder=out/'holder.py';holder.write_text(holder_source)
lab.put(run,str(holder),remote+'/holder.py')
before=lab.ssh(run, 'sha256sum ~/.ssh/authorized_keys\nfind /var/lib/apt/lists -type f ! -name lock -exec sha256sum {} \\; | sort\n')
(out/'guest-before.sha256').write_text(before)
identity=lab.ssh(run, 'cat /etc/os-release\ncinnamon --version\nloginctl show-session "$(loginctl list-sessions --no-legend | awk \'$3=="lab" {print $1;exit}\')" -p Type -p Desktop -p State\ncommand -v xterm mate-terminal gnome-terminal || true\nif test -e /usr/local/lib/putgd-lab/package_install.py; then echo helper-present; else echo helper-absent; fi\n')
(out/'guest-identity.txt').write_text(identity)
hook=f'nohup python3 /home/lab/{remote}/holder.py > /home/lab/{remote}/holder.log 2>&1 < /dev/null & exit 1'
lab.ssh(run,'sudo -n /usr/bin/apt-get -o Dir::Etc::sourcelist=/dev/null -o Dir::Etc::sourceparts=- -o APT::Get::List-Cleanup=0 -o '+shlex.quote('APT::Update::Pre-Invoke::='+hook)+' update >/home/lab/'+remote+'/holder-start.log 2>&1 || true\n')
deadline=time.monotonic()+35
while True:
    raw=lab.ssh(run,f'if test -f ~/{remote}/holder.json; then cat ~/{remote}/holder.json; fi\n',timeout=5)
    if raw.strip(): break
    assert time.monotonic()<deadline,'holder failed to acquire lock'
    time.sleep(.25)
held=json.loads(raw)
(out/'holder.json').write_text(json.dumps(held,indent=2)+'\n')
started=time.monotonic()
try:
    try:
        lab.install_package(run, out/'never-uploaded.deb')
    except lab.LabError as error:
        message=str(error)
    else:
        raise AssertionError('install should refuse apt lock')
    elapsed=time.monotonic()-started
    assert f'holder PID {held["pid"]} (python3)' in message,message
    assert elapsed<30,elapsed
    evidence=next(run.glob('install-*'))
    log=(evidence/'staging.log').read_text()
    assert str(held['pid']) in log
    assert not (evidence/'helper.json').exists()
    assert not (evidence/'result.json').exists()
    assert lab.ssh(run,'if test -d ~/putgd-lab/install-active; then echo held; else echo released; fi\n').strip()=='released'
    receipt={'ok':True,'placement':'boat','sandbox':'bx_chwa2hdm','sourceCommit':source,'run':str(run),'runId':lab.state(run)['id'],
             'holder':held,'message':message,'elapsedSeconds':elapsed,'stagingLog':str(evidence/'staging.log'),
             'controllerSha256':lab.hash_file('/tmp/sp/tools/lab/lab.py'),'helperSha256':lab.hash_file('/tmp/sp/tools/lab/package_install.py')}
    (out/'lock-held-result.json').write_text(json.dumps(receipt,indent=2)+'\n')
    (out/'message.txt').write_text('put.gd lab install staging - live apt lock test\nBoat bx_chwa2hdm / Cinnamon X11 / '+source[:12]+'\n\n'+message+'\n\nFailed staging.log copied to host evidence.\nElapsed: %.2f seconds\n'%elapsed)
    lab.put(run,str(out/'message.txt'),remote+'/message.txt')
    lab.ssh(run,f'kill -0 {held["pid"]}\n')
    lab.guest_session(run, 'export XDG_STATE_HOME=$HOME/putgd-lab/state\nexport XDG_CACHE_HOME=$HOME/putgd-lab/cache\n'
        'nohup gnome-terminal --title="Live lab install staging evidence" --geometry=116x24 -- sh -c '
        +shlex.quote('cat "$HOME/putgd-lab/staging-polish/message.txt"; printf "\\nLock holder still alive at this saved frame. Press Enter to close.\\n"; read answer')
        +' > ~/putgd-lab/staging-polish/viewer.log 2>&1 < /dev/null &\n')
    time.sleep(3)
    lab.screenshot(run,'lock-held-message')
finally:
    stop='''import json,os,signal
from pathlib import Path
held=json.loads(Path('/home/lab/putgd-lab/staging-polish/holder.json').read_text())
pid=held['pid'];fd=os.pidfd_open(pid)
assert int(Path(f'/proc/{pid}/stat').read_text().rsplit(')',1)[1].split()[19])==held['startTicks']
assert Path(f'/proc/{pid}').stat().st_uid==os.getuid()==held['uid']
signal.pidfd_send_signal(fd,signal.SIGTERM)
os.close(fd)
print(json.dumps({'stoppedExactPid':pid,'startTicks':held['startTicks']}))
'''
    stopped=lab.ssh(run,"python3 - <<'PYSTOP'\n"+stop+'\nPYSTOP\n')
    (out/'holder-stopped.json').write_text(stopped)
after=lab.ssh(run, 'sha256sum ~/.ssh/authorized_keys\nfind /var/lib/apt/lists -type f ! -name lock -exec sha256sum {} \\; | sort\n')
(out/'guest-after.sha256').write_text(after)
assert before==after,'guest authorization or apt indexes changed'
print(json.dumps(receipt,indent=2))
