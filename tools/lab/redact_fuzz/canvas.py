"""A Pango/Cairo canvas that records ground truth while it draws (#1931).

Text goes through Pango (HarfBuzz shaping, fontconfig fallback, RTL, CJK, colour emoji), the
stack GTK draws with. Every `text()` call records:
- the words a perfect recognizer would return (whitespace-separated, ink-tight vertically),
  the "oracle" input that separates rule failures from recognition failures;
- for each labelled segment, its boxes per visual line and its character boxes, so the scorer
  can say how much of a secret a mark leaves visible.
Coordinates are logical; `scale` (HiDPI) multiplies them into image pixels.
"""
import math

import gi

gi.require_version('Pango', '1.0')
gi.require_version('PangoCairo', '1.0')
import cairo  # noqa: E402
from gi.repository import Pango, PangoCairo  # noqa: E402

from content import Val  # noqa: E402

_FAMILIES = None


def installed_families():
    global _FAMILIES
    if _FAMILIES is None:
        _FAMILIES = {f.get_name() for f in PangoCairo.FontMap.get_default().list_families()}
    return _FAMILIES


def pick(R, names):
    have = [n for n in names if n in installed_families()]
    return R.choice(have) if have else names[-1]


UI_SANS = ['Cantarell', 'Adwaita Sans', 'Ubuntu', 'Ubuntu Sans', 'Noto Sans', 'DejaVu Sans', 'Open Sans', 'Lato',
           'IBM Plex Sans', 'Liberation Sans', 'Inter', 'Roboto', 'Sans']
MONO = ['Adwaita Mono', 'Hack', 'Ubuntu Mono', 'Ubuntu Sans Mono', 'DejaVu Sans Mono', 'Noto Sans Mono', 'Liberation Mono',
        'IBM Plex Mono', 'DM Mono', 'JetBrains Mono', 'Fira Code', 'Source Code Pro', 'Monospace']
SERIF = ['Noto Serif', 'DejaVu Serif', 'Liberation Serif', 'IBM Plex Serif', 'Serif']


def rgb(c):
    if isinstance(c, str):
        c = c.lstrip('#')
        return tuple(int(c[i:i + 2], 16) / 255 for i in (0, 2, 4))
    return tuple(v / 255 for v in c)


