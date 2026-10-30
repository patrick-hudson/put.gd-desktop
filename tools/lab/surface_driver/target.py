"""Adapters over the existing lab and physical transports. Run on the QEMU host.

The controller never creates VMs. --run selects an owned lab.py run; --host-profile
selects physical.py's pinned transport, forwarded uinput socket and VNC listener.
"""
import json
from functools import wraps
from pathlib import Path
import shlex
import subprocess
import time
import uuid
from tools.lab import lab, rfb
from tools.lab.physical_host import PhysicalHost
from .api import RecipeBlocked, RecipeFailed
from .catalog import image_modules

QT_KEYS = {'Ctrl':'ctrl','Control':'ctrl','Shift':'shift','Alt':'alt','Meta':'meta_l','Super':'meta_l',
           'Return':'ret','Enter':'ret','Escape':'esc','Esc':'esc','Space':'spc','Backspace':'backspace',
           'Tab':'tab','Backtab':'tab','Delete':'delete','Insert':'insert','Home':'home','End':'end',
           'PageUp':'pgup','PageDown':'pgdn','Left':'left','Right':'right','Up':'up','Down':'down','Menu':'menu'}
PUNCTUATION = dict(zip('-=[];\'`,./\\', ['minus','equal','bracket_left','bracket_right','semicolon','apostrophe','grave_accent','comma','dot','slash','backslash']))
SHIFTED = dict(zip('!@#$%^&*()_+{}:"~<>?|', '1234567890-=[];\'`,./\\'))


def qcodes(chord):
    parts = chord[:-2].split('+')+['+'] if chord.endswith('++') else (chord.split('+') if chord != '+' else ['+'])
    result = []
    for part in parts:
        if part=='Backtab':
            result+=['shift','tab']
        elif part in QT_KEYS:
            result.append(QT_KEYS[part])
        elif len(part) == 1 and part in SHIFTED:
            result += ['shift', PUNCTUATION.get(SHIFTED[part], SHIFTED[part])]
        elif len(part) == 1 and (part.isascii() and part.isalnum() or part in PUNCTUATION):
            result.append(PUNCTUATION.get(part, part.lower()))
        elif part.lower() in {f'f{i}' for i in range(1, 13)}:
            result.append(part.lower())
        else:
            raise RecipeBlocked('QMP US keyboard cannot type portable key ' + repr(part))
    return list(dict.fromkeys(result))


def checked_socket(path):
    if not path.startswith('/') or len(path.encode('utf-8')) > 107:
        raise RecipeBlocked('resolved Unix socket exceeds 107 UTF-8 bytes or is not absolute: ' + path)
    return path


def desktop_transport(function):
    @wraps(function)
    def invoke(*args, **kwargs):
        try:
            args[0].heartbeat()
            return function(*args, **kwargs)
        except RecipeBlocked:
            raise
        except (OSError, RuntimeError) as error:
            raise RecipeBlocked(function.__name__+' desktop transport unavailable: '+type(error).__name__) from error
    return invoke


