#!/usr/bin/env bash
# f44 host: the production path of build C (local tag v0.1.1-nightly.784, never pushed).
set -uo pipefail
L=/home/patrick/work/put.gd-zsync
cd "$L/out/C"
C=putgd-0.1.1-nightly.784-x86_64.AppImage
show() { printf '\n$ %s\n' "$*"; "$@"; echo "[exit $?]"; }
printf '# %s, %s\n' "$(. /etc/os-release; echo "$PRETTY_NAME")" "$(date -u +%FT%TZ)"
show git -C "$L/tree" describe --tags --exact-match
show git -C "$L/tree" status --short --untracked-files=normal
show ./$C --appimage-updateinformation
printf '\n$ head -8 %s.zsync\n' "$C"; head -8 "$C.zsync"
show env SSL_CERT_FILE=/etc/pki/tls/certs/ca-bundle.crt "$L/update-lab/tool/squashfs-root/AppRun" --describe "$PWD/$C"
printf '\n$ jq .update appimage-build.json\n'; python3 -c 'import json; print(json.dumps(json.load(open("appimage-build.json"))["update"], indent=2))'
printf '\n$ collect.py checks: verified_zsync(receipt, image, tag v0.1.1-nightly.784)\n'
cd "$L/contracts-tree" && git fetch -q origin claude/appimage-zsync-update-info && git checkout -q --detach FETCH_HEAD
python3 - "$L/out/C" <<'PY'
import json, sys
from pathlib import Path
sys.path.insert(0, 'tools/releases')
import collect
directory = Path(sys.argv[1])
receipt = json.loads((directory / 'appimage-build.json').read_text())
image = directory / receipt['artifact']['file']
release = {'tag': 'v0.1.1-nightly.784'}
path = collect.verified_zsync(directory, receipt, image, image.name, release, 'patrick-hudson/put.gd-desktop')
print('accepted', path.name)
try:
    collect.verified_zsync(directory, receipt, image, image.name, {'tag': 'v0.1.1-rc.1'}, 'patrick-hudson/put.gd-desktop')
except ValueError as error:
    print('rejected as v0.1.1-rc.1:', error)
lab = Path(sys.argv[1]).parent / 'B'
lab_receipt = json.loads((lab / 'appimage-build.json').read_text())
try:
    collect.verified_zsync(lab, lab_receipt, lab / lab_receipt['artifact']['file'], lab_receipt['artifact']['file'], release, 'patrick-hudson/put.gd-desktop')
except ValueError as error:
    print('rejected lab build B:', error)
PY
