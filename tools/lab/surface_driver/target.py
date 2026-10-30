"""Adapters over the existing lab and physical transports. Run on the QEMU host.

The controller never creates VMs. --run selects an owned lab.py run; --host-profile
selects physical.py's pinned transport, forwarded uinput socket and VNC listener.
"""
import json
from contextlib import contextmanager
from functools import wraps
import os
from pathlib import Path
import shlex
import selectors
import subprocess
import time
import uuid
from tools.lab import lab, rfb
from tools.lab.physical_host import PhysicalHost
from .api import RecipeBlocked, RecipeFailed
from .catalog import image_modules
from .critical import critical, entry

QT_KEYS = {'Ctrl':'ctrl','Control':'ctrl','Shift':'shift','Alt':'alt','AltGr':'alt_r','Meta':'meta_l','Super':'meta_l',
           'Return':'ret','Enter':'ret','Escape':'esc','Esc':'esc','Space':'spc','Backspace':'backspace',
           'Tab':'tab','Backtab':'tab','Delete':'delete','Insert':'insert','Home':'home','End':'end',
           'PageUp':'pgup','PageDown':'pgdn','Left':'left','Right':'right','Up':'up','Down':'down','Menu':'menu'}
PUNCTUATION = dict(zip('-=[];\'`,./\\', ['minus','equal','bracket_left','bracket_right','semicolon','apostrophe','grave_accent','comma','dot','slash','backslash']))
SHIFTED = dict(zip('!@#$%^&*()_+{}:"~<>?|', '1234567890-=[];\'`,./\\'))


# Seconds for one release: owned-endpoint check, connect and reply. A release
# runs in a critical section, so this also bounds how long it holds a deferred
# recipe deadline or termination. Physical releases add the SSH lock heartbeat
# and the forwarded-endpoint proof.
RELEASE_TIMEOUT = 3.0
PHYSICAL_RELEASE_TIMEOUT = 10.0


@entry
def input_release(review=None, failed=None, label='an input release'):
    """Defer the recipe deadline and termination signals over a bounded step.

    Do not cancel/rearm the timer: a pending deadline must still fail the
    recipe once the step has completed (critical.py). QMP and RFB have socket
    timeouts independent of SIGALRM; releases also have RELEASE_TIMEOUT.
    """
    return critical(review=review, failed=failed, label=label)


PREFLIGHT = r'''
import ctypes.util, importlib, json, os, shutil
session = os.environ.get('XDG_SESSION_TYPE')
commands = ['python3', 'dbus-monitor', 'gdbus']
commands += ['xdotool', 'xprop', 'xwininfo', 'xclip', 'xrandr'] if session == 'x11' else ['wl-paste', 'wl-copy']
missing = [name for name in commands if not shutil.which(name)]
for module, package in [('pyatspi', 'python3-pyatspi'), ('dbus', 'python3-dbus'), ('PyQt6.QtGui', 'python3-pyqt6')]:
    try:
        importlib.import_module(module)
    except (ImportError, ValueError):
        missing.append(package)
if session == 'x11' and not ctypes.util.find_library('X11'):
    missing.append('libX11')
print(json.dumps({'session': session, 'missing': missing}))
'''


# Clipboard bytes never cross SSH, enter a log, or touch disk. The process
# serves the restored MIME data until the desktop clipboard manager or the
# next copy takes ownership; quitting immediately would clear X11 selections.
CLIPBOARD = r'''
import os, signal, sys
from pathlib import Path
from PyQt6.QtCore import QMimeData, QTimer
from PyQt6.QtGui import QGuiApplication
app = QGuiApplication([])
clipboard = app.clipboard()
saved = QMimeData()
source = clipboard.mimeData()
size = 0
if source:
    for mime in source.formats():
        data = source.data(mime)
        size += len(data)
        if size > 32 * 1024 * 1024:
            sys.exit(2)
        saved.setData(mime, data)
state = Path(sys.argv[1])
state.write_text('ready')
restore = False
restored = False
def request(*_):
    global restore
    restore = True
def tick():
    global restore, restored
    if restore and not restored:
        clipboard.setMimeData(saved)
        restored = True
        state.write_text('restored')
    elif restored and not clipboard.ownsClipboard():
        app.quit()
signal.signal(signal.SIGUSR1, request)
timer = QTimer()
timer.timeout.connect(tick)
timer.start(50)
app.exec()
'''


