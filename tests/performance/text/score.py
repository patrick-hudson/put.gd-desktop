#!/usr/bin/env python3
"""Score synthetic/public corpus only; never use private recognition output.

python3 score.py corpus.json --tesseract --scale 1 --output baseline-1.json
python3 score.py corpus.json --results helper-results.json --output helper-score.json
Manifest: {captures:[{id,class,image,transcript,font_pixels,urls:[],emails:[]}]}
Paths are relative to the manifest. Results map capture IDs to text strings or
helper JSON objects with a text field (also accepts the CLI's result envelope).
2x/3x baselines require the distribution ImageMagick convert command.
"""
import argparse
from collections import Counter, defaultdict
import hashlib
import json
from pathlib import Path
import re
import statistics
import subprocess
import time
import unicodedata


def normalize(text):
    return ' '.join(unicodedata.normalize('NFC', text).split())


def distance(reference, actual):
    """Exact unit-cost Levenshtein distance, bit-parallel over Unicode points."""
    if not reference:
        return len(actual)
    masks = {}
    for i, char in enumerate(reference):
        masks[char] = masks.get(char, 0) | (1 << i)
    positive, negative, score = ~0, 0, len(reference)
    top = 1 << (len(reference) - 1)
    for char in actual:
        equal = masks.get(char, 0)
        xv = equal | negative
        xh = (((equal & positive) + positive) ^ positive) | equal
        ph = negative | ~(xh | positive)
        mh = positive & xh
        score += bool(ph & top) - bool(mh & top)
        ph = (ph << 1) | 1
        mh <<= 1
        positive = mh | ~(xv | ph)
        negative = ph & xv
    return score


def entities(text, kind):
    pattern = r'https?://[^\s<>"\u201c\u201d]+' if kind == 'urls' else r'[\w.+%-]+@[\w.-]+\.[A-Za-z]{2,}'
    return Counter(re.findall(pattern, normalize(text)))


def score(capture, reference, actual):
    ref, out = normalize(reference), normalize(actual)
    edits = distance(ref, out)
    result = dict(id=capture['id'], target_class=capture['class'],
                  characters=len(ref), edits=edits,
                  cer=edits / len(ref) if ref else (0.0 if not out else None),
                  reference_words=len(ref.split()), font_pixels=capture['font_pixels'])
    for kind in ('urls', 'emails'):
        expected = Counter()
        for item in capture.get(kind, []):
            text = item['text'] if isinstance(item, dict) else item
            pixels = item.get('font_pixels', capture['font_pixels']) if isinstance(item, dict) else capture['font_pixels']
            if pixels >= 11:
                expected[normalize(text)] += 1
        found = entities(out, kind)
        hits = sum((expected & found).values())
        total = sum(expected.values())
        result[kind] = dict(exact=hits, total=total, rate=hits / total if total else None)
    return result


def extract(value):
    if isinstance(value, str):
        return value
    if value.get('ok') is False:
        raise ValueError('failed recognizer response')
    if 'result' in value:
        return extract(value['result'])
    return value['text']


def aggregate(rows):
    groups = defaultdict(list)
    for row in rows:
        groups[row['target_class']].append(row)
    result = {}
    for name, group in sorted(groups.items()):
        chars = sum(r['characters'] for r in group)
        failures = sum('error' in r for r in group)
        result[name] = dict(captures=len(group), failed_captures=failures, reference_words=sum(r['reference_words'] for r in group),
                            median_cer=statistics.median([r['cer'] for r in group if r['cer'] is not None]) if not failures and any(r['cer'] is not None for r in group) else None,
                            micro_cer=sum(r['edits'] or 0 for r in group) / chars if chars and not failures else None)
        for kind in ('urls', 'emails'):
            hits = sum(r[kind]['exact'] or 0 for r in group)
            total = sum(r[kind]['total'] for r in group)
            result[name][kind] = dict(exact=hits, total=total, rate=hits / total if total and not failures else None)
    return result


def read_transcript(root, capture):
    if 'transcript_codepoints' in capture:
        return ''.join(map(chr, json.loads((root / capture['transcript_codepoints']).read_text())))
    return (root / capture['transcript']).read_text()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('manifest', type=Path)
    mode = p.add_mutually_exclusive_group(required=True)
    mode.add_argument('--tesseract', action='store_true')
    mode.add_argument('--results', type=Path)
    p.add_argument('--scale', type=int, choices=(1, 2, 3), default=1)
    p.add_argument('--language', default='eng')
    p.add_argument('--psm', type=int, default=3)
    p.add_argument('--timeout-seconds', type=float, default=60)
    p.add_argument('--output', type=Path, required=True)
    a = p.parse_args()
    if a.timeout_seconds <= 0: p.error('--timeout-seconds must be positive')
    manifest = json.loads(a.manifest.read_text())
    supplied = json.loads(a.results.read_text()) if a.results else {}
    receipt = dict(schemaVersion=1, normalization='NFC; collapse whitespace; case sensitive',
                   scale=a.scale, captures=[], inputs={})
    if a.tesseract:
        receipt['engine'] = subprocess.check_output(['tesseract', '--version'], text=True).splitlines()[0]
        receipt.update(language=a.language, psm=a.psm)
        # Resolve the distribution's actual data location without importing an engine.
        listing = subprocess.check_output(['tesseract', '--list-langs'], text=True)
        match = re.search(r'"([^"]+)"', listing)
        if match:
            data = Path(match[1]) / (a.language + '.traineddata')
            receipt['language_data'] = dict(path=str(data), bytes=data.stat().st_size,
                                            sha256=hashlib.sha256(data.read_bytes()).hexdigest())
    for capture in manifest['captures']:
        reference = read_transcript(a.manifest.parent, capture)
        image = a.manifest.parent / capture['image']
        receipt['inputs'][capture['id']] = dict(image_sha256=hashlib.sha256(image.read_bytes()).hexdigest(),
                                               transcript_sha256=hashlib.sha256(reference.encode()).hexdigest())
        start = time.monotonic()
        try:
            if a.tesseract:
                pixels = image.read_bytes()
                if a.scale != 1:
                    pixels = subprocess.run(['convert', '-', '-filter', 'Lanczos', '-resize', f'{a.scale*100}%', 'png:-'],
                                            input=pixels, capture_output=True, check=True, timeout=a.timeout_seconds).stdout
                run = subprocess.run(['tesseract', 'stdin', 'stdout', '-l', a.language, '--psm', str(a.psm), '--oem', '1'],
                                     input=pixels, capture_output=True, check=True, timeout=a.timeout_seconds)
                actual = run.stdout.decode('utf-8')
            else:
                actual = extract(supplied[capture['id']])
            row = score(capture, reference, actual)
        except (subprocess.TimeoutExpired, subprocess.CalledProcessError, KeyError, ValueError) as error:
            row = score(capture, reference, '')
            row.update(cer=None, edits=None, error='timeout' if isinstance(error, subprocess.TimeoutExpired) else 'recognizer_failed')
            for kind in ('urls', 'emails'):
                row[kind].update(exact=None, rate=None)
        row['elapsed_seconds'] = time.monotonic() - start
        receipt['captures'].append(row)
    receipt['classes'] = aggregate(receipt['captures'])
    receipt['status'] = 'incomplete' if any('error' in r for r in receipt['captures']) else 'measured'
    receipt['timeout_seconds'] = a.timeout_seconds
    with a.output.open('x') as out:
        json.dump(receipt, out, indent=2)
        out.write('\n')
    print(json.dumps(receipt['classes'], indent=2))


if __name__ == '__main__':
    main()
