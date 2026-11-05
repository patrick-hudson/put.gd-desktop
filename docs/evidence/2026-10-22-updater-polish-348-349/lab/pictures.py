"""Annotated pictures for the #348/#349 lab acceptance: red boxes on what changed, a label, and a footer naming host and build."""
import json, sys
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

R = Path(sys.argv[1])      # collected results: <host>/<run>/frames/*.png and *.json
OUT = Path(sys.argv[2])
BOLD = lambda size: ImageFont.truetype('/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf', size)
MONO = ImageFont.truetype('/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf', 17)
RED, GREEN, DARK = (220, 30, 40), (24, 140, 70), (20, 20, 20)
BEFORE = 'lab 0.1.1-nightly.9000 (nightly 33c768ec, without #348/#349)'
AFTER = 'lab 0.1.1-nightly.9001 (#358 c18ca495; PR head 047b80ec has the same src)'
UPDATED = 'lab 0.1.1-nightly.9002 after Restart put.gd (updated from 9001)'
HOSTS = {'aurora': 'Aurora 44 Plasma 6.7 Wayland, 1280x800 (VM on f44)', 'f44': 'f44 Fedora 44 GNOME 50 Wayland, 1920x1080',
         'm74': 'm74 Linux Mint 22.3 Cinnamon 6.6 X11, 1920x1080'}


def label(draw, box, text, colour, font, side='above'):
    w = draw.textlength(text, font=font); h = font.size + 10
    x0, y0, x1, y1 = box
    x, y = (x0, y0 - h - 6) if side == 'above' else (x0, y1 + 6)
    x = max(4, min(x, draw.im.size[0] - w - 16))
    draw.rectangle((x, y, x + w + 12, y + h), fill=colour)
    draw.text((x + 6, y + 4), text, fill='white', font=font)


