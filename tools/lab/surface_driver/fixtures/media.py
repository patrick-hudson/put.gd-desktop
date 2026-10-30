"""Private, hashed retention of a recording fixture's synthetic media (#796).

Before the fixture scope is removed, copy its working/frozen clips and the
exports recipes wrote into the scope (decoded and verified there) to the run's
own output directory: <out>/private-media/<recipe>/, mode 0700/0600, with a
manifest of guest and host SHA-256s. Only regular media files inside the
owned fixture root qualify; app configuration, credentials, VM disks and
anything outside the scope are never read. Caps: 32 MiB per file and 128 MiB
per run. A file over a cap is hashed on the guest and listed as not retained.
Retention has its own 60 s budget; the remaining finalizers get back the time
they had when it started. Every directory it creates is mode 0700.
The directory is private run evidence, never repository content or coverage:
a run directory inside the checkout must be git-ignored. Secret-safe export
(#726) is not applied here; the media is the synthetic moving test pattern.
"""
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import subprocess
import time

FILE_CAP = 32 * 1024 * 1024
RUN_CAP = 128 * 1024 * 1024
CHUNK = 3 * 1024 * 1024
BUDGET_SECONDS = 60
SUFFIXES = ('.mkv', '.mp4', '.webm', '.gif', '.webp')
ROOT = Path(__file__).resolve().parents[4]


def private_root(ctx):
    """The run's private media directory, or None with a reason."""
    out = getattr(ctx, 'out', None)
    if out is None:
        return None, 'run has no output directory'
    out = Path(out).resolve()
    if out.is_relative_to(ROOT):
        probe = out / 'private-media'
        ignored = subprocess.run(['git', '-C', str(ROOT), 'check-ignore', '-q', str(probe)],
                                 stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if ignored.returncode != 0:
            return None, 'run directory is inside the checkout and not git-ignored'
    return out / 'private-media', None


def inventory(scope):
    """Guest-side list of qualifying media files with size and SHA-256."""
    return scope.json('''import hashlib,json,os,stat
from pathlib import Path
root=Path(ROOT)
assert root.resolve()==root and (root/'owner').read_text()==TOKEN
rows=[]
for directory,names,files in os.walk(root):
 names[:]=[n for n in names if not Path(directory,n).is_symlink()]
 for name in sorted(files):
  p=Path(directory,name)
  if not name.lower().endswith(SUFFIXES):continue
  s=p.lstat()
  if not stat.S_ISREG(s.st_mode) or s.st_uid!=os.getuid():continue
  h=hashlib.sha256()
  with open(p,'rb') as f:
   for block in iter(lambda:f.read(1<<20),b''):h.update(block)
  rows.append({'path':str(p.relative_to(root)),'size':s.st_size,'sha256':h.hexdigest()})
print(json.dumps(sorted(rows,key=lambda r:r['path'])))
'''.replace('ROOT', repr(scope.root)).replace('TOKEN', repr(scope.token)).replace('SUFFIXES', repr(SUFFIXES)))


def _read(scope, relative, offset, length):
    import base64
    data = scope.python('''import base64,sys
from pathlib import Path
p=Path(ROOT)/RELATIVE
assert p.resolve().is_relative_to(Path(ROOT))
with open(p,'rb') as f:
 f.seek(OFFSET);sys.stdout.write(base64.b64encode(f.read(LENGTH)).decode())
'''.replace('ROOT', repr(scope.root)).replace('RELATIVE', repr(relative)).replace('OFFSET', repr(offset)).replace('LENGTH', repr(length)),
        timeout=120)
    return base64.b64decode(data)


def _private_dirs(path):
    """Create each missing level of path with mode 0700 (umask-proof)."""
    missing = []
    while not path.exists():
        missing.append(path)
        path = path.parent
    for level in reversed(missing):
        os.mkdir(level, 0o700)
        os.chmod(level, 0o700)


def retain(scope):
    """Copy, verify and list this scope's media within its own time budget."""
    ctx = scope.ctx
    if not scope.initialized:
        return None
    previous = getattr(ctx, '_deadline', None)
    started = time.monotonic()
    until = started + BUDGET_SECONDS
    if previous is not None:
        ctx._deadline = min(previous, until)
    try:
        return _retain(scope, until)
    finally:
        if previous is not None:
            # Later finalizers keep the budget they had before retention ran.
            ctx._deadline = time.monotonic() + max(0, previous - started)


def _retain(scope, until):
    ctx = scope.ctx
    directory, reason = private_root(ctx)
    recipe = getattr(getattr(ctx, 'recipe', None), 'name', None) or 'setup'
    if directory is None:
        getattr(ctx, 'note', print)('recording media not retained: ' + reason)
        return None
    destination = directory / recipe.replace('/', '_') / scope.token[:8]
    for attempt in range(2, 100):
        if not destination.exists():
            break
        destination = destination.with_name(scope.token[:8] + '-' + str(attempt))
    rows = inventory(scope)
    used = getattr(ctx, '_retained_media_bytes', 0)
    _private_dirs(destination.parent)
    os.chmod(directory, 0o700)
    os.mkdir(destination, 0o700)
    os.chmod(destination, 0o700)
    for row in rows:
        relative = PurePosixPath(row['path'])
        if relative.is_absolute() or '..' in relative.parts:
            row.update(retained=False, reason='unsafe relative path')
            continue
        if row['size'] > FILE_CAP:
            row.update(retained=False, reason='exceeds per-file cap of %d MiB' % (FILE_CAP >> 20))
            continue
        if used + row['size'] > RUN_CAP:
            row.update(retained=False, reason='exceeds per-run cap of %d MiB' % (RUN_CAP >> 20))
            continue
        if time.monotonic() >= until:
            row.update(retained=False, reason='retention time budget of %d s exceeded' % BUDGET_SECONDS)
            continue
        target = destination / Path(*relative.parts)
        _private_dirs(target.parent)
        digest, complete = hashlib.sha256(), True
        with open(os.open(target, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600), 'wb') as stream:
            for offset in range(0, row['size'], CHUNK):
                if time.monotonic() >= until:
                    complete = False
                    break
                block = _read(scope, row['path'], offset, min(CHUNK, row['size'] - offset))
                digest.update(block)
                stream.write(block)
        if not complete:
            target.unlink()
            row.update(retained=False, reason='retention time budget of %d s exceeded' % BUDGET_SECONDS)
            continue
        if digest.hexdigest() != row['sha256'] or target.stat().st_size != row['size']:
            target.unlink()
            row.update(retained=False, reason='host copy does not match the guest SHA-256')
            continue
        used += row['size']
        row.update(retained=True, host_path=str(target.relative_to(directory.parent)))
    ctx._retained_media_bytes = used
    manifest = {'schema': 1, 'private': True, 'recipe': recipe, 'synthetic': True,
                'caps': {'file_bytes': FILE_CAP, 'run_bytes': RUN_CAP}, 'files': rows}
    with open(os.open(destination / 'manifest.json', os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600), 'w') as stream:
        json.dump(manifest, stream, indent=1, sort_keys=True)
    kept = sum(1 for r in rows if r.get('retained'))
    ctx.note('recording media retained privately: %d/%d files (%s)' % (kept, len(rows), destination.relative_to(directory.parent)))
    return manifest


def register(scope):
    """Retain once per scope, before every other recording finalizer."""
    if scope.values.get('_media_retention'):
        return
    scope.values['_media_retention'] = True
    scope.defer('retain recording media', lambda: retain(scope), priority=1000)
