#!/usr/bin/env python3
"""Offline corpus measurement through test_ocr_run and the real putgd-ocr.
Run separately for each recorded engine build; never combine engine builds.
Inputs must be the synthetic/public corpus. Output contains recognized text.
"""
import argparse, hashlib, json, os
from pathlib import Path
import subprocess
from score import score, aggregate, read_transcript


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def quality_summary(rows,expected):
    complete=bool(rows) and len(rows)==expected
    return ('measured',aggregate(rows)) if complete else ('blocked',{})


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('manifest',type=Path)
    p.add_argument('--runner',type=Path,required=True)
    p.add_argument('--helper',type=Path,required=True)
    p.add_argument('--data',action='append',required=True)
    p.add_argument('--build-id',required=True)
    p.add_argument('--engine-label',required=True)
    p.add_argument('--language',action='append',help='repeat for explicit languages; defaults to eng')
    p.add_argument('--output',type=Path,required=True)
    a=p.parse_args();a.output.mkdir(mode=0o700)
    identities={str(f.resolve()):digest(f) for f in (a.runner,a.helper,a.manifest)}
    data={str(f.resolve()):digest(f) for d in a.data for f in Path(d).glob('*.traineddata')}
    if not data:raise ValueError('language data files missing')
    languages=a.language or ['eng']
    receipt=dict(engine=a.engine_label,languages=languages,build_id=a.build_id,identities=identities,language_data=data,captures=[],status='blocked')
    receipt['linked_libraries']=subprocess.check_output(['ldd',str(a.helper.resolve())],text=True,env={'LC_ALL':'C.UTF-8'})
    for line in receipt['linked_libraries'].splitlines():
        if '=>' in line and any(n in line for n in ('libtesseract','liblept','libZXing')):
            library=Path(line.split('=>',1)[1].split()[0]).resolve(strict=True)
            identities[str(library)]=digest(library)
    identities[str(Path(__file__).resolve())]=digest(Path(__file__))
    results={};rows=[]
    for c in json.loads(a.manifest.read_text())['captures']:
        image=(a.manifest.parent/c['image']).resolve();ref=read_transcript(a.manifest.parent,c)
        row=dict(id=c['id'],image_sha256=digest(image),transcript_sha256=hashlib.sha256(ref.encode()).hexdigest())
        cmd=[str(a.runner.resolve()),'--helper',str(a.helper.resolve()),'--text','--required']
        for language in languages:cmd+=['--lang',language]
        for d in a.data:cmd+=['--data',str(Path(d).resolve())]
        cmd.append(str(image))
        try:
            r=subprocess.run(cmd,env=os.environ|dict(QT_QPA_PLATFORM='offscreen'),capture_output=True,text=True,timeout=35)
            lines=r.stdout.splitlines()
            if r.returncode or len(lines)!=1:raise ValueError('runner failed or did not return exactly one result')
            v=json.loads(lines[0])
            if v.get('error') or v.get('image')!=str(image) or not isinstance(v.get('text'),str):raise ValueError('missing or failed helper result')
            for key in ('spawnMs','engineStartMs','recognizeMs','totalMs','peakRssKiB'):
                if type(v.get(key))!=int or v[key]<0:raise ValueError('missing measurement '+key)
            if digest(image)!=row['image_sha256']:raise ValueError('image changed during recognition')
            row.update({k:v[k] for k in ('spawnMs','engineStartMs','recognizeMs','totalMs','peakRssKiB','truncated','banded')})
            results[c['id']]=v;scored=score(c,ref,v['text'])
            # Keep the size bars separate even within a target class.
            scored['target_class']+='/'+('body>=11px' if c['font_pixels']>=11 else 'small<11px')
            rows.append(scored);row['status']='measured'
        except (ValueError,KeyError,subprocess.TimeoutExpired) as e:row.update(status='blocked',reason=str(e))
        receipt['captures'].append(row)
        (a.output/'receipt.json').write_text(json.dumps(receipt,indent=2))
    if any(digest(Path(f))!=h for f,h in (identities|data).items()):raise RuntimeError('measurement input changed')
    receipt['status'],receipt['classes']=quality_summary(rows,len(receipt['captures']))
    complete=receipt['status']=='measured'
    (a.output/'results.json').write_text(json.dumps(results))
    (a.output/'scores.json').write_text(json.dumps(rows,indent=2))
    (a.output/'receipt.json').write_text(json.dumps(receipt,indent=2))
    print('| Class/size | Captures | Median CER | Bar | URLs exact | Emails exact |')
    print('|---|---:|---:|---:|---:|---:|')
    for name,v in receipt['classes'].items():
        bar=2 if name.endswith('body>=11px') else 5
        entity=lambda k:f"{v[k]['exact']}/{v[k]['total']}" if v[k]['total'] else 'n/a'
        print(f"| {name} | {v['captures']} | {100*v['median_cer']:.3f}% | {bar}% | {entity('urls')} | {entity('emails')} |")
    print('Exact URL/email bar: 90%. Complete measurement:',complete,'; candidate acceptance requires human assessment.')
    return 0 if complete else 2


if __name__=='__main__':raise SystemExit(main())
