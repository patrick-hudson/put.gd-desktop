#!/bin/sh
# #523 host driver (runs on a KDE builder, in ~/putgd-lab/i523).
#   drive.sh start IMAGE_FILE MODE RUN   MODE: lab (private XDG_DATA_HOME, as lab.py) | real (~/.local/share)
#   drive.sh info RUN
#   drive.sh place RUN
#   drive.sh stop RUN
set -eu
base=$HOME/putgd-lab/i523
cmd=$1; shift
session_env() {
  pid=$(pgrep -u "$(id -u)" -x plasmashell | head -n 1)
  [ -n "$pid" ]
  for var in DISPLAY XAUTHORITY DBUS_SESSION_BUS_ADDRESS XDG_SESSION_TYPE XDG_CURRENT_DESKTOP WAYLAND_DISPLAY XDG_CONFIG_DIRS XDG_DATA_DIRS PATH XDG_SESSION_DESKTOP DESKTOP_SESSION KDE_FULL_SESSION; do
    value=$(tr '\0' '\n' < /proc/$pid/environ | sed -n "s/^$var=//p")
    [ -z "$value" ] || export "$var=$value"
  done
  export XDG_RUNTIME_DIR=/run/user/$(id -u)
}
run_env() {
  run=$base/runs/$1
  export XDG_CONFIG_HOME=$run/config XDG_CACHE_HOME=$run/cache XDG_STATE_HOME=$run/state
  if [ "$(cat "$run/mode")" = lab ]; then export XDG_DATA_HOME=$run/data; else export XDG_DATA_HOME=$HOME/.local/share; fi
}
cli() {
  root=$base/x-$(cat "$run/tag")/squashfs-root
  LD_LIBRARY_PATH="$root/usr/lib:$root/usr/lib/putgd" QT_PLUGIN_PATH="$root/usr/plugins:$root/usr/lib/qt6/plugins" \
    "$root/usr/bin/putgd-cli" "$@"
}
kwin_js() {
  # $1 marker, $2 script body; print() goes to the user journal as "js: ..."
  file=$run/$1.js
  printf '%s\n' "$2" > "$file"
  id=$(qdbus6 org.kde.KWin /Scripting org.kde.kwin.Scripting.loadScript "$file" "i523-$1")
  qdbus6 org.kde.KWin "/Scripting/Script$id" org.kde.kwin.Script.run >/dev/null
  sleep 1
  qdbus6 org.kde.KWin /Scripting org.kde.kwin.Scripting.unloadScript "i523-$1" >/dev/null
}
entries() {
  for d in "$HOME/.local/share/applications" "$run/data/applications"; do
    [ -d "$d" ] || continue
    for f in "$d"/putgd*.desktop; do
      [ -f "$f" ] || continue
      printf '%s\t%s\n' "$f" "$(sed -n 's/^\(Icon\|StartupWMClass\|X-KDE-DBUS-Restricted-Interfaces\)=//p' "$f" | tr '\n' ' ')"
    done
  done
}
case $cmd in
start)
  image=$1 mode=$2 name=$3
  case $image in *1058*) tag=1058 ;; *1101*) tag=q612 ;; *) tag=other ;; esac
  run=$base/runs/$name
  test ! -e "$run"
  mkdir -p "$run/config" "$run/cache" "$run/state" "$run/data"
  echo "$mode" > "$run/mode"; echo "$tag" > "$run/tag"; echo "$image" > "$run/image"
  if [ ! -d "$base/x-$tag/squashfs-root" ]; then
    mkdir -p "$base/x-$tag"; (cd "$base/x-$tag" && "$base/$image" --appimage-extract >/dev/null)
  fi
  [ -f "$base/edit-sample.png" ] || python3 - "$base/edit-sample.png" <<'PY'
