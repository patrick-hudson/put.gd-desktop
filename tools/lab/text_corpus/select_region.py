#!/usr/bin/env python3
"""Real RFB selector driver for workload.py's five region actions.

Run on the locked host. Export PUTGD_TEXT_LAB_SESSION matching its desktop
lease; optionally PUTGD_TEXT_RECT=x0,y0,x1,y1 (default 100,100,900,700),
PUTGD_TEXT_RFB_PORT (default 5900). The caller covers the desktop with the
synthetic target first. Argument is the operation ID, retained by workload.py.
This driver only selects; it never invokes capture or recognition itself.
"""
from datetime import datetime, timezone
import os
from pathlib import Path
import sys
import time

sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from rfb import RFB


def main():
    if len(sys.argv)!=2:raise RuntimeError('expected operation ID')
    lease=Path.home()/'putgd-lab/locks/desktop.lock'
    fields=dict(line.split(': ',1) for line in lease.read_text().splitlines() if ': ' in line)
    expected=os.environ.get('PUTGD_TEXT_LAB_SESSION')
    if not expected or fields.get('session')!=expected:raise RuntimeError('desktop lease is not owned by this session')
    now=datetime.now(timezone.utc)
    if datetime.fromisoformat(fields['expires'].replace('Z','+00:00'))<=now:raise RuntimeError('desktop lease expired')
    if (now-datetime.fromisoformat(fields['heartbeat'].replace('Z','+00:00'))).total_seconds()>300:raise RuntimeError('heartbeat is stale')
    rect=tuple(map(int,os.environ.get('PUTGD_TEXT_RECT','100,100,900,700').split(',')))
    if len(rect)!=4 or not (0<=rect[0]<rect[2] and 0<=rect[1]<rect[3]):raise RuntimeError('invalid rectangle')
    client=RFB(int(os.environ.get('PUTGD_TEXT_RFB_PORT','5900')))
    try:
        if rect[2]>=client.width or rect[3]>=client.height:raise RuntimeError('rectangle outside visible desktop')
        time.sleep(1)
        client.drag(*rect)
    finally:client.close()


if __name__=='__main__':main()
