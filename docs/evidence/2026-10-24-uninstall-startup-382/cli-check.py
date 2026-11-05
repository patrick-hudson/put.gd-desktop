import datetime
import hashlib
import json
import os
from pathlib import Path
import signal
import subprocess
import tempfile
import time

import gi
gi.require_version('Gio', '2.0')
from gi.repository import Gio, GLib

repo = Path('/home/patrick/work/pg382')
os.chdir(repo)
out = repo / 'work/i382-cli-check2'
out.mkdir(mode=0o700, parents=True, exist_ok=False)
command = ['nice','-n','10','cmake','--build','--preset','dev','--parallel','2','--target','putgd','putgd-cli']
with (out/'build.log').open('w') as log:
    result = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT)
if result.returncode:
    print((out/'build.log').read_text()[-5000:], flush=True)
    raise SystemExit(result.returncode)
print('Production resident and CLI built', flush=True)

run = Path(tempfile.mkdtemp(prefix='p382c-',dir='/tmp'))
env = dict(os.environ)
for key in ['DISPLAY','WAYLAND_DISPLAY','XDG_CURRENT_DESKTOP','XDG_SESSION_DESKTOP','XDG_SESSION_TYPE','APPIMAGE','APPDIR']:
    env.pop(key,None)
for key,name in [('XDG_CONFIG_HOME','c'),('XDG_DATA_HOME','d'),('XDG_CACHE_HOME','a'),('XDG_STATE_HOME','s'),('XDG_RUNTIME_DIR','r')]:
    folder=run/name
    folder.mkdir(mode=0o700)
    env[key]=str(folder)
env['QT_QPA_PLATFORM']='offscreen'
env['DBUS_SESSION_BUS_ADDRESS']='unix:path='+str(run/'no-bus')
socket=run/'r/putgd/session.sock'
with (out/'socket-preflight.log').open('w') as log:
    subprocess.run(['python3','tools/diagnostics/check_socket_paths.py','--socket',str(socket),str(run/'no-bus')],check=True,stdout=log,stderr=subprocess.STDOUT)
config=json.loads((repo/'config/development.example.json').read_text())
config['profiles']=[]
for action in config['actions'].values():
    action.update(destination='local',profileId=None,shortcut='')
config['archive'].update(keepUploads=True,root=str(run/'archive'))
config['preferences'].update(launchAtLogin=True,notifications=False,unlockCredentialsAtStartup=False,updates={'check':False,'channel':'build'})
config_dir=run/'c/putgd'
config_dir.mkdir(mode=0o700)
config_file=config_dir/'config.json'
config_file.write_text(json.dumps(config)+'\n')
config_file.chmod(0o600)
process=None
launcher=repo/'build/dev/putgd-launcher'
hidden=repo/'build/dev/putgd-launcher.i382-absent'
entry=run/'c/autostart/putgd.desktop'

def call(method,params=None):
    argv=[str(repo/'build/dev/putgd-cli'),'--socket',str(socket),method]
    if params is not None: argv.append(json.dumps(params))
    result=subprocess.run(argv,env=env,capture_output=True,text=True,timeout=15)
    if result.returncode: raise RuntimeError(method+': '+result.stdout+result.stderr)
    reply=json.loads(result.stdout)
    if not reply['ok']: raise RuntimeError(method+': '+result.stdout)
    return reply['result']

def wait_for(fn):
    deadline=time.monotonic()+20
    while time.monotonic()<deadline:
        if process.poll() is not None: raise RuntimeError('resident exited '+str(process.returncode))
        try:
            value=fn()
            if value: return value
        except (RuntimeError,FileNotFoundError):
            pass
        time.sleep(.1)
    raise TimeoutError('bounded startup/configuration check')

def gio_accepts(path):
    try:
        return Gio.DesktopAppInfo.new_from_filename(str(path)) is not None
    except TypeError:
        # PyGObject reports GIO's rejected entry as a NULL-constructor error.
        return False

try:
    with (out/'resident.log').open('w') as log:
        process=subprocess.Popen([str(repo/'build/dev/putgd'),'--background'],env=env,stdout=log,stderr=subprocess.STDOUT,start_new_session=True)
        initial=wait_for(lambda: (state if (state:=call('ui.snapshot')['diagnostics']['loginStartup']).get('enabled') is True and state.get('applied') is True and entry.exists() else None))
        key=GLib.KeyFile()
        key.load_from_file(str(entry),GLib.KeyFileFlags.NONE)
        expected=str(launcher if launcher.exists() else repo/'build/dev/putgd')
        assert key.get_string('Desktop Entry','TryExec')==expected
        assert gio_accepts(entry)
        subprocess.run(['desktop-file-validate',str(entry)],check=True,capture_output=True,text=True)
        digest=hashlib.sha256(entry.read_bytes()).hexdigest()
        target=Path(expected)
        assert not hidden.exists()
        target.rename(hidden)
        try:
            assert not gio_accepts(entry)
            assert entry.exists() and hashlib.sha256(entry.read_bytes()).hexdigest()==digest
            assert json.loads(config_file.read_text())['preferences']['launchAtLogin'] is True
        finally:
            hidden.rename(target)
        assert gio_accepts(entry)
        metadata=call('settings.get')['metadata']
        call('settings.patch',{'patch':{'preferences':{'launchAtLogin':False}},'expectedRevision':metadata['observedRevision']})
        final=wait_for(lambda: (state if (state:=call('ui.snapshot')['diagnostics']['loginStartup']).get('enabled') is False and state.get('applied') is True and not entry.exists() else None))
        result={'source':subprocess.check_output(['git','rev-parse','HEAD'],text=True).strip(),
                'host':subprocess.check_output(['hostname'],text=True).strip(),
                'class':'Isolated offscreen production resident/CLI plus GLib/GIO parser checks; no GUI, package removal or real login acceptance',
                'socket':str(socket),'initial':initial,'final':final,
                'tryExecDecodedByGLib':expected,'entryValidated':True,'missingExecutableIgnoredByGio':True,
                'restoredExecutableRecognizedByGio':True,'preferenceAndEntryRetainedWhileExecutableAbsent':True,
                'productionCliPatchRemovedManagedEntry':True,'residentPid':process.pid,
                'appSha256':hashlib.sha256((repo/'build/dev/putgd').read_bytes()).hexdigest()}
        (out/'result.json').write_text(json.dumps(result,indent=2)+'\n')
        print(json.dumps(result,indent=2),flush=True)
finally:
    if process is not None:
        if process.poll() is None:
            assert os.getpgid(process.pid)==process.pid
            os.killpg(process.pid,signal.SIGTERM)
            try: process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                os.killpg(process.pid,signal.SIGKILL)
                process.wait(timeout=5)
        (out/'cleanup.json').write_text(json.dumps({'residentPid':process.pid,'exitCode':process.returncode,'privateRun':str(run),'running':process.poll() is None},indent=2)+'\n')
    print('Private run retained for evidence:',run,flush=True)
