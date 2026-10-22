#!/bin/bash
# snap.sh LABEL: what Gear Lever, AppImageLauncher and put.gd have on disk and running (#412 lab).
cd ~/i412 && . ./genv.sh
out=~/i412/snap-$1.txt
{
echo "## $1 $(date -u +%FT%TZ)"
for d in ~/AppImages ~/Applications; do [ -d "$d" ] && { echo "### ls -la $d"; ls -la --time-style=full-iso "$d"; echo "### sha256 / inode"; find "$d" -maxdepth 1 -type f -exec sh -c 'printf "%s %s\n" "$(stat -c "%i %s" "$1")" "$(sha256sum "$1")"' _ {} \; ; }; done
echo "### desktop entries naming put.gd"
grep -l -i "putgd\|put.gd" ~/.local/share/applications/*.desktop 2>/dev/null | while read -r f; do echo "#### $f"; cat "$f"; done
echo "### Gear Lever --list-installed"
flatpak run it.mijorus.gearlever --list-installed 2>&1 | grep -v MESA
echo "### Gear Lever config"
find ~/.var/app/it.mijorus.gearlever -type f \( -name "*.json" -o -name "*.ini" -o -name "*.conf" \) 2>/dev/null | while read -r f; do echo "#### $f"; cat "$f"; echo; done
gsettings --schemadir ~/.local/share/flatpak/app/it.mijorus.gearlever/current/active/files/share/glib-2.0/schemas list-recursively it.mijorus.gearlever 2>/dev/null | grep -i -E "update|url|apps" | head -20
echo "### AppImageLauncher"
dpkg -l appimagelauncher 2>/dev/null | tail -1; pgrep -af appimagelauncherd; ls -la ~/.config/appimagelauncher.cfg 2>/dev/null && cat ~/.config/appimagelauncher.cfg
echo "### put.gd processes"
for p in $(pgrep -u "$(id -u)" -x putgd); do echo "pid $p exe=$(readlink /proc/$p/exe) APPIMAGE=$(tr '\0' '\n' < /proc/$p/environ | grep ^APPIMAGE= | cut -d= -f2-)"; done
echo "### update.status"
cli() { LD_LIBRARY_PATH=~/i412/cli968/squashfs-root/usr/lib:~/i412/cli968/squashfs-root/usr/lib/putgd ~/i412/cli968/squashfs-root/usr/bin/putgd-cli "$@"; }
cli update.status 2>&1 | python3 -c 'import json,sys
try: d=json.load(sys.stdin)
except Exception as e: print("no resident:", e); sys.exit()
r=d.get("result",d); r=r.get("result",r) if "install" not in r else r; i=r.get("install",{})
print(json.dumps({"build":r.get("build"),"result":r.get("result"),"install":{k:i.get(k) for k in ("state","image","restartRequired","backup","installed","error","available")}},indent=1))'
echo "### update-install.json"; cat ~/.local/state/putgd/update-install.json 2>/dev/null; echo
echo "### hidden update files"; ls -la ~/AppImages/.putgd-update-* ~/Applications/.putgd-update-* 2>/dev/null
} > "$out" 2>&1
cat "$out"
