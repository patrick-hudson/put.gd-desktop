"""Scene families for the sensitive-text fuzz harness (#1931).

Each family draws one realistic screen on a Canvas and labels what in it is sensitive
(`Val(..., 'secret', kind)`), what only looks sensitive (`'decoy'`) and what may be covered
either way (`'neutral'`). Unlabelled text is ordinary: a cover over it is a false positive too.
`FAMILIES` maps a name to (function, weight). A family function takes (R, cv, th, ctx) where
`ctx` holds the scene's user (login, real name), host name and fonts.
"""
import base64
import dataclasses

import content as C
from canvas import MONO, SERIF, UI_SANS, pick
from content import Val

S, D, N = 'secret', 'decoy', 'neutral'

THEMES = {
    'light': dict(bg='#ffffff', panel='#f6f8fa', fg='#1f2328', muted='#656d76', accent='#0969da', border='#d0d7de', grid='#d8dee4', sel='#b6d7ff', kw='#cf222e', st='#0a3069', num='#0550ae', com='#6e7781', dark=False),
    'adwaita': dict(bg='#fafafa', panel='#ebebeb', fg='#2e3436', muted='#77767b', accent='#3584e4', border='#cdc7c2', grid='#dedede', sel='#c0d7f5', kw='#a51d2d', st='#26a269', num='#1c71d8', com='#9a9996', dark=False),
    'adwaita-dark': dict(bg='#242424', panel='#303030', fg='#ffffff', muted='#9a9996', accent='#78aeed', border='#454545', grid='#3a3a3a', sel='#3d5a80', kw='#f66151', st='#8ff0a4', num='#99c1f1', com='#77767b', dark=True),
    'breeze-dark': dict(bg='#232627', panel='#31363b', fg='#fcfcfc', muted='#a1a9b1', accent='#3daee9', border='#4d4d4d', grid='#3b4045', sel='#2a5e7f', kw='#da4453', st='#f67400', num='#2980b9', com='#7f8c8d', dark=True),
    'vscode-dark': dict(bg='#1e1e1e', panel='#252526', fg='#d4d4d4', muted='#858585', accent='#007acc', border='#3c3c3c', grid='#333333', sel='#264f78', kw='#569cd6', st='#ce9178', num='#b5cea8', com='#6a9955', dark=True),
    'solarized-light': dict(bg='#fdf6e3', panel='#eee8d5', fg='#586e75', muted='#93a1a1', accent='#268bd2', border='#d3cbb7', grid='#e4ddc8', sel='#d6e4ea', kw='#859900', st='#2aa198', num='#d33682', com='#93a1a1', dark=False),
    'nord': dict(bg='#2e3440', panel='#3b4252', fg='#d8dee9', muted='#7b88a1', accent='#88c0d0', border='#4c566a', grid='#434c5e', sel='#434c5e', kw='#81a1c1', st='#a3be8c', num='#b48ead', com='#616e88', dark=True),
    'dracula': dict(bg='#282a36', panel='#21222c', fg='#f8f8f2', muted='#6272a4', accent='#bd93f9', border='#44475a', grid='#343746', sel='#44475a', kw='#ff79c6', st='#f1fa8c', num='#bd93f9', com='#6272a4', dark=True),
    'ubuntu-term': dict(bg='#300a24', panel='#3c0f2e', fg='#ffffff', muted='#c0a0b8', accent='#e95420', border='#5e2750', grid='#4a1a3c', sel='#7a3a6a', kw='#e95420', st='#8ae234', num='#729fcf', com='#a08898', dark=True),
    'green-term': dict(bg='#0c0c0c', panel='#161616', fg='#33ff66', muted='#1f9e45', accent='#33ff66', border='#1f3f2a', grid='#123018', sel='#1f5f33', kw='#66ffaa', st='#ccff99', num='#99ffcc', com='#1f9e45', dark=True),
    'lowcontrast': dict(bg='#e9e9e9', panel='#dedede', fg='#8a8a8a', muted='#a6a6a6', accent='#7a9cc6', border='#cfcfcf', grid='#d6d6d6', sel='#cfd9e6', kw='#9a7a7a', st='#7a9a7a', num='#7a7a9a', com='#b0b0b0', dark=False),
    'slack-aubergine': dict(bg='#ffffff', panel='#3f0e40', fg='#1d1c1d', muted='#616061', accent='#1264a3', border='#dddddd', grid='#eeeeee', sel='#e8f5fa', kw='#e01e5a', st='#2eb67d', num='#1264a3', com='#868686', dark=False),
    'dim-dark': dict(bg='#1b1d23', panel='#22252c', fg='#c8ccd4', muted='#5c6370', accent='#2f6fb3', border='#2c313a', grid='#262a31', sel='#2c3a52', kw='#4c5f9e', st='#2d4f7f', num='#5a4b7a', com='#3e4451', dark=True),
    'discord': dict(bg='#313338', panel='#2b2d31', fg='#dbdee1', muted='#949ba4', accent='#5865f2', border='#3f4147', grid='#3a3c42', sel='#404249', kw='#ff7b72', st='#a5d6ff', num='#79c0ff', com='#8b949e', dark=True),
}
AVATAR = ['#e57373', '#f06292', '#ba68c8', '#7986cb', '#4fc3f7', '#4db6ac', '#81c784', '#ffb74d', '#a1887f', '#90a4ae', '#3b82f6', '#16a34a']
HOSTS = ['devbox', 'thinkpad', 'nova', 'atlas', 'build-01', 'kestrel', 'orion', 'pi4', 'ws-42', 'fedora', 'debian', 'mint']


def sec(text, kind, tag=''):
    return Val(text, S, kind, tag)


def dec(text, kind='', tag=''):
    return Val(text, D, kind, tag)


def neu(text, kind='', tag=''):
    return Val(text, N, kind, tag)


def lh(size):
    return int(size * 1.45) + 1


def window(cv, th, x, y, w, h, title, font, size=12):
    cv.rect(x, y, w, h, th['bg'])
    cv.rect(x, y, w, 30, th['panel'])
    cv.line(x, y + 30, x + w, y + 30, th['border'])
    cv.text(x + 14, y + 8, title, font, size, th['muted'])
    for i, col in enumerate(['#ff5f57', '#febc2e', '#28c840']):
        cv.circle(x + w - 20 - i * 20, y + 15, 5, col)
    return x, y + 30