class Canvas:
    def __init__(self, width, height, bg, scale=1.0, antialias='grey', hint='slight'):
        self.w, self.h, self.scale = width, height, scale
        self.surface = cairo.ImageSurface(cairo.FORMAT_ARGB32, int(math.ceil(width * scale)), int(math.ceil(height * scale)))
        self.cr = cairo.Context(self.surface)
        self.cr.scale(scale, scale)
        opts = cairo.FontOptions()
        opts.set_antialias({'grey': cairo.ANTIALIAS_GRAY, 'subpixel': cairo.ANTIALIAS_SUBPIXEL, 'none': cairo.ANTIALIAS_NONE}[antialias])
        if antialias == 'subpixel':
            opts.set_subpixel_order(cairo.SUBPIXEL_ORDER_RGB)
        opts.set_hint_style({'none': cairo.HINT_STYLE_NONE, 'slight': cairo.HINT_STYLE_SLIGHT, 'full': cairo.HINT_STYLE_FULL}[hint])
        self.fontopts = opts
        self.rect(0, 0, width, height, bg)
        self.clip_x = None  # logical x past which text is hidden (an editor's minimap): neither drawn nor recorded
        self.words = []   # [text, x, y, w, h] in image pixels
        self.spans = []   # {label, kind, tag, text, pieces: [[x0,y0,x1,y1]], chars: [[x0,y0,x1,y1]]}

    # ----- shapes
    def rect(self, x, y, w, h, color, radius=0, alpha=1.0):
        cr = self.cr
        cr.set_source_rgba(*rgb(color), alpha)
        if radius:
            r = min(radius, w / 2, h / 2)
            cr.new_sub_path()
            cr.arc(x + w - r, y + r, r, -math.pi / 2, 0)
            cr.arc(x + w - r, y + h - r, r, 0, math.pi / 2)
            cr.arc(x + r, y + h - r, r, math.pi / 2, math.pi)
            cr.arc(x + r, y + r, r, math.pi, 3 * math.pi / 2)
            cr.close_path()
        else:
            cr.rectangle(x, y, w, h)
        cr.fill()

    def frame(self, x, y, w, h, color, width=1.0, radius=0):
        cr = self.cr
        cr.set_source_rgb(*rgb(color))
        cr.set_line_width(width)
        if radius:
            r = radius
            cr.new_sub_path()
            cr.arc(x + w - r, y + r, r, -math.pi / 2, 0)
            cr.arc(x + w - r, y + h - r, r, 0, math.pi / 2)
            cr.arc(x + r, y + h - r, r, math.pi / 2, math.pi)
            cr.arc(x + r, y + r, r, math.pi, 3 * math.pi / 2)
            cr.close_path()
        else:
            cr.rectangle(x + width / 2, y + width / 2, w - width, h - width)
        cr.stroke()

    def line(self, x0, y0, x1, y1, color, width=1.0):
        cr = self.cr
        cr.set_source_rgb(*rgb(color))
        cr.set_line_width(width)
        cr.move_to(x0, y0)
        cr.line_to(x1, y1)
        cr.stroke()

    def circle(self, cx, cy, r, color):
        cr = self.cr
        cr.set_source_rgb(*rgb(color))
        cr.arc(cx, cy, r, 0, 2 * math.pi)
        cr.fill()

    def ring(self, cx, cy, r, color, width=1.5):
        cr = self.cr
        cr.set_source_rgb(*rgb(color))
        cr.set_line_width(width)
        cr.arc(cx, cy, r, 0, 2 * math.pi)
        cr.stroke()

    def poly(self, points, color, width=1.5, fill=False):
        cr = self.cr
        cr.set_source_rgb(*rgb(color))
        cr.set_line_width(width)
        cr.move_to(*points[0])
        for pt in points[1:]:
            cr.line_to(*pt)
        if fill:
            cr.close_path()
            cr.fill()
        else:
            cr.stroke()

    def gradient(self, x, y, w, h, c0, c1, vertical=True):
        g = cairo.LinearGradient(x, y, x if not vertical else x, y + h if vertical else y)
        if not vertical:
            g = cairo.LinearGradient(x, y, x + w, y)
        g.add_color_stop_rgb(0, *rgb(c0))
        g.add_color_stop_rgb(1, *rgb(c1))
        self.cr.set_source(g)
        self.cr.rectangle(x, y, w, h)
        self.cr.fill()

    # ----- text
    def layout(self, text, family, size, weight='normal', italic=False, width=None, letter_spacing=0.0, wrap='word', rtl=None, features=None):
        lay = PangoCairo.create_layout(self.cr)
        PangoCairo.context_set_font_options(lay.get_context(), self.fontopts)
        desc = Pango.FontDescription()
        desc.set_family(family)
        desc.set_absolute_size(size * Pango.SCALE)
        desc.set_weight({'normal': Pango.Weight.NORMAL, 'bold': Pango.Weight.BOLD, 'light': Pango.Weight.LIGHT,
                         'medium': Pango.Weight.MEDIUM, 'semibold': Pango.Weight.SEMIBOLD}[weight])
        if italic:
            desc.set_style(Pango.Style.ITALIC)
        lay.set_font_description(desc)
        attrs = Pango.AttrList()
        if letter_spacing:
            attrs.insert(Pango.attr_letter_spacing_new(int(letter_spacing * Pango.SCALE)))
        if features:
            attrs.insert(Pango.attr_font_features_new(features))
        lay.set_attributes(attrs)
        if width:
            lay.set_width(int(width * Pango.SCALE))
            lay.set_wrap({'word': Pango.WrapMode.WORD, 'char': Pango.WrapMode.CHAR, 'mixed': Pango.WrapMode.WORD_CHAR}[wrap])
        if rtl is not None:
            lay.set_auto_dir(False)
            lay.get_context().set_base_dir(Pango.Direction.RTL if rtl else Pango.Direction.LTR)
            lay.context_changed()
        lay.set_text(text, -1)
        return lay

    def measure(self, segments, family, size, **kw):
        text = ''.join(s.text if isinstance(s, Val) else s for s in _segments(segments))
        lay = self.layout(text, family, size, **kw)
        _, logical = lay.get_pixel_extents()
        return logical.width, logical.height

    def text(self, x, y, segments, family, size, color, weight='normal', italic=False, width=None, letter_spacing=0.0,
             wrap='word', align='left', rtl=None, features=None, record=True, highlight=None):
        """Draw text at (x, y) (top left of the layout). `segments`: a str, a Val, or a list of both.
        Returns (width, height) of the layout in logical pixels."""
        segs = _segments(segments)
        text = ''.join(s.text if isinstance(s, Val) else s for s in segs)
        lay = self.layout(text, family, size, weight, italic, width, letter_spacing, wrap, rtl, features)
        if width and align != 'left':
            lay.set_alignment({'right': Pango.Alignment.RIGHT, 'center': Pango.Alignment.CENTER}[align])
        ink, logical = lay.get_pixel_extents()
        if highlight:  # (start, end, colour): a selection band behind part of the text
            s0, s1, hc = highlight
            b = _byte(text)
            r0, r1 = lay.index_to_pos(b[s0]), lay.index_to_pos(b[s1] if s1 < len(text) else b[-1])
            hx0 = x + min(r0.x, r1.x) / Pango.SCALE
            hx1 = x + max(r0.x + (r0.width if s1 >= len(text) else 0), r1.x) / Pango.SCALE
            self.rect(hx0, y + r0.y / Pango.SCALE, max(1, hx1 - hx0), r0.height / Pango.SCALE, hc)
        self.cr.save()
        if self.clip_x is not None:
            self.cr.rectangle(0, 0, self.clip_x, self.h)
            self.cr.clip()
        self.cr.move_to(x, y)
        self.cr.set_source_rgb(*rgb(color))
        PangoCairo.show_layout(self.cr, lay)
        self.cr.restore()
        if record:
            self._record(lay, text, segs, x, y)
        return logical.width, logical.height

    def _record(self, lay, text, segs, ox, oy):
        sc = self.scale
        b = _byte(text)
        # character boxes (logical x, line ink y)
        it = lay.get_iter()
        lines = []
        while True:
            line = it.get_line_readonly()
            ink, logical = it.get_line_extents()
            lines.append((line.start_index, line.start_index + line.length, ink, logical))
            if not it.next_line():
                break
        def line_of(bi):
            for i, (s, e, _, _) in enumerate(lines):
                if s <= bi < e or (bi == e and i == len(lines) - 1):
                    return i
            return len(lines) - 1
        chars = []
        for ci, ch in enumerate(text):
            if ch in '\n':
                chars.append(None)
                continue
            r = lay.index_to_pos(b[ci])
            li = line_of(b[ci])
            ink = lines[li][2]
            x0 = (min(r.x, r.x + r.width)) / Pango.SCALE
            x1 = (max(r.x, r.x + r.width)) / Pango.SCALE
            y0 = ink.y / Pango.SCALE
            y1 = (ink.y + ink.height) / Pango.SCALE
            if ink.height <= 0:
                y0, y1 = r.y / Pango.SCALE, (r.y + r.height) / Pango.SCALE
            box = ((ox + x0) * sc, (oy + y0) * sc, (ox + x1) * sc, (oy + y1) * sc)
            # a character drawn past the image's edge is not in the picture: neither seen nor read
            right = self.surface.get_width() if self.clip_x is None else self.clip_x * sc
            if box[0] >= right - 1 or box[1] >= self.surface.get_height() - 1 or box[2] <= 0 or box[3] <= 0:
                chars.append(None)
                continue
            chars.append((li,) + box)
        # oracle words: runs of non-space characters within one line
        cur = []
        def flush():
            if cur:
                xs0 = min(c[1] for _, c in cur)
                ys0 = min(c[2] for _, c in cur)
                xs1 = max(c[3] for _, c in cur)
                ys1 = max(c[4] for _, c in cur)
                self.words.append([''.join(ch for ch, _ in cur), round(xs0), round(ys0), max(1, round(xs1 - xs0)), max(1, round(ys1 - ys0))])
                cur.clear()
        prev_line = None
        for ci, ch in enumerate(text):
            c = chars[ci]
            if c is None or ch.isspace() or (prev_line is not None and c[0] != prev_line):
                flush()
            if c is not None and not ch.isspace():
                cur.append((ch, c))
                prev_line = c[0]
        flush()
        # labelled spans
        pos = 0
        for s in segs:
            t = s.text if isinstance(s, Val) else s
            if isinstance(s, Val) and s.label in ('secret', 'decoy', 'neutral'):
                cs = [chars[i] for i in range(pos, pos + len(t)) if chars[i] is not None and not text[i].isspace()]
                # Part of it hidden (past the edge, under a minimap or the next pane): not a whole secret on screen.
                hidden = any(chars[i] is None and not text[i].isspace() for i in range(pos, pos + len(t)))
                if cs:
                    pieces = {}
                    for li, x0, y0, x1, y1 in cs:
                        pc = pieces.setdefault(li, [x0, y0, x1, y1])
                        pc[0], pc[1], pc[2], pc[3] = min(pc[0], x0), min(pc[1], y0), max(pc[2], x1), max(pc[3], y1)
                    self.spans.append({'label': s.label, 'kind': s.kind, 'tag': s.tag, 'text': t,
                                       'pieces': [[round(v) for v in pc] for _, pc in sorted(pieces.items())],
                                       'chars': [[round(x0), round(y0), round(x1), round(y1)] for _, x0, y0, x1, y1 in cs]})
                    if hidden:
                        self.spans[-1]['clipped'] = True
            pos += len(t)

    def caret(self, x, y, h, color, width=1.0):
        self.rect(x, y, width, h, color)

    def png(self, path):
        self.surface.write_to_png(path)


def _segments(segments):
    if isinstance(segments, (str, Val)):
        return [segments]
    return list(segments)


def _byte(text):
    """Byte offset of each character (Pango indexes UTF-8 bytes), plus the end."""
    out, n = [], 0
    for ch in text:
        out.append(n)
        n += len(ch.encode('utf-8'))
    out.append(n)
    return out
