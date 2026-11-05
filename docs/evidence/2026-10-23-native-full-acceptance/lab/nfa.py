#!/usr/bin/env python3
"""Native package full-acceptance driver (docs/acceptance-matrix.md, "Native package full-acceptance profile").

Run on the desktop's host (or inside a VM guest) from SSH, beside afa.py, i326.py, rerun.py, rfb.py,
cover_player.py and s3_server.py (stage.sh copies them). Stdlib only.

A-K reuse afa.py, #415's guarded AppImage driver, with the installed package in place of the image:
put.gd is /usr/bin/putgd and /usr/bin/putgd-cli, with no AppImage environment. Every afa.py and rerun.py
subcommand works here unchanged (`nfa.py capture NAME region ...`, `nfa.py rerun live-s3 NAME ...`,
`nfa.py i326 handler-on NAME`).
`setup` replaces afa's (nothing to extract; `--profile default` uses put.gd's own default profile, for
items L, M, I, P and Q), and the lifecycle subcommands below are this pass's own.

Every link-opening click is guarded: the http and https default must be #326's stand-in recorder, or a
temporary Firefox (ffdefault.py, ffprofile.py), the guard ua_app.py uses (#415). Package commands run only
through `pkg`, which simulates the transaction first and refuses anything beyond put.gd.

  nfa.py state [--save F]                         packages, repository files, keys, default profile, autostart,
                                                  extension, OpenH264, put.gd processes
  nfa.py fetch TAG TARGET DIR                     asset + SHA256SUMS from the GitHub release, digest checked;
                                                  the pkg.put.gd mirror's SHA256SUMS compared and minisign-verified
  nfa.py repo-add PACKAGE NAME                    the repository file and key from PACKAGE's bytes, installed by hand
  nfa.py pkg NAME LABEL -- CMD ...                apt/apt-get/dnf/dpkg/rpm on put.gd only: simulate, check, run (sudo -n)
  nfa.py identity NAME TAG                        installed version vs CalVer and the release, digest of the repo's
                                                  package, verify, installed entries, AppStream
  nfa.py setup NAME --vnc-port P --input M [--profile private|default] [--keep-template] [--config-path F] [--no-launch]
  nfa.py configure NAME [--keep-template]         the fixture destination on an already running resident (after L's menu start)
  nfa.py seed NAME                                schema-5 era state through dev.133's own CLI (default profile)
  nfa.py grow NAME --to 10000                     copy the real history rows with fresh ids (resident stopped)
  nfa.py snapshot NAME --label L                  config, history.sqlite (user_version, counts, integrity), outputs
  nfa.py compare NAME BEFORE AFTER                what a migration changed
  nfa.py history-check NAME                       open, search, copy link + image, delete one from destination
  nfa.py login NAME enable|check                  Launch at login on the default profile; after a real login, what runs
  nfa.py autostart NAME --label L                 the entry, the journal since login, failed user units (#382)
  nfa.py tryexec NAME                             #382 probe: the run's entry plus TryExec=/usr/bin/putgd
  nfa.py extension NAME --label L                 GNOME: the packaged extension's state and put.gd's capability
  nfa.py s3list PREFIX [--purge]                  put.gd/PREFIX listed with key set 2 (boto3; d69)
  nfa.py teardown NAME                            default-profile folders, keychain entry, autostart entry the run made
  nfa.py key NAME CHORD... / type NAME TEXT       real key input, guarded like clicks; --label keeps a full frame
  nfa.py frame NAME --label L                     a full-desktop frame (allow-full on) and put.gd's processes
  nfa.py updates NAME --label L [--open]          Settings -> Updates on a repository install; --open: Open software updates
  nfa.py launch NAME [--exe E --cli C] / quit NAME   start the run's program again (after an upgrade or grow) / quit it
"""
import argparse, hashlib, json, os, re, shutil, sqlite3, subprocess, sys, tempfile, time, urllib.error, urllib.parse, urllib.request, uuid
from datetime import datetime, timezone
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
os.environ.setdefault('AFA_ROOT', str(Path.home() / 'work/put.gd-nfa'))
import afa  # noqa: E402  #415's guarded copy (docs/evidence/2026-10-22-rail-391-notify-415/lab/afa.py)

ROOT = afa.ROOT
REPO = 'patrick-hudson/put.gd-desktop'
HOME = Path.home()
DEFAULT_DIRS = [HOME / '.config/putgd', HOME / '.local/state/putgd', HOME / '.cache/putgd', HOME / '.local/share/putgd']
AUTOSTART = HOME / '.config/autostart/putgd.desktop'
DEB = Path('/etc/apt').exists() and shutil.which('dpkg-query') is not None
SYSTEM_FILES = ['/etc/apt/sources.list.d/putgd.sources', '/usr/share/keyrings/putgd-archive-keyring.gpg', '/etc/yum.repos.d/putgd.repo',
                '/etc/yum.repos.d/putgd.repo.rpmsave', '/etc/pki/rpm-gpg/RPM-GPG-KEY-putgd', '/etc/pki/rpm-gpg/RPM-GPG-KEY-putgd-nightly',
                '/etc/pki/rpm-gpg/RPM-GPG-KEY-putgd-stable', '/usr/share/gnome-shell/extensions/putgd@put.gd/metadata.json']
# Never changed by a put.gd transaction (COORDINATION.md rule 1).
PROTECTED = re.compile(r'^(mutter|libmutter|gnome-shell|kwin|plasma|xorg|xserver|xwayland|mesa|libgl|libegl|nvidia|pipewire|libpipewire|wireplumber|'
                       r'xdg-desktop-portal|cinnamon|muffin|sddm|gdm|lightdm)', re.I)
out, iso, sha256 = afa.out, afa.iso, afa.sha256


# ---------------------------------------------------------------- the installed package in place of the AppImage

class NativeRun(afa.Run):
    """afa.Run for /usr/bin/putgd. meta['profile'] 'default' runs put.gd's own profile and runtime directory."""
    @property
    def appdir(self): return Path('/')

    def env(self, private=True):
        env = super().env(private and self.meta.get('profile') != 'default')
        if self.meta.get('profile') == 'default':
            for key in ('XDG_CONFIG_HOME', 'XDG_STATE_HOME', 'XDG_CACHE_HOME'): env.pop(key, None)
            env['XDG_RUNTIME_DIR'] = f'/run/user/{os.getuid()}'
        return env

    def cli_env(self, private=True): return self.env(private)

    def cli(self, method, params=None, timeout=70, private=True):
        r = subprocess.run([self.meta.get('cli', '/usr/bin/putgd-cli'), method, json.dumps(params or {})], env=self.cli_env(private),
                           timeout=timeout, capture_output=True, text=True)
        try: return json.loads(r.stdout)
        except ValueError: return {'ok': False, 'raw': r.stdout[-400:], 'stderr': r.stderr[-400:], 'exit': r.returncode}

    def socket_request(self, method, params):
        # Never through meta['runtime']: that is the run's own /tmp/nfa.* folder, which cleanup deletes. A default-profile
        # run talks to /run/user/UID, the session's real runtime directory (rehearsal 2026-10-23: deleting it broke the session).
        path = str(Path(self.env()['XDG_RUNTIME_DIR']) / 'putgd/session.sock')
        with afa.socket.socket(afa.socket.AF_UNIX, afa.socket.SOCK_STREAM) as s:
            s.settimeout(20); s.connect(path)
            s.sendall(json.dumps({'schemaVersion': 1, 'requestId': 'nfa-' + method, 'session': path, 'method': method, 'params': params}).encode() + b'\n')
            data = b''
            while not data.endswith(b'\n'):
                chunk = s.recv(65536)
                if not chunk: break
                data += chunk
        return json.loads(data)

    @property
    def config_home(self): return Path(self.env().get('XDG_CONFIG_HOME') or HOME / '.config')

    @property
    def state_home(self): return Path(self.env().get('XDG_STATE_HOME') or HOME / '.local/state')

    def logs(self, needle=None):
        lines = []
        for path in sorted((self.state_home / 'putgd').rglob('putgd-*.jsonl')) if (self.state_home / 'putgd').exists() else []:
            for line in path.read_text(errors='replace').splitlines():
                if needle and needle not in line: continue
                try: lines.append(json.loads(line))
                except ValueError: pass
        return lines


def runtime_of(run): return run.env()['XDG_RUNTIME_DIR']


def ensure(run):
    """A run folder for package steps that come before `setup` (item L's install): a stub that setup completes."""
    if not run.meta:
        (run.dir / 'logs').mkdir(parents=True, exist_ok=True); run.dir.chmod(0o700)
        run.meta = {'name': run.name, 'stub': True, 'created': iso(), 'pids': {}}; run.save()
    return run


class Unit:
    """The resident's transient user unit, standing in for afa's Popen (quit_resident waits on it)."""
    def __init__(self, name): self.name = name
    def wait(self, timeout=None): return 0


