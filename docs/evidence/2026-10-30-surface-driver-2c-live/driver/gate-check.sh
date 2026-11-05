#!/bin/bash
# Gate refusals through the production CLI (no target is touched on refusal).
cd ~/work/put.gd/src
B=7e2d7ef62a08ad1874163930a4d76381420c2bfc
common="--desktop mint-cinnamon-x11 --package deb --destination s3 --recipes core_demo --build $B"
mkdir -p ~/sd726/gate && cd ~/sd726/gate && rm -rf empty-catalog stub out-*; mkdir -p empty-catalog stub
printf '#!/bin/sh\ncat >/dev/null\nexit 0\n' > stub/tesseract; chmod 755 stub/tesseract
cd ~/work/put.gd/src
echo "== physical builder desktop (native placement)"
python3 tools/lab/surface_driver/run.py $common --host m74 --placement native --host-profile /home/user/sd726/gate/none.json --lock-session s --catalog ~/sd726/catalog --out ~/sd726/gate/out-physical 2>&1 | tail -1; echo "exit=${PIPESTATUS[0]} out_exists=$(test -e ~/sd726/gate/out-physical && echo yes || echo no)"
echo "== empty catalog"
python3 tools/lab/surface_driver/run.py $common --host boat --placement boat --boat-id bx_3b94gv6d --run work/lab-runs/c726 --catalog ~/sd726/gate/empty-catalog --out ~/sd726/gate/out-empty 2>&1 | tail -1; echo "exit=${PIPESTATUS[0]} out_exists=$(test -e ~/sd726/gate/out-empty && echo yes || echo no)"
echo "== blind OCR (exits 0, reads nothing)"
PATH=~/sd726/gate/stub:$PATH python3 tools/lab/surface_driver/run.py $common --host boat --placement boat --boat-id bx_3b94gv6d --run work/lab-runs/c726 --catalog ~/sd726/catalog --out ~/sd726/gate/out-ocr 2>&1 | tail -1; echo "exit=${PIPESTATUS[0]} out_exists=$(test -e ~/sd726/gate/out-ocr && echo yes || echo no)"
echo "== admitted: boat placement, exact-build catalog, real OCR"
python3 -c "
import sys;sys.path.insert(0,'.')
from tools.lab.surface_driver.run import secret_safe_evidence_blockers
print('blockers', secret_safe_evidence_blockers('boat','/home/user/sd726/catalog','$B',False))
print('wrong build', secret_safe_evidence_blockers('boat','/home/user/sd726/catalog','0123456789',False))
print('guest', secret_safe_evidence_blockers('guest','/home/user/sd726/catalog','$B',False))"