def frame(source, marks, footer):
    image = Image.open(source).convert('RGB'); draw = ImageDraw.Draw(image)
    font = BOLD(max(16, image.size[0] // 70))
    for box, text, colour, side in marks:
        draw.rectangle(box, outline=colour, width=4); label(draw, box, text, colour, font, side)
    small = BOLD(max(14, image.size[0] // 100)); w = draw.textlength(footer, font=small)
    draw.rectangle((0, image.size[1] - small.size - 14, w + 16, image.size[1]), fill=DARK)
    draw.text((8, image.size[1] - small.size - 8), footer, fill='white', font=small)
    return image


def side_by_side(left, right, name, scale=0.75):
    images = [i.resize((int(i.size[0] * scale), int(i.size[1] * scale)), Image.LANCZOS) for i in (left, right)]
    out = Image.new('RGB', (images[0].size[0] + images[1].size[0] + 16, max(i.size[1] for i in images)), 'white')
    out.paste(images[0], (0, 0)); out.paste(images[1], (images[0].size[0] + 16, 0))
    out.save(OUT / name, optimize=True); print(name, out.size)


def save(image, name):
    image.save(OUT / name, optimize=True); print(name, image.size)


def status_json(host):
    """update.status install.result.zsync for the before and after runs, as putgd-cli printed it."""
    lines, marks = [], {}
    for run, build in ((f'{host}-before', BEFORE), (f'{host}-after', AFTER)):
        install = json.loads((R / host / run / 'install.json').read_text())['install']['install']
        zsync = install['result']['zsync']
        text = json.dumps({'output': zsync['output']}, ensure_ascii=True)
        lines.append(f'# {build}: putgd-cli update.status -> install.result.zsync (state {install["state"]}, method {install["result"]["method"]})')
        lines.append(f'"tool": "{zsync["tool"]}", "used": {json.dumps(zsync["used"])},')
        chunks = [text[i:i + 150] for i in range(0, len(text), 150)]
        first = len(lines)
        lines.extend(chunks); lines.append('')
        marks[first] = run.endswith('before')
    width, step = 1720, 25
    image = Image.new('RGB', (width, 70 + step * len(lines) + 46), (24, 26, 31)); draw = ImageDraw.Draw(image)
    draw.text((20, 18), f'#349: zsync.output in update.status, before and after ({HOSTS[host]})', fill=(225, 228, 235), font=BOLD(20))
    for i, line in enumerate(lines):
        y = 60 + i * step
        draw.text((24, y), line, fill=(150, 156, 168) if line.startswith('#') else (225, 228, 235), font=MONO)
    for first, before in marks.items():
        text = lines[first]
        if before:
            at = text.index('\\u001b[2K\\r'); x = 24 + draw.textlength(text[:at], font=MONO); w = draw.textlength('\\u001b[2K\\r', font=MONO)
            box = (x - 4, 60 + first * step - 3, x + w + 4, 60 + (first + 1) * step - 3)
            draw.rectangle(box, outline=RED, width=3); label(draw, (box[0], box[3], box[2], box[3]), 'escape + carriage return kept (9000)', RED, BOLD(18), 'below')
        else:
            at = text.index('100.00%'); x = 24 + draw.textlength(text[:at], font=MONO); w = draw.textlength('100.00% done (109.89 of 109.89 MiB)...', font=MONO)
            box = (x - 4, 60 + first * step - 3, x + w + 4, 60 + (first + 1) * step - 3)
            draw.rectangle(box, outline=GREEN, width=3); label(draw, (box[0], box[3], box[2], box[3]), 'clean final state (9001)', GREEN, BOLD(18), 'below')
    draw.text((20, image.size[1] - 34), 'Full JSON: results/' + host + '-before/install.json and results/' + host + '-after/install.json', fill=(150, 156, 168), font=MONO)
    save(image, f'{host}-349-zsync-output-before-after.png')


def f(host, run, name): return R / host / run / 'frames' / f'{name}.png'


def footer(host, build): return f'{HOSTS[host]} - {build}'


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    hosts = [h for h in HOSTS if (R / h).exists()]
    # Aurora: the #348 bug shows on its 1280x800 screen; the fix lands on Status from the same single ui.show.
    if 'aurora' in hosts:
        side_by_side(
            frame(f('aurora', 'aurora-before', 'first-show'), [((238, 510, 1234, 676), '9000: first ui.show, Status below the fold', RED, 'above')], footer('aurora', BEFORE)),
            frame(f('aurora', 'aurora-after', 'first-show'), [((240, 588, 1232, 630), '9001: one ui.show lands on Status', GREEN, 'above')], footer('aurora', AFTER)),
            'aurora-348-first-show-before-after.png')
        save(frame(f('aurora', 'aurora-after', 'restarted'), [((240, 553, 1232, 632), 'After Restart put.gd: one ui.show, Status and Install in view', GREEN, 'above')],
                   footer('aurora', UPDATED)), 'aurora-348-after-restart.png')
    if 'f44' in hosts:
        save(frame(f('f44', 'f44-after', 'first-show'), [((538, 785, 1574, 822), '9001: one ui.show lands on Updates\' Status', GREEN, 'above')], footer('f44', AFTER)),
             'f44-348-first-show.png')
        save(frame(f('f44', 'f44-after', 'restarted'), [((538, 785, 1574, 868), 'After Restart put.gd: one ui.show, Status and Install in view', GREEN, 'above')],
                   footer('f44', UPDATED)), 'f44-348-after-restart.png')
    if 'm74' in hosts:
        save(frame(f('m74', 'm74-after', 'first-show'), [((290, 730, 1324, 772), '9001: one ui.show lands on Updates\' Status', GREEN, 'above')], footer('m74', AFTER)),
             'm74-348-first-show.png')
        save(frame(f('m74', 'm74-after', 'restarted'), [((290, 728, 1324, 812), 'After Restart put.gd: one ui.show, Status and Install in view', GREEN, 'above')],
                   footer('m74', UPDATED)), 'm74-348-after-restart.png')
    for host in hosts:
        status_json(host)
    if (R / 'm74' / 'm74-mb').exists():
        memory()


def memory():
    """Idle RSS medians on m74: lab 9000 (base) against lab 9001 (PR), cold and after an update.check."""
    load = lambda tag: json.loads((R / 'm74' / f'm74-{tag}' / 'mem.json').read_text())['mem']
    base, head = load('mb'), load('mh')
    rows = [('cold idle', base['A']['rssMedianMiB'], head['A']['rssMedianMiB']), ('idle after update.check', base['B']['rssMedianMiB'], head['B']['rssMedianMiB'])]
    image = Image.new('RGB', (1300, 560), 'white'); draw = ImageDraw.Draw(image)
    draw.text((30, 20), 'Idle resident RSS on m74 Cinnamon X11, median of 180 s (MiB), axis from 24 MiB', fill=DARK, font=BOLD(22))
    colours = [((120, 130, 150), 'lab 9000 (nightly 33c768ec, without #348/#349)'), ((31, 100, 214), 'lab 9001 (#358)')]
    for j, (c, name) in enumerate(colours):
        draw.rectangle((40 + j * 600, 66, 60 + j * 600, 86), fill=c); draw.text((70 + j * 600, 64), name, fill=DARK, font=MONO)
    floor, top, scale = 450, 130, (450 - 130) / 7.0
    for i, (name, a, b) in enumerate(rows):
        x0 = 140 + i * 560
        for j, v in enumerate((a, b)):
            x = x0 + j * 170; y = floor - (v - 24) * scale
            draw.rectangle((x, y, x + 130, floor), fill=colours[j][0]); draw.text((x + 22, y - 30), f'{v:.2f}', fill=DARK, font=BOLD(20))
        delta = b - a
        draw.text((x0, floor + 12), f'{name}: {delta:+.2f} MiB', fill=DARK, font=BOLD(20))
    draw.text((30, floor + 60), f"Pss {base['B']['pssMiB']:.2f} -> {head['B']['pssMiB']:.2f} MiB after a check; no libssl, libcrypto or Qt Network mapped; load <= 0.11.", fill=(60, 60, 60), font=MONO)
    save(image, 'm74-memory-idle-9000-vs-9001.png')


if __name__ == '__main__':
    main()
