#!/bin/bash
# fqtb.sh HOST MODE FFROOT BROWSERPID... : #418 real-browser F, phase B. Close the browser by its PIDs, Delete from destination,
# clean the run, restore the Firefox/mimeapps state byte for byte, remove only new paths under FFROOT.
set -u
h=$1 mode=$2 ffroot=$3; shift 3; n=$h-F
root=$HOME/work/put.gd-qt418/afa; cd "$root" || exit 2; export AFA_ROOT=$root AFA_F_GUARD=$root/ff-$n.json
t() { date -u +%FT%TZ; }
for p in "$@"; do kill "$p" 2>/dev/null; done; sleep 3
for p in "$@"; do kill -0 "$p" 2>/dev/null && echo "browser PID $p still running"; done
python3 -I tool/afa.py delete $n > runs/$n/delete.json
python3 -c "import json;d=json.load(open('runs/$n/delete.json'));print('delete', {k:d.get(k) for k in ('remoteAfter','remoteStatus','localExistsAfter')})"
python3 -I -c "import sys; sys.path.insert(0,'tool'); import afa; print(afa.quit_resident(afa.Run('$n')))" > runs/$n/quit.txt 2>&1
python3 -I tool/afa.py cleanup $n > runs/$n/cleanup.json
if [ "$mode" = ffdefault ]; then python3 -I tool/ffdefault.py off "$root/ff-$n.json" > ff-off-$n.json
else python3 -I tool/ffprofile.py off "$ffroot" "$root/ff-$n.json" > ff-off-$n.json; fi
cat ff-off-$n.json
if [ -f ff-before-$n.txt ]; then
  find "$ffroot" -maxdepth 3 | sort > ff-after-$n.txt; comm -13 <(sort ff-before-$n.txt) ff-after-$n.txt > ff-new-$n.txt
  while read -r p; do [ -e "$p" ] && rm -rf -- "$p" && echo "removed new $p"; done < <(sort -r ff-new-$n.txt)
fi
echo "$(t) F phase B: browser PIDs $* closed; Delete from destination; run cleaned; $mode off ($(head -c 200 ff-off-$n.json)); new Firefox paths: $(wc -l < ff-new-$n.txt 2>/dev/null || echo n/a)" >> timeline-$h.txt