def native_launch(run, label):
    """put.gd as a menu start puts it: /usr/bin/putgd --background as a transient unit of the graphical session's user
    manager (systemd-run --user), so the updater it opens gets polkit's active-session rules, which a child of the SSH
    shell doesn't (the update profile's lesson, #314/#489)."""
    sock = Path(runtime_of(run)) / 'putgd/session.sock'
    logpath = run.dir / f'logs/launch-{label}.log'
    exe = run.meta.get('exe', '/usr/bin/putgd')
    env = run.env()
    unit = f"putgd-nfa-{run.name}-{label}-{int(time.time())}"
    command = ['systemd-run', '--user', '--collect', f'--unit={unit}', f'--property=StandardOutput=append:{logpath}',
               f'--property=StandardError=append:{logpath}'] + [f'--setenv={k}={v}' for k, v in env.items()] + [exe, '--background']
    subprocess.run(command, check=True, capture_output=True, env={**os.environ, 'XDG_RUNTIME_DIR': f'/run/user/{os.getuid()}',
                                                                  'DBUS_SESSION_BUS_ADDRESS': f'unix:path=/run/user/{os.getuid()}/bus'})
    run.meta.setdefault('units', []).append(unit); run.save()
    ready = run.wait(lambda: sock.exists() and (lambda d: d.get('ok') and d['result'].get('journalReady', True))(run.cli('diagnostics', timeout=15)), 90)
    if not ready: raise SystemExit(f'resident not ready ({label}); see logs/launch-{label}.log')
    resident = [x for x in native_tree(run) if x['comm'] == 'putgd']
    run.meta['pids']['apprun'] = resident[0]['pid'] if resident else None; run.save()
    return Unit(unit)


def put_processes(runtime=None):
    """put.gd's processes of this user: any executable under /usr/bin/putgd* or a putgd library folder; with RUNTIME, only that profile's."""
    procs = []
    for p in Path('/proc').iterdir():
        if not p.name.isdigit(): continue
        try:
            if p.stat().st_uid != os.getuid(): continue
            exe = os.readlink(p / 'exe')
            if not (re.match(r'^/usr/(bin|libexec)/putgd', exe) or re.search(r'/lib(64|/x86_64-linux-gnu)?/putgd/', exe) or '.mount_putgd' in exe): continue
            env = (p / 'environ').read_bytes()
            if runtime and f'XDG_RUNTIME_DIR={runtime}'.encode() not in env.split(b'\0'): continue
            maps = (p / 'maps').read_text(errors='replace') if (p / 'maps').exists() else ''
            procs.append({'pid': int(p.name), 'ppid': int((p / 'stat').read_text().split(') ', 1)[1].split()[1]), 'exe': exe,
                          'comm': (p / 'comm').read_text().strip(), 'cmd': (p / 'cmdline').read_bytes().replace(b'\0', b' ').decode(errors='replace')[:200],
                          'qtPlatformPlugins': sorted({Path(l.split()[-1]).name for l in maps.splitlines() if '/platforms/' in l and l.split()[-1].endswith('.so')})})
        except OSError: continue
    return sorted(procs, key=lambda x: x['pid'])


def native_tree(run): return put_processes(runtime_of(run))


def native_leftovers(run):
    entries = sorted(p.name for p in (afa.DATA_HOME / 'applications').glob('putgd*.desktop') if p.name != afa.RECORDER)
    return {'processes': [f"{x['pid']} {x['comm']} {x['exe']}" for x in put_processes()], 'mounts': [], 'entries': entries}


def native_motion(run, clip):
    """The installed putgd-record --motion, with no APPDIR (that variable means an AppImage to it)."""
    r = subprocess.run(['/usr/bin/putgd-record', '--motion', str(clip), '8'], env=run.cli_env(), capture_output=True, text=True, timeout=300)
    found = re.search(r'^motion=([0-9.]+) compared=(\d+) samples=(\d+) durationMs=(\d+) size=(\S+)', r.stdout, re.M)
    if found: return {'motion': float(found.group(1)), 'compared': int(found.group(2)), 'durationMs': int(found.group(4)), 'size': found.group(5), 'by': 'putgd-record'}
    try:  # animated WebP, which FFmpeg can't decode: Pillow, as afa.py does
        from PIL import Image, ImageChops, ImageStat
        import statistics
        image = Image.open(clip); count = getattr(image, 'n_frames', 1)
        picks = sorted({round(i * (count - 1) / 7) for i in range(8)}) if count > 1 else [0]
        frames = []
        for index in picks: image.seek(index); frames.append(image.convert('L').copy())
        diffs = [statistics.mean(ImageStat.Stat(ImageChops.difference(a, b)).mean) for a, b in zip(frames, frames[1:])]
        return {'motion': round(statistics.mean(diffs), 3) if diffs else 0.0, 'frames': count, 'size': '%dx%d' % image.size, 'by': 'pillow'}
    except Exception as error:
        return {'error': (r.stdout + r.stderr).strip()[-200:] + ' / ' + type(error).__name__}


def safe_defaults(env):
    """The #415 guard, widened as in ua_app.safe_handler: http and https go to #326's stand-in recorder, the temporary
    Firefox entry (ffdefault.py on), or Firefox whose every install defaults to a putgd-afa2-* profile (ffprofile.py on).
    Exits 4 otherwise, so no link reaches the user's own browser profile."""
    import configparser
    found = afa.recorder_defaults(env)
    first = {s: (v[0].rstrip() if v else '') for s, v in found.items()}
    ok = all(v.endswith((afa.RECORDER, 'putgd-afa2-firefox.desktop')) for v in first.values())
    if ok and all(v.endswith(afa.RECORDER) for v in first.values()) and not (afa.DATA_HOME / 'applications' / afa.RECORDER).exists(): ok = False
    if not ok and all('firefox' in v for v in first.values()):
        for root in (HOME / '.mozilla/firefox', HOME / '.config/mozilla/firefox', HOME / 'snap/firefox/common/.mozilla/firefox', HOME / '.var/app/org.mozilla.firefox/.mozilla/firefox'):
            ini = root / 'profiles.ini'
            if ini.exists():
                c = configparser.RawConfigParser(); c.optionxform = str; c.read(ini)
                installs = [x for x in c.sections() if x.startswith('Install')]
                ok = bool(installs) and all(c.get(x, 'Default', fallback='').startswith('putgd-afa2-') for x in installs)
    if not ok and os.environ.get('NFA_DISPOSABLE_GUEST') == '1':
        # A disposable lab VM (DMI names QEMU) has no personal browser profile: its own default browser may open links,
        # as in the AppImage pass's VM item F. Never honoured on a physical builder.
        dmi = Path('/sys/class/dmi/id/sys_vendor').read_text(errors='replace') + Path('/sys/class/dmi/id/product_name').read_text(errors='replace')
        ok = 'QEMU' in dmi
    if ok: return found
    print(json.dumps({'refused': 'no click: links could reach the user\'s own browser (i326.py handler-on, or ffdefault.py/ffprofile.py on)',
                      'gio': found}), file=sys.stderr, flush=True)
    raise SystemExit(4)


def patch_afa():
    afa.Run = NativeRun
    afa.launch, afa.tree, afa.leftovers, afa.motion, afa.require_recorder = native_launch, native_tree, native_leftovers, native_motion, safe_defaults
    # afa's own `click`, `tray-click` and `quit-tray` take real input too: guard them as well.
    for name in ('click', 'tray_click', 'quit_tray', 'tray_menu', 'switcher', 'capture', 'edit', 'record', 'control', 'history', 'delete', 'settings'):
        original = getattr(afa, name)
        def guarded(args, _original=original):
            run = NativeRun(args.name)
            if run.meta: safe_defaults(run.meta['session'])
            return _original(args)
        setattr(afa, name, guarded)


# ---------------------------------------------------------------- system state

def sh(*cmd, timeout=60, env=None):
    r = subprocess.run(list(cmd), capture_output=True, text=True, timeout=timeout, env=env)
    return r.returncode, (r.stdout or '').strip(), (r.stderr or '').strip()


def file_info(path):
    p = Path(path)
    if not p.exists(): return None
    return {'sha256': sha256(p), 'bytes': p.stat().st_size, 'mode': oct(p.stat().st_mode & 0o7777), 'mtime': iso(datetime.fromtimestamp(p.stat().st_mtime, timezone.utc))}


def installed():
    if DEB:
        code, o, _ = sh('dpkg-query', '-W', '-f=${db:Status-Abbrev}|${Version}', 'putgd')
        return {'manager': 'dpkg', 'status': o.split('|')[0].strip() if code == 0 else None, 'version': o.split('|')[1] if code == 0 and '|' in o else None}
    code, o, _ = sh('rpm', '-q', '--qf', '%{VERSION}-%{RELEASE}', 'putgd')
    return {'manager': 'rpm', 'version': o if code == 0 else None}


def system_state():
    st = {'at': iso(), 'host': os.uname().nodename, 'package': installed(),
          'files': {f: file_info(f) for f in SYSTEM_FILES},
          'defaultProfile': {str(d): (sum(1 for _ in d.rglob('*')) if d.exists() else None) for d in DEFAULT_DIRS},
          'autostart': AUTOSTART.read_text() if AUTOSTART.exists() else None,
          'userExtension': (HOME / '.local/share/gnome-shell/extensions/putgd@put.gd').exists(),
          'processes': put_processes()}
    if shutil.which('gsettings') and shutil.which('gnome-shell'):
        env, _, _ = afa.session_environment()
        st['enabledExtensions'] = sh('gsettings', 'get', 'org.gnome.shell', 'enabled-extensions', env=env)[1]
    if not DEB:
        st['openh264'] = {p: sh('rpm', '-q', p)[1] for p in ('openh264', 'noopenh264', 'gstreamer1-plugin-openh264', 'mozilla-openh264')}
    return st


def state(args):
    st = system_state(); out(st)
    if args.save: Path(args.save).write_text(json.dumps(st, indent=1) + '\n')


# ---------------------------------------------------------------- assets

def get(url, timeout=120):
    req = urllib.request.Request(url, headers={'User-Agent': 'putgd-nfa'})
    with urllib.request.urlopen(req, timeout=timeout) as r: return r.read()


def release_url(tag, name): return f'https://github.com/{REPO}/releases/download/{urllib.parse.quote(tag)}/{urllib.parse.quote(name)}'


