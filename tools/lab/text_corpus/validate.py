#!/usr/bin/env python3
"""Check corpus completeness independently of recognition scores; no GUI work.

Manifest capture rows additionally carry host, theme, scale, receipt, and
transcript_verified (true only after checking the visible region). Missing
coverage is a blocked result, never an implicit quality pass.
"""
import argparse
from collections import defaultdict
import hashlib
import json
from pathlib import Path
import struct

HOSTS=('m74','d69','f44','k72','budgie','hyprland')
CLASSES=('terminal','browser-local','browser-github','browser-wikipedia','editor-code','small-ui','monitor')


def transcript_bytes(root, c):
    if 'transcript_codepoints' in c:
        return ''.join(map(chr,json.loads((root/c['transcript_codepoints']).read_text()))).encode()
    return (root/c['transcript']).read_bytes()


def validate(path):
    manifest=json.loads(path.read_text()); groups=defaultdict(list); errors=[]; hashes={}
    for c in manifest['captures']:
        label=c['id'];groups[(c['host'],c['class'])].append(c)
        try:
            image=path.parent/c['image']; transcript=transcript_bytes(path.parent,c)
            data=image.read_bytes()
            if data[:8]!=b'\x89PNG\r\n\x1a\n':raise ValueError('not PNG')
            width,height=struct.unpack('>II',data[16:24])
            if min(width,height)<=0:raise ValueError('empty image')
            if not c.get('transcript_verified'):errors.append(label+': visible-region transcript needs verification')
            if not isinstance(c['font_pixels'],(int,float)) or c['font_pixels']<=0:raise ValueError('font pixels missing')
            if not (path.parent/c['receipt']).is_file():raise ValueError('capture receipt missing')
            hashes[label]=dict(image=hashlib.sha256(data).hexdigest(),transcript=hashlib.sha256(transcript).hexdigest())
        except (OSError,KeyError,ValueError) as e:errors.append(label+': '+str(e))
    for host in HOSTS:
        for target in CLASSES:
            rows=groups[host,target]
            words=sum(len(transcript_bytes(path.parent,c).decode().split()) for c in rows)
            if words<200:errors.append(f'{host}/{target}: {words}/200 words')
            for theme in ('light','dark'):
                selected=[c for c in rows if c.get('theme')==theme]
                if not any(c.get('scale')==1 for c in selected):errors.append(f'{host}/{target}/{theme}: 100% missing')
                if not any(1<c.get('scale',1)<2 for c in selected):errors.append(f'{host}/{target}/{theme}: fractional scale missing')
    return dict(status='blocked' if errors else 'complete',missing=errors,hashes=hashes)


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('manifest',type=Path);a=p.parse_args()
    result=validate(a.manifest);print(json.dumps(result,indent=2));raise SystemExit(2 if result['missing'] else 0)