import struct, sys, zlib
w, h = 640, 400
rows = b''.join(b'\0' + bytes(c for x in range(w) for c in ((x * 255) // w, (y * 255) // h, 160)) for y in range(h))
chunk = lambda t, d: struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
open(sys.argv[1], 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(rows)) + chunk(b'IEND', b''))
PY
  session_env; run_env "$name"
  [ ! -e "$HOME/.local/share/putgd" ] || [ "$mode" = lab ] || { echo "~/.local/share/putgd exists already" >&2; exit 3; }
  (cd "$run" && setsid nohup "$base/$image" --background > "$run/app.log" 2>&1 < /dev/null & echo $! > "$run/apprun.pid")
  i=0; until cli capabilities >/dev/null 2>&1; do i=$((i+1)); [ $i -lt 60 ] || { echo "no resident" >&2; exit 4; }; sleep 1; done
  # Local copies only, into this run's own folder: no destination, nothing in ~/Documents.
  mkdir -p "$run/archive"
  patch=$(cli settings.get '{}' | python3 -c '
import json, sys
r = json.load(sys.stdin)["result"]
archive = dict(r["document"]["archive"], keepUploads=True, root=sys.argv[1])
print(json.dumps({"patch": {"archive": archive}, "expectedRevision": r["metadata"]["acceptedRevision"]}))' "$run/archive")
  cli settings.patch "$patch" | head -c 200; echo
  cli ui.show '{"view":"settings"}'; echo
  sleep 2
  cli editor.open "{\"source\":\"file\",\"path\":\"$base/edit-sample.png\"}"; echo
  sleep 4
  echo "apprun $(cat "$run/apprun.pid")"
  entries
  ;;
info)
  run=$base/runs/$1; session_env; run_env "$1"
  since=$(date '+%Y-%m-%d %H:%M:%S')
  kwin_js info 'for (const w of workspace.windowList()) { if (String(w.resourceClass).indexOf("putgd") === 0 || String(w.desktopFileName).indexOf("putgd") === 0) print("I523 " + JSON.stringify({caption: w.caption, resourceClass: w.resourceClass, resourceName: w.resourceName, desktopFileName: w.desktopFileName, pid: w.pid, geometry: [w.frameGeometry.x, w.frameGeometry.y, w.frameGeometry.width, w.frameGeometry.height]})); }'
  journalctl --user --since "$since" --no-pager -o cat | sed -n 's/^\(js: \)\{0,1\}I523 //p'
  entries
  for p in $(pgrep -u "$(id -u)" -x putgd-ui) $(pgrep -u "$(id -u)" -x putgd); do printf 'proc %s %s\n' "$p" "$(readlink /proc/$p/exe)"; done
  ;;
place)
  run=$base/runs/$1; session_env; run_env "$1"
  # PLACE="DX DY DW DH EX EY EW EH" (dashboard, editor); default for 1920x1080
  set -- ${PLACE:-40 40 1000 900 1000 330 880 640}
  kwin_js place "$(printf '%s' 'let n = 0; for (const w of workspace.windowList()) { if (String(w.resourceClass).indexOf("putgd") !== 0) continue; const g = w.frameGeometry; if (w.caption.indexOf("Edit") >= 0 || w.caption.indexOf("edit") >= 0) { w.frameGeometry = {x: EX, y: EY, width: Math.min(g.width, EW), height: Math.min(g.height, EH)}; } else { w.frameGeometry = {x: DX, y: DY, width: Math.min(g.width, DW), height: Math.min(g.height, DH)}; } n++; } print("I523 placed " + n);' | sed "s/DX/$1/; s/DY/$2/; s/DW/$3/; s/DH/$4/; s/EX/$5/; s/EY/$6/; s/EW/$7/; s/EH/$8/")"
  ;;
stop)
  run=$base/runs/$1; session_env; run_env "$1"
  cli ui.quit '{}' || true; echo
  pid=$(cat "$run/apprun.pid"); i=0
  while kill -0 "$pid" 2>/dev/null && [ $i -lt 20 ]; do sleep 1; i=$((i+1)); done
  if kill -0 "$pid" 2>/dev/null; then echo "apprun $pid still running; TERM"; kill -TERM "$pid"; sleep 3; fi
  echo "left:"; entries
  pgrep -u "$(id -u)" -af 'putgd' | grep -v -e pgrep -e 'putgd-signer' -e 'In-memory builder' || true
  ;;
esac