def asset_name(sums, target):
    pattern = {'appimage': r'-x86_64\.AppImage$', 'fedora44': r'-fedora44-x86_64\.rpm$'}.get(target, rf'-{re.escape(target)}-x86_64\.deb$')
    names = [l.split()[-1].lstrip('*') for l in sums.splitlines() if l.strip()]
    found = [n for n in names if re.search(pattern, n) and n.startswith('putgd-') and not n.startswith('putgd-gnome')]
    if len(found) != 1: raise SystemExit(f'{target}: {found or "no asset"} in SHA256SUMS')
    return found[0]


def fetch(args):
    dest = Path(args.dir); dest.mkdir(parents=True, exist_ok=True)
    sums = get(release_url(args.tag, 'SHA256SUMS')).decode()
    (dest / f'SHA256SUMS-{args.tag}').write_text(sums)
    name = asset_name(sums, args.target)
    want = next(l.split()[0] for l in sums.splitlines() if l.split()[-1].lstrip('*') == name)
    path = dest / name
    if not path.exists() or sha256(path) != want:
        tmp = path.with_suffix(path.suffix + '.part'); tmp.write_bytes(get(release_url(args.tag, name), timeout=900)); tmp.replace(path)
    rep = {'tag': args.tag, 'asset': name, 'path': str(path), 'sha256': sha256(path), 'expected': want, 'ok': sha256(path) == want}
    # The pkg.put.gd mirror: the same SHA256SUMS, signed with the channel's minisign key.
    try:
        mirror = get(f'https://pkg.put.gd/releases/{urllib.parse.quote(args.tag)}/SHA256SUMS'); sig = get(f'https://pkg.put.gd/releases/{urllib.parse.quote(args.tag)}/SHA256SUMS.minisig')
        rep['mirror'] = {'sameAsGitHub': mirror.decode() == sums}
        channel = 'stable' if re.fullmatch(r'v\d+\.\d+\.\d+', args.tag) else 'nightly'
        key = HERE / f'putgd-{channel}.minisign.pub'
        if shutil.which('minisign') and key.exists():
            with tempfile.TemporaryDirectory() as d:
                Path(d, 'SHA256SUMS').write_bytes(mirror); Path(d, 'SHA256SUMS.minisig').write_bytes(sig)
                code, o, e = sh('minisign', '-V', '-p', str(key), '-m', f'{d}/SHA256SUMS')
                rep['mirror'].update({'minisign': code == 0, 'output': (o or e)[-300:]})
        else: rep['mirror']['minisign'] = 'minisign or the public key is missing on this host'
    except Exception as e: rep['mirror'] = {'error': repr(e)[:200]}
    out(rep)
    if not rep['ok']: raise SystemExit(3)


# ---------------------------------------------------------------- package commands

def extract(package, into):
    into.mkdir(parents=True, exist_ok=True)
    if package.endswith('.deb'): subprocess.run(['dpkg-deb', '-x', package, str(into)], check=True)
    else: subprocess.run(f'rpm2cpio {shlex_quote(package)} | cpio -idm --quiet', shell=True, cwd=into, check=True)


def shlex_quote(s):
    import shlex; return shlex.quote(s)


def repo_add(args):
    """Item L: the repository file and key, as the accepted package would install them, put in place by hand."""
    run = ensure(NativeRun(args.name))
    pkg = str(Path(args.package).resolve())
    with tempfile.TemporaryDirectory() as d:
        root = Path(d); extract(pkg, root)
        pairs = ([('etc/apt/sources.list.d/putgd.sources', '/etc/apt/sources.list.d/putgd.sources'),
                  ('usr/share/keyrings/putgd-archive-keyring.gpg', '/usr/share/keyrings/putgd-archive-keyring.gpg')] if pkg.endswith('.deb') else
                 [('usr/share/putgd/repo/putgd.repo', '/etc/yum.repos.d/putgd.repo')] +
                 [(f'etc/pki/rpm-gpg/{k}', f'/etc/pki/rpm-gpg/{k}') for k in ('RPM-GPG-KEY-putgd', 'RPM-GPG-KEY-putgd-nightly', 'RPM-GPG-KEY-putgd-stable')])
        added = []
        for src, dst in pairs:
            if Path(dst).exists(): raise SystemExit(f'{dst} exists; refusing')
            if not (root / src).exists(): raise SystemExit(f'{src} not in {pkg}')
        for src, dst in pairs:
            subprocess.run(['sudo', '-n', 'install', '-m', '0644', '-o', 'root', '-g', 'root', str(root / src), dst], check=True)
            added.append({'path': dst, 'sha256': sha256(dst)})
    run.meta.setdefault('systemFilesAdded', []).extend(a['path'] for a in added); run.save()
    out(run.record('repoAdd', {'package': Path(pkg).name, 'added': added}))


PKG_TOOLS = {'apt', 'apt-get', 'dnf', 'dpkg', 'rpm', 'pkcon'}
PKG_ONLY = {'update', 'makecache', 'check-update', 'refresh', 'get-updates'}
PKG_VERBS = {'install', 'upgrade', 'remove', 'purge', 'reinstall', 'erase', 'downgrade', 'distro-sync', 'update-package', 'remove-package', 'install-local'}


def pkg_guard(cmd):
    """Only put.gd: every package argument is putgd, putgd=VERSION, putgd-VERSION or a local putgd file. Never a bare
    upgrade, autoremove or a distro-wide sync. Returns (verb, targets) or raises SystemExit."""
    if not cmd or Path(cmd[0]).name not in PKG_TOOLS: raise SystemExit(f'not a package tool: {cmd[:1]}')
    words = [w for w in cmd[1:] if not w.startswith('-')]
    if not words: raise SystemExit('no verb')
    verb = words[0]
    if Path(cmd[0]).name == 'pkcon' and verb == 'update': verb = 'update-package'
    if Path(cmd[0]).name in ('dnf',) and verb == 'update': verb = 'upgrade'
    if verb in PKG_ONLY and len(words) == 1: return verb, []
    if verb not in PKG_VERBS: raise SystemExit(f'verb {verb!r} refused')
    targets = words[1:]
    if not targets: raise SystemExit(f'bare {verb} refused')
    for t in targets:
        if not (t == 'putgd' or re.fullmatch(r'putgd[=-][0-9][\w.~+:-]*', t) or re.fullmatch(r'(\./|/)\S*putgd-[\w.~+%-]+\.(deb|rpm)', t)):
            raise SystemExit(f'target {t!r} is not put.gd')
    return verb, targets


def simulate(cmd):
    """The transaction before it runs: apt-get -s, or dnf --assumeno. Lists what would change."""
    tool = Path(cmd[0]).name
    if tool in ('apt', 'apt-get'): sim = ['apt-get', '-s'] + [w for w in cmd[1:] if w not in ('-y', '--yes')]
    elif tool == 'dnf': sim = ['sudo', '-n', 'dnf', '--assumeno'] + [w for w in cmd[1:] if w not in ('-y', '--assumeyes')]
    else: return None
    r = subprocess.run(sim, capture_output=True, text=True, timeout=600, stdin=subprocess.DEVNULL)
    text = r.stdout + r.stderr
    changes = []
    if tool in ('apt', 'apt-get'):
        for m in re.finditer(r'^(Inst|Remv|Purg|Conf) (\S+)(?: \[([^\]]+)\])?(?: \(([^ ]+))?', text, re.M):
            if m.group(1) != 'Conf': changes.append({'op': m.group(1), 'package': m.group(2), 'from': m.group(3), 'to': m.group(4)})
    else:
        section = None
        for line in text.splitlines():
            head = re.match(r'^(Installing|Upgrading|Removing|Downgrading|Reinstalling|Installing dependencies|Installing weak dependencies|Removing unused dependencies|Replacing)\b', line.strip())
            if head: section = head.group(1); continue
            m = re.match(r'^\s+(\S+)\s+x86_64|^\s+(\S+)\s+noarch', line)
            if section and m: changes.append({'op': section, 'package': m.group(1) or m.group(2)})
    return {'command': sim, 'exit': r.returncode, 'changes': changes, 'tail': text[-1500:]}


def pkg(args):
    run = ensure(NativeRun(args.name))
    cmd = args.command[1:] if args.command and args.command[0] == '--' else args.command
    verb, targets = pkg_guard(cmd)
    rep = {'label': args.label, 'command': cmd, 'before': system_state(), 'startedAt': iso()}
    if targets:
        sim = simulate(cmd); rep['simulation'] = sim
        if sim:
            touched = [c for c in sim['changes'] if c['package'].split(':')[0] != 'putgd']
            bad = [c for c in touched if PROTECTED.match(c['package']) or c['op'] in ('Remv', 'Purg', 'Removing', 'Upgrading', 'Downgrading', 'Replacing')
                   and verb not in ('remove', 'purge', 'erase')]
            if verb in ('remove', 'purge', 'erase'):
                bad = [c for c in touched if PROTECTED.match(c['package']) or c['op'] not in ('Remv', 'Purg', 'Removing', 'Removing unused dependencies')]
            rep['otherPackages'] = touched
            if bad and not args.allow_other:
                rep['refused'] = bad; out(run.record('pkg:' + args.label, rep)); raise SystemExit(5)
    log = run.dir / f'logs/pkg-{args.label}.log'
    t0 = time.monotonic()
    # Plain `sudo -n CMD`: the Ubuntu golden's sudo allows only apt-get. stdin is /dev/null, so a dpkg conffile
    # prompt reads EOF and fails instead of passing unseen.
    r = subprocess.run(['sudo', '-n'] + cmd, capture_output=True, text=True, timeout=1800, stdin=subprocess.DEVNULL)
    text = r.stdout + r.stderr
    log.write_text(text)
    rep.update({'exit': r.returncode, 'seconds': round(time.monotonic() - t0, 1), 'log': str(log),
                'conffilePrompt': bool(re.search(r"Configuration file '|\(Y/I/N/O/D/Z\)|EOF on stdin at conffile prompt", text)),
                'signature': [l.strip() for l in text.splitlines() if re.search(r'InRelease|Release\.gpg|NO_PUBKEY|EXPKEYSIG|BADSIG|repomd|GPG|[Ss]ignature|OpenPGP|Importing', l)][:20],
                'fetched': [l.strip() for l in text.splitlines() if re.search(r'^(Get:|Hit:|Ign:|Err:)|putgd.*(\.deb|\.rpm)|pkg\.put\.gd', l.strip())][:30],
                'tail': text[-2500:], 'after': system_state()})
    out(run.record('pkg:' + args.label, rep))
    if r.returncode: raise SystemExit(r.returncode)


