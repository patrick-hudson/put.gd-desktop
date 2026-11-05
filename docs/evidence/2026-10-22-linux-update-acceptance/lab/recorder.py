#!/usr/bin/env python3
"""recorder.py on|off|status STATE: #326's stand-in URL recorder as the session's real http(s) default, standalone.

i326.py handler-on does the same inside an afa.py run; the Linux update profile has no afa run, so this is the
same thing on its own. The entry is named putgd326-url.desktop, the name ua_app.safe_handler (and afa.py's #415
guard) accept. A link opened while it is on is appended to OPENED instead of reaching any browser.
on:  backs up ~/.config/mimeapps.list and ~/.config/kde-mimeapps.list byte for byte into STATE (JSON), writes
     the entry and its script, and makes it the http and https default with xdg-mime. Prints what gio resolves.
off: restores both files byte for byte and removes the entry and its script. Prints gio again.
"""
import json, os, subprocess, sys
from pathlib import Path

HOME = Path.home()
FILES = [HOME / '.config/mimeapps.list', HOME / '.config/kde-mimeapps.list']
ENTRY = HOME / '.local/share/applications/putgd326-url.desktop'
ROOT = HOME / 'work/put.gd-ua'
SCRIPT, OPENED = ROOT / 'record-url.sh', ROOT / 'opened-urls.txt'


def gio():
    found = {}
    for scheme in ('http', 'https'):
        r = subprocess.run(['gio', 'mime', f'x-scheme-handler/{scheme}'], capture_output=True, text=True)
        found[scheme] = ((r.stdout or r.stderr).splitlines() or [''])[0]
    return found


def on(state):
    if state.exists() or ENTRY.exists(): raise SystemExit('already on')
    backup = {str(f): f.read_bytes().hex() if f.exists() else None for f in FILES}
    state.write_text(json.dumps({'files': backup, 'gioBefore': gio()}))
    SCRIPT.write_text(f'#!/bin/sh\nprintf "%s %s\\n" "$(date -u +%FT%TZ)" "$1" >> {OPENED}\n'); SCRIPT.chmod(0o700)
    ENTRY.parent.mkdir(parents=True, exist_ok=True)
    ENTRY.write_text(f'[Desktop Entry]\nType=Application\nName=put.gd acceptance URL recorder\nExec={SCRIPT} %u\n'
                     'MimeType=x-scheme-handler/https;x-scheme-handler/http;\nNoDisplay=true\n')
    subprocess.run(['xdg-mime', 'default', ENTRY.name, 'x-scheme-handler/https', 'x-scheme-handler/http'], check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    print(json.dumps({'entry': str(ENTRY), 'gioBefore': json.loads(state.read_text())['gioBefore'], 'gioNow': gio()}))


def off(state):
    st = json.loads(state.read_text())
    for f, data in st['files'].items():
        p = Path(f)
        if data is None:
            if p.exists(): p.unlink()
        else: p.write_bytes(bytes.fromhex(data))
    ENTRY.unlink(missing_ok=True); SCRIPT.unlink(missing_ok=True)
    same = all(((Path(f).read_bytes().hex() if Path(f).exists() else None) == d) for f, d in st['files'].items())
    state.unlink()
    print(json.dumps({'restored': same, 'entryRemoved': not ENTRY.exists(), 'gioBefore': st['gioBefore'], 'gioNow': gio(),
                      'opened': OPENED.read_text().splitlines() if OPENED.exists() else []}))


if __name__ == '__main__':
    action, state = sys.argv[1], Path(sys.argv[2]).resolve()
    if action == 'status': print(json.dumps({'on': state.exists(), 'gio': gio()}))
    else: {'on': on, 'off': off}[action](state)
