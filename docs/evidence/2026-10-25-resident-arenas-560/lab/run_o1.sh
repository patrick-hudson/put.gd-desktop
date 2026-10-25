#!/bin/bash
set -eu
cd /home/patrick/work/i560o
out=$1
app=$2
case "$out" in /tmp/p560o[0-9a-z]) ;; *) exit 2;; esac
busdir="$out"b
display=157
umask 077
test ! -e /tmp/.X157-lock
test ! -e /tmp/.X11-unix/X157
mkdir "$busdir"
python3 tools/diagnostics/check_socket_paths.py --socket "$busdir/bus" "$out/runtime/putgd/session.sock" /tmp/.X11-unix/X157 > "$busdir/preflight.json"
cat > "$busdir/bus.conf" <<XML
<!DOCTYPE busconfig PUBLIC "-//freedesktop//DTD D-Bus Bus Configuration 1.0//EN" "http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd">
<busconfig><type>session</type><listen>unix:path=$busdir/bus</listen><auth>EXTERNAL</auth><policy context="default"><allow send_destination="*"/><allow receive_sender="*"/><allow own="*"/></policy></busconfig>
XML
set +e
nice -n10 xvfb-run --server-num="$display" -s "-screen 0 1280x800x24" dbus-run-session --config-file="$busdir/bus.conf" -- python3 /home/patrick/work/i560o-h/harness/resident_burst_o1.py "$out" "$app"
status=$?
set -e
python3 - "$busdir" "$status" <<'PY'
import json,sys
from pathlib import Path
root=Path(sys.argv[1])
(root/'wrapper-cleanup.json').write_text(json.dumps({'runExit':int(sys.argv[2]),'busSocketAbsent':not (root/'bus').exists(),'xSocketAbsent':not Path('/tmp/.X11-unix/X157').exists()},indent=2))
PY
exit "$status"