# ---------------------------------------------------------------- A and J: identity and the package manager's view

def identity(args):
    run = NativeRun(args.name)
    tag = args.tag
    m = re.fullmatch(r'v(\d{2})\.(\d{3,4})\.(\d+)-nightly', tag)
    rep = {'tag': tag, 'installed': installed()}
    code, vj, err = sh('/usr/bin/putgd', '--version-json', env=afa.session_environment()[0])
    try: rep['versionJson'] = json.loads(vj)
    except ValueError: rep['versionJson'] = {'raw': (vj or err)[-400:]}
    builds = json.loads(get(release_url(tag, 'release-builds.json')))
    commit = json.dumps(builds)
    found = re.search(r'"(?:gitCommit|commit|sourceCommit)"\s*:\s*"([0-9a-f]{40})"', commit)
    rep['releaseCommit'] = found.group(1) if found else None
    if rep['releaseCommit']:
        c = json.loads(get(f'https://api.github.com/repos/{REPO}/commits/{rep["releaseCommit"]}'))
        rep['commitDateUtc'] = c['commit']['committer']['date']
    if m and rep.get('commitDateUtc'):
        d = datetime.strptime(rep['commitDateUtc'], '%Y-%m-%dT%H:%M:%SZ')
        expect = f'{m.group(1)}.{d.month}{d.day:02d}.{m.group(3)}~nightly'
        rep['calver'] = {'expected': expect, 'tagDate': f'{m.group(1)}.{m.group(2)}', 'commitDate': f'{d:%y}.{d.month}{d.day:02d}',
                         'installedMatches': (rep['installed']['version'] or '').startswith(expect)}
    sums = get(release_url(tag, 'SHA256SUMS')).decode()
    target = args.target
    name = asset_name(sums, target); want = next(l.split()[0] for l in sums.splitlines() if l.split()[-1].lstrip('*') == name)
    # What the repository serves: download the package as the package manager would, without installing it.
    dl = run.dir / 'download'; dl.mkdir(exist_ok=True)
    if DEB: subprocess.run(['apt-get', 'download', 'putgd'], cwd=dl, capture_output=True, timeout=600)
    else: subprocess.run(['dnf', 'download', '--destdir', str(dl), 'putgd'], capture_output=True, timeout=600)
    got = sorted(dl.glob('putgd*'))
    rep['repositoryPackage'] = [{'file': g.name, 'sha256': sha256(g), 'matchesRelease': sha256(g) == want} for g in got]
    rep['releaseAsset'] = {'name': name, 'sha256': want}
    rep['verify'] = sh('dpkg', '--verify', 'putgd')[1:] if DEB else sh('rpm', '-V', 'putgd')[1:]
    files = sh('dpkg', '-L', 'putgd')[1] if DEB else sh('rpm', '-ql', 'putgd')[1]
    listed = files.splitlines()
    rep['entries'] = {'desktop': [f for f in listed if f.endswith('.desktop')], 'icons': [f for f in listed if '/icons/' in f],
                      'metainfo': [f for f in listed if 'metainfo' in f], 'extension': [f for f in listed if 'gnome-shell/extensions' in f],
                      'executables': [f for f in listed if f.startswith('/usr/bin/')], 'repo': [f for f in listed if '/repo/' in f or 'sources.list.d' in f or 'rpm-gpg' in f]}
    meta = next((f for f in rep['entries']['metainfo'] if f.endswith('.xml')), None)
    if meta and shutil.which('appstreamcli'): rep['appstream'] = sh('appstreamcli', 'validate', '--no-net', meta)
    rep['menuEntry'] = (lambda t: {k: v for k, v in (l.split('=', 1) for l in t.splitlines() if '=' in l) if k in ('Name', 'Exec', 'Icon', 'NoDisplay', 'Categories', 'TryExec')})(
        Path('/usr/share/applications/putgd.desktop').read_text()) if Path('/usr/share/applications/putgd.desktop').exists() else None
    if DEB:
        rep['policy'] = sh('apt-cache', 'policy', 'putgd')[1].splitlines()
    else:
        rep['checkUpdate'] = sh('dnf', 'check-update', 'putgd', timeout=300)
    out(run.record('identity', rep))


# ---------------------------------------------------------------- setup (afa's, without an image)

def setup(args):
    run = NativeRun(args.name)
    if run.meta and not run.meta.get('stub'): raise SystemExit('run exists')
    stub = run.meta or {}
    before = {str(d): d.exists() for d in DEFAULT_DIRS}
    # Config and data must be absent. State and cache may hold earlier runs' logs (left as found on the builders):
    # they're listed, and teardown removes only what this run adds.
    blocking = [d for d in (HOME / '.config/putgd', HOME / '.local/share/putgd') if d.exists()]
    if args.profile == 'default' and blocking:
        raise SystemExit(f'default profile folders exist: {[str(d) for d in blocking]}; refusing (ask the coordinator)')
    if args.profile == 'private' and (HOME / '.local/share/putgd').exists():
        # Private runs keep the user's data home (as afa.py), so put.gd's data root is shared: refuse an existing one.
        raise SystemExit(f'{HOME / ".local/share/putgd"} exists; refusing (ask the coordinator)')
    run.dir.mkdir(parents=True, mode=0o700, exist_ok=True)
    for sub in ('config', 'state', 'cache', 'archive', 'frames', 'crops', 'out', 'logs', 'working'):
        (run.dir / sub).mkdir(mode=0o700, exist_ok=True)
    session, shell, _ = afa.session_environment()
    runtime = subprocess.check_output(['mktemp', '-d', '/tmp/nfa.XXXXXX'], text=True).strip()
    if args.appimage:
        # An older AppImage (dev.133 where its DEB can't run): the image is the program, its own extracted putgd-cli the CLI.
        image = Path(args.appimage).resolve()
        (run.dir / 'appdir').mkdir(exist_ok=True)
        subprocess.run([str(image), '--appimage-extract'], cwd=run.dir / 'appdir', check=True, stdout=subprocess.DEVNULL)
        root = run.dir / 'appdir/squashfs-root'
        wrapper = run.dir / 'cli.sh'
        wrapper.write_text(f'#!/bin/sh\nexport LD_LIBRARY_PATH={root}/usr/lib:{root}/usr/lib/putgd QT_PLUGIN_PATH={root}/usr/plugins:{root}/usr/lib/qt6/plugins\n'
                           f'exec {root}/usr/bin/putgd-cli "$@"\n'); wrapper.chmod(0o700)
        args.exe, args.cli = str(image), str(wrapper)
    run.meta = {'name': args.name, 'appimage': '/usr/bin/putgd', 'appdir': '/', 'exe': args.exe, 'cli': args.cli, 'profile': args.profile,
                'runtime': runtime, 'session': session, 'shell': shell, 'host': os.uname().nodename, 'vnc': args.vnc_port, 'input': args.input,
                'before': {'homePaths': {str(p): (sorted(str(f) for f in p.rglob('*')) if p.exists() else None) for p in DEFAULT_DIRS},
                           'applications': sorted(p.name for p in (afa.DATA_HOME / 'applications').glob('*')) if (afa.DATA_HOME / 'applications').exists() else [],
                           'autostart': AUTOSTART.exists(), 'defaultProfile': before},
                'pids': {}, 'created': iso(), 'package': installed(), 'systemFilesAdded': stub.get('systemFilesAdded', []),
                'credentialsMode': args.credentials}
    run.save()
    fixture = subprocess.Popen([sys.executable, '-I', str(HERE / 's3_server.py')], stdout=subprocess.PIPE, stderr=open(run.dir / 'logs/fixture.log', 'w'),
                               text=True, start_new_session=True)
    run.meta['port'] = int(fixture.stdout.readline()); run.meta['pids']['fixture'] = fixture.pid; run.save()
    if args.no_launch:  # item L: the first start is the menu click, on an untouched profile; `configure` comes after
        out({'setup': True, 'launched': False, 'run': run.meta}); return
    resident = native_launch(run, 'setup')
    configure(args, run, resident)


