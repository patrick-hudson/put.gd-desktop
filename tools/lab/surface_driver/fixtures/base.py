"""Target-only fixture ownership, bounded execution, and failure-safe finalizers."""
from __future__ import annotations

import json
from pathlib import Path, PurePosixPath
import shlex
import time
import uuid

from ..api import RecipeBlocked, RecipeFailed
from ..critical import critical, entry


class Fixture(dict):
    """Recipe descriptors accept both mapping and attribute access."""
    def __getattr__(self, name):
        try:
            return self[name]
        except KeyError as error:
            raise AttributeError(name) from error


class Scope:
    def __init__(self, ctx):
        self.ctx = ctx
        self.token = uuid.uuid4().hex
        self.root = ctx.target.root + '/fx-' + self.token[:8]
        self.finalizers = []
        self.errors = []
        self.values = {}
        self.initialized = False
        self._environment = {}
        self.processes = []

    def defer(self, label, function, *, priority=0):
        self.finalizers.append((priority, label, function))

    def python(self, code, *, timeout=60):
        result = self.ctx.shell("python3 - <<'SD_FIXTURE_PY'\n" + code + '\nSD_FIXTURE_PY\n', timeout=timeout)
        if result['returncode']:
            # Neither stderr nor command payloads cross the evidence boundary.
            import inspect
            caller = inspect.currentframe().f_back
            while caller and caller.f_code.co_filename == __file__:
                caller = caller.f_back
            location = Path(caller.f_code.co_filename).name + ':' + str(caller.f_lineno) if caller else 'fixture'
            del caller
            raise RecipeBlocked('fixture target command failed at ' + location)
        return result['stdout']

    def json(self, code, **kwargs):
        try:
            return json.loads(self.python(code, **kwargs))
        except ValueError as error:
            raise RecipeBlocked('fixture target returned invalid metadata') from error

    def initialize(self):
        if self.initialized:
            return
        self.python('''import os,stat
from pathlib import Path
root=Path(ROOT)
s=root.lstat()
assert root.is_absolute() and root.resolve()==root and stat.S_ISDIR(s.st_mode)
assert s.st_uid==os.getuid() and not s.st_mode & 0o077
assert root!=Path.home() and root!=Path('/tmp')
p=Path(OWNED)
p.mkdir(mode=0o700)
(p/'owner').write_text(TOKEN)
'''.replace('ROOT', repr(self.ctx.target.root)).replace('OWNED', repr(self.root)).replace('TOKEN', repr(self.token)))
        self.initialized = True
        self.defer('owned directory', lambda: self.python('''import os,shutil
from pathlib import Path
p=Path(ROOT)
assert p.resolve()==p and not p.is_symlink() and (p/'owner').read_text()==TOKEN
for line in Path('/proc/self/mountinfo').read_text().splitlines():
 mount=Path(line.split()[4].replace(chr(92)+'040',' '))
 assert mount!=p and not mount.is_relative_to(p)
for identity in PROCESSES:
 proc=Path('/proc')/str(identity['pid'])/'stat'
 if proc.exists():
  fields=proc.read_text().split(') ',1)[1].split()
  assert fields[19]!=identity['start'] or fields[0]=='Z'
shutil.rmtree(p)
'''.replace('ROOT', repr(self.root)).replace('TOKEN', repr(self.token)).replace('PROCESSES', repr(self.processes))), priority=-1000)

    def write(self, path, data):
        import base64
        if not PurePosixPath(path).is_relative_to(PurePosixPath(self.root)):
            raise ValueError('fixture writes must stay in their owned root')
        self.python('''import base64,os
from pathlib import Path
p=Path(PATH)
p.parent.mkdir(mode=0o700,parents=True,exist_ok=True)
with open(os.open(p,os.O_WRONLY|os.O_CREAT|os.O_EXCL,0o600),'wb') as f:
 f.write(base64.b64decode(DATA))
'''.replace('PATH', repr(path)).replace('DATA', repr(base64.b64encode(data).decode())))

    def launch(self, argv, *, environment=None, name='helper'):
        """Start exactly one owned process; retain PID + Linux start time, not a pattern."""
        self.initialize()
        result = self.json('''import json,os,subprocess
from pathlib import Path
with open(LOG,'xb') as out:
 p=subprocess.Popen(ARGV,env=dict(os.environ,**ENV),stdin=subprocess.DEVNULL,stdout=out,stderr=out,start_new_session=True)
identity={'pid':p.pid,'start':Path('/proc/%d/stat'%p.pid).read_text().split(') ',1)[1].split()[19]}
print(json.dumps(identity))
'''.replace('LOG', repr(self.root+'/'+name+'.log')).replace('ARGV', repr(argv)).replace('ENV', repr(environment or {})))
        self.defer(name, lambda: self.stop(result))
        self.processes.append(result)
        return result

    def stop(self, identity):
        self.python('''import os,signal,time
from pathlib import Path
pid=IDENTITY['pid'];p=Path('/proc')/str(pid)
try:
 if not p.exists():raise ProcessLookupError
 fd=os.pidfd_open(pid)
 try:
  assert p.stat().st_uid==os.getuid()
  assert (p/'stat').read_text().split(') ',1)[1].split()[19]==IDENTITY['start']
  signal.pidfd_send_signal(fd,signal.SIGTERM)
  for _ in range(30):
   if not p.exists() or (p/'stat').read_text().split(') ',1)[1].split()[0]=='Z':break
   time.sleep(.1)
  else:
   signal.pidfd_send_signal(fd,signal.SIGKILL)
   import select
   assert select.select([fd],[],[],5)[0], 'owned helper did not exit after SIGKILL'
 finally:os.close(fd)
except (ProcessLookupError,FileNotFoundError):pass
'''.replace('IDENTITY', repr(identity)))

    def patch(self, patch):
        current = self.ctx.call('settings.get')
        return self.ctx.call('settings.patch', {'expectedRevision': current['metadata']['observedRevision'], 'patch': patch})

    def save(self, document):
        current = self.ctx.call('settings.get')
        return self.ctx.call('settings.save', {'expectedRevision': current['metadata']['observedRevision'], 'document': document})

    def restart(self, environment=None, *, before=None):
        """Restart only this private resident; keep the real desktop/portal bus."""
        target = self.ctx.target
        if environment is None:
            self._environment.clear()
        else:
            self._environment.update(environment)
        if self._environment and not self.values.get('_resident_restore'):
            self.values['_resident_restore'] = True
            self.defer('resident environment', lambda: self.restart(), priority=-500)
        self.ctx.call('ui.quit')
        target.started = False
        original = None
        try:
            # Never race a resident still releasing its private socket.
            self.ctx.wait(lambda: self.ctx.shell('test ! -e '+shlex.quote(target.socket))['returncode']==0,
                          'private resident releases socket', timeout=15)
            if before:
                before()
        except BaseException as error:
            original = error
            raise
        finally:
            try:
                self._relaunch()
            except BaseException:
                # A transient launch failure must not strand later finalizers.
                try:
                    self._relaunch()
                except BaseException:
                    if original is None:
                        raise
                else:
                    if original is None:
                        raise

    def _relaunch(self):
        target = self.ctx.target
        self.ctx.wait(lambda: self.ctx.shell('test ! -e '+shlex.quote(target.socket))['returncode']==0,
                      'private resident releases socket before relaunch', timeout=15)
        env = ' '.join(shlex.quote(k+'='+v) for k,v in self._environment.items())
        log = self.root+'/resident.log' if self._environment else target.root+'/app.log'
        command = 'nohup env '+env+' '+shlex.quote(target.app_executable)+' --background >> '+shlex.quote(log)+' 2>&1 < /dev/null &'
        if self.ctx.shell(command)['returncode']:
            raise RecipeBlocked('cannot restart private fixture resident')
        target.started = True
        def ready():
            response = self.ctx.cli('capabilities')
            try:
                return json.loads(response['stdout']).get('ok')
            except ValueError:
                return False
        self.ctx.wait(ready, 'private fixture resident ready', timeout=25)

    @entry
    def close(self):
        # Independent cleanup budget: recipe deadline expiry must not strand nft,
        # mounts, helpers, keyring collections or recording jobs. A deadline or
        # termination signal waits for the finalizers (bounded by that budget).
        with critical(getattr(self.ctx, 'review', None), label='fixture cleanup'):
            previous = self.ctx._deadline
            self.ctx._deadline = time.monotonic()+180
            try:
                self.finalizers.sort(key=_priority)
                while self.finalizers:
                    _, label, function = self.finalizers.pop()
                    try:
                        function()
                    except Exception as error:
                        self.errors.append(label+': '+type(error).__name__)
            finally:
                self.ctx._deadline = previous
            errors, self.errors = self.errors, []
        return errors


def _priority(item):
    return item[0]
