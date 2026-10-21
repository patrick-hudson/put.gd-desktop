#!/usr/bin/env python3
"""Annotate the #259 lab frames: trim the magenta cover, caption, numbered boxes and a legend."""
import sys
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

FONTS = Path(sys.argv[1])  # src/ui/fonts
SRC = Path(sys.argv[2])
OUT = Path(sys.argv[3])
OUT.mkdir(parents=True, exist_ok=True)
bold = lambda s: ImageFont.truetype(str(FONTS / 'IBMPlexSans-SemiBold.ttf'), s)
regular = lambda s: ImageFont.truetype(str(FONTS / 'IBMPlexSans-Regular.ttf'), s)
AMBER = (255, 176, 32)
INK = (240, 242, 246)
BAND = (24, 27, 33)

SHOTS = [
    ('m74-cand-pics/chooser.png', 'm74-cinnamon-portal-gtk.png',
     'Linux Mint 22.3 Cinnamon (X11): Review → Save copy… opens the desktop’s GTK chooser',
     'xdg-desktop-portal-gtk via org.freedesktop.portal.FileChooser, parent x11:<xid>, desktop in Mint-Y-Dark-Aqua',
     [((22, 18, 1120, 876), 'The GTK file chooser, drawn by the portal backend, not by put.gd'),
      ((84, 62, 1108, 90), 'Suggested name recording.mp4'),
      ((1044, 792, 1112, 822), 'The format filter (MP4)')]),
    ('d69-cand/chooser.png', 'd69-plasma-portal-kde.png',
     'Debian 13 Plasma 6.3 (Wayland): Review → Save copy… opens the KDE chooser',
     'xdg-desktop-portal-kde via org.freedesktop.portal.FileChooser, Breeze Dark; no parent handle on Wayland (see docs)',
     [((42, 46, 1004, 614), 'The KDE file chooser, drawn by the portal backend'),
      ((248, 470, 998, 504), 'Suggested name recording.mp4'),
      ((248, 508, 998, 542), 'The format filter (MP4)')]),
    ('f44-cand/chooser.png', 'f44-gnome-portal-gnome.png',
     'Fedora 44 GNOME 50 (Wayland): Review → Save copy… opens the GNOME chooser',
     'xdg-desktop-portal-gnome (Nautilus) via org.freedesktop.portal.FileChooser, dark style',
     [((52, 62, 944, 614), 'The GNOME file chooser, in Nautilus’s own process'),
      ((442, 561, 734, 597), 'Suggested name recording.mp4'),
      ((244, 564, 310, 594), 'The format filter (MP4)')]),
    ('d69-cand-fallback-2/chooser.png', 'd69-fallback-dark.png',
     'No portal (PUTGD_FILE_DIALOG=fallback): Qt’s dialog, now themed — Dark',
     'Debian 13 Plasma; labels, list, header, fields and buttons from the Theme tokens',
     [((103, 98, 182, 594), 'Readable labels: Look in, File name, Files of type'),
      ((213, 172, 907, 522), 'The list on the theme surface, not Fusion’s white'),
      ((724, 98, 908, 126), 'IconArt toolbar glyphs'),
      ((360, 133, 798, 168), 'Columns wide enough for their values')]),
    ('d69-cand-fallback-light/chooser.png', 'd69-fallback-light.png',
     'No portal (PUTGD_FILE_DIALOG=fallback): Qt’s dialog, now themed — Light',
     'Debian 13 Plasma; the same dialog follows put.gd’s Light scheme',
     [((103, 98, 182, 594), 'Readable labels'),
      ((213, 172, 907, 522), 'The list on the theme surface'),
      ((724, 98, 908, 126), 'IconArt toolbar glyphs')]),
    ('m74-cand-fallback/chooser.png', 'm74-fallback-dark.png',
     'No portal on Cinnamon (X11): the themed fallback over the review window',
     'Linux Mint 22.3; before #259 this dialog had dark-on-dark labels and a white list (issue screenshot)',
     [((182, 112, 258, 510), 'Readable labels'),
      ((292, 148, 792, 436), 'The list on the theme surface'),
      ((610, 112, 796, 138), 'IconArt toolbar glyphs')]),
]


def magenta(p):
    return abs(p[0] - 255) <= 40 and p[1] <= 40 and abs(p[2] - 255) <= 40


def trim(image):
    w, h = image.size
    px = image.load()
    xs, ys = [], []
    for y in range(0, h, 2):
        for x in range(0, w, 2):
            if not magenta(px[x, y]):
                xs.append(x); ys.append(y)
    pad = 14
    return (max(0, min(xs) - pad), max(0, min(ys) - pad), min(w, max(xs) + pad), min(h, max(ys) + pad))


for source, name, title, subtitle, boxes in SHOTS:
    image = Image.open(SRC / source).convert('RGB')
    x0, y0, x1, y1 = trim(image)
    image = image.crop((x0, y0, x1, y1))
    legend_h = 34 * len(boxes) + 18
    band = 92
    canvas = Image.new('RGB', (max(image.width, 900), band + image.height + legend_h), BAND)
    canvas.paste(image, (0, band))
    draw = ImageDraw.Draw(canvas)
    draw.text((18, 14), title, font=bold(24), fill=INK)
    draw.text((18, 52), subtitle, font=regular(17), fill=(170, 178, 192))
    for number, ((bx0, by0, bx1, by1), label) in enumerate(boxes, 1):
        r = (bx0 - x0, by0 - y0 + band, bx1 - x0, by1 - y0 + band)
        draw.rounded_rectangle(r, radius=6, outline=AMBER, width=3)
        cx, cy = r[0] - 12, r[1] - 12
        draw.ellipse((cx - 14, cy - 14, cx + 14, cy + 14), fill=AMBER)
        draw.text((cx, cy), str(number), font=bold(18), fill=(20, 20, 20), anchor='mm')
        ly = band + image.height + 12 + (number - 1) * 34
        draw.ellipse((18, ly, 44, ly + 26), fill=AMBER)
        draw.text((31, ly + 13), str(number), font=bold(16), fill=(20, 20, 20), anchor='mm')
        draw.text((56, ly + 1), label, font=regular(19), fill=INK)
    canvas.save(OUT / name, optimize=True)
    print(OUT / name, canvas.size)
