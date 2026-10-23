#!/bin/bash
# u9_guest.sh gl|gl-own|ail-install|ail: U9 inside a VM guest (#412's method), N -> N+1 on the live feed.
#   gl       Gear Lever integrates N; put.gd started from Gear Lever's menu entry runs Download and install (update.install,
#            the MCP path) and update.restart; then quit and start again from the same entry; Gear Lever's record and entry
#            are unchanged, --list-updates reports none, and no .putgd-update-* file is left.
#   gl-own   Gear Lever's own --update on a freshly integrated copy of N; the file becomes N+1 (SHA256SUMS).
#   ail-install  AppImageLauncher's release DEB (#412's, digest-checked on the host) with apt-get.
#   ail      the same as gl, through `ail-cli integrate` and AppImageLauncher's menu entry; then its updater's check.
# State snapshots go to ~/ua/u9/*.txt; frames are taken from the host's VNC by the caller.
set -u
mode=$1
N=26.1022.1017-nightly N1=26.1022.1019-nightly
SHA1019=e5b7ad2caecaa7f5737bdfb5d2f0fd4e3c74ce5d98a8a68142caf4efd43525bc
OUT=$HOME/ua/u9; mkdir -p $OUT
GL="flatpak run it.mijorus.gearlever"
CLI=$HOME/ua/cli/squashfs-root
[ -x $CLI/usr/bin/putgd-cli ] || (mkdir -p $HOME/ua/cli && cd $HOME/ua/cli && $HOME/ua/assets/putgd-$N-x86_64.AppImage --appimage-extract > /dev/null)
cli() { LD_LIBRARY_PATH=$CLI/usr/lib:$CLI/usr/lib/putgd QT_PLUGIN_PATH=$CLI/usr/plugins XDG_RUNTIME_DIR=/run/user/$(id -u) $CLI/usr/bin/putgd-cli "$@"; }
# Gear Lever --remove trashes the file; the small guests need the space back (only our put.gd copies).
untrash() { rm -f $HOME/.local/share/Trash/files/put*.appimage $HOME/.local/share/Trash/info/put*.appimage.trashinfo; }
resident() { for p in $(pgrep -u $(id -u) -x putgd); do echo "$p $(tr '\0' '\n' < /proc/$p/environ | grep -E '^(APPIMAGE|DESKTOPINTEGRATION)=' | tr '\n' ' ')"; done; }
wait_ready() { for i in $(seq 1 60); do cli diagnostics 2>/dev/null | grep -q '"journalReady":true' && return 0; sleep 1; done; echo "resident not ready"; }
# gio launch in the session's environment, detached (a systemd-run service would kill the app when gio exits).
launch_entry() { (env $(systemctl --user show-environment | grep -E '^(WAYLAND_DISPLAY|DISPLAY|XDG_CURRENT_DESKTOP|XDG_SESSION_TYPE|DESKTOP_SESSION|XAUTHORITY)=') setsid nohup gio launch "$1" > /dev/null 2>&1 < /dev/null &); sleep 3; wait_ready; }
snap() {  # NAME APPDIR ENTRYGLOB
  { echo "## $1 $(date -u +%FT%TZ)"; echo "### ls -la $2"; ls -la --time-style=full-iso "$2"; echo "### sha256 / inode"
    for f in "$2"/*; do [ -f "$f" ] && echo "$(stat -c %i "$f") $(sha256sum "$f")"; done
    echo "### desktop entries"; for e in $3; do [ -f "$e" ] && { echo "#### $e"; grep -E '^(Name|Exec|TryExec)=' "$e"; sha256sum "$e"; }; done
    echo "### residents"; resident; } > $OUT/snap-$1.txt; cat $OUT/snap-$1.txt | grep -E '^(## |[0-9]+ [0-9a-f]{64}|#### |Exec=|[0-9]+ APPIMAGE)'; }
install_update() {
  cli update.check '{}' > /dev/null
  cli ui.show '{"view":"updates"}' > /dev/null
  cli update.install '{"confirm":true}' | python3 -c "import json,sys; d=json.load(sys.stdin); print('install started', d.get('ok'), (d.get('error') or {}).get('code'))"
  for i in $(seq 1 300); do s=$(cli update.status '{}'); echo "$s" | python3 -c "import json,sys; i=json.load(sys.stdin)['result']['install']; sys.exit(0 if not i.get('running') and i.get('state') in ('installed','failed') else 1)" && break; sleep 1; done
  echo "$s" | python3 -c "import json,sys; i=json.load(sys.stdin)['result']['install']; r=i.get('result') or {}; print('install', i.get('state'), r.get('method'), (i.get('error') or {}).get('code'), 'backup', r.get('backup'))"
  cli ui.show '{"view":"updates"}' > /dev/null
}
restart_update() {
  old=$(pgrep -u $(id -u) -x putgd | head -1)
  cli update.restart '{"confirm":true}' > /dev/null
  for i in $(seq 1 60); do [ -e /proc/$old ] || break; sleep 1; done; sleep 3; wait_ready
  cli update.status '{}' | python3 -c "import json,sys; d=json.load(sys.stdin)['result']; print('after restart', d['build']['version'], (d.get('install') or {}).get('installed', {}).get('backupRemoved') if isinstance((d.get('install') or {}).get('installed'), dict) else None)"
  cli ui.show '{"view":"updates"}' > /dev/null; resident
}
case $mode in
gl)
  mkdir -p $HOME/ua/gl && cp $HOME/ua/assets/putgd-$N-x86_64.AppImage $HOME/ua/gl/ && chmod 755 $HOME/ua/gl/*.AppImage
  yes | $GL --integrate $HOME/ua/gl/putgd-$N-x86_64.AppImage 2>&1 | tail -2
  $GL --list-installed 2>&1 | grep -i put
  snap gl-0-integrated $HOME/AppImages "$HOME/.local/share/applications/*put*"
  entry=$(grep -l 'AppImages/' $HOME/.local/share/applications/*.desktop | head -1); echo "entry $entry"
  launch_entry "$entry"; resident
  install_update; snap gl-1-installed $HOME/AppImages "$entry"
  restart_update
  cli ui.quit '{}' > /dev/null; sleep 4; resident
  launch_entry "$entry"; cli ui.show '{"view":"updates"}' > /dev/null
  cli update.status '{}' | python3 -c "import json,sys; print('menu start', json.load(sys.stdin)['result']['build']['version'])"
  snap gl-2-after-menu-launch $HOME/AppImages "$entry"
  $GL --list-installed 2>&1 | grep -i put; $GL --list-updates 2>&1 | tail -3 | tee $OUT/gl-list-updates.txt
  ls -a $HOME/AppImages | grep -c putgd-update || true ;;
gl-own)
  cli ui.quit '{}' > /dev/null 2>&1; sleep 3
  name=$($GL --list-installed 2>&1 | grep -i 'put.gd' | awk '{print $1}' | head -1)
  yes | $GL --remove "$(ls $HOME/AppImages/*.appimage | head -1)" 2>&1 | tail -1; untrash
  cp $HOME/ua/assets/putgd-$N-x86_64.AppImage $HOME/ua/gl/own-$N.AppImage && chmod 755 $HOME/ua/gl/own-$N.AppImage
  yes | $GL --integrate $HOME/ua/gl/own-$N.AppImage 2>&1 | tail -1
  f=$(ls $HOME/AppImages/*.appimage | head -1); echo "integrated $f $(sha256sum < $f | cut -c1-12)"
  $GL --list-updates 2>&1 | tail -3
  yes | $GL --update "$f" 2>&1 | tail -3
  f2=$(ls $HOME/AppImages/*.appimage | head -1); echo "after Gear Lever update $f2 $(sha256sum < $f2 | cut -c1-64)"
  [ "$(sha256sum < $f2 | cut -c1-64)" = $SHA1019 ] && echo "Gear Lever update = 1019 (SHA256SUMS)" || echo "Gear Lever update NOT 1019"
  $GL --list-updates 2>&1 | tail -2
  yes | $GL --remove "$f2" 2>&1 | tail -1; untrash; ls -a $HOME/AppImages 2>&1; ls $HOME/.local/share/applications/ ;;
ail-install)
  sudo -n apt-get install -y $HOME/ua/ail/appimagelauncher_3.0.0-beta-2-gha287.96cb937_amd64.deb > $OUT/ail-install.log 2>&1; echo "ail install $?"
  systemctl --user enable --now appimagelauncherd.service 2>&1 | tail -1; systemctl --user is-active appimagelauncherd.service ;;
ail)
  mkdir -p $HOME/ua/ail-img && cp $HOME/ua/assets/putgd-$N-x86_64.AppImage $HOME/ua/ail-img/ && chmod 755 $HOME/ua/ail-img/*.AppImage
  ail-cli integrate $HOME/ua/ail-img/putgd-$N-x86_64.AppImage 2>&1 | tail -2
  snap ail-0-integrated $HOME/Applications "$HOME/.local/share/applications/appimagekit_*"
  entry=$(ls $HOME/.local/share/applications/appimagekit_*put*.desktop 2>/dev/null | head -1); echo "entry $entry"
  launch_entry "$entry"; resident
  install_update; snap ail-1-installed $HOME/Applications "$entry"
  restart_update
  cli ui.quit '{}' > /dev/null; sleep 4
  launch_entry "$entry"
  cli update.status '{}' | python3 -c "import json,sys; print('menu start', json.load(sys.stdin)['result']['build']['version'])"
  snap ail-2-after-menu-launch $HOME/Applications "$HOME/.local/share/applications/appimagekit_*"
  f=$(ls $HOME/Applications/*.AppImage | head -1)
  /opt/appimagelauncher.AppDir/usr/bin/update --check-for-update "$f" > $OUT/ail-updater-check.log 2>&1; echo "AIL updater check exit $? $(tail -1 $OUT/ail-updater-check.log)"
  ls $HOME/.local/share/applications/ | grep -c appimagekit ;;
esac
