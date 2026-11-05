#!/bin/bash
# Revision of run_o2.sh for the o3 harness (same checks): explicit, validated roots; returns the harness status.
# usage: run_o2.sh SRC HARNESS OUT APP   (OUT must be a new /tmp/p560XX directory)
set -euo pipefail
src=$1 harness=$2 out=$3 app=$4
[[ $src = /* && -f $src/tools/diagnostics/check_socket_paths.py ]] || { echo "bad SRC $src" >&2; exit 2; }
[[ $harness = /* && -f $harness/resident_burst_o3.py ]] || { echo "bad HARNESS $harness" >&2; exit 2; }
[[ $out =~ ^/tmp/p560[a-z0-9]{2,4}$ && ! -e $out ]] || { echo "bad or existing OUT $out" >&2; exit 2; }
[[ -x $app ]] || { echo "bad APP $app" >&2; exit 2; }
cd "$src"
busdir="$out"b
display=157
umask 077
test ! -e /tmp/.X157-lock
test ! -e /tmp/.X11-unix/X157
mkdir "$busdir"
python3 "$src/tools/diagnostics/check_socket_paths.py" --socket "$busdir/bus" "$out/runtime/putgd/session.sock" /tmp/.X11-unix/X157 > "$busdir/preflight.json"
cat > "$busdir/bus.conf" <<XML
<!DOCTYPE busconfig PUBLIC "-//freedesktop//DTD D-Bus Bus Configuration 1.0//EN" "http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd">
<busconfig><type>session</type><listen>unix:path=$busdir/bus</listen><auth>EXTERNAL</auth><policy context="default"><allow send_destination="*"/><allow receive_sender="*"/><allow own="*"/></policy></busconfig>
XML
set +e
I560_SRC="$src" I560_HARNESS="$harness" nice -n10 xvfb-run --server-num="$display" -s "-screen 0 1280x800x24" dbus-run-session --config-file="$busdir/bus.conf" -- python3 "$harness/resident_burst_o3.py" "$out" "$app"
status=$?
set -e
python3 - "$busdir" "$status" <<'PY'
import json,sys
from pathlib import Path
root=Path(sys.argv[1])
(root/'wrapper-cleanup.json').write_text(json.dumps({'runExit':int(sys.argv[2]),'busSocketAbsent':not (root/'bus').exists(),'xSocketAbsent':not Path('/tmp/.X11-unix/X157').exists()},indent=2))
PY
exit "$status"
