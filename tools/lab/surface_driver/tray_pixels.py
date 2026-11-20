"""Find put.gd's tray icon and its open menu by their pixels, for trays that export neither to AT-SPI.

Cinnamon's panel, budgie-panel and Omarchy's quickshell draw put.gd's StatusNotifier item but give it
no accessible node. The template is never a stored image: it is the IconPixmap frames (or the
IconName PNG) the running resident publishes at match time, so the season's glyph (turkey, tree or
mark), the panel tint and any badge are whatever it shows now. A host may scale the pixmap or
recolour it, so a glyph is compared by its contrast with the panel colour, drawn unchanged and as
its bare shape, against the capture's contrast with the panel, at the size it was drawn. A match must be unique, or there is
no match: the caller reports the cell as not run with the reason, never as a failure.
Pure Python and Pillow; no numpy.
"""
from __future__ import annotations

from collections import deque
from difflib import SequenceMatcher
import re

# A drawn glyph that is put.gd's correlates at about 0.9 or more with its alpha; other tray glyphs,
# the clock's digits and other seasons' glyphs stay well below. Both bounds were measured on the
# Cinnamon, Budgie and Hyprland panels (tests/desktop/test_surface_tray_pixels.py keeps them honest).
MIN_SCORE = 0.8
MARGIN = 0.12          # Any other candidate this close to the best, or reaching MIN_SCORE, makes it ambiguous.
CONTRAST = 40          # A pixel this far (largest channel difference) from the panel colour is drawn.
ALPHA = 64             # A template pixel this opaque is part of the glyph's outline box.


class NoMatch(Exception):
    """The icon or menu could not be found unambiguously; the message says why."""


def _image_modules():
    from PIL import Image, ImageChops
    return Image, ImageChops


def pixmap_image(width, height, data):
    """A StatusNotifierItem IconPixmap frame (ARGB32, network byte order) as an RGBA image."""
    Image, _ = _image_modules()
    data = bytes(data)
    if width <= 0 or height <= 0 or len(data) != width * height * 4:
        raise ValueError('IconPixmap frame size does not match its data')
    alpha, red, green, blue = (Image.frombytes('L', (width, height), data[i::4]) for i in range(4))
    return Image.merge('RGBA', (red, green, blue, alpha))


def png_image(data):
    """An IconName PNG (Ayatana hosts) as an RGBA image."""
    import io
    Image, _ = _image_modules()
    with Image.open(io.BytesIO(data)) as image:
        return image.convert('RGBA')


def glyph(frame):
    """An RGBA frame cut to the glyph's outline box; None for an empty frame."""
    box = frame.getchannel('A').point(lambda a: 255 if a >= ALPHA else 0).getbbox()
    return frame.crop(box) if box else None


def band(size, edge, thickness):
    """The panel strip of an output: edge is top, bottom, left or right; thickness in pixels."""
    width, height = size
    thickness = max(1, min(int(thickness), height if edge in ('top', 'bottom') else width))
    return {'top': (0, 0, width, thickness), 'bottom': (0, height - thickness, width, height),
            'left': (0, 0, thickness, height), 'right': (width - thickness, 0, width, height)}[edge]


def background(image):
    """The most common colour of an RGB image: a panel's own colour."""
    colours = image.getcolors(image.width * image.height)
    return max(colours)[1]


def contrast(image, colour):
    """Each pixel's largest channel difference from COLOUR, as 'L'."""
    Image, ImageChops = _image_modules()
    difference = ImageChops.difference(image.convert('RGB'), Image.new('RGB', image.size, colour))
    red, green, blue = difference.split()
    return ImageChops.lighter(ImageChops.lighter(red, green), blue)


def blobs(mask, gap, longest=None):
    """Boxes (x0, y0, x1, y1) of drawn pixels, joining pieces closer than GAP (a glyph and its stand).

    A piece longer than LONGEST (a panel's border line) is left out before joining.
    """
    width, height = mask.size
    on = mask.tobytes()
    seen = bytearray(width * height)
    boxes = []
    for start in range(width * height):
        if not on[start] or seen[start]:
            continue
        seen[start] = 1
        queue = deque([start])
        x0 = x1 = start % width
        y0 = y1 = start // width
        while queue:
            index = queue.popleft()
            x, y = index % width, index // width
            x0, x1, y0, y1 = min(x0, x), max(x1, x), min(y0, y), max(y1, y)
            for dy in (-1, 0, 1):
                for dx in (-1, 0, 1):
                    nx, ny = x + dx, y + dy
                    if 0 <= nx < width and 0 <= ny < height:
                        other = ny * width + nx
                        if on[other] and not seen[other]:
                            seen[other] = 1
                            queue.append(other)
        if longest is None or max(x1 - x0, y1 - y0) < longest:
            boxes.append([x0, y0, x1 + 1, y1 + 1])
    merged = True
    while merged:
        merged = False
        for i in range(len(boxes)):
            for j in range(i + 1, len(boxes)):
                a, b = boxes[i], boxes[j]
                if (a[0] - gap <= b[2] and b[0] - gap <= a[2] and a[1] - gap <= b[3] and b[1] - gap <= a[3]):
                    boxes[i] = [min(a[0], b[0]), min(a[1], b[1]), max(a[2], b[2]), max(a[3], b[3])]
                    del boxes[j]
                    merged = True
                    break
            if merged:
                break
    return [tuple(b) for b in boxes]


