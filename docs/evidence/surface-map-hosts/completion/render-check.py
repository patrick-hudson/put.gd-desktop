import argparse
import importlib.util
import json
import subprocess
from pathlib import Path
from playwright.sync_api import sync_playwright
from PIL import Image, ImageDraw, ImageFont

ROOT = Path.cwd()
parser = argparse.ArgumentParser()
parser.add_argument('--output', type=Path, required=True)
OUT = parser.parse_args().output.resolve()
OUT.mkdir(parents=True, exist_ok=True)
if any(OUT.iterdir()):
    raise SystemExit('output must be a fresh empty directory')
BASE = '5985e9043ea81a3ac33d2931203384e40bde035e'
BEFORE = '9cd26ff96a9da6ec3d52ebb770ffeb0a03d44852'
AFTER = '9ad7a2c9085e066407bd1c3f6c05c9d50ea0b9b0'
assert subprocess.check_output(['git', 'rev-parse', 'HEAD'], text=True).strip() == AFTER, 'run from the exact code checkout'
def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module

def export(rev, rel, target):
    (OUT/target).parent.mkdir(parents=True, exist_ok=True)
    (OUT/target).write_bytes(subprocess.check_output(['git', 'show', f'{rev}:{rel}']))

export(BEFORE, 'tools/lab/surface_map/surface_map.html', 'before.html')
export(BASE, 'tools/lab/surface_map/surface_map.py', 'baseline/nightly.py')
export(BASE, 'tools/lab/surface_map/surface_map.html', 'nightly.html')
export(BASE, 'tools/lab/surface_map/model.json', 'nightly-model.json')
current = load('current', ROOT/'tools/lab/surface_map/surface_map.py')
nightly = load('nightly', OUT/'baseline/nightly.py')
model = current.load_model()
entries = [current.make_entry('editor.undo', 'mint-cinnamon-x11', 'deb', functional='passed', visual='reviewed-ok', at='2026-10-29T00:00:00Z'),
           current.make_entry('editor.redo', 'fedora-gnome-wayland', 'rpm', functional='failed', visual='reviewed-wrong', at='2026-10-29T00:00:00Z'),
           current.make_entry('editor.undo', 'neon-kde-wayland', 'deb', functional='failed', at='2026-10-29T00:00:00Z')]
(OUT/'v1.json').write_text(json.dumps({'schemaVersion': 1, 'entries': entries}, indent=2)+'\n')
for tool, m, name, template in ((nightly, nightly.load_model(OUT/'nightly-model.json'), 'nightly-v1', OUT/'nightly.html'),
                               (current, model, 'before-v1', OUT/'before.html'),
                               (current, model, 'after-v1', current.TEMPLATE_PATH)):
    data = tool.build_page_data(m, entries, None, OUT)
    data['generated'] = '2026-10-29T00:00:00Z'
    (OUT/(name+'.html')).write_text(tool.render(data, template))

host_entries = [current.make_entry('editor.undo', 'mint-cinnamon-x11', 'deb', host='f44', placement='guest', functional='passed'),
                current.make_entry('editor.undo', 'ubuntu-gnome-wayland', 'deb', host='f44', placement='guest', guest='golden', functional='failed', receipt='docs/synthetic-ubuntu-receipt.json')]
(OUT/'popover-v2.html').write_text(current.render(current.build_page_data(model, host_entries, None, OUT)))

