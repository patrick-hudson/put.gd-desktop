import sys
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont
S = Path(sys.argv[1]); out = Path(sys.argv[2])
F = lambda n, b=False: ImageFont.truetype('/usr/share/fonts/truetype/dejavu/DejaVuSans%s.ttf' % ('-Bold' if b else ''), n)
M = ImageFont.truetype('/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf', 15)
GREEN, BLUE, INK, PAPER = (22, 140, 60), (37, 99, 235), (20, 20, 24), (250, 250, 250)
frame = Image.open(S / 'frames/neon-b-settings.png').convert('RGB')
cap = Image.open(S / 'frames/neon-b-capture.png').convert('RGB')
d = ImageDraw.Draw(frame)
for box in ((0, 0, 32, 30), (272, 752, 330, 800)):
    for i in range(4): d.rectangle((box[0] - i + 2, box[1] - i + 2, box[2] + i - 2, box[3] + i - 2), outline=GREEN)
W = 940
a = frame.resize((W, round(frame.height * W / frame.width)), Image.LANCZOS)
b = cap.resize((W, round(cap.height * W / cap.width)), Image.LANCZOS)
text = [
 'guest (lab@neon-lab), through lab.py guest_session at 01d2b9f9:',
 '$ readlink ~/putgd-lab/data/applications',
 '/home/lab/.local/share/applications',
 '$ ls ~/.local/share/applications            # written by AppRun, read by KWin',
 'putgd-appimage-B3dg3AMQ.desktop   X-KDE-DBUS-Restricted-Interfaces=org.kde.KWin.ScreenShot2',
 '                                  Exec="/tmp/.mount_putgd-PGakgf/usr/bin/putgd"',
 'putgd-record-appimage-4WFFRXjw.desktop   X-KDE-Wayland-Interfaces=zkde_screencast_unstable_v1',
 'putgd-ui.desktop      Icon=/tmp/.mount_putgd-PGakgf/usr/share/icons/hicolor/scalable/apps/putgd.svg',
 'putgd-record.desktop  StartupWMClass=putgd-record',
 '$ ls ~/putgd-lab/data       ->  applications putgd      (put.gd data stays private; no ~/.local/share/putgd)',
 '$ lab.py cli --method capabilities   -> desktop: available true, compositor-native, org.kde.KWin.ScreenShot2 v5,',
 '                                        permission_entry_matches_executable true, permission_rejected false',
 '$ lab.py cli --method action.invoke {"action":"desktop"}   -> operation e8bbd5eb… state completed, error none',
 '  local: saved /home/lab/putgd-lab/archive/images/Screen-Capture-2026-10-23-19-22-05.png (117,822 bytes, right)',
 '$ lab.py cli --method ui.quit   -> AppRun removed all four entries; ~/.local/share/applications empty',
]
th = 22 * len(text) + 20
H = 100 + 40 + a.height + 20 + th + 70
c = Image.new('RGB', (2 * W + 60, H), PAPER); d = ImageDraw.Draw(c)
d.text((20, 16), '#530 lab fix: guest_session makes AppRun\'s entries visible to KWin; the icon shows and capture works', fill=INK, font=F(25, True))
d.text((20, 54), 'neon User 6.8.1 VM — Plasma 6.8.1 / KWin 6.8.1 Wayland, golden neon-user-6.8.1-20261023-9ef808278853, overlay neon-i523b on f44 (1280×800)', fill=INK, font=F(17))
y0 = 96
for i, (img, colour, title) in enumerate(((a, GREEN, 'Wall frame 19:21: put.gd icon in the title bar and task manager'),
                                           (b, BLUE, 'put.gd\'s own desktop capture 19:22 (KWin ScreenShot2), saved locally'))):
    x = 20 + i * (W + 20)
    d.text((x, y0), title, fill=colour, font=F(19, True))
    c.paste(img, (x, y0 + 32))
    d.rectangle((x - 3, y0 + 29, x + W + 2, y0 + 32 + img.height + 2), outline=colour, width=3)
y = y0 + 32 + a.height + 18
d.rectangle((20, y, 2 * W + 40, y + th), fill=(30, 33, 40))
for i, line in enumerate(text):
    d.text((32, y + 10 + 22 * i), line, fill=(230, 232, 236) if not line.startswith('$') else (155, 214, 255), font=M)
y += th + 12
d.text((20, y), 'Build: 26.1023.1058-nightly (commit 05991d9d, Qt 6.8.3), release asset SHA-256 ce707fcf…, installed with lab.py install --appimage and started with lab.py start.', fill=INK, font=F(15))
d.text((20, y + 22), 'Runner: tools/lab/lab.py at 01d2b9f9 (PR #530). Qt 6.8.3 can\'t send a window icon, so the icon here comes from the visible putgd-ui.desktop. Taken 2026-10-24 UTC by the #523 session.', fill=INK, font=F(15))
c.save(out, optimize=True); print(c.size)
