#!/bin/sh
# #293 phase 2 live check on one host: a real GitHub nightly, installed from its file,
# adds pkg.put.gd's repository; the system and the app see it. Usage: live-check.sh LABEL TAG PHASE
# PHASE: install | erase
set -u
label=$1; tag=$2; phase=$3
version=${tag#v}
dir=/home/patrick/work/put.gd-pkg293-live
case $label in fedora44) kind=rpm; file=/etc/yum.repos.d/putgd.repo ;; *) kind=deb; file=/etc/apt/sources.list.d/putgd.sources ;; esac
asset=putgd-$version-$label-x86_64.$kind
say() { printf '\n### %s\n' "$*"; }
installed() { if [ $kind = deb ]; then dpkg-query -W -f='${Version} ${Status}\n' putgd 2>&1; else rpm -q putgd 2>&1; fi; }
apt_q() { sudo -n env DEBIAN_FRONTEND=noninteractive apt-get -q -o Dpkg::Use-Pty=0 "$@" </dev/null; }
case $phase in
install)
  say "host"; . /etc/os-release; echo "$PRETTY_NAME; $(uname -r)"; date -u +%FT%TZ
  say "download $asset and SHA256SUMS from the GitHub release $tag"
  mkdir -p $dir && cd $dir || exit 1
  base=https://github.com/patrick-hudson/put.gd-desktop/releases/download/$tag
  curl -fsSL -o "$asset" "$base/$asset" && curl -fsSL -o SHA256SUMS "$base/SHA256SUMS" || exit 1
  grep " $asset\$" SHA256SUMS | sha256sum -c - || exit 1
  say "before"; installed; ls -l $file 2>&1
  say "install from the file"
  if [ $kind = deb ]; then apt_q install -y "./$asset" 2>&1 | tail -2; else sudo -n dnf install -y "./$asset" 2>&1 | tail -2; fi
  installed
  say "repository file"; ls -l $file && cat $file | grep -vE '^ [A-Za-z0-9+/=.]|^ \.$|BEGIN PGP|END PGP'
  if [ $kind = deb ]; then
    say "keyring"; ls -l /usr/share/keyrings/putgd-archive-keyring.gpg; mkdir -p -m 700 $dir/gnupg; gpg --homedir $dir/gnupg --show-keys --with-colons /usr/share/keyrings/putgd-archive-keyring.gpg | grep '^fpr' | head -1
    say "conffile"; dpkg-query -W -f='${Conffiles}\n' putgd | grep putgd.sources
    say "apt update (lists only) and policy"
    sudo -n apt-get -q update 2>&1 | grep -E 'pkg\.put\.gd|^[EW]:'
    apt-cache policy putgd | head -8
  else
    say "key"; ls -l /etc/pki/rpm-gpg/RPM-GPG-KEY-putgd; mkdir -p -m 700 $dir/gnupg; gpg --homedir $dir/gnupg --show-keys --with-colons /etc/pki/rpm-gpg/RPM-GPG-KEY-putgd | grep '^fpr' | head -1
    say "rpm config files"; rpm -qc putgd
    say "dnf sees the repository"
    dnf -q repolist --enabled 2>&1 | grep -i putgd
    dnf -q --refresh --repo=putgd-nightly repoquery putgd 2>&1 | tail -3
  fi
  say "the app's own check against the live feed (putgd-cli --check-update nightly)"
  /usr/bin/putgd-cli --check-update nightly | python3 -c '
import json, sys
d = json.load(sys.stdin); r = d.get("result") or {}; a = r.get("action") or {}
print("ok", d.get("ok"), "error", (d.get("error") or {}).get("code"))
print("current", (r.get("build") or {}).get("version"), "latest", (r.get("result") or r).get("latest"), "available", (r.get("result") or r).get("updateAvailable"))
print("action", a.get("kind"), a.get("repository"), a.get("manager"), a.get("channel"), (a.get("updater") or {}).get("id"), "|", a.get("command"))'
  ;;
pending)  # N installed: the system and the app see N+1 waiting in pkg.put.gd
  say "N+1 waiting: what the system sees ($(date -u +%FT%TZ))"
  if [ $kind = deb ]; then sudo -n apt-get -q update 2>&1 | grep -E 'pkg\.put\.gd|^[EW]:'; apt-cache policy putgd | head -8; apt list --upgradable 2>/dev/null | grep putgd
  else dnf -q --refresh --repo=putgd-nightly repoquery putgd 2>&1 | tail -3; dnf -q check-upgrade putgd 2>&1 | grep putgd; fi
  if command -v pkcon >/dev/null; then say "PackageKit"; pkcon -p get-updates 2>&1 | grep -i putgd; fi
  say "the app's own check against the live feed"
  /usr/bin/putgd-cli --check-update nightly | python3 -c '
import json, sys
d = json.load(sys.stdin); r = d.get("result") or {}; a = r.get("action") or {}
print("ok", d.get("ok"), "latest", r.get("latest"), "available", r.get("updateAvailable"), "| action", a.get("kind"), a.get("repository"), (a.get("updater") or {}).get("id"))'
  ;;
upgrade)  # only putgd, through the repository
  say "upgrade putgd through pkg.put.gd"
  if [ $kind = deb ]; then apt_q install -y --only-upgrade putgd 2>&1 | grep -E 'putgd|upgraded,'; else sudo -n dnf upgrade -y putgd 2>&1 | grep -E 'putgd|Complete|OpenPGP|key'; fi
  installed; ls -l $file
  say "the app afterwards"
  /usr/bin/putgd-cli --check-update nightly | python3 -c '
import json, sys
d = json.load(sys.stdin); r = d.get("result") or {}; a = r.get("action") or {}
print("ok", d.get("ok"), "latest", r.get("latest"), "available", r.get("updateAvailable"), "| action", a.get("kind"), a.get("repository"))'
  ;;
erase)
  say "erase putgd"
  if [ $kind = deb ]; then apt_q purge -y putgd 2>&1 | tail -1; else sudo -n dnf remove -y putgd 2>&1 | tail -1; fi
  installed; ls -l $file 2>&1
  if [ $kind = deb ]; then ls /usr/share/keyrings/putgd-archive-keyring.gpg 2>&1; sudo -n apt-get -q update 2>&1 | grep -c pkg.put.gd
  else ls /etc/pki/rpm-gpg/RPM-GPG-KEY-putgd 2>&1
    k=$(rpm -q gpg-pubkey --qf '%{NAME}-%{VERSION}-%{RELEASE}\n' | grep -i 4032e28d893baf66f4ac23d9a11d38d55536b371); [ -n "$k" ] && sudo -n rpm -e "$k"; echo "putgd keys in rpmdb: $(rpm -q gpg-pubkey --qf '%{SUMMARY}\n' | grep -ci put.gd)"
    for d in /var/cache/libdnf5/putgd-nightly-*; do [ -d "$d" ] && sudo -n rm -rf "$d"; done; ls -d /var/cache/libdnf5/putgd-* 2>/dev/null; fi
  rm -rf $dir; ls -d $dir 2>&1
  ;;
esac
