#!/usr/bin/env python3
"""Deterministic synthetic OCR fixtures (Pillow and distro zint required).

Run on a builder: python3 generate.py --output /private/evidence/synthetic
Generated credentials are shape fixtures only, never issued credentials.
Images/transcripts/ground truth are evidence, not repository inputs.
"""
import argparse
import base64
import hashlib
import json
from pathlib import Path
import subprocess
import textwrap

PROSE = ('The garden path follows the river through a quiet valley. Each morning '
         'the team checks the display and reads the sample aloud. Clear letters '
         'and careful spacing help readers compare the original page with the result. ')
FONTS = ('DejaVuSans.ttf', 'DejaVuSerif.ttf', 'DejaVuSansMono.ttf', 'LiberationSans-Regular.ttf')


def card(prefix='999999000000000'):
    for last in range(10):
        value = prefix + str(last)
        digits = list(map(int, value[::-1]))
        total = sum(d if i % 2 == 0 else (2*d if d < 5 else 2*d-9) for i, d in enumerate(digits))
        if total % 10 == 0:
            return value
    raise AssertionError('Luhn')


def sensitive():
    # Construct the synthetic shapes, avoiding literal credential-like source strings.
    run = lambda c, n: c * n
    jwt = '.'.join(base64.urlsafe_b64encode(x).decode().rstrip('=') for x in
                   (b'{"alg":"none"}', b'{"sub":"synthetic-invalid"}', b'not-a-signature'))
    return [('email', 'corpus@example.invalid'), ('phone', '+1 202 555 0142'),
            ('phone', '020 7946 0958'), ('address', '192.0.2.42'), ('address', '2001:db8::42'),
            ('card', card()), ('token', 'AK' + 'IA' + run('A', 16)),
            ('token', 'gh' + 'p_' + run('a', 36)), ('token', 'gl' + 'pat-' + run('b', 20)),
            ('token', 'xo' + 'xb-' + '000000000000-' * 2 + run('c', 24)),
            ('token', 'sk_' + 'test_' + run('d', 24)), ('token', jwt),
            ('token', '-----BEGIN ' + 'PRIVATE KEY-----\n' + base64.b64encode(b'synthetic invalid key material'*3).decode() + '\n-----END PRIVATE KEY-----'),
            ('token', hashlib.sha256(b'synthetic corpus').hexdigest()),
            ('token', base64.b64encode(b'synthetic long run shape fixture').decode())]


