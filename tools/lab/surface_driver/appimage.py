"""Guest-only KWin discovery of AppRun's exact mounted executable entries."""
import inspect
import shlex

from .api import RecipeBlocked
from .critical import critical, entry


def sync_entries(root, socket_path, cleanup=False):
    import configparser
    import hashlib
    import json
    import os
    from pathlib import Path
    import shlex
    import socket
    import stat
    import struct
    import subprocess
    import time

    root = Path(root)
    info = root.lstat()
    assert root.resolve() == root and stat.S_ISDIR(info.st_mode)
    assert info.st_uid == os.getuid() and not info.st_mode & 0o077
    manifest = root / 'kwin-mirrors.json'
    owned = json.loads(manifest.read_text()) if manifest.exists() else []
    applications = Path.home() / '.local/share/applications'
    cache_env = dict(os.environ, XDG_DATA_HOME=str(applications.parent))
    # The service cache belongs to the desktop session, not the private app.
    cache_env.pop('XDG_CACHE_HOME', None)
    cache_env['XDG_CONFIG_HOME'] = str(Path.home() / '.config')
    cache_env['XDG_RUNTIME_DIR'] = os.environ['PIPEWIRE_RUNTIME_DIR']

    def refresh():
        subprocess.run(['kbuildsycoca6', '--noincremental'], env=cache_env,
                       check=True, capture_output=True, timeout=20)

    if cleanup:
        for entry in owned:
            assert Path(entry['name']).name == entry['name']
            path = applications / entry['name']
            if not path.exists() and not path.is_symlink():
                continue
            info = path.lstat()
            assert stat.S_ISREG(info.st_mode) and info.st_uid == os.getuid()
            assert info.st_ino == entry['inode'] and info.st_dev == entry['device']
            assert hashlib.sha256(path.read_bytes()).hexdigest() == entry['sha256']
            path.unlink()
        if owned:
            refresh()
        manifest.unlink(missing_ok=True)
        return {'cleanup': True, 'files': [entry['name'] for entry in owned]}

    if 'KDE' not in os.environ.get('XDG_CURRENT_DESKTOP', '').upper():
        return
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as peer:
        peer.connect(socket_path)
        pid, uid, _ = struct.unpack('3i', peer.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED, 12))
    assert uid == os.getuid() and pid > 1
    proc = Path('/proc') / str(pid)
    environment = dict(value.split(b'=', 1) for value in (proc / 'environ').read_bytes().split(b'\0') if b'=' in value)
    assert environment[b'XDG_RUNTIME_DIR'].decode() == str(root / 'runtime')
    if b'APPIMAGE' not in environment:
        return
    executable = (proc / 'exe').resolve(strict=True)
    data = Path(environment[b'XDG_DATA_HOME'].decode())
    assert data.resolve() == data and data.is_relative_to(root)

    def read_entry(path):
        info = path.lstat()
        assert stat.S_ISREG(info.st_mode) and info.st_uid == uid
        parser = configparser.ConfigParser(interpolation=None)
        parser.read_string(path.read_text())
        entry = parser['Desktop Entry']
        return entry, shlex.split(entry['Exec'])

    deadline = time.monotonic() + 10
    while True:
        matches = []
        for path in (data / 'applications').glob('putgd-appimage-*.desktop'):
            entry, argv = read_entry(path)
            if argv == [str(executable)] and entry.get('X-KDE-DBUS-Restricted-Interfaces') == 'org.kde.KWin.ScreenShot2':
                matches.append(path)
        if matches:
            break
        assert time.monotonic() < deadline, 'AppRun entry does not name the running executable'
        time.sleep(.1)
    assert len(matches) == 1
    for path in (data / 'applications').glob('putgd-record-appimage-*.desktop'):
        entry, argv = read_entry(path)
        if argv == [str(executable.with_name('putgd-record'))]:
            assert entry.get('X-KDE-Wayland-Interfaces') == 'zkde_screencast_unstable_v1'
            matches.append(path)
    applications.mkdir(parents=True, exist_ok=True)
    assert applications.resolve() == applications and applications.stat().st_uid == uid
    for source in matches:
        destination = applications / source.name
        payload = source.read_bytes()
        digest = hashlib.sha256(payload).hexdigest()
        previous = next((entry for entry in owned if entry['name'] == source.name), None)
        if previous:
            info = destination.lstat()
            assert info.st_ino == previous['inode'] and info.st_dev == previous['device']
            assert not destination.is_symlink() and hashlib.sha256(destination.read_bytes()).hexdigest() == digest == previous['sha256']
            continue
        with destination.open('xb') as stream:
            stream.write(payload)
        info = destination.lstat()
        owned.append(dict(name=source.name, inode=info.st_ino, device=info.st_dev, sha256=digest))
        manifest.write_text(json.dumps(owned))
    refresh()
    assert (proc / 'exe').resolve(strict=True) == executable
    return {'pid': pid, 'executable': str(executable), 'data_home': str(data),
            'files': [path.name for path in matches], 'before_first_capture': True}


@entry
def prepare(target, *, cleanup=False):
    with critical(label='exact AppImage KWin entries'):
        return _prepare(target, cleanup=cleanup)


def _prepare(target, *, cleanup=False):
    """Called after every launch, before returning control to capture recipes.

    All restarts pass here too: a new AppImage mount gets a new exact entry.
    No connection is allowed to make its first capture before cache refresh.
    """
    if cleanup and not getattr(target, '_kwin_entries', False):
        return
    if not cleanup and target.physical:
        return  # This fixture intentionally writes only disposable guests.
    import json
    source = inspect.getsource(sync_entries)
    code = 'import json\n' + source + '\nprint(json.dumps(sync_entries(' + repr(target.root) + ',' + repr(target.socket) + ',' + repr(cleanup) + ')))\n'
    if not cleanup:
        target._kwin_entries = True  # Register cleanup even if copying/refresh fails.
    result = target.execute('python3 -c ' + shlex.quote(code), timeout=35)
    if result['returncode']:
        raise RecipeBlocked('AppImage KWin entry ' + ('cleanup' if cleanup else 'pre-capture staging') + ' refused', issue=961)
    receipt = json.loads(result['stdout'])
    if receipt and (receipt.get('files') or not cleanup):
        if not hasattr(target, 'kwin_entry_receipts'):
            target.kwin_entry_receipts = []
        target.kwin_entry_receipts.append(receipt)
