#!/bin/sh
# #293 phase 2 lab acceptance on one host (SSH, sudo -n). Usage: accept-pkg.sh LABEL PHASE
# Packages: ~/work/put.gd-pkg293-lab/out/putgd-0.1.1-nightly.<N>-LABEL-x86_64.{deb,rpm}
set -u
label=$1; phase=$2
out=$HOME/work/put.gd-pkg293-lab/out
ev=$HOME/work/put.gd-pkg293-lab/evidence; mkdir -p "$ev"
if [ "$label" = fedora44 ]; then kind=rpm; file=/etc/yum.repos.d/putgd.repo; else kind=deb; file=/etc/apt/sources.list.d/putgd.sources; fi
pkg() { echo "$out/putgd-0.1.1-nightly.$1-$label-x86_64.$kind"; }
say() { printf '\n### %s\n' "$*"; }
installed() { if [ $kind = deb ]; then dpkg-query -W -f='${Version} ${Status}\n' putgd 2>&1; else rpm -q putgd 2>&1; fi; }
repo_state() {
  if [ -e "$file" ]; then echo "$file present sha256=$(sha256sum "$file" | cut -c1-16)"; else echo "$file ABSENT"; fi
  if [ $kind = deb ]; then dpkg-query -W -f='conffiles:${Conffiles}\n' putgd 2>/dev/null; ls -la /usr/share/keyrings/putgd-archive-keyring.gpg 2>&1;
  else ls -la /etc/pki/rpm-gpg/RPM-GPG-KEY-putgd /usr/share/putgd/repo/putgd.repo 2>&1; rpm -q --qf '[%{FILEFLAGS:fflags} %{FILENAMES}\n]' putgd 2>/dev/null | grep -E 'yum.repos.d|rpm-gpg'; fi
}
apt_q() { sudo -n apt-get -o DPkg::Lock::Timeout=300 -o Dpkg::Options::=--force-confdef -o Dpkg::Options::=--force-confold "$@"; }
{
case $phase in
  pre)
    say "host"; uname -a; cat /etc/os-release | grep -E '^(PRETTY_NAME|VERSION_ID)='; date -u
    say "before"; installed; repo_state ;;
  install)  # N from the file; the package adds the repository
    n=$3
    say "install $(basename "$(pkg "$n")") from the file"
    if [ $kind = deb ]; then apt_q install -y "$(pkg "$n")"; else sudo -n dnf install -y "$(pkg "$n")"; fi
    echo "rc=$?"; installed; repo_state; [ -e "$file" ] && cat "$file" | sed -n '1,9p' ;;
  upgrade)  # through the repository
    say "upgrade through the repository"
    if [ $kind = deb ]; then apt_q update 2>&1 | grep -iE 'pkg.put.gd|err|warn|sign'; apt-cache policy putgd; echo "apt upgrade would: $(apt-get -s upgrade 2>/dev/null | grep '^Inst putgd ')"; apt_q install --only-upgrade -y putgd 2>&1 | tail -4
    else sudo -n dnf -y upgrade --refresh putgd 2>&1 | tail -8; fi
    echo "rc=$?"; installed; repo_state ;;
  policy)
    say "what the system sees"
    if [ $kind = deb ]; then apt_q update 2>&1 | grep -iE 'pkg.put.gd|err|sign'; apt-cache policy putgd; apt list --upgradable 2>/dev/null | grep putgd
    else sudo -n dnf -q --refresh check-update putgd; dnf repoquery --upgrades putgd 2>/dev/null; fi
    say "PackageKit"; pkcon refresh >/dev/null 2>&1; pkcon get-updates 2>&1 | grep -iE 'putgd|There are no' ;;
  pkupdate)  # PackageKit installs it without --allow-untrusted, as GNOME Software would
    say "pkcon update putgd (only trusted)"; sudo -n pkcon -y update putgd 2>&1 | tail -6; echo "rc=$?"; installed ;;
  remove)
    say "purge / remove"
    if [ $kind = deb ]; then apt_q purge -y putgd 2>&1 | tail -3; else sudo -n dnf remove -y putgd 2>&1 | tail -3; fi
    echo "rc=$?"; installed; repo_state ;;
  deleted)  # fresh install of N+1, delete the repo file, upgrade to N+2 from the file
    a=$3; b=$4
    say "fresh install $a, delete $file, upgrade to $b from the file"
    if [ $kind = deb ]; then apt_q install -y "$(pkg "$a")" >/dev/null; else sudo -n dnf install -y "$(pkg "$a")" >/dev/null; fi
    installed; repo_state; sudo -n rm -f "$file"; repo_state
    if [ $kind = deb ]; then apt_q install -y "$(pkg "$b")" 2>&1 | tail -2; else sudo -n dnf install -y "$(pkg "$b")" 2>&1 | tail -2; fi
    echo "rc=$?"; installed; repo_state
    say "remove after the deletion"
    if [ $kind = deb ]; then apt_q purge -y putgd 2>&1 | tail -1; else sudo -n dnf remove -y putgd 2>&1 | tail -1; fi
    installed; repo_state ;;
  forged)  # rpm: consistent forged repos (made on d69) offering an unsigned and a foreign-signed N+3; dnf must refuse both
    for kind_dir in unsigned foreign; do
      say "forged repository: $kind_dir RPM"
      python3 -m http.server 18294 --bind 127.0.0.1 --directory "$HOME/work/put.gd-pkg293-lab/forged/$kind_dir" >/dev/null 2>&1 & server=$!; sleep 1
      R=$(mktemp -d /tmp/pgforged.XXXX)
      sed "s#baseurl=.*#baseurl=http://127.0.0.1:18294/#; s#^\[putgd-nightly\]#[putgd-forged]#" /usr/share/putgd/repo/putgd.repo > "$R/forged.repo"
      sudo -n dnf -y --setopt=reposdir="$R" --setopt=cachedir="$R/cache" --repo=putgd-forged upgrade putgd 2>&1 | grep -iE "putgd|gpg|signature|key|error|Nothing|Complete" | head -8
      echo "dnf rc=$?"; installed
      kill $server; rm -rf "$R"
    done ;;
  tamper)  # a loopback copy of the lab repository with one byte changed; the client must refuse
    T=$(mktemp -d /tmp/pgtamper.XXXX); cd "$T"
    if [ $kind = deb ]; then
      base=apt/$label/dists/nightly
      mkdir -p "$base/main/binary-amd64"
      curl -sSf -o "$base/InRelease" "https://pkg.put.gd/lab/$base/InRelease"
      curl -sSf -o "$base/main/binary-amd64/Packages" "https://pkg.put.gd/lab/$base/main/binary-amd64/Packages"
    else
      base=rpm/fedora/44/x86_64/nightly/repodata
      mkdir -p "$base"; curl -sSf -o "$base/repomd.xml" "https://pkg.put.gd/lab/$base/repomd.xml"
      curl -sSf -o "$base/repomd.xml.asc" "https://pkg.put.gd/lab/$base/repomd.xml.asc"
      for f in $(grep -o 'href="repodata/[^"]*"' "$base/repomd.xml" | cut -d'"' -f2); do curl -sSf -o "rpm/fedora/44/x86_64/nightly/$f" "https://pkg.put.gd/lab/rpm/fedora/44/x86_64/nightly/$f"; done
    fi
    python3 -m http.server 18293 --bind 127.0.0.1 >/dev/null 2>&1 & server=$!; sleep 1
    attempt() {  # $1 = description
      say "tamper: $1"
      if [ $kind = deb ]; then
        L=$T/state; rm -rf "$L"; mkdir -p "$L/lists/partial" "$L/cache/archives/partial" "$L/etc"
        sed "s#URIs: .*#URIs: http://127.0.0.1:18293/apt/$label#" "$file" > "$L/etc/putgd.sources"
        apt-get -o Dir::Etc::SourceList=/dev/null -o Dir::Etc::SourceParts="$L/etc" -o Dir::State::Lists="$L/lists" -o Dir::Cache="$L/cache" \
          -o Debug::NoLocking=1 -o APT::Sandbox::User="$(id -un)" update 2>&1 | grep -E '127.0.0.1|^[EW]:' | head -6
        echo "apt saw putgd: $(ls "$L/lists" | grep -c Packages) Packages lists"
      else
        R=$T/repos; rm -rf "$R"; mkdir -p "$R"
        sed "s#baseurl=.*#baseurl=http://127.0.0.1:18293/rpm/fedora/44/x86_64/nightly#" "$file" > "$R/putgd.repo"
        dnf -y --setopt=reposdir="$R" --setopt=cachedir="$T/cache-$$-$(date +%s%N)" --repo=putgd-nightly repoquery putgd 2>&1 | tail -4
        echo "dnf rc=$?"
      fi
    }
    attempt "untouched copy (control)"
    if [ $kind = deb ]; then
      cp "$base/InRelease" "$T/InRelease.good"
      sed -i '0,/Architectures: amd64/s//Architectures: amd65/' "$base/InRelease"; attempt "InRelease: signed text changed"
      cp "$T/InRelease.good" "$base/InRelease"; printf 'Package: putgd\nVersion: 9.9.9\n' >> "$base/main/binary-amd64/Packages"; attempt "Packages changed, InRelease untouched"
    else
      cp "$base/repomd.xml" "$T/repomd.good"
      sed -i '0,/<revision>/s//<revision>9/' "$base/repomd.xml"; attempt "repomd.xml changed"
      cp "$T/repomd.good" "$base/repomd.xml"; p=$(ls "$base"/*-primary.xml*); printf 'x' >> "$p"; attempt "primary.xml changed, repomd.xml untouched"
    fi
    kill $server; cd /; rm -rf "$T" ;;
esac
} 2>&1 | tee -a "$ev/$label-$phase.txt"
