#!/bin/sh
# Run on the QEMU host: sh stage-gnome-witness.sh RUN [SOURCE_DIR].
# RUN must pass lab.py's disposable-overlay authentication. Reboot that overlay
# with guest_reboot.py afterwards, then verify the extension is ACTIVE.
set -eu
exec python3 "$(dirname "$0")/stage_gnome_witness.py" "$@"
