#!/bin/bash
# ak1.sh NAME INPUT VNCPORT [DRAG RECRECT SMALLDRAG] : #418 A-K phase 1 on one desktop with the PR's CI AppImage:
# ac.sh (A, B start, C, #348/#349/#344), dk.sh (B tray/window, D, E, F loopback recorder, G, H), then qt418.py badfile.
set -u
n=$1 input=$2 vnc=$3 drag=${4:-400,200,1520,880} rect=${5:-640,360,640,360} small=${6:-1000,500,1080,560}
root=$HOME/work/put.gd-qt418/afa; cd "$root" || exit 2
export AFA_ROOT=$root IMG=$root/images/putgd-26.1023.1068-dev+g6a325a722afd-x86_64.AppImage SUMS=$root/images/SHA256SUMS.ci TAG=v26.1023.1068-dev+g6a325a722afd
t() { date -u +%FT%TZ; }
echo "$(t) ac.sh start" >> timeline-$n.txt
bash ac.sh "$n" "$input" "$vnc" > ac-$n.out 2>&1; echo "$(t) ac.sh done rc=$?" >> timeline-$n.txt
bash dk.sh "$n" "$drag" "$rect" "$small" > dk-$n.out 2>&1; echo "$(t) dk.sh done rc=$?" >> timeline-$n.txt
python3 -I tool/qt418.py badfile "$n" samples/cve-2025-10729-pattern-orphan.svg samples/cve-2026-8168-deep-nesting.svg \
  samples/cve-2025-5683-icns-block-length.icns samples/fuzz-tiff-00.tiff --label "$n-badfile" > runs/$n/badfile.json 2> runs/$n/badfile.err
echo "$(t) badfile done rc=$?" >> timeline-$n.txt
echo AK1-DONE >> timeline-$n.txt
