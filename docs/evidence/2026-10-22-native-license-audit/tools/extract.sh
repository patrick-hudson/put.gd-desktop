#!/usr/bin/env bash
# extract.sh TAG DIR REPOKEYS: unpack the packages fetched by fetch.sh without
# installing them, record their metadata and collect the keys they ship.
set -euo pipefail
tag=$1 dir=$(realpath "$2") repokeys=$(realpath "$3") version=${1#v}
mkdir -p "$dir/x" "$dir/meta" "$dir/keys"
for label in ubuntu24.04 ubuntu26.04 debian13; do
  deb=$dir/github/putgd-$version-$label-x86_64.deb
  dpkg-deb -R "$deb" "$dir/x/$label"
  dpkg-deb --info "$deb" > "$dir/meta/$label.info"
  dpkg-deb --contents "$deb" > "$dir/meta/$label.contents"
  ar t "$deb" > "$dir/meta/$label.members"
  cp "$dir/x/$label/usr/share/keyrings/putgd-archive-keyring.gpg" "$dir/keys/$label-archive-keyring.gpg"
  # Signed-By holds the armored key inline, one leading space per line.
  sed -n '/^Signed-By:/,$p' "$dir/x/$label/etc/apt/sources.list.d/putgd.sources" | sed '1d;s/^ //;s/^\.$//' \
    | gpg --dearmor > "$dir/keys/$label-sources-signed-by.gpg"
done
rpm=$dir/github/putgd-$version-fedora44-x86_64.rpm
mkdir -p "$dir/x/fedora44"
rpm2cpio "$rpm" | (cd "$dir/x/fedora44" && cpio -idm --quiet --no-absolute-filenames)
rpm -qp --nosignature -i "$rpm" > "$dir/meta/fedora44.info"
rpm -qp --nosignature --scripts "$rpm" > "$dir/meta/fedora44.scripts"
rpm -qp --nosignature --requires "$rpm" > "$dir/meta/fedora44.requires"
rpm -qp --nosignature --recommends "$rpm" > "$dir/meta/fedora44.recommends"
rpm -qp --nosignature --provides "$rpm" > "$dir/meta/fedora44.provides"
rpm -qp --nosignature --qf '[%{FILEFLAGS:fflags}\t%{FILEMODES:perms}\t%{FILEUSERNAME}:%{FILEGROUPNAME}\t%{FILESIZES}\t%{FILENAMES}\n]' "$rpm" > "$dir/meta/fedora44.files"
rpm -qp --nosignature --qf '%{SIGPGP:pgpsig}|%{RSAHEADER:pgpsig}|%{DSAHEADER:pgpsig}\n' "$rpm" > "$dir/meta/fedora44.signature"
for key in RPM-GPG-KEY-putgd RPM-GPG-KEY-putgd-nightly RPM-GPG-KEY-putgd-stable; do
  # The per-channel names arrived with #428.
  [ ! -e "$dir/x/fedora44/etc/pki/rpm-gpg/$key" ] || cp "$dir/x/fedora44/etc/pki/rpm-gpg/$key" "$dir/keys/fedora44-$key"
done
cp "$repokeys/putgd-nightly.gpg" "$dir/keys/repo-putgd-nightly.gpg"
cp "$repokeys/putgd-stable.gpg" "$dir/keys/repo-putgd-stable.gpg"
for k in "$dir"/keys/*; do printf '%s ' "$(basename "$k")"; gpg --show-keys --with-colons "$k" 2>/dev/null | awk -F: '$1=="fpr"{print $10; exit}'; done