def configure(args, run=None, resident=None):
    """The fixture destination on the running resident (afa's document, or an older build's own with --keep-template)."""
    run = run or NativeRun(args.name)
    template = run.ok('settings.developmentTemplate')['document']
    doc = afa.config_document(run, template)
    if getattr(args, 'keep_template', False):  # an older build (dev.133, nightly.947): its own template, with only the fixture route added
        doc = old_build_document(run, template)
    cfg = Path(args.config_path).expanduser() if getattr(args, 'config_path', None) else run.config_home / 'putgd/config.json'
    cfg.parent.mkdir(parents=True, exist_ok=True)
    cfg.write_text(json.dumps(doc, indent=1)); run.meta['configPath'] = str(cfg); run.save()
    run.ok('settings.reload')
    snap = run.wait(lambda: (lambda s: s if s.get('ok') and s['result'].get('metadata', {}).get('validation', {}).get('ok', True) else None)(run.cli('settings.get')), 20)
    # The fixture's own synthetic key pair (tests/fixtures/s3_server.py), as afa.py's `start` stores it.
    profile_id = 'nfa-fixture' if getattr(args, 'keep_template', False) else 'afa-fixture'
    stored = run.socket_request('setup.storeCredentials', {'profileId': profile_id, 'expectedRevision': ((snap or {}).get('result') or {}).get('metadata', {}).get('acceptedRevision'),
                                                            'credentials': {'accessKey': afa.ACCESS, 'secretKey': afa.SECRET}})
    job = run.wait(lambda: (lambda j: j if j.get('ok') and j['result']['state'] != 'running' else None)(run.cli('setup.inspect', {'id': stored['result']['setupId']})), 30) if stored.get('ok') else None
    run.meta['credentialsStored'] = {'profile': profile_id, 'ok': stored.get('ok'), 'state': (job or {}).get('result', {}).get('state'), 'error': (stored.get('error') or {}).get('code')}
    run.save()
    if getattr(args, 'stop', False) and resident: afa.quit_resident(run, resident)
    out({'setup': bool(snap), 'validation': (snap or {}).get('result', {}).get('metadata', {}).get('validation'), 'credentials': run.meta['credentialsStored'], 'run': run.meta})


def old_build_document(run, template):
    """A config an older build accepts: its own template, every action to the fixture profile, a customized
    shortcut and notifications, local outputs in the run, credentials in the keychain under a run-specific name."""
    doc = json.loads(json.dumps(template))
    port = run.meta['port']
    ref = f'putgd-nfa-{run.name}'
    profile = {'credentials': {'mode': 'keychain', 'reference': ref}, 'id': 'nfa-fixture', 'label': 'Loopback fixture (nfa)', 'provider': 's3',
               'settings': {'aclMode': 'public-read', 'addressing': 'path', 'bucket': 'fixture', 'connectTimeoutMs': 5000, 'endpoint': f'http://127.0.0.1:{port}',
                            'lowSpeedBytesPerSecond': 1, 'lowSpeedTimeSeconds': 120, 'objectPrefix': 'nfa/', 'publicUrlBase': f'http://127.0.0.1:{port}/fixture',
                            'region': 'us-east-1', 'requestTimeoutMs': 120000, 'storageClass': 'STANDARD'}}
    known = set((template.get('profiles') or [{}])[0].get('settings', {}).keys()) if template.get('profiles') else None
    if known: profile['settings'] = {k: v for k, v in profile['settings'].items() if k in known}
    doc['profiles'] = [profile]
    if run.meta.get('credentialsMode') == 'session':
        # A locked login keyring (an autologin VM) can't take the secret without a password prompt: the uploads use
        # session credentials, and a second profile keeps a keychain reference whose secret is never stored.
        profile['credentials'] = {'mode': 'session', 'reference': None}
        keychain = json.loads(json.dumps(profile)); keychain.update({'id': 'nfa-keychain', 'label': 'Keychain reference (nfa)',
                                                                      'credentials': {'mode': 'keychain', 'reference': ref}})
        doc['profiles'].append(keychain)
    for name, action in (doc.get('actions') or {}).items():
        if isinstance(action, dict) and 'destination' in action: action.update({'destination': 'upload', 'profileId': 'nfa-fixture'})
    if isinstance(doc.get('actions', {}).get('region'), dict): doc['actions']['region']['shortcut'] = 'Ctrl+Shift+9'
    if isinstance(doc.get('archive'), dict): doc['archive'].update({'root': str(run.dir / 'archive'), 'keepUploads': True})
    prefs = doc.setdefault('preferences', {})
    if 'notifications' in prefs: prefs['notifications'] = False
    if 'launchAtLogin' in prefs: prefs['launchAtLogin'] = False
    run.meta['keychainReference'] = ref; run.save()
    return doc


# ---------------------------------------------------------------- M: seed, grow, snapshot, compare

# Data and config roots: GenericDataLocation/putgd and GenericConfigLocation/putgd since #56 (2026-10-18); builds
# before it (dev.133) used putgd/putgd, which the first newer start moves into place.
DATA_ROOTS = [HOME / '.local/share/putgd', HOME / '.local/share/putgd/putgd']


def history_db(run):
    found = [r / 'history.sqlite' for r in DATA_ROOTS if (r / 'history.sqlite').exists()]
    if not found: raise SystemExit(f'no history.sqlite under {[str(r) for r in DATA_ROOTS]}')
    return found[0]


def config_files(run):
    base = run.config_home / 'putgd'
    return {str(base / 'config.json'): file_info(base / 'config.json'), str(base / 'putgd/config.json'): file_info(base / 'putgd/config.json')}


def config_file(run):
    return next((Path(p) for p, info in config_files(run).items() if info), None)


def seed(args):
    """Through the running older build's own CLI: credentials in the keychain, uploads under the lab cover, one failed
    upload (fixture stopped), one deletion from the destination, and local outputs. (dev.133's history.clear clears
    everything, so it isn't used.)"""
    run = NativeRun(args.name)
    rep = {'build': run.ok('ui.snapshot').get('build') if run.cli('ui.snapshot').get('ok') else None}
    rep['credentials'] = dict(run.meta.get('credentialsStored') or {}, mode=run.meta.get('credentialsMode'), reference=run.meta.get('keychainReference'))
    afa.cover(argparse.Namespace(name=args.name, action='start', pattern=False, video_size='640x360', output=None, precheck=True, quiet=True))
    ops = []
    try:
        for i in range(args.uploads):
            r = run.cli('action.invoke', {'action': 'desktop'})
            op = afa.operation_done(run, r['result']['operationId']) if r.get('ok') else None
            ops.append(afa.summarize_op(op) if op else {'invoke': r})
        # A failed upload, kept as a retained operation: the fixture is stopped for one capture.
        os.kill(run.meta['pids']['fixture'], 19)
        r = run.cli('action.invoke', {'action': 'desktop'})
        failed = afa.operation_done(run, r['result']['operationId'], timeout=200) if r.get('ok') else None
        os.kill(run.meta['pids']['fixture'], 18)
        rep['failed'] = afa.summarize_op(failed) if failed else {'invoke': r}
    finally:
        afa.cover(argparse.Namespace(name=args.name, action='stop', quiet=True))
    rep['uploads'] = ops
    records = run.ok('history.list', {'limit': 50, 'offset': 0})['records']
    uploaded = [h for h in records if h.get('uploadStatus') == 'uploaded']
    if uploaded:
        prep = run.ok('delete.prepare', {'ids': [uploaded[0]['id']]})
        conf = run.ok('delete.confirm', {'ids': [uploaded[0]['id']], 'acknowledgement': prep['acknowledgement']})
        rep['deleted'] = run.wait(lambda: (lambda x: x['result'] if x.get('ok') and x['result'].get('state') in ('completed', 'failed', 'cancelled') else None)(
            run.cli('delete.inspect', {'id': conf.get('id') or uploaded[0]['id']})), 90)
        time.sleep(3)
        rep['deletedRow'] = next(({'name': r['name'], 'remoteStatus': r.get('remoteStatus')} for r in run.ok('history.list', {'limit': 50, 'offset': 0})['records']
                                  if r['id'] == uploaded[0]['id']), None)
    rep['history'] = run.ok('history.list', {'limit': 1, 'offset': 0}).get('total')
    rep['outputs'] = sorted(str(p.relative_to(run.dir)) for p in (run.dir / 'archive').rglob('*') if p.is_file())
    out(run.record('seed', rep))


def grow(args):
    """Scale the real history to --to entries: copy existing rows with fresh ids (resident stopped). Each copy is
    the original row with every occurrence of its id replaced; operations and journals are shared, as for
    repeated uploads of one operation. Afterwards the same older build must open it (history.list total)."""
    run = NativeRun(args.name)
    if native_tree(run): raise SystemExit('stop the resident first')
    db = history_db(run)
    con = sqlite3.connect(db); con.row_factory = sqlite3.Row
    version = con.execute('PRAGMA user_version').fetchone()[0]
    rows = [dict(r) for r in con.execute('SELECT * FROM history')]
    states = {r['id']: dict(r) for r in con.execute('SELECT * FROM item_state')} if con.execute("SELECT name FROM sqlite_master WHERE name='item_state'").fetchone() else {}
    if not rows: raise SystemExit('no history rows to copy')
    need = args.to - len(rows)
    with con:
        for i in range(max(need, 0)):
            base = rows[i % len(rows)]; new = str(uuid.uuid4())
            row = {k: (v.replace(base['id'], new) if isinstance(v, str) else v.replace(base['id'].encode(), new.encode()) if isinstance(v, bytes) else v) for k, v in base.items()}
            row['id'] = new
            con.execute(f'INSERT INTO history ({",".join(row)}) VALUES ({",".join("?" * len(row))})', list(row.values()))
            if base['id'] in states:
                s = dict(states[base['id']]); s['id'] = new
                con.execute(f'INSERT INTO item_state ({",".join(s)}) VALUES ({",".join("?" * len(s))})', list(s.values()))
    total = con.execute('SELECT count(*) FROM history').fetchone()[0]; con.close()
    out(run.record('grow', {'db': str(db), 'userVersion': version, 'realRows': len(rows), 'total': total}))


