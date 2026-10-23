#!/bin/sh
# stage.sh HOST_DIR: what a builder (or VM guest) needs in its tool folder, copied from this checkout.
# Copy the result with an explicit absolute destination (COORDINATION.md rule 10), for example
#   rsync -rlpc --dry-run "$tmp/" m74:/home/patrick/work/put.gd-nfa/tool/
# afa.py and i326.py are #415's guarded copies; rerun.py, ffdefault.py, ffprofile.py and s3creds.py are #397's.
set -eu
dst=$1 repo=$(git -C "$(dirname "$0")" rev-parse --show-toplevel)
mkdir -p "$dst"
cp "$repo"/docs/evidence/2026-10-23-native-full-acceptance/lab/nfa.py \
   "$repo"/docs/evidence/2026-10-22-rail-391-notify-415/lab/afa.py "$repo"/docs/evidence/2026-10-22-rail-391-notify-415/lab/i326.py \
   "$repo"/docs/evidence/2026-10-22-appimage-full-acceptance-rerun/lab/rerun.py "$repo"/docs/evidence/2026-10-22-appimage-full-acceptance-rerun/lab/ffdefault.py \
   "$repo"/docs/evidence/2026-10-22-appimage-full-acceptance-rerun/lab/ffprofile.py "$repo"/docs/evidence/2026-10-22-appimage-full-acceptance-rerun/lab/s3creds.py \
   "$repo"/docs/evidence/2026-10-22-appimage-full-acceptance-rerun/lab/gtk_pattern.py "$repo"/docs/evidence/2026-10-22-linux-update-acceptance/lab/relogin.sh \
   "$repo"/docs/evidence/2026-10-23-native-full-acceptance/lab/nk.sh "$repo"/docs/evidence/2026-10-23-native-full-acceptance/lab/clip_owner.py \
   "$repo"/tools/lab/rfb.py "$repo"/tools/lab/cover_player.py "$repo"/tools/lab/host_input.py "$repo"/tests/fixtures/s3_server.py \
   "$repo"/packaging/linux/repo/keys/putgd-nightly.minisign.pub "$repo"/packaging/linux/repo/keys/putgd-stable.minisign.pub "$dst"/
# The Linux update profile's live-infrastructure check (U1), once its drivers are on nightly.
u1="$repo"/docs/evidence/2026-10-22-linux-update-acceptance/lab/u1_infra.py
[ -f "$u1" ] && cp "$u1" "$dst"/
echo "staged $(ls "$dst" | wc -l) files in $dst"