def _selection(R, segs, th):
    """Sometimes select part of a secret on the line (the selection starts inside it)."""
    if isinstance(segs, (str, Val)) or R.random() > 0.08:
        return None
    pos = 0
    for i, sg in enumerate(segs):
        t = sg.text if isinstance(sg, Val) else sg
        if isinstance(sg, Val) and sg.label == S and len(t) >= 8:
            # a copy: one Val can be drawn on several rows, and only this occurrence is selected
            segs[i] = dataclasses.replace(sg, tag=(sg.tag + '+selected').lstrip('+'))
            return (pos + len(t) // 3, pos + len(t), th['sel'])
        pos += len(t)
    return None


def term_lines(R, cv, th, ctx, lines, x0=12, y0=40, size=None, font=None):
    font = font or ctx['mono']
    size = size or ctx['mono_size']
    y = y0
    for segs in lines:
        if y > cv.h - size * 2:
            break
        cv.text(x0, y, segs, font, size, th['fg'], features=ctx.get('features'), highlight=_selection(R, segs, th))
        y += lh(size)
    return y


def prompt(ctx, cwd='~'):
    return [sec(ctx['login'], 'name', 'prompt'), f'@{ctx["host"]}:{cwd}$ ']


# =============================================================================================
def calendar(R, cv, th, ctx):
    f, size = ctx['ui'], ctx['ui_size']
    x0, y0 = 24, 24
    months = R.randint(1, 2)
    day_w = R.choice([30, 34, 38, 44])
    for m in range(months):
        mx = x0 + m * (day_w * 7 + 40)
        month = R.choice(['January', 'March', 'June', 'September', 'November', 'December'])
        cv.text(mx, y0, f'{month} 2026', f, size + 3, th['fg'], weight='bold')
        names = R.choice([['Mo', 'Tu', 'We', 'Th', 'Fr', 'Sa', 'Su'], ['Mon', 'Tue', 'Wed', 'Thu', 'Fri', 'Sat', 'Sun'], ['M', 'T', 'W', 'T', 'F', 'S', 'S'], ['S', 'M', 'T', 'W', 'T', 'F', 'S']])
        week_no = R.random() < 0.35
        gx = mx + (day_w if week_no else 0)
        for i, n in enumerate(names):
            cv.text(gx + i * day_w + 6, y0 + 34, n, f, size - 1, th['muted'])
        start = R.randint(0, 6)
        day = 1 - start
        last = R.choice([30, 31])
        wk = R.randint(1, 48)
        today = R.randint(1, last)
        for row in range(6):
            ry = y0 + 60 + row * int(day_w * 0.9)
            if week_no:
                cv.text(mx + 4, ry, dec(str(wk + row), 'week_number'), f, size - 2, th['muted'])
            for col in range(7):
                d = day + row * 7 + col
                shown = d if 1 <= d <= last else (d + 30 if d < 1 else d - last)
                color = th['fg'] if 1 <= d <= last else th['muted']
                cx = gx + col * day_w
                if d == today:
                    cv.circle(cx + day_w / 2 - 2, ry + size * 0.65, size * 1.05, th['accent'])
                    color = '#ffffff'
                cv.text(cx + (6 if shown >= 10 else 10), ry, dec(str(shown), 'calendar_day'), f, size, color)
    # agenda
    ax = x0 + months * (day_w * 7 + 40) + (day_w if R.random() < 0.3 else 0) + 20
    if ax + 300 < cv.w:
        y = y0
        cv.text(ax, y, 'Upcoming', f, size + 2, th['fg'], weight='bold')
        y += 36
        for _ in range(R.randint(3, 7)):
            t = f'{R.randint(7, 18):02d}:{R.choice(["00", "15", "30", "45"])}'
            kind = R.random()
            if kind < 0.3:
                fn, ln = C.person(R)
                segs = [dec(t, 'time'), '  Call with ', f'{fn} ', '(', C.email(R, fn, ln), ')']
            elif kind < 0.5:
                segs = [dec(t, 'time'), '  Dial-in ', C.phone(R), '  PIN ', dec(C.rs(R, C.DIG, R.choice([6, 8])), 'pin')]
            elif kind < 0.6:
                segs = [dec(t, 'time'), f' – {R.randint(8, 19):02d}:{R.choice(["00", "30"])}  ', 'Room ', dec(f'{R.randint(1, 9)}.{R.randint(10, 40)}', 'room')]
            else:
                segs = [dec(t, 'time'), '  ', C.sentence(R, R.randint(2, 5))]
            cv.text(ax, y, segs, f, size, th['fg'])
            y += lh(size) + 6
    return {'variant': f'months{months}'}


def spreadsheet(R, cv, th, ctx):
    f, size = ctx['ui'], ctx['ui_size']
    variant = R.choice(['finance', 'finance', 'customers', 'ids', 'cells'])
    gutter = 40
    head_h = 24
    row_h = int(size * 1.9)
    cols = {'finance': ['Region'] + [str(y) for y in range(R.randint(2014, 2022), 2027)][:R.randint(4, 7)] + ['Total'],
            'customers': ['Name', 'Email', 'Phone', 'Card', 'City'],
            'ids': ['Order', 'Tracking', 'SKU', 'EAN', 'Shipped'],
            'cells': ['Q1', 'Q2', 'Q3', 'Q4', 'FY']}[variant]
    widths = []
    for c in cols:
        widths.append({'Email': 230, 'Card': 170, 'Phone': 150, 'Tracking': 230, 'Name': 140, 'Region': 110}.get(c, R.choice([70, 80, 90, 110])))
    tight = R.random() < 0.4  # narrow cells: text touches the gridlines
    pad = 2 if tight else 6
    # column letters and header
    cv.rect(0, 0, cv.w, head_h, th['panel'])
    x = gutter
    for i, w in enumerate(widths):
        cv.text(x + w / 2 - 4, 4, chr(65 + i), f, size - 1, th['muted'])
        x += w
    y = head_h
    rows = min(int((cv.h - head_h) / row_h) - 1, 22)
    locale = R.choice(['en', 'en', 'fr', 'de', 'plain'])
    for r in range(rows + 1):
        cv.text(8, y + 4, str(r + 1), f, size - 2, th['muted'])
        x = gutter
        for ci, (c, w) in enumerate(zip(cols, widths)):
            if r == 0:
                cell = dec(c, 'year_header') if c.isdigit() else c
                weight = 'bold'
            else:
                weight = 'normal'
                if variant in ('finance', 'cells'):
                    if ci == 0 and variant == 'finance':
                        cell = R.choice(['North', 'South', 'EMEA', 'APAC', 'LATAM', 'West', 'Online', 'Retail'])
                    else:
                        v = R.choice([R.randint(1000, 9999), R.randint(10000, 9999999)])
                        if locale == 'en':
                            s = f'{v:,}'
                        elif locale == 'fr':
                            s = f'{v:,}'.replace(',', ' ')
                        elif locale == 'de':
                            s = f'{v:,}'.replace(',', '.')
                        else:
                            s = str(v)
                        cell = dec(s, 'table_number', locale)
                elif variant == 'customers':
                    fn, ln = C.person(R)
                    cell = {'Name': f'{fn} {ln}', 'Email': C.email(R, fn, ln), 'Phone': C.phone(R), 'Card': C.card(R, sep=R.choice([' ', '-', ''])),
                            'City': R.choice(['Lyon', 'Austin', 'Osaka', 'Leeds', 'Porto', 'Graz', 'Pune'])}[c]
                else:
                    cell = {'Order': dec(str(R.randint(1000000, 99999999)), 'order_number'),
                            'Tracking': R.choice([dec('1Z' + C.rs(R, C.UP + C.DIG, 16), 'tracking'), dec(' '.join(C.rs(R, C.DIG, 4) for _ in range(5)) + ' ' + C.rs(R, C.DIG, 2), 'tracking_usps'),
                                                  dec(C.rs(R, C.DIG, 12), 'tracking_fedex')]),
                            'SKU': dec(C.rs(R, C.UP, 3) + '-' + C.rs(R, C.DIG, 5), 'sku'), 'EAN': dec(C.ean13(R), 'ean13'),
                            'Shipped': dec(f'2026-{R.randint(1, 12):02d}-{R.randint(1, 28):02d}', 'date')}[c]
            text = cell.text if isinstance(cell, Val) else cell
            tw, _ = cv.measure(text, f, size)
            right = variant in ('finance', 'cells') and ci > (0 if variant == 'finance' else -1) and r > 0
            tx = x + w - pad - tw if right else x + pad
            cv.text(tx, y + (row_h - size) / 2 - 2, cell, f, size, th['fg'], weight=weight)
            x += w
        y += row_h
    # gridlines on top
    x = gutter
    for w in widths + [0]:
        cv.line(x, 0, x, y, th['grid'])
        x += w
    for r in range(rows + 2):
        cv.line(0, head_h + r * row_h, x, head_h + r * row_h, th['grid'])
    return {'variant': variant + ('-tight' if tight else ''), 'locale': locale}


def dashboard(R, cv, th, ctx):
    f, size = ctx['ui'], ctx['ui_size']
    x, y = 24, 20
    cv.text(x, y, R.choice(['Overview', 'Usage', 'Billing', 'Analytics']), f, size + 6, th['fg'], weight='bold')
    y += 46
    tiles = R.randint(3, 4)
    tw = (cv.w - 48 - (tiles - 1) * 16) / tiles
    for i in range(tiles):
        tx = x + i * (tw + 16)
        cv.rect(tx, y, tw, 90, th['panel'], radius=10)
        label, value = R.choice([('Revenue', dec(C.money(R), 'money')), ('Active users', dec(f'{R.randint(1000, 999999):,}', 'count')),
                                 ('Uptime', dec(f'{R.uniform(99, 99.999):.3f}%', 'percent')), ('Requests', dec(f'{R.randint(10**6, 10**9):,}', 'count')),
                                 ('Orders', dec(f'{R.randint(1000, 99999):,}', 'count')), ('ARR', dec(C.money(R, R.choice(["en", "fr", "de"])), 'money'))])
        cv.text(tx + 14, y + 12, label, f, size - 1, th['muted'])
        cv.text(tx + 14, y + 36, value, f, size + 10, th['fg'], weight='bold')
        pts = [(tx + 14 + k * (tw - 28) / 20, y + 82 - R.random() * 16) for k in range(21)]
        cv.poly(pts, th['accent'], 1.2)
    y += 112
    # chart with axes
    cw, ch = cv.w - 48, min(cv.h - y - 70, 320)
    if ch > 120:
        cv.rect(x, y, cw, ch + 50, th['panel'], radius=10)
        ox, oy = x + 70, y + ch
        ticks = R.choice([[str(v) for v in range(2017, 2027)], ['Jan', 'Feb', 'Mar', 'Apr', 'May', 'Jun', 'Jul', 'Aug', 'Sep', 'Oct', 'Nov', 'Dec'],
                          [f'{d:02d}' for d in range(1, 31, 3)], [f'{h:02d}:00' for h in range(0, 24, 3)]])
        step = (cw - 100) / max(1, len(ticks) - 1)
        for i, t in enumerate(ticks):
            cv.text(ox + i * step - 12, oy + 8, dec(t, 'axis_tick'), f, size - 2, th['muted'])
        top = R.choice([1000, 5000, 20000, 100000, 2500000])
        for k in range(5):
            v = top * k // 4
            yy = oy - k * (ch - 30) / 4
            cv.line(ox, yy, ox + cw - 100, yy, th['grid'])
            cv.text(x + 10, yy - size / 2 - 2, dec(f'{v:,}' if R.random() < 0.6 else str(v), 'axis_tick'), f, size - 2, th['muted'])
        kind = R.choice(['bars', 'dense', 'lines'])
        n = len(ticks) if kind == 'bars' else R.randint(60, 140)
        bw = (cw - 100) / n
        for i in range(n):
            hgt = R.random() * (ch - 40)
            if kind in ('bars', 'dense'):
                cv.rect(ox + i * bw + bw * 0.15, oy - hgt, max(1, bw * 0.7), hgt, R.choice(AVATAR) if kind == 'bars' else th['accent'])
        if kind == 'lines':
            for s in range(R.randint(1, 3)):
                pts = [(ox + i * (cw - 100) / 50, oy - 20 - R.random() * (ch - 60)) for i in range(51)]
                cv.poly(pts, AVATAR[s * 3], 1.5)
        # legend with round or square swatches
        lx = x + 70
        for i in range(R.randint(2, 4)):
            col = AVATAR[(i * 3) % len(AVATAR)]
            if R.random() < 0.5:
                cv.circle(lx + 5, y + ch + 34, 5, col)
            else:
                cv.rect(lx, y + ch + 29, 10, 10, col)
            name = R.choice(['Web', 'API', 'Mobile', 'Batch', 'Search', 'Storage', 'Egress', 'Compute'])
            w, _ = cv.text(lx + 16, y + ch + 26, name, f, size - 1, th['fg'])
            lx += w + 40
    return {'variant': 'tiles'}


def receipt(R, cv, th, ctx):
    f = pick(R, MONO) if R.random() < 0.6 else ctx['ui']
    size = R.choice([12, 13, 14])
    paper = '#ffffff' if not th['dark'] else '#f4f1ea'
    cv.rect(0, 0, cv.w, cv.h, '#d9d9d9')
    pw = min(cv.w - 40, R.choice([380, 420, 460, 560]))
    px = (cv.w - pw) / 2
    cv.rect(px, 0, pw, cv.h, paper)
    ink = '#222222'
    y = 20
    store = R.choice(['CORNER MARKET', 'BLUE BOTTLE CAFE', 'HARDWARE DEPOT', 'CITY PHARMACY', 'Bäckerei Lindner'])
    cv.text(px + 20, y, store, f, size + 4, ink, weight='bold')
    y += 30
    for line in [f'{R.randint(1, 999)} {R.choice(["Market St", "Hauptstr.", "Rue de Lyon", "High Street"])}', ['Tel ', C.phone(R)], ['VAT ', dec(R.choice(['DE', 'FR', 'GB']) + C.rs(R, C.DIG, 9), 'vat')]]:
        cv.text(px + 20, y, line, f, size, ink)
        y += lh(size)
    y += 6
    cv.text(px + 20, y, [dec(f'{R.randint(1, 28):02d}/11/2026', 'date'), '  ', dec(f'{R.randint(7, 21):02d}:{R.randint(0, 59):02d}:{R.randint(0, 59):02d}', 'time'), '  #', dec(str(R.randint(100000, 9999999)), 'receipt_no')], f, size, ink)
    y += lh(size) + 8
    lead = R.random() < 0.6
    for _ in range(R.randint(4, 9)):
        item = R.choice(['Oat milk 1L', 'Espresso', 'Croissant', 'Batteries AA x4', 'Screws M4', 'Paracetamol', 'Bread', 'Apples 1kg', 'USB-C cable'])
        price = f'{R.randint(1, 49)}.{R.randint(0, 99):02d}'
        dots = '.' * max(2, int((pw - 60) / (size * 0.6)) - len(item) - len(price)) if lead else ' ' * 4
        cv.text(px + 20, y, f'{item} {dots} {price}', f, size, ink)
        y += lh(size)
    y += 8
    for line in [['SUBTOTAL ', dec(f'{R.randint(20, 300)}.{R.randint(0, 99):02d}', 'money')], ['VISA ', dec('**** **** **** ' + C.rs(R, C.DIG, 4), 'masked_card')],
                 ['AUTH CODE ', dec(C.rs(R, C.DIG + C.UP, 6), 'auth_code'), '  TID ', dec(C.rs(R, C.DIG, 8), 'terminal_id')],
                 ['AID ', dec('A0000000031010', 'emv_aid')], ['Order #', dec(str(R.randint(10**6, 10**9)), 'order_number')]]:
        cv.text(px + 20, y, line, f, size, ink)
        y += lh(size)
    if R.random() < 0.35:
        cv.text(px + 20, y, ['Card ', C.card(R)], f, size, ink)
        y += lh(size)
    y += 10
    # a linear barcode with its digits
    if y + 90 < cv.h:
        bx = px + 30
        while bx < px + pw - 40:
            wbar = R.choice([1, 1, 2, 3])
            cv.rect(bx, y, wbar, 56, ink)
            bx += wbar + R.choice([1, 2, 2, 3])
        e = C.ean13(R)
        cv.text(px + 40, y + 60, dec(f'{e[0]} {e[1:7]} {e[7:]}', 'ean13_digits'), f, size, ink)
    return {'variant': 'leaders' if lead else 'plain'}


def sysmon(R, cv, th, ctx):
    f, size = ctx['mono'], ctx['mono_size']
    y = 12
    ncpu = R.choice([4, 8, 12, 16])
    half = (cv.w - 30) / 2
    chars = int(half / (size * 0.6)) - 14
    for i in range(ncpu):
        col, row = i % 2, i // 2
        pct = R.uniform(0, 100)
        n = int(chars * pct / 100)
        segs = [f'{i:>2}[', '|' * n, ' ' * (chars - n), f'{pct:5.1f}%]']
        cv.text(12 + col * half, y + row * lh(size), segs, f, size, th['fg'])
    y += (ncpu // 2) * lh(size) + 4
    for name in ['Mem', 'Swp']:
        n = R.randint(0, chars)
        cv.text(12, y, [f'{name}[', '|' * n, ' ' * (chars - n), f'{R.uniform(0.1, 31):.2f}G/{R.choice([16, 32, 64])}.0G]'], f, size, th['fg'])
        y += lh(size)
    cv.text(12 + half, y - 2 * lh(size), f'Tasks: {R.randint(80, 400)}, {R.randint(300, 2000)} thr; {R.randint(1, 5)} running', f, size, th['fg'])
    cv.text(12 + half, y - lh(size), f'Load average: {R.uniform(0, 4):.2f} {R.uniform(0, 4):.2f} {R.uniform(0, 4):.2f}  Uptime: {R.randint(1, 90)} days, {R.randint(0, 23):02d}:{R.randint(0, 59):02d}:{R.randint(0, 59):02d}', f, size, th['fg'])
    y += 8
    cv.rect(0, y, cv.w, lh(size), th['accent'])
    cv.text(12, y, '    PID USER      PRI  NI  VIRT   RES   SHR S CPU% MEM%   TIME+  Command', f, size, th['bg'])
    y += lh(size)
    while y < cv.h - lh(size):
        user = R.choice([sec(ctx['login'][:9], 'name', 'htop_user'), 'root', 'systemd+', 'postgres', 'www-data'])
        cmd = R.choice(['/usr/bin/gnome-shell', 'firefox', 'postgres: checkpointer', 'node server.js', '/usr/lib/xorg/Xorg', 'python3 -m http.server 8000', 'kwin_wayland', 'dockerd'])
        segs = [f'{R.randint(1, 999999):>7} ', user, ' ' * max(1, 10 - len(user.text if isinstance(user, Val) else user)),
                f'{R.randint(0, 39):>3} {R.randint(-20, 19):>3} {R.randint(1, 9999)}M {R.randint(1, 999)}M {R.randint(1, 99)}M {R.choice("SRD")} {R.uniform(0, 99):4.1f} {R.uniform(0, 20):4.1f} {R.randint(0, 99)}:{R.randint(0, 59):02d}.{R.randint(0, 99):02d} {cmd}']
        cv.text(12, y, segs, f, size, th['fg'])
        y += lh(size)
    return {'variant': f'cpu{ncpu}'}


def checksums(R, cv, th, ctx):
    lines = []
    app = R.choice(['put.gd-26.1123.6661-x86_64.AppImage', 'ubuntu-26.04-desktop-amd64.iso', 'Fedora-Workstation-Live-45.iso', 'node-v22.11.0-linux-x64.tar.xz', 'release.tar.gz'])
    for _ in range(R.randint(3, 6)):
        k = R.choice(['sha256sum', 'sha256sum', 'md5sum', 'b3sum', 'git rev-parse HEAD', 'git show-ref', 'git ls-remote origin', 'docker inspect', 'podman ps', 'gpg', 'ssh-keygen', 'submodule'])
        if k in ('sha256sum', 'md5sum', 'b3sum'):
            n = 32 if k == 'md5sum' else 64
            lines.append(prompt(ctx, '~/Downloads') + [f'{k} {app}'])
            lines.append([dec(C.sha(R, n), 'digest_bare', k), f'  {app}'])
        elif k == 'git rev-parse HEAD':
            lines.append(prompt(ctx, '~/src/app') + [k])
            lines.append([dec(C.sha(R), 'git_sha_bare')])
        elif k == 'git show-ref':
            lines.append(prompt(ctx, '~/src/app') + [k])
            for ref in ['refs/heads/main', 'refs/heads/nightly', 'refs/remotes/origin/main', 'refs/tags/v1.4.2']:
                lines.append([dec(C.sha(R), 'git_sha_ref'), ' ' + ref])
        elif k == 'git ls-remote origin':
            lines.append(prompt(ctx, '~/src/app') + [k])
            for ref in ['HEAD', 'refs/heads/main', 'refs/pull/1931/head']:
                lines.append([dec(C.sha(R), 'git_sha_ref'), '\t' + ref])
        elif k == 'docker inspect':
            lines.append(prompt(ctx) + ["docker inspect --format '{{.Id}}' web"])
            lines.append([dec(C.sha(R, 64), 'container_id')])
        elif k == 'podman ps':
            lines.append(prompt(ctx) + ['podman ps --no-trunc --format "{{.ID}} {{.Names}}"'])
            lines.append([dec(C.sha(R, 64), 'container_id'), ' web-1'])
        elif k == 'gpg':
            lines.append(prompt(ctx) + ['gpg --fingerprint ', C.email(R)])
            fp = C.rs(R, '0123456789ABCDEF', 40)
            lines.append(['      ', dec(' '.join(fp[i:i + 4] for i in range(0, 20, 4)) + '  ' + ' '.join(fp[i:i + 4] for i in range(20, 40, 4)), 'gpg_fingerprint')])
        elif k == 'ssh-keygen':
            lines.append(prompt(ctx) + ['ssh-keygen -lf ~/.ssh/id_ed25519.pub'])
            lines.append(['256 SHA256:', dec(C.rs(R, C.B64, 43), 'ssh_fp'), ' ', C.email(R), ' (ED25519)'])
        else:
            lines.append(prompt(ctx, '~/src/app') + ['git submodule status'])
            lines.append([' ', dec(C.sha(R), 'git_sha_bare'), ' third_party/zxing (v2.2.1)'])
    term_lines(R, cv, th, ctx, lines)
    return {'variant': 'terminal'}


def logs(R, cv, th, ctx):
    form = R.choice(['json', 'kv', 'journal', 'nginx', 's3', 'sentry'])
    lines = []
    for _ in range(60):
        ts = f'2026-11-{R.randint(1, 28):02d}T{R.randint(0, 23):02d}:{R.randint(0, 59):02d}:{R.randint(0, 59):02d}.{R.randint(0, 999):03d}Z'
        if form == 'kv':
            lines.append([dec(ts, 'timestamp'), f' {R.choice(["INFO ", "WARN ", "DEBUG", "ERROR"])} req=', dec(C.uuid(R), 'uuid'), ' trace_id=', dec(C.sha(R, 32), 'trace_id'),
                          ' span=', dec(C.sha(R, 16), 'span_id'), f' {R.choice(["GET", "POST"])} /v1/orders/', dec(str(R.randint(10**6, 10**8)), 'order_number'),
                          f' {R.choice([200, 201, 404, 500])} {R.randint(1, 900)}ms ip=', C.ipv4(R)])
        elif form == 'json':
            lines.append(['{"ts":"', dec(ts, 'timestamp'), '","level":"info","msg":"', R.choice(['request done', 'cache miss', 'job queued']), '","requestId":"', dec(C.uuid(R), 'uuid'),
                          '","traceId":"', dec(C.sha(R, 32), 'trace_id'), '","userId":', dec(str(R.randint(10**5, 10**9)), 'user_id'), ',"durationMs":', str(R.randint(1, 999)), '}'])
        elif form == 'journal':
            pid = R.randint(1000, 99999)
            lines.append([dec(f'Nov {R.randint(1, 28):2d} {R.randint(0, 23):02d}:{R.randint(0, 59):02d}:{R.randint(0, 59):02d}', 'timestamp'), f' {ctx["host"]} sshd[{pid}]: Accepted publickey for ',
                          sec(ctx['login'], 'name', 'journal'), ' from ', C.ipv4(R), f' port {R.randint(30000, 65000)} ssh2: ED25519 SHA256:', dec(C.rs(R, C.B64, 43), 'ssh_fp')])
        elif form == 'nginx':
            lines.append([C.ipv4(R), ' - - [', dec(f'{R.randint(1, 28):02d}/Nov/2026:{R.randint(0, 23):02d}:{R.randint(0, 59):02d}:{R.randint(0, 59):02d} +0000', 'timestamp'),
                          f'] "GET /{R.choice(["", "api/v1/items", "static/app.3f2a9c1b.js", "favicon.ico"])} HTTP/1.1" {R.choice([200, 304, 404])} {R.randint(100, 99999)} "-" "Mozilla/5.0 (X11; Linux x86_64; rv:132.0) Gecko/20100101 Firefox/132.0"'])
        elif form == 's3':
            lines.append(['x-amz-request-id: ', dec(C.rs(R, '0123456789ABCDEF', 16), 'request_id')])
            lines.append(['x-amz-id-2: ', dec(C.rs(R, C.B64, 76), 'amz_id_2')])
            lines.append(['X-Amz-Cf-Id: ', dec(C.rs(R, C.B64URL, 56) + '==', 'cf_id')])
        else:
            lines.append(['Event ID: ', dec(C.sha(R, 32), 'sentry_event'), '   ', dec(ts, 'timestamp')])
        if R.random() < 0.05:
            leak = R.choice([['Authorization: Bearer ', C.token(R, 'jwt')], ['token=', C.token(R, R.choice(['gh', 'slack', 'stripe']))], ['user=', C.email(R)]])
            lines.append(leak)
    term_lines(R, cv, th, ctx, lines, size=ctx['mono_size'] - R.choice([0, 1, 2]))
    return {'variant': form}


def lockfiles(R, cv, th, ctx):
    form = R.choice(['npm', 'yarn', 'gosum', 'cargo', 'poetry', 'flake', 'pip', 'pnpm'])
    lines = []
    for _ in range(25):
        pkg = R.choice(['lodash', 'react', 'serde', 'tokio', 'requests', 'urllib3', 'golang.org/x/net', 'github.com/stretchr/testify', 'nixpkgs', 'esbuild', 'chalk', 'semver'])
        ver = C.version(R)
        if form == 'npm':
            lines += [f'    "node_modules/{pkg}": {{', f'      "version": "{ver}",', f'      "resolved": "https://registry.npmjs.org/{pkg}/-/{pkg}-{ver}.tgz",',
                      ['      "integrity": "', dec('sha512-' + C.rs(R, C.B64, 86) + '==', 'sri'), '",'], '    },']
        elif form == 'yarn':
            lines += [f'{pkg}@^{ver}:', f'  version "{ver}"', ['  resolved "https://registry.yarnpkg.com/', pkg, '/-/', f'{pkg}-{ver}.tgz#', dec(C.sha(R), 'yarn_sha1'), '"'],
                      ['  integrity ', dec('sha512-' + C.rs(R, C.B64, 86) + '==', 'sri')], '']
        elif form == 'gosum':
            lines += [[f'{pkg} v{ver} h1:', dec(C.rs(R, C.B64, 43) + '=', 'go_h1')], [f'{pkg} v{ver}/go.mod h1:', dec(C.rs(R, C.B64, 43) + '=', 'go_h1')]]
        elif form == 'cargo':
            lines += ['[[package]]', f'name = "{pkg}"', f'version = "{ver}"', 'source = "registry+https://github.com/rust-lang/crates.io-index"', ['checksum = "', dec(C.sha(R, 64), 'digest'), '"'], '']
        elif form == 'poetry':
            lines += [f'name = "{pkg}"', f'version = "{ver}"', 'files = [', ['    {file = "', f'{pkg}-{ver}-py3-none-any.whl", hash = "sha256:', dec(C.sha(R, 64), 'digest'), '"},'], ']']
        elif form == 'flake':
            lines += [f'    "{pkg}": {{', '      "locked": {', ['        "narHash": "', dec('sha256-' + C.rs(R, C.B64, 43) + '=', 'nar_hash'), '",'],
                      ['        "rev": "', dec(C.sha(R), 'git_sha_rev'), '",'], f'        "lastModified": {R.randint(1700000000, 1735000000)},', '      },']
        elif form == 'pip':
            lines += [f'{pkg}=={ver} \\', ['    --hash=sha256:', dec(C.sha(R, 64), 'digest'), ' \\'], ['    --hash=sha256:', dec(C.sha(R, 64), 'digest')]]
        else:
            lines += [f'  /{pkg}@{ver}:', ['    resolution: {integrity: ', dec('sha512-' + C.rs(R, C.B64, 86) + '==', 'sri'), '}'], '    dev: false']
    ide(R, cv, th, ctx, lines, lang=form)
    return {'variant': form}


def packages(R, cv, th, ctx):
    form = R.choice(['apt', 'pip', 'npm', 'dnf', 'brew'])
    lines = [prompt(ctx) + [{'apt': 'apt list --upgradable', 'pip': 'pip list', 'npm': 'npm ls --depth=0', 'dnf': 'dnf check-update', 'brew': 'brew outdated --verbose'}[form]]]
    for _ in range(40):
        name = R.choice(['libssl3t64', 'openssl-libs', 'python3-requests', 'firefox', 'libqt6core6', 'mesa-vulkan-drivers', 'react', '@types/node', 'typescript', 'urllib3',
                         'kernel-core', 'linux-image-generic', 'systemd', 'curl', 'git', 'vim-common', 'eslint', '@babel/core', 'numpy', 'pandas'])
        v1, v2 = C.version(R), C.version(R)
        if R.random() < 0.12:
            v1 = dec(f'{R.randint(1, 9)}.{R.randint(0, 20)}.{R.randint(0, 20)}.{R.randint(1, 99)}', 'version_ipv4')
        if form == 'apt':
            lines.append([f'{name}/noble-updates ', v1, f' amd64 [upgradable from: {v2}]'])
        elif form == 'pip':
            lines.append([f'{name:<24}', v1])
        elif form == 'npm':
            lines.append([R.choice(['├── ', '└── ']), name, '@', v1])
        elif form == 'dnf':
            lines.append([f'{name}.x86_64'.ljust(34), '1:', v1, '-3.fc45'.ljust(16), 'updates'])
        else:
            lines.append([name, ' (', v2, ') < ', v1])
    term_lines(R, cv, th, ctx, lines)
    return {'variant': form}


def gitgraph(R, cv, th, ctx):
    form = R.choice(['graph', 'log', 'blame'])
    lines = []
    if form == 'graph':
        lines.append(prompt(ctx, '~/src/app') + ['git log --graph --oneline --decorate --all'])
        g = ['* ', '| * ', '|/  ', '* ', '*   ', '|\\  ', '| * ']
        for _ in range(40):
            lines.append([R.choice(g), dec(C.sha(R, 7), 'short_sha'), ' ', R.choice(['', '(HEAD -> main, origin/main) ', '(tag: v1.4.2) ', '']), C.sentence(R, R.randint(3, 8))])
    elif form == 'log':
        for _ in range(8):
            fn, ln = C.person(R)
            lines += [['commit ', dec(C.sha(R), 'git_sha_commit')], ['Author: ', f'{fn} {ln} <', C.email(R, fn, ln), '>'],
                      ['Date:   ', dec(f'Sun Nov {R.randint(1, 28)} {R.randint(0, 23):02d}:{R.randint(0, 59):02d}:{R.randint(0, 59):02d} 2026 +0100', 'date')], '', '    ' + C.sentence(R), '']
            if R.random() < 0.3:
                lines.insert(-1, ['    Change-Id: ', dec('I' + C.sha(R), 'gerrit_change_id')])
            if R.random() < 0.3:
                lines.insert(-1, ['    Signed-off-by: ', f'{fn} {ln} <', C.email(R, fn, ln), '>'])
    else:
        for i in range(40):
            fn, ln = C.person(R)
            lines.append([dec(C.sha(R, 8), 'short_sha'), f' ({fn} {ln} ', dec(f'2026-{R.randint(1, 11):02d}-{R.randint(1, 28):02d} {R.randint(0, 23):02d}:{R.randint(0, 59):02d}:{R.randint(0, 59):02d} +0100', 'date'),
                          f' {i + 1:>3}) ', R.choice(['def main():', '    return x', '    log.info("ok")', 'import os', '', '    # TODO']) ])
    term_lines(R, cv, th, ctx, lines)
    return {'variant': form}


CODE = {
    'py': ['import os', 'import requests', '', 'API_URL = "https://api.example.com/v2"', 'TIMEOUT = 30', '', 'def fetch(order_id: int) -> dict:', '    """Fetch one order."""',
           '    r = requests.get(f"{API_URL}/orders/{order_id}", timeout=TIMEOUT)', '    r.raise_for_status()', '    return r.json()', '', 'class Cache:', '    def __init__(self, size=1024):',
           '        self.size = size', '        self.items = {}', '', 'if __name__ == "__main__":', '    print(fetch(1234567))'],
    'js': ["import express from 'express';", "const app = express();", "const PORT = process.env.PORT || 3000;", "", "app.get('/health', (req, res) => {", "  res.json({ ok: true, uptime: process.uptime() });",
           "});", "", "app.listen(PORT, () => console.log(`listening on ${PORT}`));"],
    'go': ['package main', '', 'import (', '\t"fmt"', '\t"net/http"', ')', '', 'func main() {', '\thttp.HandleFunc("/", handler)', '\tfmt.Println("listening on :8080")', '\thttp.ListenAndServe(":8080", nil)', '}'],
}


def ide(R, cv, th, ctx, body=None, lang=None):
    f, size = ctx['mono'], ctx['mono_size']
    tabs_h = 30
    cv.rect(0, 0, cv.w, tabs_h, th['panel'])
    cv.text(14, 8, R.choice(['main.py', 'server.js', 'config.go', 'settings.py', 'package-lock.json', 'go.sum', 'Cargo.lock']), ctx['ui'], ctx['ui_size'] - 1, th['fg'])
    minimap = R.random() < 0.6
    mm_w = 90 if minimap else 0
    gutter_w = size * 3.2
    lines = body
    if lines is None:
        base = list(CODE[R.choice(list(CODE))])
        lines = []
        for ln in base:
            lines.append(ln)
            r = R.random()
            if r < 0.06:
                lines.append(['SECRET_KEY = "', C.token(R, R.choice(['hex40', 'b64_40', 'stripe'])), '"'])
            elif r < 0.10:
                lines.append(['DEFAULT_ID = "', dec(C.uuid(R), 'uuid'), '"'])
            elif r < 0.13:
                lines.append(['EXPECTED_SHA = "', dec(C.sha(R, 64), 'digest_const'), '"  # fixture'])
            elif r < 0.16:
                lines.append(['ACCENT = "', dec('#' + C.rs(R, C.HEX, 6), 'css_hex'), '"'])
            elif r < 0.18:
                lines.append(['client = Client(api_key="', C.token(R, R.choice(['openai', 'anthropic', 'gh'])), '")'])
    y = tabs_h + 8
    blame_line = R.randint(0, max(0, len(lines) - 1))
    if minimap:
        cv.clip_x = cv.w - mm_w
    for i, segs in enumerate(lines):
        if y > cv.h - 40:
            break
        cv.text(6, y, str(i + 1).rjust(3), f, size, th['muted'])
        w, hgt = cv.text(gutter_w + 8, y, segs, f, size, th['fg'] if i % 3 else th['st'], features=ctx.get('features'), highlight=_selection(R, segs, th),
                         width=ctx.get('softwrap'), wrap='char')
        if ctx.get('softwrap'):  # a soft-wrapped row takes as many lines as it was laid out on
            y += max(0, hgt - lh(size))
        if i == blame_line and R.random() < 0.5:
            fn, ln = C.person(R)
            cv.text(gutter_w + 8 + w + 30, y, f'{fn} {ln}, {R.randint(2, 11)} days ago • {C.sentence(R, 3)}', ctx['ui'], size - 1, th['com'], italic=True)
        y += lh(size)
    cv.clip_x = None
    if minimap:
        mx = cv.w - mm_w
        cv.rect(mx, tabs_h, mm_w, cv.h - tabs_h, th['panel'])
        yy = tabs_h + 4
        while yy < cv.h - 4:
            xx = mx + 6
            for _ in range(R.randint(0, 6)):
                ww = R.randint(2, 18)
                cv.rect(xx, yy, ww, 2, R.choice([th['fg'], th['kw'], th['st'], th['com']]), alpha=0.7)
                xx += ww + R.randint(1, 3)
            yy += 3
    cv.rect(0, cv.h - 22, cv.w, 22, th['accent'])
    cv.text(10, cv.h - 19, f'Ln {R.randint(1, 400)}, Col {R.randint(1, 80)}   Spaces: 4   UTF-8   LF   {R.choice(["Python 3.12", "JavaScript", "Go 1.23"])}', ctx['ui'], ctx['ui_size'] - 2, '#ffffff')
    return {'variant': 'minimap' if minimap else 'plain'}


def diff(R, cv, th, ctx):
    lines = [['diff --git a/.env b/.env'], ['index ', dec(C.sha(R, 7), 'short_sha'), '..', dec(C.sha(R, 7), 'short_sha'), ' 100644'], '--- a/.env', '+++ b/.env', f'@@ -{R.randint(1, 40)},7 +{R.randint(1, 40)},7 @@']
    for _ in range(R.randint(10, 24)):
        r = R.random()
        if r < 0.2:
            sign = R.choice(['+', '-'])
            lines.append([sign, C.token(R, R.choice(['aws', 'gh', 'slack', 'stripe', 'openai', 'jwt']))])
        elif r < 0.35:
            sign = R.choice(['+', '-'])
            lines.append([f'{sign}API_TOKEN=', C.token(R, R.choice(['gh', 'hex40', 'b64_40']))])
        elif r < 0.45:
            lines.append(['{+', C.token(R, R.choice(['gh', 'aws', 'npm'])), '+}', ' [-', dec('REDACTED', 'placeholder'), '-]'])
        elif r < 0.52:
            lines.append([R.choice(['+', '-']), 'DB_PASSWORD=', sec(C.password(R), 'key', 'diff_glued_name')])
        elif r < 0.56:
            lines.append([R.choice(['+', '-']), C.card(R, sep='-')])
        elif r < 0.6:
            lines.append([R.choice(['+', '-', ' ']), '"integrity": "', dec('sha512-' + C.rs(R, C.B64, 86) + '==', 'sri'), '"'])
        else:
            lines.append([R.choice(['+', '-', ' ']), R.choice(['PORT=3000', 'LOG_LEVEL=debug', 'NODE_ENV=production', 'FEATURE_X=true', '# keep in sync'])])
    term_lines(R, cv, th, ctx, lines)
    return {'variant': 'unified'}


def chat(R, cv, th, ctx):
    f, size = ctx['ui'], ctx['ui_size']
    side = R.random() < 0.6
    sx = 220 if side else 0
    if side:
        cv.rect(0, 0, sx, cv.h, th['panel'] if not th['dark'] else '#1e1f22')
        y = 16
        for ch in ['# general', '# deploys', '# random', '# support', R.choice(['# design', '# billing'])]:
            cv.text(16, y, ch, f, size, '#ffffff' if th['panel'].startswith('#3f') or th['dark'] else th['fg'])
            y += 28
    y = 16
    me = ctx['realName']
    while y < cv.h - 70:
        mine = R.random() < 0.25
        fn, ln = (me.split()[0], me.split()[-1]) if mine else C.person(R)
        who = sec(f'{fn} {ln}', 'name', 'chat_author') if mine else f'{fn} {ln}'
        ax = sx + 16
        av = R.choice(['circle', 'initials', 'photo', 'square'])
        col = R.choice(AVATAR)
        if av == 'circle':
            cv.circle(ax + 18, y + 18, 18, col)
        elif av == 'initials':
            cv.circle(ax + 18, y + 18, 18, col)
            cv.text(ax + 8, y + 8, f'{fn[0]}{ln[0]}', f, size, '#ffffff', weight='bold', record=False)
        elif av == 'photo':
            cv.gradient(ax, y, 36, 36, col, R.choice(AVATAR))
            cv.circle(ax + 18, y + 14, 8, '#f1d1b5')
        else:
            cv.rect(ax, y, 36, 36, col, radius=6)
        nx = ax + 48
        w, _ = cv.text(nx, y, who, f, size, th['fg'], weight='bold')
        cv.text(nx + w + 10, y + 1, dec(f'{R.randint(1, 12)}:{R.randint(0, 59):02d} {R.choice(["AM", "PM"])}', 'time'), f, size - 2, th['muted'])
        r = R.random()
        if r < 0.12:
            msg = ['can you email ', C.email(R), ' about it?']
        elif r < 0.22:
            msg = ['call me on ', C.phone(R), ' if it breaks again']
        elif r < 0.30:
            msg = ['the staging key is ', C.token(R, R.choice(['gh', 'openai', 'stripe', 'aws', 'slack'])), ' — rotate after']
        elif r < 0.36:
            msg = ['Your verification code is ', sec(C.rs(R, C.DIG, 6), 'gap', 'otp')]
        elif r < 0.44:
            msg = ['order ', dec(f'#{R.randint(10**6, 10**8)}', 'order_number'), ' shipped, tracking ', dec('1Z' + C.rs(R, C.UP + C.DIG, 16), 'tracking')]
        elif r < 0.50:
            msg = ['bumped to ', dec(C.version(R), 'version'), ' and redeployed ', R.choice(['🚀', '🎉', '✅', '👍'])]
        elif r < 0.55:
            msg = ['server is at ', C.ipv4(R), ' now']
        elif r < 0.60:
            msg = [R.choice(['🔑', '👉', '📧']), C.token(R, R.choice(['gh', 'openai'])) if R.random() < 0.5 else C.email(R)]
        elif r < 0.64:
            msg = ['@', R.choice(C.FIRST).lower(), ' ', C.sentence(R, 5), ' ', R.choice(['😂', '🙏', '❤️', '🔥', '👀'])]
        else:
            msg = [C.sentence(R, R.randint(4, 14)), R.choice(['', '', ' 😅', ' 🎉', ' 👍'])]
        _, mh = cv.text(nx, y + size + 8, msg, f, size, th['fg'], width=cv.w - nx - 30)
        bottom = y + size + 8 + max(mh, size)  # a wrapped message pushes the next one down
        if R.random() < 0.2:
            rx = nx
            for emo in R.sample(['👍', '❤️', '😂', '🎉', '👀', '✅'], R.randint(1, 3)):
                cv.rect(rx, bottom + 6, 46, 22, th['panel'], radius=10)
                cv.text(rx + 6, bottom + 7, f'{emo} {R.randint(1, 12)}', f, size - 2, th['fg'])
                rx += 54
            bottom += 30
        y = bottom + 20
    return {'variant': 'sidebar' if side else 'plain'}


def email_client(R, cv, th, ctx):
    f, size = ctx['ui'], ctx['ui_size']
    form = R.choice(['reading', 'reading', 'source'])
    if form == 'source':
        fn, ln = C.person(R)
        lines = [['Return-Path: <', C.email(R, fn, ln), '>'], ['Received: from mail-', dec(C.rs(R, C.LO + C.DIG, 6), 'host_part'), '.google.com (mail.google.com [', C.ipv4(R), '])'],
                 ['Message-ID: <', dec(C.rs(R, C.ALNUM + '=+', 36) + '@mail.gmail.com', 'message_id'), '>'],
                 ['DKIM-Signature: v=1; a=rsa-sha256; c=relaxed/relaxed; d=gmail.com; s=20230601;'], ['        h=to:subject:message-id:date:from:mime-version; bh=', dec(C.rs(R, C.B64, 43) + '=', 'dkim_bh'), ';'],
                 ['        b=', dec(C.rs(R, C.B64, 70), 'dkim_b')], ['        ', dec(C.rs(R, C.B64, 70), 'dkim_b')], ['        ', dec(C.rs(R, C.B64, 40) + '==', 'dkim_b')],
                 ['From: ', f'{fn} {ln} <', C.email(R, fn, ln), '>'], ['To: ', sec(ctx['realName'], 'name', 'mail_to'), ' <', C.email(R), '>'], 'Subject: ' + C.sentence(R, 5),
                 ['X-Google-Smtp-Source: ', dec(C.rs(R, C.B64, 60), 'smtp_source')], 'Content-Type: text/plain; charset="UTF-8"']
        term_lines(R, cv, th, ctx, lines, font=ctx['mono'], size=ctx['mono_size'] - 1)
        return {'variant': form}
    lw = 320
    cv.rect(0, 0, lw, cv.h, th['panel'])
    y = 12
    for _ in range(12):
        fn, ln = C.person(R)
        cv.text(14, y, f'{fn} {ln}', f, size, th['fg'], weight='bold')
        cv.text(lw - 60, y, dec(R.choice([f'{R.randint(1, 12)}:{R.randint(0, 59):02d}', 'Nov ' + str(R.randint(1, 28)), 'Yesterday']), 'date'), f, size - 2, th['muted'])
        cv.text(14, y + size + 6, C.sentence(R, 5)[:38], f, size - 1, th['muted'])
        y += 2 * size + 22
    x = lw + 24
    fn, ln = C.person(R)
    y = 20
    cv.text(x, y, C.sentence(R, 6), f, size + 5, th['fg'], weight='bold')
    y += 40
    for label, value in [('From', [f'{fn} {ln} <', C.email(R, fn, ln), '>']), ('To', [sec(ctx['realName'], 'name', 'mail_to'), ' <', C.email(R), '>']),
                         ('Cc', [C.email(R), ', ', C.email(R)]), ('Date', [dec(f'Sun, {R.randint(1, 28)} Nov 2026 {R.randint(0, 23):02d}:{R.randint(0, 59):02d}:{R.randint(0, 59):02d} +0000', 'date')])]:
        cv.text(x, y, [f'{label}: '] + value, f, size, th['fg'])
        y += lh(size) + 2
    y += 14
    body = [[f'Hi {ctx["realName"].split()[0]},'], [C.sentence(R, 12)], [C.sentence(R, 9)]]
    if R.random() < 0.3:
        body.append(['Your temporary password is ', sec(C.password(R), 'key', 'temp_password'), '. Change it after you sign in.'])
    if R.random() < 0.3:
        body.append(['Reset link: https://accounts.example.com/reset?token=', C.token(R, 'b64_40')])
    if R.random() < 0.25:
        body.append(['Mot de passe : ', sec(R.choice(['prairie-lune', 'Soleil2026!', 'chat-noir-42']), 'key', 'fr_password')])
    body += ['', ['Best,'], [f'{fn} {ln}'], [R.choice(['Head of Ops', 'Support', 'Account Manager'])], ['M ', C.phone(R)], ['T ', C.phone(R)], [C.email(R, fn, ln)]]
    for segs in body:
        _, hgt = cv.text(x, y, segs, f, size, th['fg'], width=cv.w - x - 30)
        y += max(lh(size), hgt) + 2
    return {'variant': form}


def contact_card(R, cv, th, ctx):
    f, size = ctx['ui'], max(13, ctx['ui_size'])
    cv.text(16, 8, dec(f'{R.randint(0, 23):02d}:{R.randint(0, 59):02d}', 'time'), f, size - 1, th['fg'], weight='bold')
    cv.text(cv.w - 70, 8, f'{R.randint(5, 100)}%', f, size - 2, th['fg'])
    form = R.choice(['contact', 'contact', 'about'])
    y = 50
    if form == 'contact':
        fn, ln = C.person(R)
        cv.circle(cv.w / 2, y + 50, 46, R.choice(AVATAR))
        cv.text(cv.w / 2 - 20, y + 32, f'{fn[0]}{ln[0]}', f, 30, '#ffffff', weight='bold', record=False)
        y += 116
        cv.text(cv.w / 2 - 80, y, f'{fn} {ln}', f, size + 8, th['fg'])
        y += 50
        country = R.choice(['us', 'uk', 'de', 'fr', 'jp', 'cn', 'in', 'br', 'au', 'ru', 'kr', 'nl', 'es', 'it'])
        for label, value in [('mobile', C.phone(R, country)), ('work', C.phone(R, country)), ('email', C.email(R, fn, ln)), ('home', C.email(R, fn, ln)),
                             ('birthday', dec(f'{R.randint(1, 28)} {R.choice(["March", "June", "Oct"])} {R.randint(1960, 2005)}', 'date')),
                             ('address', f'{R.randint(1, 200)} {R.choice(["Elm St", "Rue Victor Hugo", "Hauptstraße"])}')]:
            cv.text(30, y, label, f, size - 2, th['muted'])
            cv.text(30, y + size + 4, value, f, size + 1, th['accent'] if label in ('mobile', 'work', 'email', 'home') else th['fg'])
            y += 2 * size + 24
            cv.line(30, y - 10, cv.w - 30, y - 10, th['border'])
    else:
        rows = [('Device name', f"{ctx['realName'].split()[0]}'s Pixel"), ('Model', R.choice(['Pixel 9', 'Galaxy S24', 'Fairphone 5'])), ('Android version', str(R.randint(13, 16))),
                ('Build number', dec(f'{C.rs(R, C.UP, 2)}{R.randint(1, 4)}A.{R.randint(240101, 251231)}.{R.randint(1, 30):03d}', 'build_number')),
                ('IMEI', neu(C.imei(R), 'imei')), ('IMEI (slot 2)', neu(' '.join([(i := C.imei(R))[:2], i[2:8], i[8:14], i[14]]), 'imei')),
                ('Serial number', neu(C.rs(R, C.UP + C.DIG, 12), 'serial')), ('IP address', C.ipv4(R)), ('IPv6', C.ipv6(R)),
                ('Wi-Fi MAC address', neu(C.mac(R), 'mac')), ('Bluetooth address', neu(C.mac(R).upper(), 'mac')), ('Phone number', C.phone(R)),
                ('ICCID', neu(' '.join([(cc := '8901' + C.rs(R, C.DIG, 15))[i:i + 4] for i in range(0, 19, 4)]), 'iccid')), ('Up time', f'{R.randint(1, 300)}:{R.randint(0, 59):02d}:{R.randint(0, 59):02d}')]
        for label, value in rows:
            if y > cv.h - 40:
                break
            cv.text(24, y, label, f, size, th['fg'])
            cv.text(24, y + size + 4, value, f, size - 1, th['muted'])
            y += 2 * size + 18
    return {'variant': form}


def network(R, cv, th, ctx):
    form = R.choice(['ip', 'ss', 'nmcli', 'router', 'ping', 'nmap'])
    if form == 'router':
        f, size = ctx['ui'], ctx['ui_size']
        y = 24
        cv.text(24, y, 'Wireless Settings', f, size + 6, th['fg'], weight='bold')
        y += 48
        for label, value in [('Network Name (SSID)', R.choice(['HomeNet-5G', 'FRITZ!Box 7590 XY', 'Vodafone-AB12'])), ('Security', 'WPA2-PSK [AES]'),
                             (R.choice(['WPA Key', 'Passphrase', 'Pre-shared Key', 'Password', 'Wi-Fi Password']), sec(R.choice(['orchard-lantern-42', C.password(R, False), 'tuna salad 77']), 'key', 'label_far')),
                             ('Channel', str(R.choice([1, 6, 11, 36, 44]))), ('WAN IP', C.ipv4(R)), ('Gateway', C.ipv4(R)), ('DNS', [C.ipv4(R), ', ', C.ipv4(R)]), ('MAC', neu(C.mac(R).upper(), 'mac'))]:
            cv.text(24, y, label, f, size, th['muted'])
            cv.text(260 + R.randint(0, 60), y, value, f, size, th['fg'])
            y += lh(size) + 12
        return {'variant': form}
    lines = []
    if form == 'ip':
        lines.append(prompt(ctx) + ['ip addr'])
        for i, dev in enumerate(['lo', 'enp3s0', 'wlp2s0', 'docker0']):
            lines.append(f'{i + 1}: {dev}: <BROADCAST,MULTICAST,UP,LOWER_UP> mtu 1500 qdisc fq_codel state UP group default qlen 1000')
            lines.append(['    link/ether ', neu(C.mac(R), 'mac'), ' brd ', neu('ff:ff:ff:ff:ff:ff', 'mac')])
            lines.append(['    inet ', C.ipv4(R), ' brd ', C.ipv4(R), ' scope global dynamic noprefixroute ', dev])
            lines.append(['    inet6 ', C.ipv6(R), '/64 scope link'])
            lines.append(f'       valid_lft {R.randint(1000, 90000)}sec preferred_lft {R.randint(1000, 90000)}sec')
    elif form == 'ss':
        lines.append(prompt(ctx) + ['ss -tlnp'])
        lines.append('State   Recv-Q  Send-Q   Local Address:Port    Peer Address:Port  Process')
        for _ in range(16):
            lines.append(['LISTEN  0       4096     ', C.ipv4(R), '        0.0.0.0:*', f'          users:(("{R.choice(["sshd", "nginx", "postgres", "node"])}",pid={R.randint(100, 99999)},fd={R.randint(3, 40)}))'])
    elif form == 'nmcli':
        lines.append(prompt(ctx) + ['nmcli -s connection show HomeNet'])
        for k, v in [('connection.id', 'HomeNet'), ('connection.uuid', dec(C.uuid(R), 'uuid')), ('connection.interface-name', 'wlp2s0'), ('802-11-wireless.ssid', 'HomeNet'),
                     ('802-11-wireless.mac-address', neu(C.mac(R).upper(), 'mac')), ('802-11-wireless-security.key-mgmt', 'wpa-psk'),
                     ('802-11-wireless-security.psk', sec(R.choice(['orchard-lantern', C.password(R, False), C.password(R)]), 'key', 'nm_psk')),
                     ('ipv4.addresses', C.ipv4(R)), ('ipv4.gateway', C.ipv4(R)), ('ipv4.dns', C.ipv4(R)), ('IP4.ADDRESS[1]', C.ipv4(R)), ('IP6.ADDRESS[1]', C.ipv6(R))]:
            lines.append([f'{k + ":":<40}', v])
    elif form == 'ping':
        tgt = C.ipv4(R)
        lines.append(prompt(ctx) + ['ping -c 5 ', tgt])
        for i in range(5):
            lines.append(['64 bytes from ', tgt, f': icmp_seq={i + 1} ttl={R.randint(40, 64)} time={R.uniform(1, 90):.1f} ms'])
        lines.append(f'5 packets transmitted, 5 received, 0% packet loss, time {R.randint(4000, 4100)}ms')
    else:
        lines.append(prompt(ctx) + ['nmap -sV ', C.ipv4(R)])
        lines.append(['Nmap scan report for ', R.choice(['nas.lan', 'printer.lan', 'router.lan']), ' (', C.ipv4(R), ')'])
        lines.append('PORT     STATE SERVICE VERSION')
        for port, svc in [(22, 'ssh     OpenSSH 9.6p1 Ubuntu 3ubuntu13.5'), (80, 'http    nginx 1.24.0'), (443, 'ssl/http nginx 1.24.0'), (5432, 'postgresql PostgreSQL DB 16.4')]:
            lines.append(f'{port}/tcp'.ljust(9) + 'open  ' + svc)
        lines.append(['MAC Address: ', neu(C.mac(R).upper(), 'mac'), ' (Synology)'])
    term_lines(R, cv, th, ctx, lines)
    return {'variant': form}


def csv_editor(R, cv, th, ctx):
    """A customer export open in an editor: line numbers in the gutter, fields glued by commas."""
    lines = ['id,name,email,phone,card,created']
    for i in range(30):
        fn, ln = C.person(R)
        lines.append([str(R.randint(1000, 99999)), f',{fn} {ln},', C.email(R, fn, ln), ',', C.phone(R, R.choice(['us', 'uk', 'de'])), ',', C.card(R, sep=R.choice(['', ' ', '-'])), ',',
                      dec(f'2026-{R.randint(1, 11):02d}-{R.randint(1, 28):02d}', 'date')])
    ide(R, cv, th, ctx, lines)
    return {'variant': 'csv'}


def split_panes(R, cv, th, ctx):
    """Two terminals side by side (tmux): a git log on the left, a token file on the right, rows aligned."""
    f, size = ctx['mono'], ctx['mono_size']
    half = cv.w / 2
    cv.line(half, 0, half, cv.h, th['border'], 2)
    y = 12
    i = 0
    while y < cv.h - 2 * size:
        left = R.choice([['commit ', dec(C.sha(R), 'git_sha_commit')], ['    ', C.sentence(R, 4)], ['sha256: ', dec(C.sha(R, 64)[:40], 'digest')], ['Author: ', C.email(R)]])
        cv.text(10, y, left, f, size, th['fg'])
        right = R.choice([[C.token(R, R.choice(['hex40', 'twilio_sid_secret', 'b64_40']))], ['# rotated ', dec(f'2026-11-{R.randint(1, 28):02d}', 'date')], [C.token(R, 'aws')]])
        cv.text(half + 10, y, right, f, size, th['fg'])
        y += lh(size)
        i += 1
    return {'variant': 'tmux'}


def file_manager(R, cv, th, ctx):
    f, size = ctx['ui'], ctx['ui_size']
    cv.rect(0, 0, cv.w, 40, th['panel'])
    cv.text(16, 11, ['/home/', sec(ctx['login'], 'name', 'path'), '/', R.choice(['Pictures', 'Downloads', 'Pictures/Screenshots'])], f, size, th['fg'])
    y = 52
    cv.text(16, y, 'Name', f, size, th['muted'], weight='bold')
    cv.text(cv.w - 330, y, 'Size', f, size, th['muted'], weight='bold')
    cv.text(cv.w - 220, y, 'Modified', f, size, th['muted'], weight='bold')
    y += lh(size) + 8
    while y < cv.h - 30:
        dd, mm, hh, mi, ss = R.randint(1, 28), R.randint(1, 12), R.randint(0, 23), R.randint(0, 59), R.randint(0, 59)
        name = R.choice([dec(f'Screenshot from 2026-{mm:02d}-{dd:02d} {hh:02d}-{mi:02d}-{ss:02d}.png', 'screenshot_name', 'gnome'), dec(f'Screenshot_20261{mm % 10}{dd:02d}_{hh:02d}{mi:02d}{ss:02d}.png', 'screenshot_name', 'kde'),
                         dec(f'IMG_2026{mm:02d}{dd:02d}_{hh:02d}{mi:02d}{ss:02d}.jpg', 'camera_name'), dec(f'VID-2026{mm:02d}{dd:02d}-WA{R.randint(0, 99):04d}.mp4', 'camera_name'),
                         dec(f'Screenshot 2026-{mm:02d}-{dd:02d} at {hh:02d}.{mi:02d}.{ss:02d}.png', 'screenshot_name', 'mac'), dec(f'invoice-2026-{R.randint(1, 999999):06d}.pdf', 'file_name'),
                         [sec(ctx['login'], 'name', 'file'), '-backup.tar.gz'], dec(f'scan_{R.randint(10**9, 10**10 - 1)}.pdf', 'file_name')])
        cv.text(16, y, name, f, size, th['fg'])
        cv.text(cv.w - 330, y, dec(R.choice([f'{R.uniform(0.1, 900):.1f} MB', f'{R.randint(1, 999)} kB', f'{R.randint(1000, 999999):,} bytes']), 'size'), f, size, th['fg'])
        cv.text(cv.w - 220, y, dec(R.choice([f'{dd} Nov 2026 {hh:02d}:{mi:02d}', 'Yesterday', f'{mm}/{dd}/26', f'{hh:02d}:{mi:02d}']), 'date'), f, size, th['fg'])
        y += lh(size) + 10
    return {'variant': 'list'}


def crypto(R, cv, th, ctx):
    f, size = ctx['ui'], ctx['ui_size']
    form = R.choice(['wallet', 'wallet', 'seed'])
    y = 24
    if form == 'seed':
        cv.text(24, y, R.choice(['Your recovery phrase', 'Secret Recovery Phrase', 'Write down these words']), f, size + 6, th['fg'], weight='bold')
        y += 50
        n = R.choice([12, 24])
        words = [R.choice(C.BIP39) for _ in range(n)]
        cols = 3 if n == 12 else 4
        cw = (cv.w - 48) / cols
        for i, w in enumerate(words):
            cv.rect(24 + (i % cols) * cw, y + (i // cols) * 44, cw - 12, 36, th['panel'], radius=8)
            cv.text(36 + (i % cols) * cw, y + (i // cols) * 44 + 8, [f'{i + 1}. ', sec(w, 'gap', 'seed_word')], f, size, th['fg'])
        return {'variant': form}
    cv.text(24, y, dec(f'{R.uniform(0, 50):.8f} BTC', 'balance'), f, size + 12, th['fg'], weight='bold')
    y += 60
    for label, value in [('Bitcoin address', neu(R.choice(['1', '3']) + C.b58(R, 33), 'btc_address')), ('SegWit address', neu('bc1q' + C.rs(R, 'qpzry9x8gf2tvdw0s3jn54khce6mua7l', 38), 'btc_address')),
                         ('Solana', neu(C.b58(R, 44), 'sol_address')), ('Ethereum', neu('0x' + C.rs(R, C.HEX + 'ABCDEF', 40), 'eth_address')),
                         ('Last transaction', dec('0x' + C.sha(R, 64), 'eth_tx')), ('Signature', dec(C.b58(R, 88), 'sol_signature'))]:
        cv.text(24, y, label, f, size - 1, th['muted'])
        _, hgt = cv.text(24, y + size + 6, value, ctx['mono'], size, th['fg'], width=cv.w - 60, wrap='char')
        y += size + 6 + max(hgt, size) + 26
    return {'variant': form}


def sports_games_music(R, cv, th, ctx):
    f, size = ctx['ui'], ctx['ui_size']
    form = R.choice(['league', 'leaderboard', 'music'])
    y = 20
    if form == 'league':
        cv.text(24, y, 'Pos  Team                     P   W   D   L   GF  GA  GD  Pts', ctx['mono'], size, th['muted'])
        y += lh(size) + 6
        for pos in range(1, 21):
            p_ = R.randint(10, 38)
            w = R.randint(0, p_)
            d = R.randint(0, p_ - w)
            l_ = p_ - w - d
            gf, ga = R.randint(10, 90), R.randint(10, 90)
            team = R.choice(['Arsenal', 'Real Betis', 'Bayern', 'Ajax', 'Porto', 'Celtic', 'Lyon', 'Napoli', 'Benfica', 'Sevilla'])
            cv.text(24, y, dec(f'{pos:>3}  {team:<24} {p_:>2}  {w:>2}  {d:>2}  {l_:>2}  {gf:>3} {ga:>3} {gf - ga:>+3}  {3 * w + d:>3}', 'table_row'), ctx['mono'], size, th['fg'])
            y += lh(size) + 4
    elif form == 'leaderboard':
        for rank in range(1, 16):
            fn, ln = C.person(R)
            cv.text(24, y, [f'#{rank}  ', f'{fn}{R.randint(1, 999)}', '   ', dec(f'{R.randint(10**4, 10**7):,}', 'score'), '   Lv ', str(R.randint(1, 99)), '   ', neu(f'SW-{C.rs(R, C.DIG, 4)}-{C.rs(R, C.DIG, 4)}-{C.rs(R, C.DIG, 4)}', 'friend_code')], f, size, th['fg'])
            y += lh(size) + 8
    else:
        cv.text(24, y, R.choice(['Discovery', 'Random Access Memories', 'Kind of Blue', 'OK Computer']), f, size + 8, th['fg'], weight='bold')
        y += 44
        cv.text(24, y, [dec(f'℗ {R.randint(1970, 2026)}', 'year'), '  ', dec(f'{R.randint(8, 16)} songs, {R.randint(30, 79)} min', 'count'), '  ISRC ', dec(C.rs(R, C.UP, 2) + C.rs(R, C.UP + C.DIG, 3) + C.rs(R, C.DIG, 7), 'isrc')], f, size - 1, th['muted'])
        y += 34
        for t in range(1, 15):
            cv.text(24, y, [dec(f'{t:02d}', 'track_no'), '   ', R.choice(['One More Time', 'Aerodynamic', 'Digital Love', 'So What', 'Airbag', 'Paranoid Android', 'Contact', 'Giorgio']),
                            '   ', dec(f'{R.randint(1, 9)}:{R.randint(0, 59):02d}', 'duration')], f, size, th['fg'])
            y += lh(size) + 10
    return {'variant': form}


def maps(R, cv, th, ctx):
    f, size = ctx['ui'], ctx['ui_size']
    cv.rect(0, 0, cv.w, cv.h, '#e8eae6' if not th['dark'] else '#1f2a33')
    for _ in range(30):
        cv.line(R.randint(0, cv.w), R.randint(0, cv.h), R.randint(0, cv.w), R.randint(0, cv.h), '#ffffff' if not th['dark'] else '#3c4a55', R.choice([2, 4, 8]))
    for _ in range(12):
        cv.text(R.randint(0, cv.w - 120), R.randint(0, cv.h - 20), R.choice(['Market St', 'Mission', 'Oak Ave', 'Rue du Bac', 'Hauptbahnhof', 'Central Park']), f, size - 2, th['muted'])
    px, py = 20, 20
    cv.rect(px, py, 380, 300, th['bg'], radius=12)
    lat, lon = R.uniform(-60, 60), R.uniform(-170, 170)
    y = py + 14
    for segs in [[R.choice(['Blue Bottle Coffee', 'Bäckerei Lindner', 'City Library', 'Hotel Central'])], [f'{R.uniform(3.5, 5):.1f} ★ ({R.randint(10, 9999):,})'],
                 [f'{R.randint(1, 300)} {R.choice(["Market St", "Hauptstr.", "Rue de Lyon"])}'], ['Phone: ', C.phone(R)],
                 [dec(f'{lat:.6f}, {lon:.6f}', 'coordinates')], [dec(f'{abs(lat):.0f}°{R.randint(0, 59)}\'{R.uniform(0, 59):.1f}"{"N" if lat > 0 else "S"} {abs(lon):.0f}°{R.randint(0, 59)}\'{R.uniform(0, 59):.1f}"{"E" if lon > 0 else "W"}', 'coordinates_dms')],
                 [dec(C.rs(R, '23456789CFGHJMPQRVWX', 4) + C.rs(R, '23456789CFGHJMPQRVWX', 4) + '+' + C.rs(R, '23456789CFGHJMPQRVWX', 2), 'plus_code')],
                 [f'{R.randint(2, 59)} min ({R.uniform(0.5, 40):.1f} km)']]:
        cv.text(px + 16, y, segs, f, size, th['fg'])
        y += lh(size) + 8
    return {'variant': 'place'}


def design(R, cv, th, ctx):
    f, size = ctx['ui'], ctx['ui_size']
    y = 24
    form = R.choice(['picker', 'palette', 'css'])
    if form == 'css':
        lines = [':root {']
        for _ in range(18):
            lines.append(['  --', R.choice(['accent', 'bg', 'fg', 'muted', 'border', 'shadow', 'brand']), f'-{R.choice([50, 100, 200, 500, 700, 900])}: ', dec('#' + C.rs(R, C.HEX, 6), 'css_hex'), ';'])
        lines.append(['  --gradient: linear-gradient(90deg, ', dec('#' + C.rs(R, C.HEX, 6), 'css_hex'), ' 0%, ', dec('#' + C.rs(R, C.HEX, 6), 'css_hex'), ' 100%);'])
        lines.append('}')
        ide(R, cv, th, ctx, lines)
        return {'variant': form}
    n = R.randint(4, 6)
    sw = (cv.w - 48) / n
    for i in range(n):
        hx = C.rs(R, '0123456789ABCDEF', 6)
        r_, g_, b_ = int(hx[:2], 16), int(hx[2:4], 16), int(hx[4:], 16)
        cv.rect(24 + i * sw, y, sw - (0 if form == 'palette' else 10), 200, '#' + hx)
        tc = '#000000' if r_ * 0.3 + g_ * 0.6 + b_ * 0.1 > 140 else '#ffffff'
        label = dec(hx if form == 'palette' else '#' + hx, 'css_hex', 'palette')
        cv.text(24 + i * sw + 12, y + 170, label, f, size + 2, tc, weight='bold')
        cv.text(24 + i * sw + 12, y + 210, dec(f'rgb({r_}, {g_}, {b_})', 'rgb'), f, size - 1, th['fg'])
        cv.text(24 + i * sw + 12, y + 230, dec(f'{r_} {g_} {b_}', 'rgb'), f, size - 1, th['muted'])
    return {'variant': form}


def forms(R, cv, th, ctx):
    f, size = ctx['ui'], ctx['ui_size']
    form = R.choice(['password_manager', 'api_keys', 'oauth_app', 'recovery', 'wireguard', 'checkout'])
    y = 24
    field_w = min(cv.w - 60, 560)

    def field(label, value, caret=False, mono=True):
        nonlocal y
        cv.text(28, y, label, f, size - 1, th['muted'])
        y += size + 8
        cv.rect(28, y, field_w, size * 2.2, th['panel'], radius=6)
        cv.frame(28, y, field_w, size * 2.2, th['border'], radius=6)
        w, _ = cv.text(38, y + size * 0.55, value, ctx['mono'] if mono else f, size, th['fg'])
        if caret:
            cv.caret(38 + w + R.choice([0, 1, 2]), y + size * 0.45, size * 1.25, th['fg'], R.choice([1, 1.5, 2]))
        y += size * 2.2 + 18

    if form == 'password_manager':
        cv.text(28, y, R.choice(['GitHub', 'Bank of Example', 'Work VPN', 'Router']), f, size + 6, th['fg'], weight='bold')
        y += 46
        field('Username', C.email(R) if R.random() < 0.6 else sec(ctx['login'], 'name', 'username'), mono=False)
        field('Password', sec(C.password(R, R.random() < 0.6), 'key', 'label_above'))
        field('One-time password (TOTP)', sec(f'{C.rs(R, C.DIG, 3)} {C.rs(R, C.DIG, 3)}', 'gap', 'otp'))
        field('Website', 'https://github.com/login', mono=False)
    elif form == 'api_keys':
        cv.text(28, y, 'API keys', f, size + 6, th['fg'], weight='bold')
        y += 46
        field('Publishable key', dec(C.p('pk', '_live_') + C.rs(R, C.ALNUM, 24), 'publishable'))
        field('Secret key', C.token(R, R.choice(['stripe', 'openai', 'anthropic'])))
        field('Webhook signing secret', sec(C.p('wh', 'sec_') + C.rs(R, C.ALNUM, 32), 'key', 'whsec'))
        field('Restricted key', sec(C.p('rk', '_live_') + C.rs(R, C.ALNUM, 24), 'key', 'rk'))
    elif form == 'oauth_app':
        cv.text(28, y, 'OAuth application', f, size + 6, th['fg'], weight='bold')
        y += 46
        field('Client ID', neu(C.rs(R, C.ALNUM, 20), 'client_id'))
        field('Client secret', sec(C.rs(R, C.ALNUM, R.choice([24, 28, 32, 40])), 'key', 'client_secret'))
        field('Redirect URI', 'https://app.example.com/oauth/callback', mono=False)
        field('Signing key ID', dec(C.uuid(R), 'uuid'))
    elif form == 'recovery':
        cv.text(28, y, 'Recovery codes', f, size + 6, th['fg'], weight='bold')
        y += 46
        for i in range(10):
            cv.text(28 + (i % 2) * 200, y + (i // 2) * (size + 14), sec(f'{C.rs(R, C.UP + C.DIG, 4)}-{C.rs(R, C.UP + C.DIG, 4)}' if R.random() < 0.5 else f'{C.rs(R, C.LO + C.DIG, 5)}-{C.rs(R, C.LO + C.DIG, 5)}', 'gap', 'recovery_code'), ctx['mono'], size, th['fg'])
        y += 5 * (size + 14) + 20
    elif form == 'wireguard':
        lines = ['[Interface]', ['PrivateKey = ', sec(C.rs(R, C.B64, 43) + '=', 'key', 'wg_private')], ['Address = ', C.ipv4(R)], 'DNS = 1.1.1.1', '',
                 '[Peer]', ['PublicKey = ', dec(C.rs(R, C.B64, 43) + '=', 'wg_public')], ['PresharedKey = ', sec(C.rs(R, C.B64, 43) + '=', 'key', 'wg_psk')],
                 ['Endpoint = ', C.ipv4(R)], 'AllowedIPs = 0.0.0.0/0, ::/0', 'PersistentKeepalive = 25']
        term_lines(R, cv, th, ctx, lines)
        return {'variant': form}
    else:
        cv.text(28, y, 'Payment', f, size + 6, th['fg'], weight='bold')
        y += 46
        caret = R.random() < 0.7
        if R.random() < 0.35:  # Stripe-style one-row field: number, expiry and CVC on one line
            cv.text(28, y, 'Card information', f, size - 1, th['muted'])
            y += size + 8
            cv.rect(28, y, field_w, size * 2.2, th['panel'], radius=6)
            cv.frame(28, y, field_w, size * 2.2, th['border'], radius=6)
            card = C.card(R, sep=' ')
            cv.text(38, y + size * 0.55, [card, '      ', dec(f'{R.randint(1, 12):02d} / {R.randint(26, 34)}', 'expiry'), '      ', sec(C.rs(R, C.DIG, 3), 'gap', 'cvc')], ctx['mono'], size, th['fg'])
            y += size * 2.2 + 18
            return {'variant': 'checkout-onerow'}
        if R.random() < 0.3:  # an order reference on the same baseline as the card number
            cv.text(28, y, ['Reference ', dec(str(R.randint(1, 99)), 'reference_no'), '        ', C.card(R, sep=' ')], ctx['mono'], size, th['fg'])
            y += lh(size) + 12
        field('Card number', C.card(R, sep=R.choice([' ', ' ', ''])), caret=caret)
        field('Expiry', dec(f'{R.randint(1, 12):02d} / {R.randint(26, 34)}', 'expiry'))
        field('CVC', sec(C.rs(R, C.DIG, 3), 'gap', 'cvc'))
        field('Email', C.email(R), caret=False, mono=False)
        return {'variant': form + ('-caret' if caret else '')}
    return {'variant': form}


def licences(R, cv, th, ctx):
    f, size = ctx['ui'], ctx['ui_size']
    y = 30
    cv.text(28, y, R.choice(['Activate your licence', 'Product key', 'Register']), f, size + 6, th['fg'], weight='bold')
    y += 50
    for label, value in [('Licence key', sec('-'.join(C.rs(R, C.UP + C.DIG, 5) for _ in range(5)), 'gap', 'licence')), ('Licensed to', sec(ctx['realName'], 'name', 'licence')),
                         ('Serial', neu(C.rs(R, C.UP + C.DIG, 4) + '-' + C.rs(R, C.DIG, 6) + '-' + C.rs(R, C.UP + C.DIG, 4), 'serial')), ('Machine ID', neu(C.sha(R, 32), 'machine_id')),
                         ('Order', dec(f'#{R.randint(10**6, 10**8)}', 'order_number')), ('Steam key', sec('-'.join(C.rs(R, C.UP + C.DIG, 5) for _ in range(3)), 'gap', 'licence'))]:
        cv.text(28, y, label, f, size - 1, th['muted'])
        cv.text(28, y + size + 6, value, ctx['mono'], size + 1, th['fg'])
        y += 2 * size + 30
    return {'variant': 'dialog'}


I18N = {
    'ru': ('Здравствуйте! Пожалуйста, проверьте настройки учётной записи и сообщите нам.', 'Пароль', 'Телефон', 'Почта', 'Ключ API'),
    'el': ('Γεια σας! Παρακαλώ ελέγξτε τις ρυθμίσεις του λογαριασμού σας σήμερα.', 'Κωδικός', 'Τηλέφωνο', 'Email', 'Κλειδί API'),
    'zh': ('您好！请检查您的账户设置，并在今天之前回复我们。', '密码', '电话', '邮箱', '密钥'),
    'ja': ('こんにちは。アカウント設定を確認して、本日中にご返信ください。', 'パスワード', '電話', 'メール', 'APIキー'),
    'ar': ('مرحبا! يرجى التحقق من إعدادات حسابك والرد علينا اليوم.', 'كلمة المرور', 'الهاتف', 'البريد', 'مفتاح'),
    'he': ('שלום! אנא בדקו את הגדרות החשבון שלכם והשיבו לנו היום.', 'סיסמה', 'טלפון', 'דוא"ל', 'מפתח'),
    'de': ('Hallo! Bitte prüfen Sie Ihre Kontoeinstellungen und antworten Sie uns heute.', 'Passwort', 'Telefon', 'E-Mail', 'API-Schlüssel'),
    'fr': ('Bonjour ! Merci de vérifier les paramètres de votre compte aujourd’hui.', 'Mot de passe', 'Téléphone', 'Courriel', 'Clé API'),
    'pl': ('Dzień dobry! Proszę sprawdzić ustawienia konta i odpowiedzieć dzisiaj.', 'Hasło', 'Telefon', 'E-mail', 'Klucz API'),
    'hi': ('नमस्ते! कृपया अपनी खाता सेटिंग जांचें और आज हमें जवाब दें।', 'पासवर्ड', 'फ़ोन', 'ईमेल', 'कुंजी'),
}
COUNTRY = {'ru': 'ru', 'el': 'us', 'zh': 'cn', 'ja': 'jp', 'ar': 'us', 'he': 'us', 'de': 'de', 'fr': 'fr', 'pl': 'de', 'hi': 'in'}


def i18n(R, cv, th, ctx):
    f, size = ctx['ui'], ctx['ui_size']
    lang = R.choice(list(I18N))
    prose, pw, tel, mail, key = I18N[lang]
    rtl = lang in ('ar', 'he')
    cjk = lang in ('zh', 'ja')
    y = 24
    width = cv.w - 60
    for _ in range(R.randint(2, 4)):
        _, hgt = cv.text(30, y, prose, f, size, th['fg'], width=width, rtl=rtl, align='right' if rtl else 'left')
        y += hgt + 10
    glue = '' if cjk else ' '
    sep = '：' if cjk else ': '
    rows = [[f'{tel}{sep}', C.phone(R, COUNTRY[lang])], [f'{mail}{sep}', C.email(R)], [f'{pw}{sep}', sec(C.password(R, R.random() < 0.5), 'key', f'{lang}_password')],
            [f'{key}{sep}', C.token(R, R.choice(['openai', 'gh', 'stripe', 'aws']))], [prose[:12] + glue, C.email(R), glue + prose[12:20]]]
    if cjk:
        rows.append([prose[:8], C.token(R, R.choice(['openai', 'gh'])), prose[8:14]])
        rows.append([f'{tel}', sec(f'1{R.choice("3589")}{C.rs(R, C.DIG, 9)}', 'phone', 'cn_unbroken')])
    if lang in ('fr', 'de', 'pl', 'ru'):
        rows.append(['Total: ', dec(C.money(R, 'fr' if lang in ('fr', 'pl', 'ru') else 'de'), 'money', lang)])
        rows.append([dec(f'{R.randint(1, 28):02d}.{R.randint(1, 12):02d}.2026 {R.randint(0, 23):02d}:{R.randint(0, 59):02d}', 'date', lang)])
    if rtl:
        rows.append([f'{tel} ', sec(f'+972 5{R.randint(0, 9)}-{C.rs(R, C.DIG, 3)}-{C.rs(R, C.DIG, 4)}', 'phone', 'rtl_plus'), ' ' + prose[:10]])
    R.shuffle(rows)
    for segs in rows:
        _, hgt = cv.text(30, y, segs, f, size, th['fg'], width=width, rtl=rtl, align='right' if rtl else 'left')
        y += hgt + 12
    return {'variant': lang}


def hexdump(R, cv, th, ctx):
    form = R.choice(['xxd', 'hexdump', 'wireshark'])
    lines = [prompt(ctx) + [{'xxd': 'xxd firmware.bin | head -40', 'hexdump': 'hexdump -C capture.bin | head', 'wireshark': ''}[form]]] if form != 'wireshark' else []
    for i in range(40):
        data = [R.randint(0, 255) for _ in range(16)]
        asc = ''.join(chr(b) if 32 <= b < 127 else '.' for b in data)
        if form == 'xxd':
            lines.append([f'{i * 16:08x}: ', dec(' '.join(f'{data[j]:02x}{data[j + 1]:02x}' for j in range(0, 16, 2)), 'hex_dump'), '  ' + asc])
        elif form == 'hexdump':
            lines.append([f'{i * 16:08x}  ', dec(' '.join(f'{b:02x}' for b in data[:8]) + '  ' + ' '.join(f'{b:02x}' for b in data[8:]), 'hex_dump'), f'  |{asc}|'])
        else:
            lines.append([f'{i * 16:04x}  ', dec(' '.join(f'{b:02x}' for b in data), 'hex_dump'), '   ' + asc])
    term_lines(R, cv, th, ctx, lines)
    return {'variant': form}


def cloud_console(R, cv, th, ctx):
    f, size = ctx['ui'], ctx['ui_size']
    acct = C.rs(R, C.DIG, 12)
    cv.rect(0, 0, cv.w, 40, '#232f3e')
    cv.text(cv.w - 330, 11, [sec(ctx['login'], 'name', 'console'), ' @ ', neu(f'{acct[:4]}-{acct[4:8]}-{acct[8:]}', 'aws_account')], f, size, '#ffffff')
    y = 60
    for label, value in [('ARN', ['arn:aws:iam::', neu(acct, 'aws_account'), ':role/deploy-', C.rs(R, C.LO, 6)]), ('ETag', dec(C.sha(R, 32), 'etag')),
                         ('Request ID', dec(C.uuid(R), 'uuid')), ('Distribution ID', dec('E' + C.rs(R, C.UP + C.DIG, 13), 'cf_distribution')),
                         ('Access key ID', C.token(R, 'aws')), ('Secret access key', C.token(R, 'aws_secret')), ('Created', dec(f'2026-11-{R.randint(1, 28):02d} {R.randint(0, 23):02d}:{R.randint(0, 59):02d} UTC', 'date')),
                         ('S3 URI', f's3://{R.choice(C.COMPANIES)}-backups/2026/11/'), ('Version ID', dec(C.rs(R, C.ALNUM + '._', 32), 's3_version_id')),
                         ('KMS key', ['arn:aws:kms:eu-west-1:', neu(acct, 'aws_account'), ':key/', dec(C.uuid(R), 'uuid')])]:
        cv.text(24, y, label, f, size - 1, th['muted'])
        cv.text(240, y, value, f, size, th['fg'])
        y += lh(size) + 12
    return {'variant': 'aws'}


def browser(R, cv, th, ctx):
    f, size = ctx['ui'], ctx['ui_size']
    cv.rect(0, 0, cv.w, 80, th['panel'])
    tabs = ['Notion', 'Docs', 'Linear', 'YouTube', 'GitHub']
    for i, t in enumerate(R.sample(tabs, 3)):
        cv.rect(10 + i * 190, 6, 180, 30, th['bg'] if i == 0 else th['panel'], radius=6)
        cv.text(22 + i * 190, 12, t, f, size - 1, th['fg'])
    url = R.choice([
        ['https://www.notion.so/', R.choice(['Roadmap', 'Team-Wiki', 'Q4-Planning']), '-', dec(C.sha(R, 32), 'notion_id')],
        ['https://docs.google.com/document/d/', neu(C.rs(R, C.B64URL, 44), 'gdoc_id'), '/edit'],
        ['https://linear.app/acme/issue/ENG-', str(R.randint(100, 9999)), '/fix-the-thing'],
        ['https://www.youtube.com/watch?v=', dec(C.rs(R, C.B64URL, 11), 'video_id'), '&t=', str(R.randint(10, 999)), 's'],
        ['https://www.figma.com/design/', dec(C.rs(R, C.ALNUM, 22), 'figma_key'), '/Mockups'],
        ['https://app.example.com/oauth/callback?code=', sec(C.rs(R, C.ALNUM, R.choice([20, 32, 40])), 'gap', 'oauth_code'), '&state=', dec(C.rs(R, C.ALNUM, 16), 'oauth_state')],
        ['https://app.example.com/login/magic?token=', C.token(R, 'b64_40')],
        ['https://cloud.example.com/s/', sec(C.rs(R, C.ALNUM, 15), 'gap', 'share_link')],
        ['https://', R.choice(C.COMPANIES), '-media.s3.amazonaws.com/a.png?X-Amz-Algorithm=AWS4-HMAC-SHA256&X-Amz-Credential=', C.token(R, 'aws'), '%2F20261122%2Fus-east-1%2Fs3%2Faws4_request&X-Amz-Signature=', sec(C.sha(R, 64), 'key', 'presign_sig')],
        ['https://github.com/acme/app/commit/', dec(C.sha(R), 'git_sha_url')],
    ])
    cv.rect(10, 42, cv.w - 20, 30, th['bg'], radius=15)
    cv.text(28, 48, url, f, size, th['fg'])
    y = 110
    for _ in range(8):
        _, hgt = cv.text(40, y, C.sentence(R, R.randint(8, 18)), pick(R, SERIF) if R.random() < 0.3 else f, size + 1, th['fg'], width=cv.w - 80)
        y += hgt + 14
    return {'variant': 'urlbar'}


def watermark(R, cv, th, ctx):
    f, size = ctx['ui'], ctx['ui_size'] + 1
    y = 30
    for _ in range(14):
        r = R.random()
        if r < 0.3:
            segs = [C.sentence(R, 5), ' ', C.email(R), ' ', C.sentence(R, 3)]
        elif r < 0.5:
            segs = [C.sentence(R, 4), ' ', C.phone(R)]
        elif r < 0.6:
            segs = ['IBAN ', neu(f'DE{R.randint(10, 99)} ' + ' '.join(C.rs(R, C.DIG, 4) for _ in range(4)) + ' ' + C.rs(R, C.DIG, 2), 'iban')]
        else:
            segs = [C.sentence(R, R.randint(8, 14))]
        _, hgt = cv.text(40, y, segs, f, size, th['fg'], width=cv.w - 80)
        y += hgt + 12
    word = R.choice(['CONFIDENTIAL', 'DRAFT', 'SAMPLE', 'INTERNAL USE ONLY', 'COPY'])
    cv.cr.save()
    cv.cr.translate(cv.w * 0.15, cv.h * 0.85)
    cv.cr.rotate(-0.6)
    cv.text(0, 0, word, f, R.choice([60, 80, 110]), '#c0392b' if not th['dark'] else '#ff8a80', weight='bold', record=False)
    cv.cr.restore()
    # translucent: draw again faded is not possible with Pango alpha; overlay a veil instead
    cv.rect(0, 0, cv.w, cv.h, th['bg'], alpha=R.uniform(0.0, 0.35))
    return {'variant': word}


def http_headers(R, cv, th, ctx):
    lines = []
    form = R.choice(['devtools', 'curl'])
    if form == 'curl':
        lines.append(prompt(ctx) + ["curl -H 'Authorization: Basic ", sec(base64.b64encode(f'{R.choice(C.FIRST).lower()}:{C.password(R, False)}'.encode()).decode(), 'key', 'curl_basic'), "' -I https://api.example.com/v1/me"])
        lines.append('HTTP/2 200')
    lines += [f'{R.choice(["GET", "POST"])} /api/v1/session HTTP/1.1', 'Host: app.example.com', ['Cookie: sessionid=', sec(C.rs(R, C.LO + C.DIG, 32), 'key', 'session_cookie'), '; csrftoken=', sec(C.rs(R, C.ALNUM, 32), 'key', 'csrf')],
              ['Authorization: Bearer ', C.token(R, 'jwt')] if R.random() < 0.5 else 'Accept: application/json', 'User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:132.0) Gecko/20100101 Firefox/132.0',
              '', 'HTTP/1.1 200 OK', ['Set-Cookie: session=', sec(C.rs(R, C.B64URL, 40), 'key', 'session_cookie'), '; Path=/; HttpOnly; Secure'], ['ETag: W/"', dec(C.sha(R, 32), 'etag'), '"'],
              ['X-Request-Id: ', dec(C.uuid(R), 'uuid')], ['x-amz-id-2: ', dec(C.rs(R, C.B64, 76), 'amz_id_2')], ['traceparent: 00-', dec(C.sha(R, 32), 'trace_id'), '-', dec(C.sha(R, 16), 'span_id'), '-01'],
              ['payload: ', dec(base64.b64encode(C.sentence(R, 6).encode()).decode(), 'b64_text')]]
    term_lines(R, cv, th, ctx, lines)
    return {'variant': form}


def configs(R, cv, th, ctx):
    form = R.choice(['env', 'env_example', 'k8s', 'kubeconfig', 'compose'])
    if form == 'env' and R.random() < 0.4:
        ctx = dict(ctx, softwrap=R.choice([300, 360, 420]))  # this pane only, never the next tiled one
    if form == 'env':
        lines = [['DATABASE_URL=postgres://app:', sec(C.password(R), 'key', 'db_url_password'), '@db.internal:5432/app'], ['STRIPE_SECRET_KEY=', C.token(R, 'stripe')], ['SESSION_SECRET=', sec(C.rs(R, C.ALNUM, 24), 'key', 'env_secret')],
                 'PORT=3000', 'LOG_LEVEL=info', ['SENTRY_DSN=https://', sec(C.sha(R, 32), 'key', 'sentry_dsn'), '@o', str(R.randint(10**5, 10**6)), '.ingest.sentry.io/', str(R.randint(10**5, 10**7))],
                 ['REDIS_URL=redis://default:', sec(C.password(R), 'key', 'redis_password'), '@cache:6379'], ['BUILD_SHA=', dec(C.sha(R), 'git_sha_env')], ['RELEASE_ID=', dec(C.uuid(R), 'uuid')]]
    elif form == 'env_example':
        lines = ['# copy to .env', ['DB_PASSWORD=', dec(R.choice(['localdev', 'postgres', 'devpass', 'secret123', 'password', 'test1234']), 'example_value')], ['API_KEY=', dec('your-api-key-here', 'placeholder')],
                 ['JWT_SECRET=', dec('changeme', 'placeholder')], ['ADMIN_PASSWORD=', dec(R.choice(['admin', 'admin123', 'letmein', 'hunter2']), 'example_value')], 'PORT=3000']
    elif form == 'k8s' and R.random() < 0.4:
        pem = '\n'.join(C.pem_lines(R, 3)) + '\n'
        lines = ['apiVersion: v1', 'kind: Secret', 'type: kubernetes.io/tls', 'data:', ['  tls.crt: ', dec(base64.b64encode(b'-----BEGIN CERTIFICATE-----\n' + C.rs(R, C.B64, 48).encode()).decode()[:80], 'k8s_cert')],
                 ['  tls.key: ', sec(base64.b64encode(pem.encode()).decode()[:96], 'key', 'k8s_pem_b64')]]
    elif form == 'k8s':
        lines = ['apiVersion: v1', 'kind: Secret', 'metadata:', '  name: app-secrets', 'type: Opaque', 'data:', ['  password: ', sec(base64.b64encode(C.password(R).encode()).decode(), 'key', 'k8s_b64')],
                 ['  api-token: ', sec(base64.b64encode(C.token(R, 'gh').text.encode()).decode(), 'key', 'k8s_b64')], ['  username: ', dec(base64.b64encode(b'app').decode(), 'b64_short')]]
    elif form == 'kubeconfig':
        lines = ['apiVersion: v1', 'clusters:', '- cluster:', ['    certificate-authority-data: ', dec(C.rs(R, C.B64, 120), 'ca_cert')], ['    server: https://', C.ipv4(R), ':6443'],
                 'users:', '- name: admin', '  user:', ['    client-certificate-data: ', dec(C.rs(R, C.B64, 120), 'client_cert')], ['    client-key-data: ', sec(C.rs(R, C.B64, 120), 'key', 'client_key')],
                 ['    token: ', C.token(R, 'jwt')]]
    else:
        lines = ['services:', '  db:', '    image: postgres:16', '    environment:', ['      POSTGRES_PASSWORD: ', sec(C.password(R), 'key', 'compose')], '      POSTGRES_USER: app',
                 '  web:', ['    image: ghcr.io/acme/web@sha256:', dec(C.sha(R, 64), 'digest')], '    ports:', '      - "8080:80"']
    ide(R, cv, th, ctx, lines)
    return {'variant': form}


FAMILIES = {
    'calendar': (calendar, 3), 'spreadsheet': (spreadsheet, 3), 'dashboard': (dashboard, 2), 'receipt': (receipt, 2), 'sysmon': (sysmon, 2),
    'checksums': (checksums, 3), 'logs': (logs, 3), 'lockfiles': (lockfiles, 2), 'packages': (packages, 2), 'gitgraph': (gitgraph, 2),
    'ide': (lambda R, cv, th, ctx: ide(R, cv, th, ctx), 2), 'diff': (diff, 2), 'chat': (chat, 3), 'email': (email_client, 3), 'contact': (contact_card, 2),
    'network': (network, 2), 'files': (file_manager, 2), 'crypto': (crypto, 1), 'sports': (sports_games_music, 2), 'maps': (maps, 1), 'design': (design, 1),
    'forms': (forms, 3), 'licences': (licences, 1), 'i18n': (i18n, 3), 'hexdump': (hexdump, 1), 'cloud': (cloud_console, 2), 'browser': (browser, 2),
    'watermark': (watermark, 1), 'http': (http_headers, 2), 'configs': (configs, 2),
}
TERMINAL = {'checksums', 'logs', 'packages', 'gitgraph', 'diff', 'network', 'hexdump', 'http', 'sysmon'}


def cli_secrets(R, cv, th, ctx):
    pw = lambda: sec(C.password(R, R.random() < 0.5), 'key', 'cli')
    lines = []
    pool = [
        lambda: prompt(ctx) + ['nmcli device wifi connect HomeNet password ', pw()],
        lambda: prompt(ctx) + ['mysql -u root -p', pw(), ' -h ', C.ipv4(R)],
        lambda: prompt(ctx) + ['mysqldump --user=app --password=', pw(), ' shop > shop.sql'],
        lambda: prompt(ctx) + [C.p('cu', 'rl -u '), R.choice(C.FIRST).lower(), ':', pw(), ' https://api.example.com/v1/me'],
        lambda: prompt(ctx) + ['redis-cli -h cache -a ', pw(), ' ping'],
        lambda: prompt(ctx) + ['docker login -u ', R.choice(C.FIRST).lower(), ' -p ', pw(), ' registry.example.com'],
        lambda: prompt(ctx) + ['sshpass -p ', pw(), ' ssh deploy@', C.ipv4(R)],
        lambda: prompt(ctx) + ['PGPASSWORD=', pw(), ' psql -h db -U app shop'],
        lambda: prompt(ctx) + ['SSHPASS=', pw(), ' sshpass -e ssh admin@nas.lan'],
        lambda: prompt(ctx) + ['kubeadm join ', C.ipv4(R), ':6443 --token ', C.token(R, 'kubeadm'), ' --discovery-token-ca-cert-hash sha256:', dec(C.sha(R, 64), 'digest')],
        lambda: prompt(ctx) + ['java -Dspring.datasource.password=', pw(), ' -jar app.jar'],
        lambda: ['machine api.github.com login ', R.choice(C.FIRST).lower(), ' password ', pw()],
        lambda: ["psql> CREATE USER app WITH PASSWORD '", pw(), "';"],
        lambda: prompt(ctx) + ['export GITHUB_TOKEN=', C.token(R, 'gh')],
        lambda: prompt(ctx) + ['tailscale up --authkey ', C.token(R, 'tailscale')],
        lambda: ['# created: 2026-11-22  public key: age1', dec(C.rs(R, 'qpzry9x8gf2tvdw0s3jn54khce6mua7l', 58), 'age_public')],
        lambda: [C.token(R, 'age')],
        lambda: ["client.run('", C.token(R, 'discord'), "')"],
        lambda: ['BOT_TOKEN=', C.token(R, 'telegram')],
        lambda: prompt(ctx) + ['wg show'],
        lambda: ['  private key: ', sec(C.rs(R, C.B64, 43) + '=', 'key', 'wg_show')],
        lambda: ['  public key: ', dec(C.rs(R, C.B64, 43) + '=', 'wg_public')],
        lambda: ['Temporary password: ', sec(C.password(R), 'key', 'trailing_words'), ' (please change it on first login)'],
        lambda: ['{ password: "', sec(C.password(R), 'key', 'object_literal'), '", port: 5432 }'],
        lambda: prompt(ctx) + ["curl -H 'Authorization: Bearer ", sec(C.rs(R, C.ALNUM + '_', 26), 'key', 'opaque_bearer'), "' https://api.example.com"],
        lambda: ['API Key: ', sec(C.uuid(R), 'key', 'two_word_label')],
        lambda: ['Secret Key: ', sec(C.rs(R, C.ALNUM, R.choice([25, 27, 30])), 'key', 'two_word_label')],
        lambda: ['psk="', sec(C.password(R, False), 'key', 'wpa_psk'), '"'],
        lambda: ['wpa_passphrase=', sec('correct horse battery staple', 'key', 'wpa_passphrase')],
    ]
    for _ in range(R.randint(10, 18)):
        lines.append(R.choice(pool)())
    term_lines(R, cv, th, ctx, lines)
    return {'variant': 'mixed'}


def sysinfo(R, cv, th, ctx):
    lines = []
    for _ in range(R.randint(3, 5)):
        k = R.choice(['free', 'meminfo', 'ls', 'stat', 'systemctl', 'date', 'boots', 'hostnamectl', 'du', 'cat-file', 'pkgbuild', 'aptshow', 'pytest', 'xrandr'])
        if k == 'free':
            lines.append(prompt(ctx) + ['free -k'])
            lines.append('               total        used        free      shared  buff/cache   available')
            lines.append(['Mem:      ', dec(f'{R.randint(8000000, 64000000):>10} {R.randint(1000000, 8000000):>10} {R.randint(1000000, 8000000):>10}', 'mem_numbers'), f'    {R.randint(10000, 999999):>8}   {R.randint(1000000, 9000000):>9}  {R.randint(1000000, 9000000):>10}'])
            lines.append(['Swap:     ', dec(f'{R.randint(1000000, 9000000):>10} {R.randint(0, 999999):>10} {R.randint(1000000, 9000000):>10}', 'mem_numbers')])
        elif k == 'meminfo':
            lines.append(prompt(ctx) + ['head -4 /proc/meminfo'])
            for name in ['MemTotal', 'MemFree', 'MemAvailable', 'Buffers']:
                lines.append([f'{name + ":":<16}', dec(f'{R.randint(100000, 64000000):>9}', 'mem_numbers'), ' kB'])
        elif k == 'ls':
            lines.append(prompt(ctx, '~/Downloads') + ['ls -l'])
            for _ in range(5):
                lines.append(['-rw-r--r-- 1 ', sec(ctx['login'], 'name', 'ls'), ' ', sec(ctx['login'], 'name', 'ls'), ' ', dec(f'{R.randint(1000000, 4000000000):>10}', 'byte_size'), f' Nov {R.randint(1, 28):2d} {R.randint(0, 23):02d}:{R.randint(0, 59):02d} ', R.choice(['image.iso', 'notes.txt', 'archive.tar.zst', 'video.mkv'])])
        elif k == 'stat':
            lines.append(prompt(ctx) + ['stat notes.txt'])
            lines.append(['  Size: ', dec(str(R.randint(1000, 99999)), 'byte_size'), '      Blocks: 8          IO Block: 4096   regular file'])
            lines.append(['Device: 259,2   Inode: ', dec(str(R.randint(1000000, 99999999)), 'inode'), '     Links: 1'])
        elif k == 'systemctl':
            lines.append(prompt(ctx) + ['systemctl status --user'])
            uid = R.choice([1000, 1001])
            lines.append(['● ', sec(ctx['host'], 'gap', 'host') if False else ctx['host']])
            lines.append(['    State: running'])
            lines.append(['   CGroup: /user.slice/user-', str(uid), '.slice/', dec(f'user@{uid}.service', 'systemd_unit')])
            lines.append(['           ├─', dec(f'getty@tty{R.randint(1, 6)}.service', 'systemd_unit')])
            lines.append(['           ├─', dec(f'wg-quick@wg{R.randint(0, 2)}.service', 'systemd_unit')])
            lines.append(['           └─', dec(f'postgresql@16-main.service', 'systemd_unit')])
        elif k == 'date':
            lines.append(prompt(ctx) + ['date +%s'])
            lines.append([dec(str(R.randint(1700000000, 1799999999)), 'unix_seconds')])
            lines.append(prompt(ctx) + ['date +%s%3N'])
            lines.append([dec(str(R.randint(1700000000000, 1799999999999)), 'unix_millis')])
        elif k == 'boots':
            lines.append(prompt(ctx) + ['journalctl --list-boots | tail -3'])
            for i in range(3):
                lines.append([f'{-2 + i:>3} ', dec(C.sha(R, 32), 'boot_id'), f' Sun 2026-11-{R.randint(1, 28):02d} {R.randint(0, 23):02d}:{R.randint(0, 59):02d}:{R.randint(0, 59):02d} UTC'])
        elif k == 'hostnamectl':
            lines.append(prompt(ctx) + ['hostnamectl'])
            lines.append(['  Static hostname: ', ctx['host']])
            lines.append(['       Machine ID: ', neu(C.sha(R, 32), 'machine_id')])
            lines.append(['          Boot ID: ', dec(C.sha(R, 32), 'boot_id')])
            lines.append(['           Kernel: Linux ', dec(f'6.{R.randint(1, 17)}.{R.randint(0, 20)}-{R.randint(100, 400)}.fc45.x86_64', 'version')])
        elif k == 'du':
            lines.append(prompt(ctx) + ['du -b *'])
            for name in ['build', 'node_modules', 'target']:
                lines.append([dec(str(R.randint(1000000, 9999999999)), 'byte_size'), '\t', name])
        elif k == 'cat-file':
            lines.append(prompt(ctx, '~/src/app') + ['git cat-file -p HEAD'])
            lines.append(['tree ', dec(C.sha(R), 'git_sha_tree')])
            lines.append(['parent ', dec(C.sha(R), 'git_sha_tree')])
            fn, ln = C.person(R)
            lines.append([f'author {fn} {ln} <', C.email(R, fn, ln), f'> {R.randint(1700000000, 1799999999)} +0100'])
        elif k == 'pkgbuild':
            lines.append(["sha256sums=('", dec(C.sha(R, 64), 'digest'), "'"])
            lines.append(["            '", dec(C.sha(R, 64), 'digest'), "')"])
        elif k == 'aptshow':
            lines.append(prompt(ctx) + ['apt-cache show curl | grep -i sum'])
            lines.append(['MD5sum: ', dec(C.sha(R, 32), 'digest')])
            lines.append(['SHA256: ', dec(C.sha(R, 64), 'digest')])
        elif k == 'pytest':
            lines.append(['PASS: test_quick_redact (', str(R.randint(1, 99)), ' ms)'])
            lines.append(['total_tokens: ', dec(f'{R.randint(1000, 99999):,}', 'count'), '  input_tokens: ', dec(f'{R.randint(1, 99)}.{R.randint(0, 9)}k', 'count')])
        else:
            lines.append(prompt(ctx) + ['xrandr | head -3'])
            lines.append(['Screen 0: minimum 320 x 200, current ', dec(f'{R.choice([1920, 2560, 3840])} x {R.choice([1080, 1440, 2160])}', 'resolution'), ', maximum 16384 x 16384'])
            lines.append(['   ', dec(f'{R.choice([1920, 2560])}x{R.choice([1080, 1440])}', 'resolution'), '     ', dec(f'{R.choice([59.95, 60.00, 143.98, 165.00]):.2f}', 'refresh'), '*+'])
    term_lines(R, cv, th, ctx, lines)
    return {'variant': 'mixed'}


def code_lookalikes(R, cv, th, ctx):
    pool = [
        lambda: ['use std::', dec(R.choice(['fs::File', 'io::Read', 'collections::HashMap']), 'cpp_scope'), ';'],
        lambda: ['let f = ', dec(R.choice(['File::create', 'Face::load', 'Cache::fill', 'Code::fail', 'Base::idle']), 'cpp_scope'), '(path)?;'],
        lambda: ['auto it = ', dec(R.choice(['QMap::find', 'Ab::Cd::Ef', 'A::B::C', 'dead::beef', 'cafe::babe']), 'cpp_scope'), '(key);'],
        lambda: ['#0  0x', dec(C.sha(R, 16), 'address'), ' in ', dec('_ZN' + str(R.randint(4, 9)) + C.rs(R, C.ALNUM, R.randint(24, 40)) + 'Ev', 'mangled'), ' () from libQt6Core.so.6'],
        lambda: ['    password: ', dec('varchar(255)', 'type_annotation'), ' NOT NULL,'],
        lambda: ['    api_key: ', dec('Option<String>', 'type_annotation'), ','],
        lambda: ['    let password = ', dec('std::env::var("DB_PASSWORD")?', 'reference'), ';'],
        lambda: ['    "', R.choice(['icon', 'logo', 'avatar', 'banner']), dec(f'@{R.choice([2, 3])}x.png', 'hidpi_asset'), '",'],
        lambda: ['git clone ', dec(f'git@github.com:{R.choice(C.COMPANIES)}/{R.choice(["app", "infra", "docs"])}.git', 'git_ssh_url')],
        lambda: ['Follow us: ', dec(f'@{R.choice(C.FIRST).lower()}@mastodon.social', 'fediverse')],
        lambda: ['See section ', dec(f'{R.randint(1, 9)}.{R.randint(1, 9)}.{R.randint(1, 9)}.{R.randint(1, 9)}', 'section_number'), ' of the spec'],
        lambda: ['mask = 0b', dec('1' * 24 + '0' * 8, 'bitmask')],
        lambda: ['lol ', dec('ha' * R.randint(12, 18), 'laugh'), ' ', R.choice(['😂', '🤣', ''])],
        lambda: ['    "copyright": "', dec(f'{R.randint(1990, 2015)}-{R.randint(2016, 2026)}', 'year_range'), ' Example Inc.",'],
        lambda: ['    ports: ["', dec(f'{R.randint(1000, 9000)}-{R.randint(9001, 32767)}', 'port_range'), '"]'],
        lambda: ['EUI-64: ', neu(':'.join(format(R.randint(0, 255), '02x') for _ in range(8)), 'eui64')],
        lambda: ['assert_eq!(id, ', dec(str(R.randint(10**17, 10**19 - 1)), 'snowflake'), ');'],
        lambda: ['created_at = ', dec(str(R.randint(1700000000000, 1799999999999)), 'unix_millis')],
        lambda: ['-----BEGIN PGP SIGNATURE-----'],
        lambda: [dec(C.rs(R, C.B64, 64), 'pgp_signature')],
        lambda: ['$2b$12$', dec(C.rs(R, C.ALNUM + './', 53), 'bcrypt')],
        lambda: ['image: ghcr.io/acme/', R.choice(['auth-service', 'api-gateway', 'token-broker']), '@sha256:', dec(C.sha(R, 64), 'digest_veto')],
        lambda: ['commit ', dec(C.sha(R), 'git_sha_commit'), ' ', R.choice(['fix token refresh', 'rotate api keys', 'auth: retry']), ''],
    ]
    lines = [R.choice(pool)() for _ in range(R.randint(14, 22))]
    ide(R, cv, th, ctx, lines)
    return {'variant': 'mixed'}


def twofa(R, cv, th, ctx):
    f, size = ctx['ui'], ctx['ui_size']
    y = 30
    cv.text(28, y, 'Set up two-factor authentication', f, size + 5, th['fg'], weight='bold')
    y += 46
    # a QR-like block of squares (not decodable: a stand-in for where the code would be)
    qx, qy, cell = 28, y, 5
    for i in range(29):
        for j in range(29):
            if R.random() < 0.5 or (i < 7 and j < 7) or (i < 7 and j > 21) or (i > 21 and j < 7):
                cv.rect(qx + j * cell, qy + i * cell, cell, cell, '#000000')
    tx = qx + 29 * cell + 30
    cv.text(tx, y, "Can't scan it? Enter this key:", f, size, th['muted'])
    key = C.token(R, 'base32').text
    shown = ' '.join(key[i:i + 4] for i in range(0, len(key), 4)) if R.random() < 0.5 else key
    cv.text(tx, y + size + 10, sec(shown.lower() if R.random() < 0.2 else shown, 'gap', 'totp_seed'), ctx['mono'], size + 1, th['fg'], width=cv.w - tx - 20, wrap='char')
    y += 29 * cell + 30
    cv.text(28, y, 'Recovery codes', f, size + 2, th['fg'], weight='bold')
    y += 34
    for i in range(8):
        code = R.choice([f'{C.rs(R, C.LO + C.DIG, 5)}-{C.rs(R, C.LO + C.DIG, 5)}', f'{C.rs(R, C.DIG, 4)} {C.rs(R, C.DIG, 4)}', C.rs(R, C.HEX, 16)])
        cv.text(28 + (i % 2) * 220, y + (i // 2) * (size + 14), sec(code, 'gap', 'recovery_code'), ctx['mono'], size, th['fg'])
    return {'variant': 'setup'}


FAMILIES.update({'csv': (csv_editor, 2), 'panes': (split_panes, 1), 'cli': (cli_secrets, 3), 'sysinfo': (sysinfo, 2), 'codelike': (code_lookalikes, 2), 'twofa': (twofa, 1)})
TERMINAL |= {'cli', 'sysinfo'}
