"""Run from the code checkout; requires Playwright, Pillow and distro Chrome."""
import argparse
import gzip
import hashlib
import importlib.util
import json
import platform
import socket
import subprocess
from pathlib import Path
from playwright.sync_api import sync_playwright
from PIL import Image, ImageDraw, ImageFont

parser = argparse.ArgumentParser()
parser.add_argument('--output', type=Path, required=True)
OUT = parser.parse_args().output.resolve()
OUT.mkdir(parents=True, exist_ok=True)
assert not any(OUT.iterdir()), 'use a fresh output directory'
ROOT = Path.cwd()
BEFORE = '9ad7a2c9085e066407bd1c3f6c05c9d50ea0b9b0'
AFTER = 'bd22544cdbd66e2f6f2d0831ff4a6d3adc8fcec1'
BASE = '5985e9043ea81a3ac33d2931203384e40bde035e'
assert subprocess.check_output(['git', 'rev-parse', 'HEAD'], text=True).strip() == AFTER

def export(rev, source, name):
    target = OUT / name
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(subprocess.check_output(['git', 'show', f'{rev}:{source}']))
    return target

def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module

current = load('current', ROOT / 'tools/lab/surface_map/surface_map.py')
nightly = load('nightly', export(BASE, 'tools/lab/surface_map/surface_map.py', 'baseline/nightly.py'))
nightly_model = nightly.load_model(export(BASE, 'tools/lab/surface_map/model.json', 'nightly-model.json'))
nightly_template = export(BASE, 'tools/lab/surface_map/surface_map.html', 'nightly-template.html')
before_template = export(BEFORE, 'tools/lab/surface_map/surface_map.html', 'before-template.html')
model = current.load_model()
entries = [current.make_entry('editor.undo', 'mint-cinnamon-x11', 'deb', functional='passed', visual='reviewed-ok', at='2026-10-29T00:00:00Z'),
           current.make_entry('editor.redo', 'fedora-gnome-wayland', 'rpm', functional='failed', visual='reviewed-wrong', at='2026-10-29T00:00:00Z'),
           current.make_entry('editor.undo', 'neon-kde-wayland', 'deb', functional='failed', at='2026-10-29T00:00:00Z')]
(OUT / 'v1.json').write_text(json.dumps({'schemaVersion': 1, 'entries': entries}, indent=2) + '\n')
for tool, m, name, template in ((nightly, nightly_model, 'nightly', nightly_template),
                               (current, model, 'before', before_template),
                               (current, model, 'after', current.TEMPLATE_PATH)):
    data = tool.build_page_data(m, entries, None, OUT, label='surface map validator fixes')
    data['generated'] = '2026-10-29T00:00:00Z'
    (OUT / (name + '.html')).write_text(tool.render(data, template))
report = {'before': BEFORE, 'after': AFTER, 'nightly': BASE, 'host': socket.gethostname(),
          'platform': platform.platform(), 'session': 'isolated headless Chromium, synthetic offline HTML',
          'viewportHeight': 1000, 'measurements': [], 'errors': []}
