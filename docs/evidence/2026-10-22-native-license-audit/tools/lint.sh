#!/usr/bin/env bash
# lint.sh TAG DIR: lintian with each DEB's own distribution, rpmlint on Fedora 44,
# appstreamcli and desktop-file-validate, in throwaway rootless containers
# (images localhost/putgd-audit-*, built from the stock distro images; nothing
# is installed on the host). Output in DIR/lint/.
set -uo pipefail
tag=$1 dir=$(realpath "$2") version=${1#v}
mkdir -p "$dir/lint"
run() { nice -n 19 podman run --rm --network=none -v "$dir:/a:ro,Z" "$@"; }
for pair in ubuntu24.04:ubuntu-24.04 ubuntu26.04:ubuntu-26.04 debian13:debian-trixie; do
  label=${pair%%:*} image=localhost/putgd-audit-lintian-${pair#*:}
  deb=/a/github/putgd-$version-$label-x86_64.deb
  run "$image" sh -c "lintian --version; lintian --info --display-info --display-experimental --pedantic --tag-display-limit 0 $deb" \
    > "$dir/lint/lintian-$label.txt" 2>&1
  run "$image" sh -c "appstreamcli --version; appstreamcli validate --no-net --pedantic /a/x/$label/usr/share/metainfo/gd.put.Putgd.metainfo.xml" \
    > "$dir/lint/appstream-$label.txt" 2>&1
done
rpm=/a/github/putgd-$version-fedora44-x86_64.rpm
run localhost/putgd-audit-rpmlint sh -c "rpmlint --version; rpmlint --info $rpm" > "$dir/lint/rpmlint-fedora44.txt" 2>&1
run localhost/putgd-audit-rpmlint sh -c "appstreamcli --version; appstreamcli validate --no-net --pedantic /a/x/fedora44/usr/share/metainfo/gd.put.Putgd.metainfo.xml;
  for f in /a/x/fedora44/usr/share/applications/*.desktop; do echo \"== \$f\"; desktop-file-validate \"\$f\"; done" \
  > "$dir/lint/appstream-desktop-fedora44.txt" 2>&1
wc -l "$dir"/lint/*
