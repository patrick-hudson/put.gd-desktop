"""Annotate the #386 frames: a red box around what changed, a label naming it, and a footer naming the
desktop and the exact lab build (PR #428 head 434f17a6 plus the lab-key commit 63bfed0d and the lab
version commits). Frames are the full desktops from the hosts' wall VNC servers.
  annotate386.py FRAMES OUT      FRAMES/{m74,f44}/<frame>.png
"""
import sys
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

FRAMES, OUT = Path(sys.argv[1]), Path(sys.argv[2])
FONT = ImageFont.truetype('/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf', 22)
SMALL = ImageFont.truetype('/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf', 17)
RED = (220, 30, 40)
HOSTS = {'m74': 'm74 Linux Mint 22.3 Cinnamon X11, ubuntu24.04 DEB', 'f44': 'f44 Fedora 44 GNOME Wayland, fedora44 RPM'}
LAB = 'PR #428 434f17a6 + lab-key commit 63bfed0d, pkg.put.gd/lab'
N = 'putgd 26.1019.988-nightly (6eace23e)'
S1 = 'putgd 26.1018.989 (734c9c7b)'
S2 = 'putgd 26.1020.990 (b944c49d)'
N1 = 'putgd 26.1021.991-nightly (b8ce1446)'

# (output, host, frame, [(box, label, label xy)], build shown)
PICTURES = [
    ('m74-1-nightly-repository.png', 'm74', '1-nightly-on-nightly-max',
     [((388, 634, 1860, 696), 'Channel follows the system\'s repository; shown, not chosen', (760, 588)),
      ((388, 756, 1860, 882), 'Other channel: the terminal commands, with Copy', (640, 892)),
      ((10, 982, 132, 1002), 'This build', (140, 978))], N),
    ('m74-2-stable-repository-older.png', 'm74', '2-stable-repo-older',
     [((396, 634, 1860, 796), 'Moved to stable by hand: stable 26.1018.989 is older, so apt keeps the nightly; this command installs it now', (396, 590))], N),
    ('m74-3-stable-installed.png', 'm74', '3-stable-installed-max',
     [((388, 634, 1860, 740), 'After `apt install --allow-downgrades putgd/stable`: stable 26.1018.989 from the stable repository', (388, 590)),
      ((10, 982, 120, 1002), 'This build', (130, 978))], S1),
    ('m74-4-update-manager.png', 'm74', '5-update-manager-putgd',
     [((6, 480, 1912, 518), 'Update Manager offers putgd 26.1020.990 from the lab stable repository', (300, 540))], S1 + ' installed'),
    ('m74-5-back-on-nightly.png', 'm74', '7-back-on-nightly-max',
     [((388, 634, 1860, 740), 'Back on nightly 26.1021.991 with the copied "to nightly" commands', (760, 590))], N1),
    ('m74-6-repository-disabled.png', 'm74', '8-repo-disabled',
     [((352, 634, 1860, 670), 'Repository disabled by hand (Enabled: no): the choice is back, and no instructions', (420, 590))], N1),
    ('f44-1-nightly-repository.png', 'f44', '1-nightly-on-nightly-max',
     [((388, 672, 1860, 734), 'Channel follows /etc/yum.repos.d/putgd.repo; shown, not chosen', (760, 626)),
      ((388, 794, 1860, 908), 'Other channel: the dnf commands, with Copy', (640, 914)),
      ((10, 1022, 132, 1042), 'This build', (140, 1018))], N1),
    ('f44-2-stable-repository-older.png', 'f44', '2-stable-repo-older',
     [((396, 672, 1860, 834), 'Moved to stable by hand: 26.1020.990 is older, so dnf keeps the nightly; this command installs it now', (396, 626))], N1),
    ('f44-3-stable-update-available.png', 'f44', '3-stable-989-update-available',
     [((696, 736, 1560, 900), 'On stable 26.1018.989: 26.1020.990 arrives from the stable repository', (700, 690))], S1),
    ('f44-4-gnome-software.png', 'f44', '4-gs-fine6-2',
     [((694, 700, 1228, 754), 'GNOME Software (System Updates): putgd 26.1018.989 -> 26.1020.990', (600, 860))], S1 + ' installed'),
    ('f44-5-back-on-nightly.png', 'f44', '6-back-on-nightly-max',
     [((388, 672, 1860, 776), 'Back on nightly 26.1021.991 with the copied "to nightly" commands', (760, 626))], N1),
]


def label(draw, text, xy, size):
    w = draw.textlength(text, font=FONT); h = FONT.size + 10
    x = max(4, min(xy[0], size[0] - w - 14)); y = max(4, xy[1])
    draw.rectangle((x, y, x + w + 12, y + h), fill=RED)
    draw.text((x + 6, y + 4), text, fill='white', font=FONT)


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    for name, host, frame, marks, build in PICTURES:
        image = Image.open(FRAMES / host / f'{frame}.png').convert('RGB')
        draw = ImageDraw.Draw(image)
        for box, text, xy in marks:
            draw.rectangle(box, outline=RED, width=4)
            label(draw, text, xy, image.size)
        footer = f'{HOSTS[host]} - {build} - {LAB}'
        w = draw.textlength(footer, font=SMALL)
        draw.rectangle((0, image.size[1] - 28, w + 16, image.size[1]), fill=(20, 20, 20))
        draw.text((8, image.size[1] - 24), footer, fill='white', font=SMALL)
        image.save(OUT / name, optimize=True)
        print(name, image.size)


if __name__ == '__main__':
    main()
