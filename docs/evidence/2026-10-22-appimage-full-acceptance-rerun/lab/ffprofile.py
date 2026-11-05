#!/usr/bin/env python3
"""ffprofile.py on|off ROOT STATE: a temporary Firefox profile as the default for item F, without touching the user's profiles.

on:  backs up ROOT/profiles.ini and ROOT/installs.ini byte for byte into STATE (a JSON file), creates
     ROOT/putgd-afa2-<hex> with a user.js that only switches off the default-browser check and session
     restore, and makes it the Default of every [Install...] section, so the desktop's own Firefox opens it.
off: restores both files byte for byte from STATE and removes only the profile directory STATE recorded.
Nothing of the user's own profiles is read beyond profiles.ini/installs.ini.
"""
import configparser, io, json, os, secrets, shutil, sys
from pathlib import Path

USER_JS = '''// put.gd acceptance (item F): a throwaway profile.
user_pref("browser.shell.checkDefaultBrowser", false);
user_pref("browser.sessionstore.resume_from_crash", false);
user_pref("browser.startup.page", 0);
user_pref("browser.tabs.warnOnClose", false);
'''


def ini(text):
    c = configparser.RawConfigParser(); c.optionxform = str; c.read_string(text); return c


def dump(c):
    s = io.StringIO(); c.write(s, space_around_delimiters=False); return s.getvalue()


def on(root, state):
    if state.exists(): raise SystemExit(f'{state} exists')
    files = {n: (root / n).read_bytes().hex() if (root / n).exists() else None for n in ('profiles.ini', 'installs.ini')}
    name = 'putgd-afa2-' + secrets.token_hex(4)
    prof = root / name
    state.write_text(json.dumps({'root': str(root), 'profile': str(prof), 'files': files}))
    prof.mkdir(mode=0o700)
    (prof / 'user.js').write_text(USER_JS)
    p = ini(bytes.fromhex(files['profiles.ini']).decode())
    n = 0
    while p.has_section(f'Profile{n}'): n += 1
    p.add_section(f'Profile{n}')
    for k, v in (('Name', name), ('IsRelative', '1'), ('Path', name)): p.set(f'Profile{n}', k, v)
    installs = [s for s in p.sections() if s.startswith('Install')]
    for s in installs: p.set(s, 'Default', name); p.set(s, 'Locked', '1')
    (root / 'profiles.ini').write_text(dump(p))
    if files['installs.ini'] is not None:
        i = ini(bytes.fromhex(files['installs.ini']).decode())
        for s in i.sections(): i.set(s, 'Default', name); i.set(s, 'Locked', '1')
        (root / 'installs.ini').write_text(dump(i))
    print(json.dumps({'profile': str(prof), 'installSections': installs}))


def off(root, state):
    st = json.loads(state.read_text())
    for n, data in st['files'].items():
        f = root / n
        if data is None:
            if f.exists(): f.unlink()
        else: f.write_bytes(bytes.fromhex(data))
    prof = Path(st['profile'])
    if prof.parent == root and prof.name.startswith('putgd-afa2-') and prof.is_dir(): shutil.rmtree(prof)
    same = all(((root / n).read_bytes().hex() if (root / n).exists() else None) == d for n, d in st['files'].items())
    state.unlink()
    print(json.dumps({'restored': same, 'profileRemoved': not prof.exists(), 'profile': str(prof)}))


if __name__ == '__main__':
    action, root, state = sys.argv[1], Path(sys.argv[2]).resolve(), Path(sys.argv[3]).resolve()
    {'on': on, 'off': off}[action](root, state)
