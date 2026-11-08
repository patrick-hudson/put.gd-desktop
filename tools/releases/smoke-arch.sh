#!/usr/bin/env bash
# Clean runtime container: no build dependencies or source-tree libraries.
set -euo pipefail
package=$(realpath "$1")
pacman -U --noconfirm "$package"
pacman -Qkk putgd
pacman -Ql putgd
pacman -Qi putgd
putgd-cli --version
for binary in /usr/bin/putgd /usr/bin/putgd-cli /usr/bin/putgd-ui /usr/bin/putgd-record /usr/bin/putgd-kde-region; do
  test -x "$binary"
  if ldd "$binary" | grep 'not found'; then exit 1; fi
done
run=$(mktemp -d /tmp/pe.XXXXXX)
chmod 700 "$run"
python tools/diagnostics/check_socket_paths.py --socket "$run/r/putgd/session.sock"
mkdir -m700 "$run/r" "$run/config" "$run/data"
export XDG_RUNTIME_DIR="$run/r" XDG_CONFIG_HOME="$run/config" XDG_DATA_HOME="$run/data" QT_QPA_PLATFORM=offscreen
evidence=${PUTGD_SMOKE_EVIDENCE:-build/arch-smoke}
mkdir -p "$evidence"
/usr/bin/putgd > "$run/resident.log" 2>&1 &
pid=$!
trap 'kill "$pid" 2>/dev/null || true; wait "$pid" 2>/dev/null || true; cp "$run/resident.log" "$evidence/resident.log"; cat "$run/resident.log"' EXIT
sleep 3
kill -0 "$pid"
test -S "$XDG_RUNTIME_DIR/putgd/session.sock"
kill "$pid"
wait "$pid" || true
cp "$run/resident.log" "$evidence/resident.log"
trap - EXIT
pacman -R --noconfirm putgd
! test -e /usr/bin/putgd