with sync_playwright() as p:
    browser = p.chromium.launch(executable_path='/usr/bin/google-chrome', headless=True)
    report['chromium'] = browser.version
    page = browser.new_page(viewport={'width': 1600, 'height': 1000}, device_scale_factor=1)
    page.on('pageerror', lambda error: report['errors'].append(str(error)))
    snapshots = {}
    for name in ('nightly', 'after'):
        page.goto((OUT / (name + '.html')).as_uri())
        snapshots[name] = {}
        for desktop in ('All required desktops', 'Mint Cinnamon/X11'):
            page.get_by_role('button', name=desktop, exact=True).click()
            for view in ('Map', 'Matrix', 'Gaps'):
                page.get_by_role('button', name=view, exact=True).click()
                snapshots[name][desktop + '/' + view] = page.locator('#main').evaluate("e=>({text:e.innerText,buttons:[...e.querySelectorAll('button.d')].map(b=>[b.getAttribute('aria-label'),b.style.background]),headers:[...e.querySelectorAll('th')].map(h=>h.innerText)})")
    normalized = json.loads(json.dumps(snapshots['nightly']).replace('Aurora KDE/Wayland (AppImage)', 'Aurora KDE/Wayland'))
    assert normalized == snapshots['after'], 'v1 semantic mismatch'
    report['v1'] = {'matchedNightly': True, 'comparisons': list(snapshots['after']),
                    'normalization': 'Planned Aurora label removes (AppImage); no other normalization'}
    with gzip.open(OUT / 'v1-snapshots.json.gz', 'wt') as f:
        json.dump(snapshots, f, indent=2)
    for state in ('before', 'after'):
        page.goto((OUT / (state + '.html')).as_uri())
        # Stay in Map view; resize the same document while it is already scrolled.
        for index, width in enumerate((520, 760, 1600, 520, 1600)):
            page.set_viewport_size({'width': width, 'height': 1000})
            # Let resize layout/anchoring settle before fixing the scroll witness.
            page.evaluate('new Promise(resolve => requestAnimationFrame(() => requestAnimationFrame(resolve)))')
            page.evaluate('window.scrollTo(0,1600)')
            page.wait_for_function('window.scrollY === 1600')
            page.wait_for_function("Math.abs(parseFloat(getComputedStyle(document.documentElement).getPropertyValue('--toolbar-height'))-document.querySelector('header').getBoundingClientRect().height)<0.1")
            if state == 'after':
                page.wait_for_function("Math.abs(document.querySelector('header').getBoundingClientRect().bottom-document.querySelector('.col').getBoundingClientRect().top)<0.1")
            geometry = page.evaluate("""() => ({toolbar:document.querySelector('header').getBoundingClientRect().toJSON(),column:document.querySelector('.col').getBoundingClientRect().toJSON(),scrollY:scrollY,offset:getComputedStyle(document.querySelector('.col')).top,maxHeight:getComputedStyle(document.querySelector('.col')).maxHeight})""")
            overlap = geometry['toolbar']['bottom'] - geometry['column']['top']
            assert overlap > 0 if state == 'before' else abs(overlap) < 0.1, geometry
            if state == 'after':
                assert geometry['column']['bottom'] <= 1000 - 12 + 0.1
                assert page.locator('.col').evaluate('(e)=>e.contains(document.elementFromPoint(e.getBoundingClientRect().left+10,e.getBoundingClientRect().top+10))')
            report['measurements'].append({'state': state, 'width': width, 'index': index,
                                           'overlapPx': overlap, **geometry})
            if index >= 3:
                continue
            name = f'{state}-{width}'
            page.screenshot(path=str(OUT / (name + '-raw.png')))
            raw = Image.open(OUT / (name + '-raw.png')).convert('RGB')
            im = Image.new('RGB', (width, 1155), '#121212')
            im.paste(raw, (0, 0))
            draw = ImageDraw.Draw(im)
            yellow, cyan = '#FFD75E', '#61DFFF'
            # Draw exact top/bottom witness lines, without covering source content.
            for key, color in (('toolbar', yellow), ('column', cyan)):
                box = geometry[key]
                draw.rectangle((max(1, box['left']+1), box['top']+1,
                                min(width-2, box['right']-1), min(999, box['bottom']-1)), outline=color, width=3)
            font = ImageFont.truetype('/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf', 14)
            lines = [(f'{state.upper()} | Map | {width}x1000 | scrollY=1600', '#F0F0EC'),
                     (f'Yellow toolbar bottom: {geometry["toolbar"]["bottom"]:.2f}px', yellow),
                     (f'Cyan column top: {geometry["column"]["top"]:.2f}px | overlap: {overlap:.2f}px', cyan),
                     (f'Source: {(BEFORE if state == "before" else AFTER)[:12]}', '#F0F0EC'),
                     (f'Headless Chromium {browser.version}', '#F0F0EC'),
                     ('Synthetic page evidence; no app or desktop acceptance.', '#AAAAA6')]
            for n, (line, color) in enumerate(lines):
                draw.text((10, 1008 + n*23), line, font=font, fill=color)
            im.save(OUT / (name + '-annotated.png'))
    assert not report['errors'], report['errors']
    browser.close()
report['sha256'] = {f.name: hashlib.sha256(f.read_bytes()).hexdigest()
                    for f in sorted(OUT.glob('*-annotated.png'))}
(OUT / 'browser-checks.json').write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps({'v1Comparisons': 6, 'geometryChecks': len(report['measurements']), 'errors': report['errors'],
                  'afterOverlapPx': [m['overlapPx'] for m in report['measurements'] if m['state']=='after']}))
