#!/bin/sh
# accept-calver.sh (#373 lab acceptance, SSH, sudo -n): the published pre-#373
# nightly v0.1.1-nightly.925 upgrades to the CalVer lab nightly through apt or
# dnf from pkg.put.gd/lab. Usage: accept-calver.sh LABEL PHASE
#   pre | install-legacy | add-lab | upgrade | cleanup
set -u
SUDO="sudo -n"; [ "$(id -u)" = 0 ] && SUDO=""
label=$1; phase=$2
legacy=v0.1.1-nightly.925
root=${LABROOT:-$HOME/work/put.gd-calver373-lab}
dl=$root/legacy; mkdir -p "$dl"
lab=/etc/yum.repos.d/putgd-lab373.repo; [ "$label" = fedora44 ] || lab=/etc/apt/sources.list.d/putgd-lab373.sources
if [ "$label" = fedora44 ]; then kind=rpm; live=/etc/yum.repos.d/putgd.repo; else kind=deb; live=/etc/apt/sources.list.d/putgd.sources; fi
name=putgd-${legacy#v}-$label-x86_64.$kind
say() { printf '\n### %s (%s)\n' "$*" "$(date -u +%FT%TZ)"; }
installed() { if [ $kind = deb ]; then dpkg-query -W -f='${Package} ${Version} ${Status}\n' putgd 2>&1; else rpm -q putgd 2>&1; fi
              command -v putgd >/dev/null && putgd --version 2>&1; }
apt_q() { $SUDO apt-get -o DPkg::Lock::Timeout=300 -o Dpkg::Options::=--force-confdef -o Dpkg::Options::=--force-confold "$@"; }
case $phase in
pre)
  say "host"; uname -r; grep -E '^PRETTY_NAME=' /etc/os-release
  say "before"; installed; ls -la "$live" "$lab" 2>&1 ;;
install-legacy)
  say "download $name and the release's SHA256SUMS from GitHub"
  base=https://github.com/patrick-hudson/put.gd-desktop/releases/download/$legacy
  curl -fsSL -o "$dl/SHA256SUMS" "$base/SHA256SUMS" && curl -fsSL -o "$dl/$name" "$base/$name"
  (cd "$dl" && grep "  $name\$" SHA256SUMS | sha256sum --check --strict) || exit 1
  say "install $name from the file (it adds the live nightly repository)"
  if [ $kind = deb ]; then apt_q install -y "$dl/$name" 2>&1 | tail -3; else $SUDO dnf install -y "$dl/$name" 2>&1 | tail -3; fi
  installed; ls -la "$live"; sed -n '1,8p' "$live" ;;
add-lab)  # the lab prefix of pkg.put.gd, with the trust the installed package set up
  say "add the lab repository beside the live one"
  if [ $kind = rpm ]; then
    sed -e 's#^\[putgd-nightly\]#[putgd-lab373]#' -e 's|^name=.*|name=put.gd (lab nightly for #373)|' \
        -e 's#https://pkg.put.gd/rpm/#https://pkg.put.gd/lab/rpm/#' "$live" | $SUDO tee "$lab" >/dev/null
  else
    sed -e 's#https://pkg.put.gd/apt/#https://pkg.put.gd/lab/apt/#' "$live" | $SUDO tee "$lab" >/dev/null
  fi
  grep -E 'baseurl|URIs|Suites|gpgkey' "$lab" ;;
upgrade)
  say "what the package manager sees"
  if [ $kind = deb ]; then apt_q update 2>&1 | grep -E 'pkg.put.gd|^E:|^W:'; apt-cache policy putgd
  else $SUDO dnf -q --refresh check-update putgd; fi
  say "upgrade through the repository"
  if [ $kind = deb ]; then apt_q install --only-upgrade -y putgd 2>&1 | tail -4
  else $SUDO dnf -y upgrade --refresh putgd 2>&1 | tail -8; fi
  echo "rc=$?"
  say "after"; installed
  if [ $kind = deb ]; then v=$(dpkg-query -W -f='${Version}' putgd); dpkg --compare-versions "$v" gt 0.1.1~nightly.925 && echo "dpkg: $v > 0.1.1~nightly.925"
  else v=$(rpm -q --qf '%{VERSION}-%{RELEASE}' putgd); echo "rpm.vercmp($v, 0.1.1~nightly.925-1) = $(rpm --eval "%{lua:print(rpm.vercmp('$v', '0.1.1~nightly.925-1'))}")"; fi
  putgd --version-json 2>/dev/null | python3 -c 'import json,sys; d=json.load(sys.stdin); print({k: d[k] for k in ("version", "baseVersion", "packageVersion", "gitCommit")})' ;;
cleanup)
  say "remove putgd and the lab repository"
  if [ $kind = deb ]; then apt_q purge -y putgd 2>&1 | tail -2; else $SUDO dnf remove -y putgd 2>&1 | tail -2; fi
  $SUDO rm -f "$lab"; installed; ls -la "$live" "$lab" 2>&1 ;;
esac