def snapshot(args):
    run = NativeRun(args.name)
    cfg = config_file(run)
    rep = {'label': args.label, 'at': iso(), 'package': installed(), 'config': file_info(cfg) if cfg else None, 'configFiles': config_files(run),
           'dataRoots': {str(r): sorted(p.name for p in r.iterdir()) if r.is_dir() else None for r in DATA_ROOTS}, 'processes': native_tree(run)}
    if cfg: shutil.copy2(cfg, run.dir / f'logs/config-{args.label}.json')
    try:
        db = history_db(run)
        con = sqlite3.connect(f'file:{db}?mode=ro', uri=True)
        tables = [r[0] for r in con.execute("SELECT name FROM sqlite_master WHERE type='table'")]
        rep['history'] = {'db': str(db), 'userVersion': con.execute('PRAGMA user_version').fetchone()[0], 'integrity': con.execute('PRAGMA integrity_check').fetchone()[0],
                          'tables': {t: con.execute(f'SELECT count(*) FROM "{t}"').fetchone()[0] for t in tables}}
        cols = {r[1] for r in con.execute('PRAGMA table_info(history)')}
        for col in ('upload_status', 'local_status', 'remote_status'):
            if col in cols: rep['history'][col] = dict(con.execute(f'SELECT {col}, count(*) FROM history GROUP BY {col}').fetchall())
        if 'item_state' in tables:
            rep['history']['itemStateRemote'] = dict(con.execute('SELECT remote_status, count(*) FROM item_state GROUP BY remote_status').fetchall())
        con.close()
    except SystemExit as e: rep['history'] = {'error': str(e)}
    rep['outputs'] = {str(p.relative_to(run.dir)): sha256(p) for p in sorted((run.dir / 'archive').rglob('*')) if p.is_file()}
    if native_tree(run):
        t0 = time.monotonic(); lst = run.cli('history.list', {'limit': 5, 'offset': 0}, timeout=120)
        rep['historyList'] = {'ok': lst.get('ok'), 'total': (lst.get('result') or {}).get('total'), 'seconds': round(time.monotonic() - t0, 2)}
        rep['build'] = (run.cli('ui.snapshot').get('result') or {}).get('build')
        diag = run.cli('diagnostics').get('result') or {}
        rep['storage'] = {k: v for k, v in diag.items() if re.search(r'storage|location|root', k, re.I)}
    out(run.record('snapshot:' + args.label, rep))


def compare(args):
    run = NativeRun(args.name)
    doc = json.loads((run.dir / 'results.json').read_text())
    a, b = doc['snapshot:' + args.before], doc['snapshot:' + args.after]
    ha, hb = a.get('history', {}), b.get('history', {})
    rep = {'configSame': (a.get('config') or {}).get('sha256') == (b.get('config') or {}).get('sha256'),
           'userVersion': [ha.get('userVersion'), hb.get('userVersion')], 'integrity': hb.get('integrity'),
           'historyRows': [ha.get('tables', {}).get('history'), hb.get('tables', {}).get('history')],
           'statusCounts': {c: [ha.get(c), hb.get(c)] for c in ('upload_status', 'local_status', 'remote_status')},
           'tablesAdded': sorted(set(hb.get('tables', {})) - set(ha.get('tables', {}))),
           'outputsSame': a.get('outputs') == b.get('outputs')}
    ca, cb = run.dir / f'logs/config-{args.before}.json', run.dir / f'logs/config-{args.after}.json'
    if ca.exists() and cb.exists() and not rep['configSame']:
        import difflib
        rep['configDiff'] = list(difflib.unified_diff(ca.read_text().splitlines(), cb.read_text().splitlines(), lineterm='', n=1))[:120]
    rep['rowsPreserved'] = rep['historyRows'][0] == rep['historyRows'][1]
    out(run.record(f'compare:{args.before}->{args.after}', rep))


def history_check(args):
    """After the migration: list, search, copy a link and an image, and delete one entry from the destination."""
    run = NativeRun(args.name)
    rep = {}
    if run.meta.get('credentialsMode') == 'session':  # session credentials end with the resident: enter them again, as a user would
        snap = run.ok('settings.get')
        stored = run.socket_request('setup.storeCredentials', {'profileId': 'nfa-fixture', 'expectedRevision': snap['metadata'].get('acceptedRevision'),
                                                                'credentials': {'accessKey': afa.ACCESS, 'secretKey': afa.SECRET}})
        rep['sessionCredentials'] = stored.get('ok'); time.sleep(2)
    rep['profiles'] = [{'id': x.get('id'), 'credentials': x.get('credentials')} for x in run.ok('settings.get')['document'].get('profiles', [])]
    t0 = time.monotonic(); first = run.ok('history.list', {'limit': 20, 'offset': 0}, timeout=120); rep['listSeconds'] = round(time.monotonic() - t0, 2)
    rep['total'] = first.get('total')
    name = (first['records'][0].get('name') or '')[:6] if first['records'] else ''
    found = run.cli('history.list', {'limit': 20, 'offset': 0, 'search': name}, timeout=120)
    rep['search'] = {'term': name, 'ok': found.get('ok'), 'total': (found.get('result') or {}).get('total')}
    page = run.ok('history.list', {'limit': 50, 'offset': 0, 'uploadStatus': 'uploaded', 'remoteStatus': 'not_requested'}, timeout=120)['records']
    linked = next((r for r in page if r.get('linkAvailable')), None)
    rep['linkedEntry'] = {'name': linked['name'], 'remoteStatus': linked.get('remoteStatus')} if linked else None
    if linked:
        rep['copyLink'] = run.cli('history.copyLink', {'id': linked['id']}).get('ok')
        rep['copyImage'] = run.cli('history.copyImage', {'id': linked['id']}).get('ok')
        prep = run.ok('delete.prepare', {'ids': [linked['id']]})
        conf = run.ok('delete.confirm', {'ids': [linked['id']], 'acknowledgement': prep['acknowledgement']})
        row = lambda: next((r for r in run.ok('history.list', {'limit': 50, 'offset': 0, 'search': linked['name']})['records'] if r['id'] == linked['id']), {})
        done = run.wait(lambda: row() if row().get('remoteStatus') not in ('not_requested', 'deleting', None) else None, 90)
        rep['delete'] = {'confirm': {k: conf.get(k) for k in ('id', 'state')}, 'remoteStatus': (done or row()).get('remoteStatus'),
                         'link': (linked.get('receipt') or {}).get('directUrl')}
        try: urllib.request.urlopen(rep['delete']['link'], timeout=10); rep['delete']['remoteAfter'] = 'still served'
        except urllib.error.HTTPError as e: rep['delete']['remoteAfter'] = f'HTTP {e.code}'
        except Exception as e: rep['delete']['remoteAfter'] = repr(e)[:80]
    pv = run.cli('history.preview', {'id': first['records'][0]['id']}, timeout=60).get('result') if first['records'] else None
    rep['preview'] = {k: (pv or {}).get(k) for k in ('state', 'previewPath', 'mime', 'width', 'height')}
    if run.meta.get('keychainReference'):
        for pid in ('nfa-fixture', 'nfa-keychain'):
            rev = run.ok('settings.get')['metadata'].get('acceptedRevision')
            t = run.cli('setup.testDestination', {'profileId': pid, 'expectedRevision': rev}, timeout=60)
            job = run.wait(lambda: (lambda j: j if j.get('ok') and j['result']['state'] != 'running' else None)(
                run.cli('setup.inspect', {'id': t['result']['setupId']})), 60) if t.get('ok') else None
            rep.setdefault('testConnection', {})[pid] = {'started': t.get('ok'), 'error': (t.get('error') or {}).get('code'),
                                                         'state': (job or {}).get('result', {}).get('state'), 'result': (job or {}).get('result', {}).get('error')}
    out(run.record('historyCheck', rep))


# ---------------------------------------------------------------- I, P, Q: login startup

def login(args):
    run = NativeRun(args.name)
    if run.meta.get('profile') != 'default': raise SystemExit('Launch at login is checked on the default profile')
    if args.action == 'enable':
        if run.meta['before']['autostart']: raise SystemExit(f'{AUTOSTART} existed before this run; refusing')
        snap = run.ok('settings.get')
        r = run.cli('settings.patch', {'patch': {'preferences': {'launchAtLogin': True}}, 'expectedRevision': snap['metadata']['observedRevision']})
        time.sleep(1.5)
        run.meta['autostartWritten'] = AUTOSTART.exists(); run.save()
        out(run.record('loginEnable', {'patch': r.get('ok'), 'entry': AUTOSTART.read_text() if AUTOSTART.exists() else None,
                                       'loginStartup': (run.cli('ui.snapshot').get('result') or {}).get('diagnostics', {}).get('loginStartup')}))
    else:
        env, shell, spid = afa.session_environment()
        procs = put_processes()
        for p in procs: p['started'] = iso(datetime.fromtimestamp(Path(f'/proc/{p["pid"]}').stat().st_mtime, timezone.utc))
        out(run.record('loginCheck:' + args.label, {'entry': AUTOSTART.read_text() if AUTOSTART.exists() else None, 'processes': procs,
                                                    'shellStarted': iso(datetime.fromtimestamp(Path(f'/proc/{spid}').stat().st_mtime, timezone.utc)),
                                                    'fromPackage': any(p['exe'] == '/usr/bin/putgd' for p in procs), 'package': installed()}))