def correlation(a, b):
    """Zero-mean normalised cross-correlation of two equal-length byte sequences; 0 when either is flat."""
    n = len(a)
    mean_a, mean_b = sum(a) / n, sum(b) / n
    num = da = db = 0.0
    for x, y in zip(a, b):
        x -= mean_a
        y -= mean_b
        num += x * y
        da += x * x
        db += y * y
    return num / (da * db) ** .5 if da and db else 0.0


def similarity(drawn, glyphs, colour):
    """How well the contrast image DRAWN (cut to a candidate's box) matches the best of GLYPHS.

    Each glyph is compared twice: as the host would draw it unchanged on the panel COLOUR, and as its
    bare shape (alpha), for hosts that recolour a tray pixmap as a symbolic icon.
    """
    Image, _ = _image_modules()
    width, height = drawn.size
    best = 0.0
    for shape in glyphs:
        # Same box, same proportions: a host draws the icon square, at any size.
        if abs(width / height - shape.width / shape.height) > .25 * shape.width / shape.height:
            continue
        frame = shape.resize((width, height), Image.BILINEAR)
        drawn_as_is = contrast(Image.alpha_composite(Image.new('RGBA', frame.size, tuple(colour) + (255,)), frame), colour)
        for template in (drawn_as_is, frame.getchannel('A')):
            best = max(best, correlation(template.tobytes(), drawn.tobytes()))
    return best


def locate(image, box, glyphs, *, min_score=MIN_SCORE, margin=MARGIN):
    """The one drawn copy of GLYPHS inside BOX of IMAGE, as dict(rect=[x, y, w, h], score, runner_up).

    Raises NoMatch with the reason when no candidate reaches MIN_SCORE or another comes within MARGIN.
    """
    glyphs = [g for g in glyphs if g is not None]
    if not glyphs:
        raise NoMatch('the resident publishes no icon frames')
    strip = image.convert('RGB').crop(box)
    colour = background(strip)
    drawn = contrast(strip, colour)
    mask = drawn.point(lambda v: 255 if v >= CONTRAST else 0)
    thickness = min(strip.size)
    candidates = []
    for x0, y0, x1, y1 in blobs(mask, max(2, round(thickness * .08)), thickness * 3):
        w, h = x1 - x0, y1 - y0
        if min(w, h) < 6 or max(w, h) > thickness * 3:
            continue
        candidates.append(dict(rect=[box[0] + x0, box[1] + y0, w, h],
                               score=round(similarity(drawn.crop((x0, y0, x1, y1)), glyphs, colour), 4)))
    candidates.sort(key=lambda c: -c['score'])
    if not candidates or candidates[0]['score'] < min_score:
        best = candidates[0]['score'] if candidates else None
        raise NoMatch('no drawn panel glyph matches put.gd\'s published icon (best score %s, needs %s; '
                      '%d candidates)' % (best, min_score, len(candidates)))
    best, others = candidates[0], candidates[1:]
    runner_up = others[0]['score'] if others else 0.0
    # A second drawn copy of the glyph, however good the first, is never told apart by score.
    if runner_up >= min_score or runner_up >= best['score'] - margin:
        raise NoMatch('put.gd\'s icon is ambiguous on the panel: candidates at %s and %s score %.3f and %.3f'
                      % (best['rect'], others[0]['rect'], best['score'], runner_up))
    return dict(rect=best['rect'], score=best['score'], runner_up=runner_up, panel=list(colour),
                candidates=len(candidates))


def still_drawn(image, rect, glyphs):
    """The glyph is still drawn where locate() found it: same box, same shape, nothing over it."""
    x, y, w, h = rect
    pad = 3
    area = (max(0, x - pad), max(0, y - pad), min(image.width, x + w + pad), min(image.height, y + h + pad))
    strip = image.convert('RGB').crop(area)
    colour = background(strip)
    drawn = contrast(strip, colour)
    box = drawn.point(lambda v: 255 if v >= CONTRAST else 0).getbbox()
    if not box:
        return 0.0
    found = (area[0] + box[0], area[1] + box[1], box[2] - box[0], box[3] - box[1])
    if any(abs(a - b) > 1 for a, b in zip(found, rect)):
        return 0.0
    return similarity(drawn.crop(box), [g for g in glyphs if g is not None], colour)


