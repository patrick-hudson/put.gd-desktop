#!/usr/bin/env python3
"""ffdefault_flatpak.py on|off FFROOT STATE: the Firefox Flatpak with a temporary profile as the http(s) handler, for item F only.
FFROOT must be inside the sandbox's own data (~/.var/app/org.mozilla.firefox/...), which the Flatpak can always open.

For a desktop whose default http(s) handler is not a browser (d69 and k72: the ChatGPT app).
on:  backs up ~/.config/mimeapps.list and ~/.config/kde-mimeapps.list byte for byte into STATE, creates
     FFROOT/putgd-qt418-<hex> (a user.js that only switches off the default-browser check and session restore),
     writes ~/.local/share/applications/putgd-qt418-firefox.desktop (Exec=firefox --profile <that dir> %u) and
     makes it the default for x-scheme-handler/http and https with xdg-mime. Prints what gio now resolves.
off: restores both files byte for byte, removes the desktop entry and the profile directory, prints gio again.
"""
import json, os, secrets, shutil, subprocess, sys
from pathlib import Path

HOME = Path.home()
FILES = [HOME / '.config/mimeapps.list', HOME / '.config/kde-mimeapps.list']
ENTRY = HOME / '.local/share/applications/putgd-qt418-firefox.desktop'
USER_JS = '''// put.gd acceptance (item F): a throwaway profile.
user_pref("browser.shell.checkDefaultBrowser", false);
user_pref("browser.sessionstore.resume_from_crash", false);
user_pref("browser.startup.page", 0);
'''


def gio():
    r = subprocess.run(['gio', 'mime', 'x-scheme-handler/https'], capture_output=True, text=True)
    return (r.stdout or r.stderr).splitlines()[0] if (r.stdout or r.stderr) else ''


def on(root, state):
    if state.exists() or ENTRY.exists(): raise SystemExit('already on')
    backup = {str(f): f.read_bytes().hex() if f.exists() else None for f in FILES}
    prof = root / ('putgd-qt418-' + secrets.token_hex(4))
    state.write_text(json.dumps({'files': backup, 'profile': str(prof), 'entry': str(ENTRY), 'giobefore': gio()}))
    prof.mkdir(mode=0o700); (prof / 'user.js').write_text(USER_JS)
    ENTRY.parent.mkdir(parents=True, exist_ok=True)
    ENTRY.write_text('[Desktop Entry]\nType=Application\nName=Firefox Flatpak (put.gd acceptance, temporary profile)\n'
                     f'Exec=flatpak run org.mozilla.firefox --profile {prof} %u\nMimeType=x-scheme-handler/http;x-scheme-handler/https;\nNoDisplay=true\n')
    subprocess.run(['xdg-mime', 'default', ENTRY.name, 'x-scheme-handler/https', 'x-scheme-handler/http'], check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    print(json.dumps({'profile': str(prof), 'entry': str(ENTRY), 'gioBefore': json.loads(state.read_text())['giobefore'], 'gioNow': gio()}))


def off(state):
    st = json.loads(state.read_text())
    for f, data in st['files'].items():
        p = Path(f)
        if data is None:
            if p.exists(): p.unlink()
        else: p.write_bytes(bytes.fromhex(data))
    Path(st['entry']).unlink(missing_ok=True)
    prof = Path(st['profile'])
    if prof.name.startswith('putgd-qt418-') and prof.is_dir(): shutil.rmtree(prof)
    same = all(((Path(f).read_bytes().hex() if Path(f).exists() else None) == d) for f, d in st['files'].items())
    state.unlink()
    print(json.dumps({'restored': same, 'entryRemoved': not Path(st['entry']).exists(), 'profileRemoved': not prof.exists(),
                      'gioBefore': st['giobefore'], 'gioNow': gio()}))


if __name__ == '__main__':
    if sys.argv[1] == 'on': on(Path(sys.argv[2]).resolve(), Path(sys.argv[3]).resolve())
    else: off(Path(sys.argv[2]).resolve())