def autostart(args):
    """#382: the retained entry and what the session did with it since this login."""
    run = NativeRun(args.name)
    env, shell, spid = afa.session_environment()
    since = datetime.fromtimestamp(Path(f'/proc/{spid}').stat().st_mtime, timezone.utc) - __import__('datetime').timedelta(minutes=2)
    # This boot's user and system journals, matched on the entry and its program, not the host name.
    pattern = r'autostart/putgd\.desktop|putgd-launcher|/usr/bin/putgd|app-putgd'
    lines = []
    for scope in (['--user'], []):
        journal = sh('journalctl', *scope, '-b', '--since', since.strftime('%Y-%m-%d %H:%M:%S UTC'), '--no-pager', '-o', 'short-iso', timeout=60)[1]
        lines += [('user ' if scope else 'system ') + l for l in journal.splitlines() if re.search(pattern, l) and 'sudo[' not in l]
    rep = {'label': args.label, 'entry': AUTOSTART.read_text() if AUTOSTART.exists() else None, 'entryInfo': file_info(AUTOSTART),
           'execTargetExists': Path('/usr/bin/putgd').exists(), 'shellStarted': iso(since), 'journal': lines[:80],
           'failedUnits': sh('systemctl', '--user', '--failed', '--no-legend', '--plain', env=env)[1].splitlines(),
           'autostartUnits': sh('systemctl', '--user', 'list-units', '--all', '--no-legend', '--plain', 'app-putgd*', env=env)[1].splitlines(),
           'generatorUnit': [str(p) for p in Path(f'/run/user/{os.getuid()}/systemd/generator.late').glob('app-putgd*')] if Path(f'/run/user/{os.getuid()}/systemd/generator.late').exists() else [],
           'processes': put_processes(), 'package': installed()}
    out(run.record('autostart:' + args.label, rep))


def tryexec(args):
    """#382 probe: the run's own retained entry, the same bytes plus TryExec= its Exec program. The original bytes are kept."""
    run = NativeRun(args.name)
    if not AUTOSTART.exists(): raise SystemExit('no retained entry')
    if not run.meta.get('autostartWritten'): raise SystemExit('this run did not write the entry; refusing')
    original = AUTOSTART.read_bytes()
    (run.dir / 'logs/autostart-original.desktop').write_bytes(original)
    text = original.decode()
    # TryExec names the Exec line's own program (#382's proposal), e.g. /usr/bin/putgd-launcher.
    exe = re.search(r'^Exec="?([^"\s]+)', text, re.M).group(1)
    if 'TryExec=' not in text: text = text.replace('\nExec=', f'\nTryExec={exe}\nExec=', 1)
    AUTOSTART.write_text(text)
    out(run.record('tryexec', {'before': original.decode(), 'after': text}))


# ---------------------------------------------------------------- O: GNOME extension

def extension(args):
    run = NativeRun(args.name)
    env, _, _ = afa.session_environment()
    rep = {'label': args.label, 'system': file_info('/usr/share/gnome-shell/extensions/putgd@put.gd/metadata.json'),
           'info': sh('gnome-extensions', 'info', 'putgd@put.gd', env=env)[1].splitlines()[:12] if shutil.which('gnome-extensions') else None,
           'enabled': sh('gsettings', 'get', 'org.gnome.shell', 'enabled-extensions', env=env)[1] if shutil.which('gsettings') else None}
    if native_tree(run):
        d = run.cli('diagnostics')
        caps = (d.get('result') or {}).get('capabilities') or {}
        rep['capabilities'] = {k: v for k, v in caps.items() if re.search(r'capture|gnome|helper|extension|screenshot', k, re.I)} if isinstance(caps, dict) else caps
        def find(v):
            if isinstance(v, dict):
                if isinstance(v.get('helper'), dict) and 'outdated' in v['helper']: return v['helper']
                for x in v.values():
                    if (h := find(x)) is not None: return h
            return None
        rep['helper'] = find(d.get('result') or {})  # GnomeAdapter's helper: extensionVersion, packagedExtensionVersion, outdated
        rep['snapshot'] = {k: v for k, v in ((run.cli('ui.snapshot').get('result') or {}).get('diagnostics') or {}).items() if re.search(r'capture|helper|extension', k, re.I)}
    if args.action in ('enable', 'disable'):
        rep['action'] = {args.action: sh('gnome-extensions', args.action, 'putgd@put.gd', env=env)}
    out(run.record('extension:' + args.label, rep))


# ---------------------------------------------------------------- F: the live prefix, and teardown

def s3list(args):
    if not re.fullmatch(r'acceptance/nfa/[\w.-]+/', args.prefix): raise SystemExit('only acceptance/nfa/<desktop>/ prefixes')
    import boto3, pathlib
    for line in (pathlib.Path.home() / 'Documents/creds.txt').read_text().splitlines():
        p = line.split()
        if len(p) == 2 and ':' not in line and re.fullmatch(r'[A-Z0-9]{20}', p[0]) and len(p[1]) == 40: ak, sk = p; break
    c = boto3.client('s3', region_name='us-east-2', aws_access_key_id=ak, aws_secret_access_key=sk)
    del ak, sk
    v = c.list_object_versions(Bucket='put.gd', Prefix=args.prefix)
    versions = v.get('Versions', []) + v.get('DeleteMarkers', [])
    ups = c.list_multipart_uploads(Bucket='put.gd', Prefix=args.prefix).get('Uploads', [])
    if args.purge:
        for x in versions: c.delete_object(Bucket='put.gd', Key=x['Key'], VersionId=x['VersionId'])
        for u in ups: c.abort_multipart_upload(Bucket='put.gd', Key=u['Key'], UploadId=u['UploadId'])
    out({'at': iso(), 'prefix': 'put.gd/' + args.prefix, 'purged': args.purge, 'objects': c.list_objects_v2(Bucket='put.gd', Prefix=args.prefix).get('KeyCount', 0),
         'versions': len(v.get('Versions', [])), 'deleteMarkers': len(v.get('DeleteMarkers', [])), 'incompleteUploads': len(ups)})


def remove_runtime(run, rep):
    """Only the run's own mktemp folder: /tmp/nfa.XXXXXX, never /run/user/UID."""
    rt = run.meta.get('runtime') or ''
    if re.fullmatch(r'/tmp/nfa\.[A-Za-z0-9]{6}', rt) and Path(rt).is_dir(): shutil.rmtree(rt, ignore_errors=True); rep['removed'].append(rt)
    elif rt: rep.setdefault('kept', []).append(f'{rt} (not this run\'s mktemp folder)')


def teardown(args):
    """What a default-profile run made: its folders (only those absent before), its keychain entry (by its recorded
    name), the autostart entry it wrote, the system files repo-add put in place while no package owns them."""
    run = NativeRun(args.name)
    if native_tree(run): run.cli('ui.quit', timeout=15); run.wait(lambda: not native_tree(run), 40)
    rep = {'removed': [], 'kept': []}
    pid = run.meta['pids'].get('fixture')
    if pid and Path(f'/proc/{pid}').exists(): os.kill(pid, 15)
    if run.meta.get('profile') == 'default':
        for home_path, listed in run.meta['before'].get('homePaths', {}).items():
            path = Path(home_path)
            if not path.exists(): continue
            if listed is None: shutil.rmtree(path); rep['removed'].append(str(path)); continue
            for f in sorted(path.rglob('*'), reverse=True):  # pre-existing folder: only what this run added
                if str(f) in listed: continue
                if f.is_dir() and not any(f.iterdir()): f.rmdir(); rep['removed'].append(str(f))
                elif f.is_file() or f.is_symlink(): f.unlink(); rep['removed'].append(str(f))
    ref = run.meta.get('keychainReference')
    if ref and shutil.which('secret-tool'):
        found = subprocess.run(['secret-tool', 'lookup', 'user', ref, 'server', 'putgd'], capture_output=True, env=run.env()).returncode == 0
        if found: subprocess.run(['secret-tool', 'clear', 'user', ref, 'server', 'putgd'], env=run.env())
        rep['keychain'] = {'reference': ref, 'found': found, 'after': subprocess.run(['secret-tool', 'lookup', 'user', ref, 'server', 'putgd'], capture_output=True, env=run.env()).returncode == 0}
    elif ref: rep['keychain'] = {'reference': ref, 'note': 'secret-tool missing: remove by hand and record'}
    if run.meta.get('autostartWritten') and AUTOSTART.exists(): AUTOSTART.unlink(); rep['removed'].append(str(AUTOSTART))
    owned = set((sh('dpkg', '-L', 'putgd')[1] if DEB else sh('rpm', '-ql', 'putgd')[1]).splitlines()) if installed().get('version') else set()
    for f in run.meta.get('systemFilesAdded', []):
        if Path(f).exists() and f not in owned: subprocess.run(['sudo', '-n', 'rm', '-f', f]); rep['removed'].append(f)
        elif Path(f).exists(): rep['kept'].append(f + ' (owned by the installed package)')
    remove_runtime(run, rep)
    rep['after'] = system_state()
    out(run.record('teardown', rep))


# ---------------------------------------------------------------- H: Settings -> Updates on a repository install

UPDATERS = ('mintupdate', 'plasma-discover', 'gnome-software', 'update-manager', 'snap-store')


def updaters():
    found = []
    for proc in Path('/proc').iterdir():
        if not proc.name.isdigit(): continue
        try:
            if proc.stat().st_uid != os.getuid(): continue
            argv = (proc / 'cmdline').read_bytes().split(b'\0')
            if any(u.encode() in a for a in argv[:2] for u in UPDATERS):
                found.append({'pid': int(proc.name), 'argv': [a.decode(errors='replace') for a in argv if a][:6]})
        except OSError: continue
    return found