def changed(before, after, exclude, threshold=24):
    """Boxes of what appeared between two captures, outside the EXCLUDE box (the panel)."""
    Image, ImageChops = _image_modules()
    difference = ImageChops.difference(before.convert('RGB'), after.convert('RGB'))
    red, green, blue = difference.split()
    mask = ImageChops.lighter(ImageChops.lighter(red, green), blue).point(lambda v: 255 if v >= threshold else 0)
    mask.paste(0, exclude)
    # A popup is one rectangle; join what its text and separators leave apart.
    small = mask.reduce(4)
    boxes = blobs(small.point(lambda v: 255 if v else 0), 3)
    return [(x0 * 4, y0 * 4, min(after.width, x1 * 4), min(after.height, y1 * 4)) for x0, y0, x1, y1 in boxes]


def menu_box(before, after, panel, icon, *, reach=48):
    """The box of a popup that opened next to ICON's panel after a click, or None.

    It must border the panel strip (within REACH pixels) and overlap the icon's column, give or take
    a menu's width: what else changes on screen (a banner, the clock) is ignored.
    """
    x, y, w, h = icon
    centre = x + w / 2
    near = []
    for x0, y0, x1, y1 in changed(before, after, panel):
        touches = (abs(y1 - panel[1]) <= reach if panel[1] > 0 else abs(y0 - panel[3]) <= reach)
        if touches and x0 - 600 <= centre <= x1 + 600 and x1 - x0 >= 80 and y1 - y0 >= 120:
            near.append((x0, y0, x1, y1))
    if len(near) != 1:
        return None
    return near[0]


SHORTCUT = re.compile(r'\b(?:ctrl|shift|alt|super|meta)\s*\+\S*', re.I)


def ocr_lines(tsv):
    """Tesseract TSV as text lines top to bottom: dict(text, rect=[x, y, w, h])."""
    lines = {}
    rows = tsv.splitlines()
    for row in rows[1:]:
        cells = row.split('\t')
        if len(cells) < 12 or cells[0] != '5' or not cells[11].strip():
            continue
        key = tuple(int(v) for v in cells[2:5])
        left, top, width, height = (int(v) for v in cells[6:10])
        line = lines.setdefault(key, dict(words=[], box=[left, top, left + width, top + height]))
        line['words'].append(cells[11].strip())
        box = line['box']
        line['box'] = [min(box[0], left), min(box[1], top), max(box[2], left + width), max(box[3], top + height)]
    result = [dict(text=' '.join(v['words']), rect=[v['box'][0], v['box'][1], v['box'][2] - v['box'][0],
                                                     v['box'][3] - v['box'][1]]) for v in lines.values()]
    return sorted(result, key=lambda line: (line['rect'][1], line['rect'][0]))


def normal(text):
    text = SHORTCUT.sub(' ', text.replace('…', '...')).lower()
    return ' '.join(re.sub(r'[^a-z0-9&:.]+', ' ', text).split()).rstrip('.').strip()


def label_score(label, text):
    """How well an OCR line reads as a menu label; a label ending in * matches any value after it."""
    want, got = normal(label.rstrip('*')), normal(text)
    if label.endswith('*'):
        got = got[:len(want)]
    return SequenceMatcher(None, want, got).ratio()


def unknown_lines(labels, lines, *, threshold=.85):
    """Lines read in a menu that are none of LABELS: a row the host added, or text OCR couldn't place.

    Overlapping scroll views read some rows twice; those still match a label. Marks with fewer
    than three letters or digits (submenu arrows, a scroll bar) are not rows.
    """
    return [line['text'] for line in lines
            if len(normal(line['text']).replace(' ', '')) >= 3
            and not any(label_score(label, line['text']) >= threshold for label in labels)]


def align(labels, lines, *, threshold=.85):
    """Match LABELS to OCR LINES in order (each line used once); the matched line per label or None."""
    n, m = len(labels), len(lines)
    scores = [[label_score(a, b['text']) for b in lines] for a in labels]
    # best[i][j]: (labels matched, total score) using labels[i:] and lines[j:].
    best = [[(0, 0.0)] * (m + 1) for _ in range(n + 1)]
    for i in range(n - 1, -1, -1):
        for j in range(m - 1, -1, -1):
            options = [best[i + 1][j], best[i][j + 1]]
            if scores[i][j] >= threshold:
                count, total = best[i + 1][j + 1]
                options.append((count + 1, total + scores[i][j]))
            best[i][j] = max(options)
    result, i, j = [], 0, 0
    while i < n:
        if j < m and scores[i][j] >= threshold and best[i][j] == (best[i + 1][j + 1][0] + 1,
                                                                  best[i + 1][j + 1][1] + scores[i][j]):
            result.append(lines[j])
            i, j = i + 1, j + 1
        elif j < m and best[i][j] == best[i][j + 1]:
            j += 1
        else:
            result.append(None)
            i += 1
    return result