class Target:
    def __init__(self, *, run=None, host_profile=None, session_root=None, bin_dir=None, app_executable="putgd"):
        self.physical = PhysicalHost(json.loads(Path(host_profile).read_text())) if host_profile else None
        self.run = self.physical.run if self.physical else Path(run).resolve()
        if self.physical:
            self.info = {'id':self.physical.config['name'], 'profile': self.physical.config['profile']['name']}
        else:
            self.info = lab.state(self.run)
        self.root = session_root or '/tmp/sd-' + uuid.uuid4().hex[:8]
        self.socket = checked_socket(self.root + '/runtime/putgd/session.sock')
        checked_socket(str(self.run / 'qmp.sock'))
        self.started = False
        self.bin_dir = bin_dir
        self.app_executable = app_executable
        self.monitor_pid = None
        self._rfb = None
        self._lock = None
        self._last_heartbeat = 0
        self._lock_valid_until = 0

    def require_lock(self, host, session):
        self._lock = (host, session)
        self.heartbeat(force=True)

    def heartbeat(self, force=False):
        if not self._lock:
            return
        now=time.monotonic()
        if not force and now-self._last_heartbeat<60 and now<self._lock_valid_until:
            return
        from tools.lab import desktop_lock
        host,session=self._lock
        payload={'host':host,'session':session,'action':'heartbeat'}
        script="python3 - <<'SD_LOCK'\n"+desktop_lock.remote_script(payload)+'\nSD_LOCK\n'
        try:
            value=json.loads(self.physical.ssh(self.run,script))
        except (lab.LabError,OSError,ValueError) as error:
            raise RecipeBlocked('physical desktop lock heartbeat unavailable') from error
        if not value.get('ok') or value.get('state')!='held':
            raise RecipeBlocked('physical desktop lock is no longer held by this session')
        self._last_heartbeat=time.monotonic()
        self._lock_valid_until=self._last_heartbeat+max(0,value.get('expiresInSeconds',0)-1)

    def session(self, body, timeout=60):
        # lab.guest_session discovers the native bus and absolute Wayland socket
        # before the private runtime is substituted. Reuse exactly that path.
        if self.physical:
            profile = self.physical.config['profile']
            from tools.lab.record_run import session_environment
            # Send the existing helper to the desktop, never discover workstation DISPLAY.
            import inspect
            from tools.lab.record_run import SESSION_VARIABLES
            code = 'import os,subprocess,json\nfrom pathlib import Path\nclass RunError(RuntimeError):pass\n'
            code += 'SESSION_VARIABLES=' + repr(SESSION_VARIABLES) + '\n' + inspect.getsource(session_environment)
            code += '\ne,_=session_environment();print(json.dumps(e))\n'
            native = json.loads(self.physical.ssh(self.run, 'python3 - <<\'PY\'\n' + code + 'PY\n', timeout))
            prefix = 'set -eu\n' + ''.join('export '+k+'='+shlex.quote(v)+'\n' for k,v in native.items())
            return self.physical.ssh(self.run, prefix + body, timeout)
        return lab.guest_session(self.run, body, timeout=timeout)

    def prefix(self):
        root = shlex.quote(self.root)
        return 'umask 077\nexport QT_LINUX_ACCESSIBILITY_ALWAYS_ON=1\n' + ('export PATH='+shlex.quote(self.bin_dir)+':"$PATH"\n' if self.bin_dir else '') + ''.join('export '+key+'='+root+'/'+suffix+'\n' for key,suffix in (
            ('XDG_CONFIG_HOME','config'),('XDG_DATA_HOME','data'),('XDG_STATE_HOME','state'),
            ('XDG_CACHE_HOME','cache'),('XDG_RUNTIME_DIR','runtime')))

    def execute(self, script, timeout=60):
        self.heartbeat()
        code = 'import subprocess,json\np=subprocess.run('+repr(script)+',shell=True,text=True,capture_output=True,timeout='+repr(timeout)+')\nprint(json.dumps({"returncode":p.returncode,"stdout":p.stdout,"stderr":p.stderr}))\n'
        try:
            return json.loads(self.session(self.prefix() + "python3 - <<'SD_PY'\n" + code + 'SD_PY\n', timeout + 10))
        except (lab.LabError, subprocess.TimeoutExpired, ValueError) as error:
            raise RecipeBlocked('target session command unavailable: ' + type(error).__name__) from error

    def identity(self):
        code="""import json,os,platform
release=platform.freedesktop_os_release()
print(json.dumps({'hostname':platform.node(),'kernel':platform.release(),'machine':platform.machine(),
                  'os':{k:release.get(k) for k in ('ID','VERSION_ID','VARIANT_ID')},
                  'session_type':os.environ.get('XDG_SESSION_TYPE'),
                  'desktop':os.environ.get('XDG_CURRENT_DESKTOP'),
                  'qt_platform':os.environ.get('QT_QPA_PLATFORM')}))
"""
        result=self.execute("python3 - <<'SD_IDENTITY'\n"+code+'SD_IDENTITY\n')
        if result['returncode']:
            raise RecipeBlocked('native target identity unavailable')
        return json.loads(result['stdout'])

    def start(self):
        available=self.execute('command -v '+shlex.quote(self.app_executable)+' >/dev/null && command -v putgd-cli >/dev/null')
        if available['returncode']:
            raise RecipeBlocked(self.app_executable+' and putgd-cli must be installed before the surface run')
        result = self.execute('test ! -e '+shlex.quote(self.root)+' && mkdir -m 700 '+shlex.quote(self.root)+' && mkdir -m 700 '+
                              ' '.join(shlex.quote(self.root+'/'+s) for s in ('runtime','config','data','cache','state','files')))
        if result['returncode']:
            raise RecipeBlocked('private session directory already exists or cannot be created')
        result = self.execute('nohup '+shlex.quote(self.app_executable)+' --background > '+shlex.quote(self.root+'/app.log')+' 2>&1 < /dev/null &')
        if result['returncode']:
            raise RecipeBlocked('could not start isolated resident')
        self.started = True
        deadline = time.monotonic() + 25
        while True:
            response = self.cli('capabilities')
            if response['returncode'] == 0:
                envelope = json.loads(response['stdout'])
                if envelope.get('ok'):
                    return envelope['result']
            if time.monotonic() > deadline:
                raise RecipeBlocked('isolated resident did not answer capabilities; private app.log retained')
            time.sleep(.25)

    def cli(self, *args, timeout=60):
        return self.execute(shlex.join(['putgd-cli','--socket',self.socket,*args]), timeout)

    @desktop_transport
    def events(self, events, head=None):
        lab.input_events(self.run, events, head)

    @desktop_transport
    def key(self, codes):
        if self.physical and not (self.run / 'qmp.sock').exists():
            aliases={'meta_l':'super','spc':' ','pgup':'pageup','pgdn':'pagedown', **{v:k for k,v in PUNCTUATION.items()}}
            self.vnc().key(*(aliases.get(c,c) for c in codes))
        else:
            lab.key(self.run, codes)

    @desktop_transport
    def pointer(self, x, y, width, height, down=None, button='left', head=0):
        if self.physical and not (self.run / 'qmp.sock').exists():
            self.vnc().pointer_button(x,y,button,down)
        elif head or self.info.get("heads"):
            import math
            events=[{'type':'abs','data':{'axis':a,'value':min(32767,max(0,math.ceil(v*32768/e)))}} for a,v,e in [('x',x,width),('y',y,height)]]
            if down is not None:
                events.append({'type':'btn','data':{'button':button,'down':down}})
            self.events(events, head)
        else:
            lab.pointer(self.run,x,y,width,height,down,button)

    @desktop_transport
    def screenshot(self, path, head=0):
        Image,_,_ = image_modules()
        witness = self.physical.screenshot(self.run, 'sd-'+uuid.uuid4().hex[:8], head) if self.physical else lab.screenshot(self.run,'sd-'+uuid.uuid4().hex[:8],head)
        with Image.open(witness['path']) as im:
            im.save(path)
        return witness

    @desktop_transport
    def scroll_vnc(self, x, y, clicks):
        self.vnc().scroll(x,y,clicks)

    def vnc(self):
        if self._rfb is None:
            self._rfb = rfb.RFB(self.physical.config['vnc_port'])
        return self._rfb

    def close(self):
        errors=[]
        if self._rfb is not None:
            try:
                self._rfb.close()
            except OSError:
                errors.append('VNC connection cleanup failed')
            self._rfb = None
        if self.monitor_pid:
            try:
                self.execute('kill '+str(self.monitor_pid)+' 2>/dev/null || true')
                self.monitor_pid=None
            except RecipeBlocked:
                errors.append('notification monitor cleanup unavailable')
        if self.started:
            try:
                result=self.cli('ui.quit')
                if result['returncode']:
                    errors.append('resident quit was rejected; private session retained')
                else:
                    self.started=False
            except RecipeBlocked:
                errors.append('resident quit transport unavailable; private session retained')
        return errors
