#!/bin/sh
# Throwaway dpkg root: what dpkg does with a hand-edited conffile on upgrade.
set -eu
W=$(mktemp -d /tmp/i386-probe.XXXXXX); echo "work $W"
mk() { # name version content
  d=$W/src-$2; mkdir -p $d/DEBIAN $d/etc/apt/sources.list.d
  printf '%s\n' "$3" > $d/etc/apt/sources.list.d/probe.sources
  printf 'Package: i386probe\nVersion: %s\nArchitecture: all\nMaintainer: x <x@x>\nDescription: probe\n' $2 > $d/DEBIAN/control
  echo /etc/apt/sources.list.d/probe.sources > $d/DEBIAN/conffiles
  dpkg-deb --root-owner-group -b $d $W/$1.deb >/dev/null
}
R=$W/root; A=$R/var/lib/dpkg; mkdir -p $A/info $A/updates $R/etc/apt/sources.list.d; touch $A/status $A/available
D="dpkg --root=$R --admindir=$A --force-not-root --force-bad-path --force-script-chrootless --log=$W/dpkg.log"
mk n1 1~nightly NIGHTLY; mk n2 2~nightly NIGHTLY; mk s3 3 STABLE; mk s0 0.5 STABLE
F=$R/etc/apt/sources.list.d/probe.sources
step() { echo "== $1"; }
step "install nightly 1"; $D -i $W/n1.deb </dev/null 2>&1 | tail -1
step "user switch: write the stable copy"; echo STABLE > $F
step "same-channel upgrade (nightly 2, shipped copy unchanged)"; $D -i $W/n2.deb </dev/null 2>&1 | grep -iE "conffile|Setting up" ; echo "file: $(cat $F)"
step "cross-channel upgrade (stable 3 ships the copy the file already has)"; $D -i $W/s3.deb </dev/null 2>&1 | grep -iE "conffile|Setting up|prompt|\*\*\*"; echo "file: $(cat $F)"
step "after: dpkg records the stable copy as the shipped one"; dpkg-query --admindir=$A -W -f='${Conffiles}\n' i386probe
step "switch back to nightly, then nightly 2 again (downgrade from 3)"; echo NIGHTLY > $F; $D -i --force-downgrade $W/n2.deb </dev/null 2>&1 | grep -iE "conffile|Setting up|\*\*\*"; echo "file: $(cat $F)"
step "control: a file that differs from both copies (prompt expected, non-interactive -> default keep)"; echo CUSTOM > $F; $D -i $W/s3.deb </dev/null 2>&1 | grep -iE "conffile|Setting up|\*\*\*|keep" | head -5; echo "file: $(cat $F)"
dpkg --version | head -1
rm -rf "$W"; echo removed $W
