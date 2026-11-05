#!/bin/bash
# #342 acceptance on d69: the signer's lab scope mirrors AppImage-only lab releases under
# pkg.put.gd/lab/ with the production signer's keys and AWS profile (as the service runs it),
# from the PR's pkgrepo.py. Lab tags v0.1.1-nightly.701..703; delete_after 120 s (lab config only).
set -uo pipefail
LAB=/home/patrick/work/put.gd-i293p3/lab342
SRC=/home/patrick/work/put.gd-i293p3
export GNUPGHOME=$HOME/.local/share/putgd-signer/gnupg AWS_CONFIG_FILE=$HOME/.config/putgd-signer/aws-config
export AWS_SHARED_CREDENTIALS_FILE=$HOME/.config/putgd-signer/aws-credentials AWS_SDK_LOAD_CONFIG=1 PYTHONDONTWRITEBYTECODE=1
cd "$LAB"
python3 -c "import json,os; c=json.load(open(os.path.expanduser('~/.config/putgd-signer/signer.json'))); c['delete_after']=120; json.dump(c, open('signer-lab.json','w'))"
signer() { python3 -I "$SRC/tools/releases/pkgrepo.py" --config "$LAB/signer-lab.json" "$@"; }
base=https://pkg.put.gd/lab
check() {  # tag: what the CDN serves for it
  local tag=$1 name=putgd-${1#v}-x86_64.AppImage
  echo "--- $tag image: HEAD"
  curl -sI "$base/releases/$tag/$name" | grep -iE '^(HTTP|content-type|content-length|cache-control|x-cache|location)'
  echo "--- $tag image: three ranges"
  curl -s -D - -o "range-$tag.bin" -H 'Range: bytes=0-99,4096-4195,8000000-8000099' "$base/releases/$tag/$name" | grep -iE '^(HTTP|content-type|location)'
  echo "    multipart body bytes: $(stat -c %s "range-$tag.bin"), parts: $(grep -ac 'Content-Range: bytes' "range-$tag.bin")"
  echo "--- $tag .zsync and the channel permalink"
  curl -sI "$base/releases/$tag/$name.zsync" | grep -iE '^(HTTP|content-type|cache-control)'
  curl -s "$base/releases/$tag/$name.zsync" | sha256sum | cut -c1-64
  curl -s "$base/dl/nightly/appimage.zsync" | sha256sum | cut -c1-64
  curl -sI "$base/dl/nightly/appimage.zsync" | grep -iE '^(HTTP|content-type|cache-control)'
  curl -s "$base/update/v1/nightly.json" | python3 -c 'import json,sys; r=json.load(sys.stdin)["release"]; print("feed:", r["tag"], r["assets"]["appimage"]["name"], r["assets"]["appimage"].get("zsync"))'
}
for tag in v0.1.1-nightly.701 v0.1.1-nightly.702 v0.1.1-nightly.703; do
  echo "=== publish-local $tag"
  signer publish-local --tag "$tag" --assets "$LAB/in/$tag" || { echo "publish failed: $tag"; exit 1; }
  check "$tag"
done
echo "=== no overwrite: republishing 703 is refused"
signer publish-local --tag v0.1.1-nightly.703 --assets "$LAB/in/v0.1.1-nightly.703"; echo "exit=$?"
echo "=== retention: images in the lab tree"
find "$HOME/.local/state/putgd-signer/workspaces/lab/nightly/current/tree/releases" -name '*.AppImage*' | sort
echo "=== status"; signer status
