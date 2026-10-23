#!/usr/bin/env bash
# identity.sh PACKAGE: unpack a DEB or RPM in a temporary directory (no install)
# on a host of its distribution, run its programs' --version-json with a private
# HOME/XDG and print them beside the packaged build-info.json, then clean up.
set -euo pipefail
package=$(realpath "$1")
work=$(mktemp -d "$HOME/putgd-audit-383-identity.XXXXXX")
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/root" "$work/home"
case $package in
  *.deb) dpkg-deb -x "$package" "$work/root" ;;
  *.rpm) (cd "$work/root" && rpm2cpio "$package" | cpio -idm --quiet --no-absolute-filenames) ;;
esac
. /etc/os-release
echo "host: $(hostname) $PRETTY_NAME; package: $(basename "$package") $(sha256sum "$package" | cut -c1-64)"
echo "build-info.json: $(python3 -I -c 'import json,sys; print(json.dumps(json.load(open(sys.argv[1])), sort_keys=True))' "$work/root/usr/share/putgd/build-info.json")"
for program in putgd putgd-cli; do
  h=$work/home
  out=$(env -i HOME="$h" XDG_CONFIG_HOME="$h/c" XDG_STATE_HOME="$h/s" XDG_DATA_HOME="$h/d" XDG_CACHE_HOME="$h/k" \
        XDG_RUNTIME_DIR="$h" QT_QPA_PLATFORM=offscreen PATH=/usr/bin timeout 20 "$work/root/usr/bin/$program" --version-json)
  echo "$program --version-json: $(python3 -I -c 'import json,sys; print(json.dumps(json.loads(sys.stdin.read()), sort_keys=True))' <<<"$out")"
done