def updates(args):
    """Settings -> Updates: the channel the repository file sets (read-only), the Other channel commands, Check now,
    and with --open what Open software updates starts (update.openSoftwareUpdates, the button's own method)."""
    run = NativeRun(args.name)
    run.ok('ui.show', {'view': 'updates'}); time.sleep(1.5); run.ok('ui.show', {'view': 'updates'}); time.sleep(1)
    check = run.cli('update.check', timeout=90)
    st = run.ok('update.status')
    action = st.get('action') or {}
    rep = {'build': st.get('build'), 'channel': st.get('channel'), 'channelSource': st.get('channelSource'), 'channelSetting': st.get('channelSetting'),
           'result': st.get('result'), 'notifies': st.get('notifies'), 'install': {k: (st.get('install') or {}).get(k) for k in ('kind', 'available', 'reason')},
           'action': action, 'check': {'ok': check.get('ok'), 'error': check.get('error')},
           'repositoryFile': {f: file_info(f) for f in ('/etc/apt/sources.list.d/putgd.sources', '/etc/yum.repos.d/putgd.repo')},
           'notifications': [l.get('fields') for l in run.logs('Notification shown')][-3:]}
    frames, inp, size = afa.tools(run); time.sleep(1.5)
    rep['frame'] = afa.fullshot(run, frames.grab(), args.label)
    if args.open:
        before = {u['pid'] for u in updaters()}
        reply = run.cli('update.openSoftwareUpdates', timeout=60)
        started = run.wait(lambda: [u for u in updaters() if u['pid'] not in before], 30)
        time.sleep(args.settle)
        rep['openSoftwareUpdates'] = {'reply': reply, 'started': started, 'frame': afa.fullshot(run, frames.grab(), args.label + '-updater')}
    out(run.record('updates:' + args.label, rep))


def launch_cmd(args):
    """Start the run's program again (as a menu start would), after an upgrade or a grow; or quit it."""
    run = NativeRun(args.name)
    if args.cmd == 'quit':
        pids = [x['pid'] for x in native_tree(run) if x['comm'] == 'putgd']
        r = run.cli('ui.quit', timeout=15)
        gone = run.wait(lambda: not native_tree(run), 40)
        out(run.record('quit:' + args.label, {'pids': pids, 'reply': r.get('ok'), 'gone': bool(gone), 'left': native_tree(run)})); return
    if args.exe: run.meta['exe'] = args.exe
    if args.cli: run.meta['cli'] = args.cli
    run.save()
    t0 = time.monotonic(); native_launch(run, args.label)
    out(run.record('launch:' + args.label, {'seconds': round(time.monotonic() - t0, 2), 'processes': native_tree(run),
                                            'build': (run.cli('ui.snapshot').get('result') or {}).get('build'), 'package': installed()}))


# ---------------------------------------------------------------- real input and frames

def keys(args):
    """Real key input (xdotool, the uinput agent or the VM's VNC), guarded like every click. `type` sends text."""
    run = NativeRun(args.name)
    safe_defaults(run.meta['session'])
    frames, inp, size = afa.tools(run)
    if args.cmd == 'type': inp.type(args.text)
    else:
        for chord in args.chords: inp.key(*chord.split('+')); time.sleep(args.gap)
    time.sleep(args.settle)
    out(run.record(f'{args.cmd}:{args.label or ""}', {'chords': getattr(args, 'chords', None), 'text': getattr(args, 'text', None),
                                                      'frame': afa.fullshot(run, frames.grab(), args.label) if args.label else None}))


def frame(args):
    """A full-desktop frame of the run's display (only with `allow-full on`), plus put.gd's processes."""
    run = NativeRun(args.name)
    frames, inp, size = afa.tools(run)
    if args.park: afa.park(run, inp, size)
    time.sleep(args.wait)
    out(run.record('frame:' + args.label, {'at': iso(), 'full': afa.fullshot(run, frames.grab(), args.label), 'processes': native_tree(run)}))


# ---------------------------------------------------------------- main

OWN = {'state', 'fetch', 'repo-add', 'pkg', 'identity', 'setup', 'configure', 'seed', 'grow', 'snapshot', 'compare', 'history-check', 'login', 'autostart',
       'tryexec', 'extension', 's3list', 'teardown', 'key', 'type', 'frame', 'updates', 'launch', 'quit'}


def main():
    patch_afa()
    if len(sys.argv) > 1 and sys.argv[1] in ('rerun', 'i326'):
        # Imported after the patch, so their `from afa import Run` is NativeRun.
        module = __import__(sys.argv[1])
        sys.argv = [sys.argv[0]] + sys.argv[2:]
        return module.main()
    if len(sys.argv) > 1 and sys.argv[1] == 'cleanup' and '--appimage' in sys.argv:
        raise SystemExit('cleanup --appimage would delete /usr/bin/putgd; refused')
    if len(sys.argv) > 2 and sys.argv[1] == 'cleanup':
        meta = NativeRun(sys.argv[2]).meta
        if meta and not re.fullmatch(r'/tmp/nfa\.[A-Za-z0-9]{6}', meta.get('runtime') or ''):
            raise SystemExit(f"cleanup refused: runtime {meta.get('runtime')!r} is not this run's /tmp/nfa.* folder")
    if len(sys.argv) < 2 or sys.argv[1] in ('-h', '--help'):
        print(__doc__); print('Every other subcommand is afa.py\'s (nfa.py COMMAND -h).'); return
    if sys.argv[1] not in OWN:
        return afa.main()
    p = argparse.ArgumentParser(); s = p.add_subparsers(dest='cmd', required=True)
    a = s.add_parser('state'); a.add_argument('--save')
    a = s.add_parser('fetch'); a.add_argument('tag'); a.add_argument('target', choices=('ubuntu24.04', 'ubuntu26.04', 'debian13', 'fedora44', 'appimage')); a.add_argument('dir')
    a = s.add_parser('repo-add'); a.add_argument('package'); a.add_argument('name')
    a = s.add_parser('pkg'); a.add_argument('name'); a.add_argument('label'); a.add_argument('--allow-other', action='store_true'); a.add_argument('command', nargs=argparse.REMAINDER)
    a = s.add_parser('identity'); a.add_argument('name'); a.add_argument('tag'); a.add_argument('--target', required=True)
    a = s.add_parser('setup'); a.add_argument('name'); a.add_argument('--vnc-port', type=int, required=True)
    a.add_argument('--input', choices=('uinput', 'xdotool', 'vnc'), required=True); a.add_argument('--profile', choices=('private', 'default'), default='private')
    a.add_argument('--exe', default='/usr/bin/putgd'); a.add_argument('--cli', default='/usr/bin/putgd-cli')
    a.add_argument('--keep-template', action='store_true', help='an older build: its own template, fixture route only'); a.add_argument('--stop', action='store_true')
    a.add_argument('--config-path', help='builds before #56 (dev.133): ~/.config/putgd/putgd/config.json')
    a.add_argument('--no-launch', action='store_true', help='item L: record the run and start the fixture only')
    a.add_argument('--credentials', choices=('keychain', 'session'), default='keychain', help='--keep-template: where the fixture secret goes')
    a.add_argument('--appimage', help='an older AppImage as the program (its extracted putgd-cli as the CLI)')
    a = s.add_parser('configure'); a.add_argument('name'); a.add_argument('--keep-template', action='store_true'); a.add_argument('--config-path')
    a = s.add_parser('seed'); a.add_argument('name'); a.add_argument('--uploads', type=int, default=4)
    a = s.add_parser('grow'); a.add_argument('name'); a.add_argument('--to', type=int, default=10000)
    a = s.add_parser('snapshot'); a.add_argument('name'); a.add_argument('--label', required=True)
    a = s.add_parser('compare'); a.add_argument('name'); a.add_argument('before'); a.add_argument('after')
    a = s.add_parser('history-check'); a.add_argument('name')
    a = s.add_parser('login'); a.add_argument('name'); a.add_argument('action', choices=('enable', 'check')); a.add_argument('--label', default='login')
    a = s.add_parser('autostart'); a.add_argument('name'); a.add_argument('--label', required=True)
    a = s.add_parser('tryexec'); a.add_argument('name')
    a = s.add_parser('extension'); a.add_argument('name'); a.add_argument('--label', required=True); a.add_argument('--action', choices=('enable', 'disable'))
    a = s.add_parser('s3list'); a.add_argument('prefix'); a.add_argument('--purge', action='store_true')
    a = s.add_parser('teardown'); a.add_argument('name')
    for n in ('launch', 'quit'):
        a = s.add_parser(n); a.add_argument('name'); a.add_argument('--label', default=n); a.add_argument('--exe'); a.add_argument('--cli')
    a = s.add_parser('updates'); a.add_argument('name'); a.add_argument('--label', required=True); a.add_argument('--open', action='store_true'); a.add_argument('--settle', type=float, default=8)
    a = s.add_parser('key'); a.add_argument('name'); a.add_argument('chords', nargs='+', help='ctrl+alt+t super Return ...'); a.add_argument('--label')
    a.add_argument('--gap', type=float, default=.4); a.add_argument('--settle', type=float, default=1)
    a = s.add_parser('type'); a.add_argument('name'); a.add_argument('text'); a.add_argument('--label'); a.add_argument('--settle', type=float, default=1)
    a = s.add_parser('frame'); a.add_argument('name'); a.add_argument('--label', required=True); a.add_argument('--wait', type=float, default=0); a.add_argument('--park', action='store_true')
    args = p.parse_args()
    {'state': state, 'fetch': fetch, 'repo-add': repo_add, 'pkg': pkg, 'identity': identity, 'setup': setup, 'configure': configure, 'seed': seed, 'grow': grow,
     'snapshot': snapshot, 'compare': compare, 'history-check': history_check, 'login': login, 'autostart': autostart, 'tryexec': tryexec,
     'extension': extension, 's3list': s3list, 'teardown': teardown, 'key': keys, 'type': keys, 'frame': frame, 'updates': updates, 'launch': launch_cmd, 'quit': launch_cmd}[args.cmd](args)


if __name__ == '__main__':
    main()