PROBE_SERVER = r'''
import json, os, subprocess, sys
namespace = {'__name__': 'surface_driver_probe'}
exec(compile(SOURCE, 'surface_driver_probe.py', 'exec'), namespace)
functions = {'accessible':'accessible', 'geometry':'geometry', 'control':'control_geometry',
             'foreground':'foreground', 'windows':'windows', 'tray':'tray', 'layout':'layout',
             'notification_read':'notification_read', 'resize':'resize_witness', 'shell_state':'shell_state'}
while True:
    line = sys.stdin.buffer.readline(1024 * 1024 + 1)
    if not line:
        break
    if len(line) > 1024 * 1024 or not line.endswith(b'\n'):
        break
    try:
        request = json.loads(line)
        # AT-SPI keeps client-side caches and a busy recording need not leave
        # its event queue idle. Use a fresh bounded interpreter for accessibility
        # observations, over this same SSH channel. X11 geometry/foreground
        # checks stay in-process, so typing and drag steps pay no startup cost.
        name, query = request['probe'], request['query']
        fresh = (name in ('accessible','control') or bool(query.get('accessible')) or
                 (name == 'foreground' and (query.get('keyboard_grab') or
                  (bool(query.get('shell_cover')) and query.get('point') is not None))) or
                 (os.environ.get('XDG_SESSION_TYPE') == 'wayland' and
                  name in ('geometry','foreground','windows','layout')))
        if fresh:
            try:
                child_source = ("__name__ = 'surface_driver_probe'\n" + SOURCE +
                                "\nimport json\nprint(json.dumps({'ok':True,'result':" +
                                functions[name] + '(' + repr(query) + ")}))\n")
                child = subprocess.run([sys.executable,'-'],
                                       input=child_source, capture_output=True,
                                       text=True, timeout=12)
            except subprocess.TimeoutExpired:
                raise RuntimeError('fresh accessibility probe timed out') from None
            if child.returncode:
                raise RuntimeError('fresh accessibility probe exited unsuccessfully')
            response = json.loads(child.stdout)
        else:
            result = namespace[functions[name]](query)
            response = {'ok':True, 'result':result}
    except Exception as error:
        response = {'ok':False, 'error':type(error).__name__ + ': ' + str(error)}
    sys.stdout.write(json.dumps(response) + '\n')
    sys.stdout.flush()
'''