report = {'after': AFTER, 'nightly': BASE, 'before': BEFORE, 'viewportHeight': 1000, 'browserChecks': [], 'errors': []}
with sync_playwright() as p:
    browser = p.chromium.launch(executable_path='/usr/bin/google-chrome', headless=True)
    report['chromium'] = browser.version
    page = browser.new_page(viewport={'width':1900, 'height':1000}, device_scale_factor=1)
    page.on('pageerror', lambda error: report['errors'].append(str(error)))
    snapshots = {}
    for name in ('nightly-v1', 'after-v1'):
        page.goto((OUT/(name+'.html')).as_uri())
        snapshots[name] = {}
        for desktop in ('All required desktops', 'Mint Cinnamon/X11'):
            page.get_by_role('button', name=desktop, exact=True).click()
            for view in ('Map', 'Matrix', 'Gaps'):
                page.get_by_role('button', name=view, exact=True).click()
                snapshots[name][desktop+'/'+view] = page.locator('#main').evaluate("e=>({text:e.innerText,buttons:[...e.querySelectorAll('button.d')].map(b=>[b.getAttribute('aria-label'),b.style.background]),headers:[...e.querySelectorAll('th')].map(h=>h.innerText)})")
    normalized = json.loads(json.dumps(snapshots['nightly-v1']).replace('Aurora KDE/Wayland (AppImage)', 'Aurora KDE/Wayland'))
    assert normalized == snapshots['after-v1'], 'v1 semantic mismatch'
    report['v1'] = {'matchedNightly': True, 'views': list(snapshots['after-v1']),
                    'normalization': 'Aurora KDE/Wayland (AppImage) -> Aurora KDE/Wayland; planned label change only'}
    (OUT/'v1-snapshots.json').write_text(json.dumps(snapshots, indent=2)+'\n')
    for state in ('before', 'after'):
        page.goto((OUT/(state+'-v1.html')).as_uri())
        page.get_by_role('button', name='Matrix', exact=True).click()
        # Resize while already scrolled: observer must handle both wrapping and unwrapping.
        for width in (1900, 900, 1900):
            page.set_viewport_size({'width':width, 'height':1000})
            page.evaluate('window.scrollTo(0,2200)')
            page.wait_for_function("Math.abs(window.scrollY-2200)<1")
            if state == 'after':
                page.wait_for_function("Math.abs(document.querySelector('header').getBoundingClientRect().bottom-document.querySelector('th').getBoundingClientRect().top)<1")
            geometry = page.evaluate("""() => ({toolbar:document.querySelector('header').getBoundingClientRect().toJSON(),heading:document.querySelector('th').getBoundingClientRect().toJSON(),scrollY:window.scrollY,offset:getComputedStyle(document.querySelector('th')).top})""")
            overlap = geometry['toolbar']['bottom'] - geometry['heading']['top']
            if state == 'before':
                assert overlap > 0, geometry
            else:
                assert abs(overlap) < 1, geometry
                assert page.locator('th').first.evaluate('(e)=>document.elementFromPoint(e.getBoundingClientRect().left+10,e.getBoundingClientRect().top+5)===e')
            name = f'{state}-{width}'
            page.screenshot(path=str(OUT/(name+'-raw.png')))
            report['browserChecks'].append({'state':state,'width':width,'overlapPx':overlap,**geometry})
            im = Image.open(OUT/(name+'-raw.png')).convert('RGB')
            draw = ImageDraw.Draw(im)
            yellow, cyan = '#FFD75E', '#61DFFF'
            for key, color in (('toolbar',yellow),('heading',cyan)):
                box = geometry[key]
                draw.rectangle((max(1,box['left']+1),box['top']+1,min(width-2,box['right']-1),box['bottom']-1), outline=color,width=3)
            font = ImageFont.truetype('/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf',16)
            # Explanations live below the table, without hiding the toolbar/headings.
            y = 800
            draw.rectangle((12,y,width-12,935), fill='#121212',outline='#AAAAA6',width=2)
            draw.text((25,y+12),f'{state.upper()} | {width} x 1000 | scrollY=2200 | synthetic v1 Matrix',font=font,fill='white')
            draw.text((25,y+43),f'Yellow box: toolbar bottom {geometry["toolbar"]["bottom"]:.0f}px',font=font,fill=yellow)
            draw.text((25,y+70),f'Cyan box: heading top {geometry["heading"]["top"]:.0f}px',font=font,fill=cyan)
            draw.text((25,y+97),f'Overlap: {overlap:.0f}px' if state=='before' else 'Headings fully below toolbar; no overlap',font=font,fill='white')
            im.save(OUT/(name+'-annotated.png'))
    page.goto((OUT/'popover-v2.html').as_uri())
    page.locator('#host').select_option('f44')
    page.get_by_role('button', name='Mint Cinnamon/X11', exact=True).click()
    page.get_by_role('button', name='Matrix', exact=True).click()
    page.get_by_role('button', name='Undo on Ubuntu GNOME/Wayland on f44 (VM) · golden: functional failed', exact=True).click()
    popover = page.locator('.pop').inner_text()
    assert 'Mint Cinnamon/X11 on f44 (VM)' in popover, popover
    assert 'Ubuntu GNOME/Wayland on f44 (VM) · golden' in popover, popover
    assert 'docs/synthetic-ubuntu-receipt.json'.split('/')[-1] in popover, popover
    assert 'failed' in popover, popover
    report['matrixPopover'] = {'selectedDesktop':'Mint Cinnamon/X11', 'clickedDesktop':'Ubuntu GNOME/Wayland', 'keepsSelectedAndClickedRows':True, 'text':popover}
    browser.close()
assert not report['errors'], report['errors']
(OUT/'browser-checks.json').write_text(json.dumps(report, indent=2)+'\n')
print(json.dumps(report, indent=2))
