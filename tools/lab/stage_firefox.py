#!/usr/bin/env python3
"""Stage Mozilla's non-Snap test browser in an authenticated disposable overlay.

The receipt supplies browser.command/profile_root for a surface-driver profile
copy. Existing browsers, defaults and goldens are untouched; #680 still owns the
private profile, remoting, real input and byte-for-byte MIME restoration.
"""
import argparse
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from tools.lab import lab

SCRIPT = r'''python3 - <<'SD_FIREFOX'
import hashlib,io,json,os,re,subprocess,tarfile,urllib.request
from pathlib import Path
root=Path.home()/'.local/share/putgd-lab-firefox'
assert root.resolve()==root
receipt=root/'installation.json'
if root.exists():
 info=json.loads(receipt.read_text())
 binary=Path(info['command'][0])
 assert binary==root/'firefox/firefox' and binary.is_file() and not binary.is_symlink()
 assert hashlib.sha256(binary.read_bytes()).hexdigest()==info['binarySha256']
 print(json.dumps(info));raise SystemExit
root.mkdir(mode=0o700,parents=True)
url='https://download.mozilla.org/?product=firefox-latest-ssl&os=linux64&lang=en-US'
with urllib.request.urlopen(url,timeout=90) as response:
 final=response.geturl()
 match=re.fullmatch(r'https://(?:download-installer.cdn.mozilla.net|archive.mozilla.org)/pub/firefox/releases/([^/]+)/(linux-x86_64/en-US/firefox-[^/]+\.tar\.(?:xz|bz2))',final)
 assert match,'unexpected Mozilla download origin'
 data=response.read(256*1024*1024+1);assert len(data)<=256*1024*1024
version,member=match.groups()
checks='https://archive.mozilla.org/pub/firefox/releases/'+version+'/SHA512SUMS'
with urllib.request.urlopen(checks,timeout=60) as response:
 sums=response.read(16*1024*1024+1);assert len(sums)<=16*1024*1024
expected=[line.split()[0] for line in sums.decode().splitlines() if len(line.split())==2 and line.split()[1].lstrip('*')==member]
assert expected==[hashlib.sha512(data).hexdigest()],'Mozilla archive checksum mismatch'
with tarfile.open(fileobj=io.BytesIO(data),mode='r:*') as archive:
 assert sum(m.size for m in archive.getmembers())<=700*1024*1024
 archive.extractall(root,filter='data')
binary=root/'firefox/firefox'
assert binary.is_file() and not binary.is_symlink()
reported=subprocess.check_output([str(binary),'--version'],stderr=subprocess.DEVNULL,text=True,timeout=20).strip()
info={'command':[str(binary)],'profile_root':str(Path.home()/'.mozilla/firefox'),'version':reported,
      'archiveSha256':hashlib.sha256(data).hexdigest(),'archiveSha512':expected[0],
      'binarySha256':hashlib.sha256(binary.read_bytes()).hexdigest(),'source':final,'checksums':checks,'scope':'owned-overlay'}
with open(os.open(receipt,os.O_WRONLY|os.O_CREAT|os.O_EXCL,0o600),'w') as stream:json.dump(info,stream)
print(json.dumps(info))
SD_FIREFOX
'''


def stage(run):
    lab.verify_owned_guest(run)
    return lab.guest_session(run, SCRIPT, timeout=240)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--run', required=True)
    args = parser.parse_args()
    try:
        print(stage(args.run))
    except (lab.LabError, OSError, ValueError) as error:
        parser.exit(2, 'Firefox overlay staging failed: '+str(error)+'\n')
