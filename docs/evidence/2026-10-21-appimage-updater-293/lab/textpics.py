"""Terminal captures and the memory chart for the #293 phase 3 evidence, with the key lines called out."""
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

OUT = Path(__file__).resolve().parent / 'out'
MONO = ImageFont.truetype('/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf', 18)
BOLD = ImageFont.truetype('/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf', 20)
RED, BG, FG, DIM = (220, 30, 40), (24, 26, 31), (225, 228, 235), (150, 156, 168)


def terminal(name, title, lines, marks, footer):
    """lines: text lines; marks: {line index: label} boxes those lines in red with a label at the right."""
    width, step = 1700, 26
    height = 70 + step * len(lines) + 50
    image = Image.new('RGB', (width, height), BG); draw = ImageDraw.Draw(image)
    draw.text((20, 18), title, fill=FG, font=BOLD)
    for i, line in enumerate(lines):
        y = 60 + i * step
        draw.text((24, y), line, fill=FG if not line.startswith('#') else DIM, font=MONO)
        if i in marks:
            w = draw.textlength(line, font=MONO)
            draw.rectangle((16, y - 3, 32 + w, y + step - 3), outline=RED, width=3)
            label = marks[i]; lw = draw.textlength(label, font=BOLD)
            x = min(44 + w, width - lw - 24)
            draw.rectangle((x, y - 3, x + lw + 12, y + step - 3), fill=RED)
            draw.text((x + 6, y - 1), label, fill='white', font=BOLD)
    draw.text((20, height - 36), footer, fill=DIM, font=MONO)
    image.save(OUT / name, optimize=True)


def mirror():
    T = 'v0.1.1-nightly.874'; N = 'putgd-0.1.1-nightly.874-x86_64.AppImage'
    lines = [
        f'$ curl -sI https://pkg.put.gd/releases/{T}/{N}',
        'HTTP/2 200', 'content-type: application/vnd.appimage', 'content-length: 115124728',
        'cache-control: public, max-age=31536000, immutable',
        f"$ curl -s -D - -H 'Range: bytes=0-99,1048576-1048675,100000000-100000099' .../{N}",
        'HTTP/2 206', 'content-type: multipart/byteranges; boundary=CloudFront:F104602D039954DAD485D293F526EF63   (3 parts)',
        f'$ curl -s https://pkg.put.gd/releases/{T}/{N} | sha256sum',
        '9e3071157f47bb72deea81da3f3d132ea7121be6359f5f69122c1c5d3f959b7e   = GitHub SHA256SUMS line for the image',
        '$ sha256sum releases/.../putgd-0.1.1-nightly.874-x86_64.AppImage.zsync  dl/nightly/appimage.zsync',
        'ae98edc4b5e0f2dbccb7a082f055c79556577585d1c00f5c741d95b7014cf755  release .zsync',
        'ae98edc4b5e0f2dbccb7a082f055c79556577585d1c00f5c741d95b7014cf755  dl/nightly/appimage.zsync (max-age=300)',
    ]
    terminal('mirror-first-nightly-874.png', 'pkg.put.gd mirror: the first mirrored nightly, v0.1.1-nightly.874 (signer dc1ab32, #342)', lines,
             {4: 'immutable', 6: 'multi-range 206', 9: 'same bytes as GitHub', 12: 'permalink = release .zsync'},
             'Checked 2026-10-21 22:24Z from the workstation. Full output: results/mirror-nightly-874.txt')


def permalink():
    lines = [
        '# before put.gd-site#4 (22:24Z)',
        '$ curl -sI https://app.put.gd/dl/nightly/appimage.zsync',
        'HTTP/2 302', 'location: https://github.com/patrick-hudson/put.gd-desktop/releases/download/v0.1.1-nightly.874/...AppImage.zsync',
        '# after the apply (Apply complete! 0 added, 1 changed, 0 destroyed) and merge 11d6cb1',
        '$ curl -sI https://app.put.gd/dl/nightly/appimage.zsync',
        'HTTP/2 200', 'content-type: application/x-zsync', 'cache-control: public, max-age=300',
        'sha256 ae98edc4b5e0f2dbccb7a082f055c79556577585d1c00f5c741d95b7014cf755  (= pkg.put.gd/dl/nightly/appimage.zsync)',
        '/dl/stable/appimage.zsync -> 404     /dl/nightly/appimage -> 302 (router unchanged)     /update/v1/nightly.json -> 200',
        '$ terraform plan  (main 11d6cb1)',
        'No changes. Your infrastructure matches the configuration.',
    ]
    terminal('permalink-site-4.png', 'app.put.gd/dl/<channel>/appimage.zsync served from the signer bucket (put.gd-site#4)', lines,
             {2: 'redirect (before)', 6: 'direct, no redirect (after)', 12: 'clean plan'},
             'Checked 2026-10-21 22:30-22:35Z. Evidence: put.gd-site#4 comment.')


def memory():
    rows = [('m74 Cinnamon X11', 29.22, 29.33, 29.36), ('k72 Plasma 6.6 Wayland', 29.66, 29.86, 29.95)]
    width, height = 1500, 590
    image = Image.new('RGB', (width, height), 'white'); draw = ImageDraw.Draw(image)
    draw.text((30, 20), 'Idle resident RSS, median of 180 s (MiB): nightly 874 vs lab 886 (#343 head 04aff235)', fill=(20, 20, 20), font=BOLD)
    colours = [((120, 130, 150), 'nightly 874, cold idle'), ((31, 100, 214), 'lab 886, cold idle'), ((76, 195, 138), 'lab 886, idle after a full install')]
    base, scale, top = 470, 12.0, 24.0
    for i, (label, *values) in enumerate(rows):
        x0 = 120 + i * 640
        for j, v in enumerate(values):
            x = x0 + j * 150; y = base - (v - top) * (base - 120) / 7.0
            draw.rectangle((x, y, x + 110, base), fill=colours[j][0])
            draw.text((x + 18, y - 26), f'{v:.2f}', fill=(20, 20, 20), font=BOLD)
        draw.text((x0, base + 12), label, fill=(20, 20, 20), font=BOLD)
    for j, (c, name) in enumerate(colours):
        draw.rectangle((40 + j * 440, 70, 60 + j * 440, 90), fill=c); draw.text((68 + j * 440, 68), name, fill=(20, 20, 20), font=MONO)
    draw.text((30, base + 50), 'Axis from 24 MiB. 886 vs 874 at idle: +0.11 MiB (m74), +0.20 MiB (k72); after an install: +0.03 / +0.09 more.',
              fill=(60, 60, 60), font=MONO)
    draw.text((30, base + 76), 'No libssl, libcrypto or Qt Network is mapped in any window. Load <= 0.1 during the windows (quiet slot).',
              fill=(60, 60, 60), font=MONO)
    image.save(OUT / 'memory-idle-874-vs-886.png', optimize=True)


if __name__ == '__main__':
    OUT.mkdir(exist_ok=True); mirror(); permalink(); memory(); print('ok')