def generate(out):
    from PIL import Image, ImageDraw, ImageFont, ImageOps
    out.mkdir(mode=0o700, parents=True, exist_ok=False)
    manifest = dict(schemaVersion=1, kind='synthetic; not real-target desktop evidence', captures=[], codes=[])
    font_paths = {}
    for name in FONTS:
        matches = list(Path('/usr/share/fonts').rglob(name))
        if not matches:
            raise RuntimeError('Install distribution font: ' + name)
        font_paths[name] = matches[0]
    def page(name, lines, size=14, font=FONTS[0], dark=False, dimensions=(2200, 1400), ink=None):
        image = Image.new('RGB', dimensions, '#202124' if dark else 'white')
        draw = ImageDraw.Draw(image)
        face = ImageFont.truetype(str(font_paths[font]), size)
        y, truth = 12, []
        for category, text in lines:
            for line in text.split('\n'):
                box = draw.textbbox((12, y), line, font=face)
                if box[2] >= dimensions[0] or box[3] >= dimensions[1]:
                    raise ValueError('Fixture text clipped: ' + name)
                draw.text((12, y), line, font=face, fill=ink or ('white' if dark else 'black'))
                truth.append(dict(category=category, text=line, box=list(box)))
                y += size + 7
        image.save(out / (name + '.png'))
        transcript = '\n'.join(t['text'] for t in truth)
        (out / (name + '.codepoints.json')).write_text(json.dumps(list(map(ord, transcript + '\n'))))
        encoded_truth=[dict(category=t['category'], text_codepoints=list(map(ord,t['text'])), box=t['box']) for t in truth]
        (out / (name + '.truth.json')).write_text(json.dumps(dict(words=encoded_truth, font=font, font_pixels=size,
                         font_sha256=hashlib.sha256(font_paths[font].read_bytes()).hexdigest()), indent=2))
        manifest['captures'].append(dict(id=name, **{'class': name.split('-')[0]}, image=name+'.png',
                    transcript_codepoints=name+'.codepoints.json', font_pixels=size, dimensions=list(dimensions),
                    urls=[], emails=[t['text'] for t in truth if t['category']=='email']))
        return image
    for font in FONTS:
        for dark in (False, True):
            for size in (11, 14, 20):
                page(f'redact-{font}-{size}-{dark}', sensitive(), size, font, dark)
    page('clean-prose', [('clean', line) for line in textwrap.wrap(PROSE * 8, 120)])
    page('clean-code', [('clean', line) for line in ('def total(values):\n    result = 0\n    for value in values:\n        result += value\n    return result\n' * 8).splitlines()])
    page('hashes', [('token', hashlib.sha256(f'synthetic {i}'.encode()).hexdigest()) for i in range(30)])
    page('hidden-near-white', [('excluded', 'Hidden text should never be copied')], ink='#fefefe')
    page('hidden-tiny', [('excluded', 'Tiny letters should never be copied')], size=3)
    hostile = 'prefix\x01\x7f\x85\u202eevil\u202c\u200b\u200c\u200d\u2066suffix\u2069'
    page('hostile', [('sanitized', hostile)])
    (out/'hostile.expected.json').write_text(json.dumps(dict(raw=hostile, text='prefixevil\u200c\u200dsuffix', code='prefixevilsuffix')))
    # Dense limit fixtures have exact line truth. A vertical ink rail guarantees
    # no entirely blank row, including the final quarter of every possible band.
    for name, dims in [('limit-24mp', (6000,4000)), ('limit-over', (6000,4001)),
                       ('limit-wide', (240000,100)), ('limit-no-gap',(4000,5000)), ('limit-columns',(1200,18000))]:
        image = Image.new('RGB', dims, 'white'); draw = ImageDraw.Draw(image)
        face = ImageFont.truetype(str(font_paths[FONTS[2]]), 14)
        truth = []
        for y in range(8, dims[1]-24, 20):
            for x in range(8, dims[0]-440, 440 if name != 'limit-columns' else 600):
                text = f'Row {y:05d} column {x:06d} synthetic text.'
                draw.text((x,y), text, font=face, fill='black')
                truth.append(dict(text=text, box=list(draw.textbbox((x,y),text,font=face))))
        if name == 'limit-no-gap': draw.line((0,0,0,dims[1]-1), fill='black')
        image.save(out/(name+'.png'))
        (out/(name+'.truth.json')).write_text(json.dumps(dict(dimensions=dims, words=truth,
                              expected='input_excessive' if name in ('limit-over','limit-wide') else 'banded')))
    payloads = dict(url=b'https://example.invalid/corpus', wifi=b'WIFI:T:WPA;S:Synthetic lab;P:not-a-real-network;;',
                    contact=b'BEGIN:VCARD\nVERSION:3.0\nFN:Synthetic Fixture\nEND:VCARD', plain=b'Synthetic text',
                    binary=bytes([0,255,128,1,2]), hostile=hostile.encode())
    # zint symbols: 58 QR, 97 Micro QR, 71 Data Matrix, 92 Aztec.
    for kind, sym in [('qr',58), ('micro',97), ('datamatrix',71), ('aztec',92)]:
        for payload_name, payload in payloads.items():
            if kind == 'micro' and len(payload) > 15: continue
            for version in ((list(range(1,11))+[40] if payload_name == 'plain' else [10]) if kind == 'qr' else [None]):
                if kind == 'qr' and version == 1 and len(payload) > 14: continue
                for module in ((3, 5, 16) if version == 40 else (3, 5)):
                    name=f'code-{kind}-{payload_name}-v{version}-m{module}'
                    source=out/(name+'.bin'); source.write_bytes(payload)
                    image_path=out/(name+'.png')
                    cmd=['zint','-b',str(sym),'--binary','--scale',str(module/2),'--whitesp','4',
                         '-i',str(source),'-o',str(image_path)]
                    if version is not None: cmd += ['--vers',str(version),'--secure','2']
                    run=subprocess.run(cmd,capture_output=True)
                    if run.returncode: raise RuntimeError(name+': '+run.stderr.decode())
                    manifest['codes'].append(dict(image=image_path.name, format=kind, version=version,
                           module_pixels=module, payload_hex=payload.hex(), kind='binary' if payload_name=='binary' else 'text'))
                    im=Image.open(image_path).convert('RGB')
                    im.rotate(17,expand=True,fillcolor='white').save(out/(name+'-rotated.png'))
                    ImageOps.invert(im).save(out/(name+'-dark.png'))
                    base_entry=manifest['codes'][-1]
                    for suffix in ('-rotated','-dark'):
                        variant=dict(base_entry,image=name+suffix+'.png')
                        manifest['codes'].append(variant)
    qr_images=[out/c['image'] for c in manifest['codes'] if c['format']=='qr'][:4]
    busy=page('codes-busy', [('body',line) for line in textwrap.wrap(PROSE*8,120)])
    for i,path in enumerate(qr_images): busy.paste(Image.open(path).convert('RGB'), (100+i*300,300))
    busy.save(out/'codes-multiple-busy.png')
    (out/'manifest.json').write_text(json.dumps(manifest, indent=2)+'\n')


if __name__ == '__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output',type=Path,required=True)
    generate(p.parse_args().output)
