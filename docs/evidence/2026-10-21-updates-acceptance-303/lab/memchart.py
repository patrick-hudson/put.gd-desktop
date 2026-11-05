from PIL import Image, ImageDraw, ImageFont
rows = [  # label, before, after (idle RSS MiB, median of 180 s windows)
    ("m74 Cinnamon X11 · base .781 (#301)", 28.270, 28.273),
    ("m74 Cinnamon X11 · nightly .785 (#301)", 28.250, 28.277),
    ("f44 GNOME Wayland · base .781", 34.633, 34.641),
    ("f44 GNOME Wayland · nightly .795", 34.543, 34.633),
    ("d69 Plasma Wayland · base .781", 29.965, 29.965),
    ("d69 Plasma Wayland · nightly .795", 30.031, 30.168),
]
F = '/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf'; FB = '/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf'
font, small, bold = ImageFont.truetype(F, 15), ImageFont.truetype(F, 13), ImageFont.truetype(FB, 18)
SURF, INK, INK2, GRID = (252, 252, 251), (11, 11, 11), (82, 81, 78), (226, 225, 220)
BEFORE, AFTER = (42, 120, 214), (235, 104, 52)
W, left, right, top = 1140, 370, 60, 96
bar, gap, group = 14, 2, 52
H = top + group * len(rows) + 60
img = Image.new('RGB', (W, H), SURF); d = ImageDraw.Draw(img)
d.text((24, 18), "Idle resident RSS before and after an update check (MiB)", font=bold, fill=INK)
d.text((24, 46), "Median of 180 s windows; budget 40 MiB. Private on-screen profile, live app.put.gd feed.", font=small, fill=INK2)
# legend
lx = 24; ly = 70
for name, colour in (("before the check", BEFORE), ("after the check", AFTER)):
    d.rounded_rectangle((lx, ly + 2, lx + 14, ly + 14), 3, fill=colour); d.text((lx + 20, ly), name, font=small, fill=INK2); lx += 150
xmax = 45.0; plot = W - left - right
X = lambda v: left + v / xmax * plot
for t in range(0, 46, 5):
    d.line((X(t), top - 6, X(t), top + group * len(rows) - 10), fill=GRID, width=1)
    d.text((X(t) - 8, top + group * len(rows) - 4), str(t), font=small, fill=INK2)
for i, (label, b, a) in enumerate(rows):
    y = top + i * group
    d.text((24, y + 6), label, font=font, fill=INK)
    for j, (v, c) in enumerate(((b, BEFORE), (a, AFTER))):
        yy = y + j * (bar + gap)
        d.rounded_rectangle((X(0), yy, X(v), yy + bar), 4, fill=c)
        d.rectangle((X(0), yy, X(0) + 4, yy + bar), fill=c)  # square end at the baseline
        d.text((X(v) + 6, yy - 1), f"{v:.3f}", font=small, fill=INK2)
# budget line
bx = X(40)
for yy in range(top - 10, top + group * len(rows) - 10, 8): d.line((bx, yy, bx, yy + 4), fill=INK, width=2)
d.text((bx - 50, top - 28), "40 MiB budget", font=small, fill=INK)
img.save('/tmp/claude-1000/-home-patrick-work-put-gd--claude-worktrees-dazzling-ritchie-73155f/d80b1a79-b210-40ed-95f7-50b080c09455/scratchpad/annotated/memory-gate.png', optimize=True)