def probe_mutates(name, query):
    """Probe requests that act on the desktop rather than only observing it."""
    return name == 'foreground' and bool(query.get('raise') or query.get('settle'))


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
            if verify := getattr(args[0], '_verify_endpoints', None):
                verify()
            args[0].heartbeat(force=True)
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
        self._created_root = False
        self.clipboard_pid = None
        self._outputs = None
        self._probe_process = None
        self._probe_buffer = b''
        self._probe_source = None
        self._probe_failed = False
        self._probe_failure = None
        self._io_deadline = None

    def io_timeout(self, default):
        """Seconds for one transport step: ``default``, clipped to an open release budget."""
        deadline = getattr(self, '_io_deadline', None)
        if deadline is None:
            return default
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise TimeoutError('input release budget exhausted')
        return remaining if default is None else min(default, remaining)

    @contextmanager
    def _release_budget(self):
        self._io_deadline = time.monotonic() + (PHYSICAL_RELEASE_TIMEOUT if self.physical else RELEASE_TIMEOUT)
        try:
            yield
        finally:
            self._io_deadline = None

    def require_lock(self, host, session):
        self._lock = (host, session)
        self.heartbeat(force=True)

    def heartbeat(self, force=False):
        if not self._lock:
            return
        # CLOCK_BOOTTIME includes suspend; a desktop lease keeps aging asleep.
        now=time.clock_gettime(time.CLOCK_BOOTTIME)
        if not force and now-self._last_heartbeat<60 and now<self._lock_valid_until:
            return
        from tools.lab import desktop_lock
        host,session=self._lock
        payload={'host':host,'session':session,'action':'heartbeat'}
        script="python3 - <<'SD_LOCK'\n"+desktop_lock.remote_script(payload)+'\nSD_LOCK\n'
        try:
            value=json.loads(self.physical.ssh(self.run,script,timeout=self.io_timeout(60)))
        except (lab.LabError,OSError,ValueError,subprocess.TimeoutExpired) as error:
            raise RecipeBlocked('physical desktop lock heartbeat unavailable') from error
        expiry=value.get('expiresInSeconds',0)
        if (not value.get('ok') or value.get('state')!='held' or type(expiry) not in (int,float)
                or not time.clock_gettime(time.CLOCK_BOOTTIME)-now+1 < expiry <= desktop_lock.MAX_MINUTES*60):
            raise RecipeBlocked('physical desktop lock is no longer held by this session')
        self._last_heartbeat=time.clock_gettime(time.CLOCK_BOOTTIME)
        self._lock_valid_until=now+max(0,expiry-1)

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
        # Shell commands include resident launch, raises and CLI mutations. Treat
        # every physical execute as mutating rather than infer safety from text.
        self.heartbeat(force=bool(self.physical))
        code = 'import subprocess,json\np=subprocess.run(["sh","-s"],input='+repr(script)+',text=True,capture_output=True,timeout='+repr(timeout)+')\nprint(json.dumps({"returncode":p.returncode,"stdout":p.stdout,"stderr":p.stderr}))\n'
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

    def start(self, *, first_launch=False):
        self.preflight()
        available=self.execute('command -v '+shlex.quote(self.app_executable)+' >/dev/null && command -v putgd-cli >/dev/null')
        if available['returncode']:
            raise RecipeBlocked(self.app_executable+' and putgd-cli must be installed before the surface run')
        result = self.execute('test ! -e '+shlex.quote(self.root)+' && mkdir -m 700 '+shlex.quote(self.root)+' && mkdir -m 700 '+
                              ' '.join(shlex.quote(self.root+'/'+s) for s in ('runtime','config','data','cache','state','files')))
        if result['returncode']:
            raise RecipeBlocked('private session directory already exists or cannot be created')
        self._created_root = True
        self.preserve_clipboard()
        if not first_launch:
            # Use the checked-in schema example as a baseline; discard every
            # provider and credential reference before sending it to the guest.
            document = json.loads((Path(__file__).resolve().parents[3] / 'config/development.example.json').read_text())
            document['profiles'] = []
            document['actions'] = {name: {'destination':'local', 'profileId':None, 'shortcut':''}
                                   for name in ('region','window','desktop','clipboard','edit')}
            document['actions']['record'] = {'shortcut':''}
            document['archive'].update(root=self.root+'/files', keepUploads=True)
            document['preferences']['launchAtLogin'] = False
            document['preferences']['updates'] = {'check':False}
            code = 'import os,json\nfrom pathlib import Path\np=Path(os.environ["XDG_CONFIG_HOME"])/"putgd"\np.mkdir(mode=0o700)\nwith (p/"config.json").open("x") as f: json.dump('+repr(document)+',f)\n'
            seeded = self.execute("python3 - <<'SD_SEED'\n"+code+'SD_SEED\n')
            if seeded['returncode']:
                raise RecipeBlocked('cannot seed private local-only configuration before resident launch')
        result = self.execute('nohup '+shlex.quote(self.app_executable)+' --background > '+shlex.quote(self.root+'/app.log')+' 2>&1 < /dev/null &')
        if result['returncode']:
            raise RecipeBlocked('could not start isolated resident')
        self.started = True
        self._owns_root = True
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

    def preflight(self):
        result = self.execute("python3 - <<'SD_PREFLIGHT'\n" + PREFLIGHT + '\nSD_PREFLIGHT\n')
        if result['returncode']:
            raise RecipeBlocked('surface preflight could not run python3 on target', issue=709)
        report = json.loads(result['stdout'])
        if report['session'] not in ('x11', 'wayland'):
            raise RecipeBlocked('surface preflight needs a native X11 or Wayland session', issue=709)
        if report['missing']:
            raise RecipeBlocked('surface preflight missing target tools: ' + ', '.join(report['missing']) +
                                '; provision distro packages before starting recipes', issue=709)
        return report

    def preserve_clipboard(self):
        path = self.root + '/clipboard-keeper.py'
        script = "python3 - <<'SD_KEEPER'\nfrom pathlib import Path\nPath(" + repr(path) + ').write_text(' + repr(CLIPBOARD) + ')\nSD_KEEPER\n'
        script += 'nohup python3 ' + shlex.quote(path) + ' ' + shlex.quote(self.root + '/clipboard-state') + ' >/dev/null 2>&1 </dev/null & echo $!\n'
        result = self.execute(script)
        if result['returncode']:
            raise RecipeBlocked('cannot start private in-memory clipboard preservation')
        self.clipboard_pid = int(result['stdout'].strip())
        result = self.execute('for n in $(seq 1 50); do test -f ' + shlex.quote(self.root + '/clipboard-state') +
                              ' && exit 0; kill -0 ' + str(self.clipboard_pid) + ' 2>/dev/null || exit 1; sleep .1; done; exit 1', timeout=8)
        if result['returncode']:
            raise RecipeBlocked('clipboard preservation unavailable or existing MIME data exceeds 32 MiB; no resident started')

    def restore_clipboard(self):
        if not self.clipboard_pid:
            return
        result = self.execute('kill -USR1 ' + str(self.clipboard_pid) + '\nfor n in $(seq 1 50); do test "$(cat ' +
                              shlex.quote(self.root + '/clipboard-state') + ' 2>/dev/null)" = restored && exit 0; sleep .1; done; exit 1', timeout=8)
        if result['returncode']:
            raise RecipeBlocked('previous clipboard could not be restored')
        self.clipboard_pid = None

    def cli(self, *args, timeout=60):
        # JSON never becomes a process argument, including in the CLI child.
        if not args:
            raise RecipeBlocked('CLI method required')
        payload = args[1] if len(args) > 1 else '{}'
        code = 'import subprocess,sys\np=subprocess.run('+repr(['putgd-cli','--socket',self.socket,'--params-stdin',args[0]])+',input='+repr(payload.encode())+',stdout=subprocess.PIPE,stderr=subprocess.PIPE)\nsys.stdout.buffer.write(p.stdout);sys.stderr.buffer.write(p.stderr);sys.exit(p.returncode)\n'
        return self.execute("python3 - <<'SD_CLI'\n"+code+'SD_CLI\n', timeout)

    def probe(self, name, query, source, timeout=15):
        """One owned SSH channel, with a fresh native witness for every request.

        Keeping the X11 interpreter alive removes SSH startup from each
        character and drag step. Accessibility reads use fresh children on
        that channel. No witness is cached; a lost channel blocks input.
        A request that can change the desktop (foreground raise: xdotool
        windowactivate/windowraise) forces the lock heartbeat, like real input.
        """
        started = time.monotonic()
        self.heartbeat(force=probe_mutates(name, query))
        if self._probe_failed:
            detail = getattr(self,'_probe_failure',None)
            raise RecipeBlocked('persistent desktop probe failed; '+(detail+'; ' if detail else '')+
                                'start a new run before further input')
        if self._probe_process is None:
            # Discover through the same native-session adapter as one-shot
            # probes. Only desktop connection variables cross this boundary.
            keys = ('DISPLAY','XAUTHORITY','DBUS_SESSION_BUS_ADDRESS','XDG_SESSION_TYPE',
                    'XDG_CURRENT_DESKTOP','WAYLAND_DISPLAY','QT_QPA_PLATFORM',
                    'XDG_RUNTIME_DIR','XDG_CONFIG_DIRS','XDG_DATA_DIRS')
            script = "python3 - <<'SD_PROBE_ENV'\nimport os,json\nprint(json.dumps({k:os.environ[k] for k in " + repr(keys) + " if k in os.environ}))\nSD_PROBE_ENV\n"
            native = json.loads(self.session(script, timeout=timeout))
            command = 'set -eu\n' + ''.join('export '+k+'='+shlex.quote(v)+'\n' for k,v in native.items())
            # A static bootstrap reads the server source from stdin; source and
            # queries never enter either the SSH or guest process command line.
            bootstrap = "import json,sys;exec(compile(json.loads(sys.stdin.buffer.readline()),'surface_probe_server','exec'))"
            command += self.prefix() + 'exec python3 -u -c ' + shlex.quote(bootstrap)
            args = self.physical.ssh_arguments + [self.physical.endpoint] if self.physical else lab.ssh_args(self.run)
            self._probe_process = subprocess.Popen(args+[command], stdin=subprocess.PIPE,
                                                   stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, bufsize=0)
            self._probe_source = source
            self._probe_bootstrap = (json.dumps('SOURCE='+repr(source)+chr(10)+PROBE_SERVER)+'\n').encode()
        elif source != self._probe_source:
            self._probe_failed = True
            raise RecipeBlocked('desktop probe source changed during the run; start a new identified run')
        process = self._probe_process
        payload = (json.dumps({'probe':name,'query':query})+'\n').encode()
        if len(payload) > 1024 * 1024:
            raise RecipeBlocked('desktop probe request exceeded 1 MiB')
        # Session discovery/startup spends this same request budget. Do not
        # dispatch a request once it has expired or renew it after startup.
        deadline = started + timeout
        if time.monotonic() >= deadline:
            raise RecipeBlocked(name+' probe budget exhausted before dispatch; no request sent')
        try:
            payload = getattr(self, '_probe_bootstrap', b'') + payload
            self._probe_bootstrap = b''
            remaining_payload = memoryview(payload)
            while remaining_payload:
                sent = process.stdin.write(remaining_payload)
                if not sent:
                    raise RecipeBlocked('persistent desktop probe request was not sent')
                remaining_payload = remaining_payload[sent:]
            with selectors.DefaultSelector() as ready:
                ready.register(process.stdout, selectors.EVENT_READ)
                while b'\n' not in self._probe_buffer:
                    remaining = deadline-time.monotonic()
                    if remaining <= 0 or not ready.select(remaining):
                        status = process.poll()
                        state = 'SSH process still running; reply outstanding' if status is None else 'SSH exit '+str(status)
                        raise RecipeBlocked('persistent desktop probe timed out: '+name+'; elapsed %.3fs, budget %.3fs; '
                                            % (time.monotonic()-started, timeout) + state +
                                            '; further input withheld; start a new run')
                    data = os.read(process.stdout.fileno(),65536)
                    if not data:
                        raise RecipeBlocked('persistent desktop probe closed during '+name+
                                            '; SSH exit '+str(process.poll())+'; further input withheld')
                    self._probe_buffer += data
                    if len(self._probe_buffer) > 16 * 1024 * 1024:
                        raise RecipeBlocked('desktop probe response exceeded 16 MiB')
            line, self._probe_buffer = self._probe_buffer.split(b'\n',1)
            return json.loads(line)
        except RecipeBlocked as error:
            self._probe_failed = True
            self._probe_failure_note(name,started,timeout,error.reason)
            raise
        except (OSError, ValueError) as error:
            self._probe_failed = True
            self._probe_failure_note(name,started,timeout,type(error).__name__)
            raise RecipeBlocked('persistent desktop probe transport unavailable; '+self._probe_failure) from error
        except BaseException as error:
            self._probe_failed = True
            self._probe_failure_note(name,started,timeout,'interrupted by '+type(error).__name__)
            raise

    def _probe_failure_note(self,name,started,timeout,reason):
        # Record transport state without request values or stderr. The failure
        # flag is set before this diagnostic, including on recipe alarms.
        try:
            status = self._probe_process.poll()
        except OSError:
            status = 'unknown'
        state = 'SSH process still running; reply outstanding' if status is None else 'SSH exit '+str(status)
        self._probe_failure = name+'; '+reason+'; elapsed %.3fs, budget %.3fs; '%(time.monotonic()-started,timeout)+state

    def _close_probe(self):
        process = getattr(self, '_probe_process', None)
        if process is None:
            return
        process.stdin.close()
        try:
            process.wait(timeout=3)
        except subprocess.TimeoutExpired:
            process.terminate()  # Only the SSH child this Target started.
            try:
                process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=2)
        process.stdout.close()
        self._probe_process = None

    def release_pointer(self, x, y, width, height, button='left', head=0):
        try:
            with self._release_budget():
                self._release_pointer(x,y,width,height,button,head)
        except RecipeBlocked as error:
            if not self.physical:
                raise
            host = self._lock[0] if self._lock else self.physical.config['name']
            raise RecipeBlocked('physical desktop '+host+': '+button+' button may still be pressed; '
                                'release withheld or unconfirmed; check the desktop ('+error.reason+')') from error

    def release_events(self, events, head=None):
        """Button-up events only, within the release budget."""
        with self._release_budget():
            self.events(events, head)

    def release_keys(self, codes):
        with self._release_budget():
            self.key_state(codes, False)

    @desktop_transport
    def _release_pointer(self, x, y, width, height, button='left', head=0):
        """Release at the current guest position, without another absolute move."""
        if self.physical and not (self.run / 'qmp.sock').exists():
            # RFB requires coordinates in every pointer packet. Reuse the last
            # dispatched position, never the unvisited drag endpoint.
            connection = self.vnc()
            connection.sock.settimeout(self.io_timeout(10))
            try:
                connection.pointer_button(x,y,button,False)
            finally:
                connection.sock.settimeout(10)
        else:
            lab.input_events(self.run,[{'type':'btn','data':{'button':button,'down':False}}],
                             timeout=self.io_timeout(10))

    @desktop_transport
    def events(self, events, head=None):
        if getattr(self, '_io_deadline', None) is None:
            lab.input_events(self.run, events, head)
        else:
            lab.input_events(self.run, events, head, timeout=self.io_timeout(10))

    @desktop_transport
    def key(self, codes):
        if self.physical and not (self.run / 'qmp.sock').exists():
            if 'alt_r' in codes:
                raise RecipeBlocked('AltGr requires QMP-compatible input; the VNC keysym backend does not support it')
            aliases={'meta_l':'super','spc':' ','pgup':'pageup','pgdn':'pagedown', **{v:k for k,v in PUNCTUATION.items()}}
            self.vnc().key(*(aliases.get(c,c) for c in codes))
        else:
            lab.key(self.run, codes)

    @desktop_transport
    def key_state(self, codes, down):
        if self.physical:
            raise RecipeBlocked('physical held keys require input-agent scoped cleanup across lock loss; guest QMP only')
        lab.input_events(self.run,[{'type':'key','data':{'down':down,'key':{'type':'qcode','data':code}}} for code in codes])

    @desktop_transport
    def pointer(self, x, y, width, height, down=None, button='left', head=0):
        if self.physical and not (self.run / 'qmp.sock').exists():
            self.vnc().pointer_button(x,y,button,down)
        elif self.info.get("heads"):
            import math
            # The unbound QMP tablet spans the combined framebuffer. Display-
            # bound tablets are for per-head VNC and depend on compositor input
            # transforms; using them for QMP silently distorts guest drags.
            if self._outputs is None:
                profile = lab.load_profile(self.info['profile'])
                self._outputs = lab.guest_outputs(self.run, lab.layout_backend(profile))['outputs']
                self._outputs = [o for o in self._outputs if o.get('enabled')]
            if not 0 <= head < len(self._outputs):
                raise RecipeBlocked('QMP input head has no enabled guest output')
            output = self._outputs[head]
            if (width, height) != (output['width'], output['height']):
                raise RecipeBlocked('QMP input dimensions differ from live output; refresh the layout')
            left, top = min(o['x'] for o in self._outputs), min(o['y'] for o in self._outputs)
            x, y = x + output['x'] - left, y + output['y'] - top
            width = max(o['x'] + o['width'] for o in self._outputs) - left
            height = max(o['y'] + o['height'] for o in self._outputs) - top
            events=[{'type':'abs','data':{'axis':a,'value':min(32767,max(0,math.ceil(v*32768/e)))}} for a,v,e in [('x',x,width),('y',y,height)]]
            if down is not None:
                events.append({'type':'btn','data':{'button':button,'down':down}})
            lab.input_events(self.run,events)
        else:
            lab.pointer(self.run,x,y,width,height,down,button)

    @desktop_transport
    def capture_image(self, head=0):
        """Read pixels through shared VNC, with no raw QMP witness on disk."""
        Image,_,_ = image_modules()
        port = self.physical.config['vnc_port'] if self.physical else 5900+self.info['vnc_display']+head
        if self.info.get('heads'):
            port = next(row['port'] for row in self.info['heads'] if row['head']==head)
        client = rfb.RFB(port)
        try:
            width, height, pixels = client.frame()
            return Image.frombytes('RGB', (width, height), pixels, 'raw', 'BGRX')
        finally:
            client.close()

    def screenshot(self, path, head=0):
        # Legacy direct callers have no sensitivity geometry: fail closed with
        # a full mask. DriverContext uses capture_image and registers its masks.
        from .redact import save_frame
        image = self.capture_image(head)
        return save_frame(image, path, [(0,0,image.width,image.height)], fallback=True)

    @desktop_transport
    def scroll_vnc(self, x, y, clicks):
        button = 'wheel-up' if clicks>0 else 'wheel-down'
        for _ in range(abs(clicks)):
            try:
                self.vnc().pointer_button(x,y,button,True)
            finally:
                with input_release():
                    self.vnc().pointer_button(x,y,button,False)

    def vnc(self):
        if self._rfb is None:
            self._rfb = rfb.RFB(self.physical.config['vnc_port'])
        return self._rfb

    def close(self):
        errors=[]
        try:
            self._close_probe()
        except (OSError, subprocess.TimeoutExpired):
            errors.append('persistent desktop probe cleanup unavailable')
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
        if getattr(self, 'clipboard_pid', None):
            try:
                self.restore_clipboard()
            except RecipeBlocked:
                errors.append('previous clipboard restoration unavailable; private session retained')
        if getattr(self, '_created_root', False) and not errors and not self.started:
            # Never remove another session or recurse through a retained mount.
            # Wait for the private process family, including fixture helpers.
            code='''import os,shutil,stat,time
from pathlib import Path
root=Path(ROOT)
for attempt in range(50):
 live=[]
 for proc in Path('/proc').glob('[0-9]*'):
  try:
   env=(proc/'environ').read_bytes().split(b'\\0')
   if ('XDG_RUNTIME_DIR='+str(root/'runtime')).encode() in env and (proc/'comm').read_text().strip().startswith('putgd'):
    live.append(proc.name)
  except OSError:pass
 if not live:break
 time.sleep(.1)
else:raise RuntimeError('private resident still running')
info=root.lstat()
assert root.resolve()==root and stat.S_ISDIR(info.st_mode) and info.st_uid==os.getuid() and stat.S_IMODE(info.st_mode)==0o700
assert not list(root.glob('fx-*')), 'fixture cleanup still pending'
for line in Path('/proc/self/mountinfo').read_text().splitlines():
 mount=Path(line.split()[4].replace(chr(92)+'040',' '))
 assert mount!=root and not mount.is_relative_to(root)
shutil.rmtree(root)
'''.replace('ROOT',repr(self.root))
            try:
                result=self.execute("python3 - <<'SD_CLEANUP'\n"+code+'\nSD_CLEANUP\n',timeout=8)
                if result['returncode']:
                    errors.append('private session root cleanup refused')
                else:
                    self._created_root=False
                    self._owns_root=False
                    self.private_cleanup = 'resident-log-and-state-root-removed'
            except RecipeBlocked:
                errors.append('private session root cleanup unavailable')
        return errors
