#!/bin/sh
# stage.sh HOST_DIR: what a builder (or VM guest) needs in its run root, copied from this checkout.
# afa.py and i326.py are #415's guarded copies; ffdefault.py and ffprofile.py are #397's item F helpers.
set -eu
dst=$1 repo=$(git -C "$(dirname "$0")" rev-parse --show-toplevel)
mkdir -p "$dst"
cp "$repo"/docs/evidence/2026-10-22-linux-update-acceptance/lab/ua_app.py "$repo"/docs/evidence/2026-10-22-linux-update-acceptance/lab/ua_pkg.py \
   "$repo"/docs/evidence/2026-10-22-rail-391-notify-415/lab/afa.py "$repo"/docs/evidence/2026-10-22-rail-391-notify-415/lab/i326.py \
   "$repo"/docs/evidence/2026-10-22-appimage-full-acceptance-rerun/lab/ffdefault.py "$repo"/docs/evidence/2026-10-22-appimage-full-acceptance-rerun/lab/ffprofile.py \
   "$repo"/docs/evidence/2026-10-22-appimage-full-acceptance-rerun/lab/s3creds.py "$repo"/tools/lab/rfb.py "$repo"/tests/fixtures/s3_server.py "$dst"/
