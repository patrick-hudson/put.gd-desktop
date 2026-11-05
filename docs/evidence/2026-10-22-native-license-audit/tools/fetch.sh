#!/usr/bin/env bash
# fetch.sh TAG DIR: the four native packages, SHA256SUMS, release-builds.json
# from GitHub, and SHA256SUMS(.minisig) from pkg.put.gd, then the checksums.
set -euo pipefail
tag=$1 dir=$2 version=${1#v}
gh=https://github.com/patrick-hudson/put.gd-desktop/releases/download/$tag
mkdir -p "$dir/github" "$dir/pkg"
cd "$dir"
for name in SHA256SUMS release-builds.json \
    putgd-$version-ubuntu24.04-x86_64.deb putgd-$version-ubuntu26.04-x86_64.deb \
    putgd-$version-debian13-x86_64.deb putgd-$version-fedora44-x86_64.rpm; do
  curl -fsSL --retry 3 -o "github/$name" "$gh/$name"
done
curl -fsSL --retry 3 -o pkg/SHA256SUMS "https://pkg.put.gd/releases/$tag/SHA256SUMS"
curl -fsSL --retry 3 -o pkg/SHA256SUMS.minisig "https://pkg.put.gd/releases/$tag/SHA256SUMS.minisig"
cmp github/SHA256SUMS pkg/SHA256SUMS && echo "SHA256SUMS: GitHub and pkg.put.gd copies identical"
(cd github && sha256sum --check --strict --ignore-missing SHA256SUMS)
