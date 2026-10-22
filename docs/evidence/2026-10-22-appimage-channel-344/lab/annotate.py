"""Annotate the #344 acceptance frames: a red box around what changed, a label naming it, and a footer naming the
desktop and the exact lab build (PR #359 head a82a5192 plus the local lab-key commit 8d55de8c)."""
import sys
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

R = Path(sys.argv[1])            # the copied results: R/f44/<run>/frames, R/m74/<run>/frames
OUT = Path(sys.argv[2])
FONT = ImageFont.truetype('/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf', 22)
SMALL = ImageFont.truetype('/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf', 17)
RED = (220, 30, 40)
S, N1, N2 = 'stable 0.1.1 (60dcdfae)', 'nightly 0.1.1-nightly.9101 (c06a2dd7)', 'nightly 0.1.1-nightly.9102 (5ee14215)'
F44, M74 = 'f44 Fedora 44 GNOME Wayland', 'm74 Linux Mint 22.3 Cinnamon X11'

# (output name, source frame, [(box, label, label side)], desktop, build shown)
PICTURES = [
    ('m74-1-channel-two-choices.png', 'm74/mint-builder-combo/frames/combo-open.png',
     [((403, 684, 1309, 768), 'Channel: only Stable and Nightly, starting on the build\'s own', 'above')], M74, N1),
    ('f44-2-no-stable-yet.png', 'f44/fedora-n2s/frames/2-no-stable-yet.png',
     [((672, 732, 1415, 800), 'No stable release yet: said plainly, no button, no error', 'above')], F44, N1 + ' watching Stable'),
    ('f44-3-switch-to-stable.png', 'f44/fedora-n2s/frames/3-stable-offer.png',
     [((672, 732, 1415, 818), 'Switch to stable 0.1.1: an explicit switch, not an update', 'above')], F44, N1),
    ('f44-4-switched-restart.png', 'f44/fedora-n2s/frames/5-switch-n2s-result.png',
     [((672, 778, 1440, 866), 'Switched to stable 0.1.1 (zsync, stable key): Restart put.gd', 'above')], F44, N1 + ' -> ' + S),
    ('f44-5-switched-from-nightly.png', 'f44/fedora-n2s/frames/6-n2s-restarted.png',
     [((672, 738, 1315, 800), 'After the restart: stable 0.1.1, up to date, "Switched from nightly"', 'above'),
      ((360, 890, 400, 912), '0.1.1', 'right')], F44, S),
    ('f44-6-foreign-key-refused.png', 'f44/fedora-tamper/frames/tamper-foreign.png',
     [((672, 822, 1510, 852), 'Stable release signed by another key: refused, nothing changed', 'above')], F44, N1),
    ('m74-7-older-offer.png', 'm74/mint-builder-s2n/frames/2-older-offer.png',
     [((400, 732, 1165, 822), 'Nightly 9102 is older than this stable build: labelled (older)', 'above')], M74, S + ' watching Nightly'),
    ('m74-8-older-confirm.png', 'm74/mint-builder-s2n/frames/3b-older-confirm.png',
     [((450, 340, 950, 594), 'An older version is confirmed first (No is the default)', 'above')], M74, S),
    ('m74-9-switched-to-nightly.png', 'm74/mint-builder-s2n/frames/4-switch-s2n-result.png',
     [((400, 826, 1230, 874), 'Switched to nightly 9102 (nightly key): Restart put.gd', 'above')], M74, S + ' -> ' + N2),
    ('m74-10-switched-from-stable.png', 'm74/mint-builder-s2n/frames/5-s2n-restarted.png',
     [((400, 734, 1180, 806), 'After the restart: nightly 9102, "Switched from stable 0.1.1"', 'above')], M74, N2),
    ('m74-11-login-starts-the-switched-image.png', 'm74/mint-builder-login/frames/login-4-after-relogin.png',
     [((12, 852, 56, 876), 'Login started 0.1.1 from the configured path (nightly decoy beside it ignored)', 'right')], M74, S + ' (switched from ' + N1 + ')'),
]


def label(draw, box, text, side, size):
    w = draw.textlength(text, font=FONT); h = FONT.size + 10
    x0, y0, x1, y1 = box
    if side == 'above': x, y = x0, y0 - h - 6
    elif side == 'right': x, y = x1 + 10, y0 - 4
    else: x, y = x0, y1 + 6
    x = max(4, min(x, size[0] - w - 14)); y = max(4, y)
    draw.rectangle((x, y, x + w + 12, y + h), fill=RED)
    draw.text((x + 6, y + 4), text, fill='white', font=FONT)


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    for name, source, marks, where, build in PICTURES:
        image = Image.open(R / source).convert('RGB')
        draw = ImageDraw.Draw(image)
        for box, text, side in marks:
            draw.rectangle(box, outline=RED, width=4)
            label(draw, box, text, side, image.size)
        footer = f'{where} - {build} - lab build of #359 a82a5192 + lab-key commit 8d55de8c'
        w = draw.textlength(footer, font=SMALL)
        draw.rectangle((0, image.size[1] - 28, w + 16, image.size[1]), fill=(20, 20, 20))
        draw.text((8, image.size[1] - 24), footer, fill='white', font=SMALL)
        image.save(OUT / name, optimize=True)
        print(name, image.size)


if __name__ == '__main__':
    main()
