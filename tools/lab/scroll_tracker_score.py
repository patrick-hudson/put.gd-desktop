#!/usr/bin/env python3
"""Replays recorded scroll sequences through the scroll tracker and scores them (#1515).

Lab tool, never desktop proof. The corpus (kept out of the repository) has, per
browser and page, `reference.png` (the browser's own full-page picture),
`pinned.json` (the fixed and sticky elements' rectangles from the DOM) and one
directory per scroll style with numbered frames and a `manifest.json` that gives
every frame's true `scrollY`.

For every sequence, and for sequences derived from them (every 3rd and 6th
frame, played backwards, down-up-down, a frame of another page spliced in), it
runs `test_scroll_tracker_replay`, then compares each placed frame's position
with the truth and the image with the reference in 64-row bands, leaving out
the pinned rectangles and the scrollbar. A frame's true position is its
scrollY read before and after the picture; where the two differ the frame was
taken while the page moved and any position in between is right. One row per
sequence:

  scroll_tracker_score.py --corpus DIR --replay PATH --out DIR [--engine tracker|stitcher]
      [--pages a,b] [--styles notch,burst,continuous] [--derived] [--json FILE]

Needs numpy and Pillow (the builders have both).
"""
import argparse
import json
import math
import subprocess
import sys
from pathlib import Path

BAND = 64
# A band is equal when at most this share of its compared pixels differs by
# more than PIXEL_TOLERANCE (sum of the three channels' differences).
BAND_BAD_SHARE = 0.005
PIXEL_TOLERANCE = 24
SCROLLBAR = 15
PRIMARY = 'google,guide,wiki,mdn,github-long,python,hn,hnthread,stackoverflow,news-aj'


def style_dir(page_dir, style):
    """A style's frames: in the page's directory, or where the recorder kept a
    sequence taken after the reference (`<page>-reference-after-sequences`)."""
    for base in (page_dir, page_dir.with_name(page_dir.name + '-reference-after-sequences')):
        if (base / style / 'manifest.json').exists():
            return base / style
    return None


def reference_dir(page_dir):
    for base in (page_dir, page_dir.with_name(page_dir.name + '-reference-after-sequences')):
        if (base / 'reference.png').exists():
            return base
    return None


def load_manifest(directory):
    manifest = json.loads((directory / 'manifest.json').read_text())
    frames = []
    for frame in manifest['frames']:
        before, after = frame.get('scrollY'), frame.get('afterScrollY', frame.get('scrollY'))
        frames.append({'path': str((directory / frame['file']).resolve()), 'before': before, 'after': after})
    return manifest, frames


def derived(frames, kind, foreign=None):
    """A sequence made from a recorded one: what a fast scroller, a reader who
    goes back, or a stray frame gives the engine."""
    if kind == 'every3':
        return frames[::3]
    if kind == 'every6':
        return frames[::6]
    if kind == 'backwards':
        return list(reversed(frames))
    if kind == 'downupdown':
        half = len(frames) // 2
        return frames + list(reversed(frames[half:-1])) + frames[half + 1:]
    if kind == 'splice':
        mid = len(frames) // 2
        return frames[:mid] + [dict(foreign, foreign=True)] + frames[mid:]
    raise ValueError(kind)


def run_replay(replay, frames, out, engine, repeat=1, reuse=False):
    if reuse and (out / 'results.json').exists() and (out / 'image.png').exists():
        return json.loads((out / 'results.json').read_text())
    out.mkdir(parents=True, exist_ok=True)
    (out / 'list.txt').write_text(''.join(f['path'] + '\n' for f in frames))
    command = [replay, '--list', str(out / 'list.txt'), '--out', str(out), '--engine', engine,
               '--repeat', str(repeat)]
    result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    if result.returncode != 0:
        raise RuntimeError(f'replay failed ({result.returncode}): {result.stderr.strip()}')
    return json.loads((out / 'results.json').read_text())


def excluded_rects(pinned, viewport_height, reference_height):
    """The pinned rectangles where either picture can show them: at the top of
    the document, at its bottom, and where the screen showed them. A side bar
    as tall as the screen covers its columns in every frame, so no frame shows
    the page under it anywhere: its columns are left out from top to bottom
    (the image has the side bar's own background there, the reference the
    page's)."""
    rects = []
    for sample in pinned:
        scroll = sample['position'].get('y', 0)
        for element in sample['elements']:
            x, y, w, h = element['rect']
            if y + h >= 0.9 * viewport_height and h >= 0.75 * viewport_height:
                rects.append((int(x), 0, int(x + w + 0.999), reference_height))
                continue
            for top in (y, reference_height - viewport_height + y, scroll + y):
                rects.append((int(x), int(top), int(x + w + 0.999), int(top + h + 0.999)))
    return rects


def load_rgb(path):
    import numpy as np
    from PIL import Image
    Image.MAX_IMAGE_PIXELS = None
    return np.asarray(Image.open(path).convert('RGB'), dtype=np.int16)


class Reference:
    """The browser's full-page picture, compared in 64-row bands of document
    rows, the pinned rectangles and the scrollbar left out."""

    def __init__(self, page_dir, viewport_height):
        self.pixels = load_rgb(page_dir / 'reference.png')
        pinned = json.loads((page_dir / 'pinned.json').read_text()) if (page_dir / 'pinned.json').exists() else []
        self.rects = excluded_rects(pinned, viewport_height, self.pixels.shape[0])
        self.dx = 0
        self.reachable = None

    def align(self, frame, doc_top):
        """The horizontal shift between the screen and the reference (a page
        laid out for a viewport with a scrollbar is centred differently)."""
        import numpy as np
        h = frame.shape[0]
        lo, hi = int(h * 0.15), int(h * 0.85)
        if doc_top + hi > self.pixels.shape[0] or doc_top < 0:
            return
        best_share, best_dx = -1.0, 0
        for dx in range(-12, 13):
            x0, x1 = 100, frame.shape[1] - 100
            a = frame[lo:hi, x0:x1]
            b = self.pixels[doc_top + lo:doc_top + hi, x0 + dx:x1 + dx]
            share = float((np.abs(a - b).sum(axis=2) <= PIXEL_TOLERANCE).mean())
            if share > best_share + 1e-9 or (abs(share - best_share) <= 1e-9 and abs(dx) < abs(best_dx)):
                best_share, best_dx = share, dx
        self.dx = best_dx

    def band_share(self, rows, lo, hi):
        """Share of the band's compared pixels that differ: `rows` are the
        picture's rows for document rows [lo, hi)."""
        import numpy as np
        width = min(rows.shape[1], self.pixels.shape[1]) - SCROLLBAR - 12
        x0 = 12
        mask = np.ones((hi - lo, width - x0), dtype=bool)
        for rx0, ry0, rx1, ry1 in self.rects:
            a, b = max(lo, ry0), min(hi, ry1)
            if a < b:
                mask[a - lo:b - lo, max(0, rx0 - x0):max(0, min(width, rx1) - x0)] = False
        if mask.sum() < width:
            return None
        ours = rows[:, x0:width]
        theirs = self.pixels[lo:hi, x0 + self.dx:width + self.dx]
        bad = (np.abs(ours - theirs).sum(axis=2) > PIXEL_TOLERANCE) & mask
        return float(bad.sum() / mask.sum())

    def bands(self, first, last, whole=False):
        for band_top in range(first - first % BAND, last, BAND):
            lo, hi = max(band_top, first), min(band_top + BAND, last)
            if hi - lo >= (BAND if whole else BAND // 4):
                yield band_top, lo, hi

    def find_reachable(self, frames):
        """Bands that some frame, at its true place, shows as the reference
        does: the most any image built from these frames can match."""
        self.reachable = set()
        for frame in frames:
            if frame['before'] != frame['after']:
                continue
            pixels = load_rgb(frame['path'])
            top = round(frame['before'])
            for band_top, lo, hi in self.bands(max(0, top), min(self.pixels.shape[0], top + pixels.shape[0]), True):
                if band_top in self.reachable:
                    continue
                share = self.band_share(pixels[lo - top:hi - top], lo, hi)
                if share is not None and share <= BAND_BAD_SHARE:
                    self.reachable.add(band_top)

    def score(self, image_path, doc_top):
        image = load_rgb(image_path)
        first = max(0, doc_top)
        last = min(self.pixels.shape[0], doc_top + image.shape[0])
        equal = total = reachable = equal_reachable = 0
        worst = []
        for band_top, lo, hi in self.bands(first, last):
            share = self.band_share(image[lo - doc_top:hi - doc_top], lo, hi)
            if share is None:
                continue
            total += 1
            ok = share <= BAND_BAD_SHARE
            equal += ok
            if self.reachable is None or band_top in self.reachable:
                reachable += 1
                equal_reachable += ok
                if not ok:
                    worst.append((lo, round(share, 4)))
        return equal, total, equal_reachable, reachable, worst


def align_row(frame, reference, scroll):
    """The whole row of the reference where a frame taken at a fractional
    scrollY shows its central rows."""
    import numpy as np
    h, w = frame.shape[:2]
    best = (-1.0, round(scroll))
    for top in (math.floor(scroll), math.ceil(scroll)):
        lo, hi = int(h * 0.3), int(h * 0.7)
        if top + hi > reference.shape[0]:
            continue
        share = float((np.abs(frame[lo:hi, w // 4:3 * w // 4] - reference[top + lo:top + hi, w // 4:3 * w // 4]).sum(axis=2) == 0).mean())
        if share > best[0]:
            best = (share, top)
    return best[1]


_references = {}


def score(name, frames, results, page_dir, viewport_height, base_frames):
    summary = results['summary']
    first = frames[0]['before']
    # The first frame's whole row: where its pixels sit in the reference when
    # its scrollY is fractional, else its scrollY.
    base = round(first)
    ref_dir = reference_dir(page_dir)
    if ref_dir and first != int(first):
        base = align_row(load_rgb(frames[0]['path']), load_rgb(ref_dir / 'reference.png'), first)
    errors, wrong, placed, moving = [], [], 0, 0
    for frame, result in zip(frames, results['frames']):
        # A frame after the height cap is not placed: the image is complete
        # up to the cap.
        if result['status'] != 'placed' or frame.get('foreign'):
            if frame.get('foreign') and result['status'] == 'placed':
                wrong.append((Path(frame['path']).name, 'foreign frame placed'))
                errors.append(10 ** 6)
            continue
        placed += 1
        # scrollY is read before and after the picture, in two requests: a
        # frame taken while the page moved is right anywhere in between. A
        # fractional scrollY (Firefox at the end of a page) is drawn at one of
        # the two whole rows around it.
        low = math.floor(min(frame['before'], frame['after'])) - base
        high = math.ceil(max(frame['before'], frame['after'])) - base
        moving += low != high
        position = result['position']
        error = 0 if low <= position <= high else min(abs(position - low), abs(position - high))
        errors.append(error)
        if error:
            wrong.append((Path(frame['path']).name, position, [low, high] if low != high else low))
    row = {
        'sequence': name,
        'engine': summary['engine'],
        'frames': len(frames),
        'placed': placed,
        'noMovement': sum(r['status'] == 'no_movement' for r in results['frames']),
        'skipped': sum(r['status'] == 'skipped' for r in results['frames']),
        'afterCap': sum(r['status'] == 'cap_reached' for r in results['frames']),
        'worstError': max(errors) if errors else None,
        'takenWhileMoving': moving,
        'wrong': wrong[:6],
        'longestSkipRun': summary.get('longestSkipRun'),
        'recovered': summary.get('recovered'),
        'byExpectation': summary.get('byExpectation'),
        'onHint': summary.get('onHint'),
        'pinnedRegions': summary.get('pinnedRegions'),
        'endedOn': summary.get('endedOn'),
        'msMean': round(summary['msMean'], 2),
        'msP95': round(summary['msP95'], 2),
        'msMax': round(summary['msMax'], 2),
        'engineMsMean': round(summary['engineMsMean'], 2) if 'engineMsMean' in summary else None,
        'engineMsP95': round(summary['engineMsP95'], 2) if 'engineMsP95' in summary else None,
        'height': summary['bottom'] - summary['top'],
    }
    image = Path(results['_out']) / 'image.png'
    ref_dir = reference_dir(page_dir)
    if ref_dir and image.exists():
        key = (str(ref_dir), base_frames[0]['path'])
        if key not in _references:
            reference = Reference(ref_dir, viewport_height)
            anchor = next((f for f in base_frames if f['before'] == f['after']), None)
            if anchor:
                reference.align(load_rgb(anchor['path']), round(anchor['before']))
            reference.find_reachable(base_frames)
            _references[key] = reference
        reference = _references[key]
        doc_top = base + int(summary['top'])
        equal, total, equal_reachable, reachable, worst = reference.score(image, doc_top)
        row.update({'bandsEqual': equal, 'bands': total, 'bandsReachable': reachable,
                    'bandsEqualReachable': equal_reachable,
                    'bandsShare': round(equal_reachable / reachable, 4) if reachable else None,
                    'dx': reference.dx, 'worstBands': worst[:5]})
    return row


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--corpus', required=True, type=Path, help='the corpus root (holds chrome/, firefox/)')
    parser.add_argument('--replay', required=True, help='test_scroll_tracker_replay')
    parser.add_argument('--out', required=True, type=Path)
    parser.add_argument('--engine', default='tracker', choices=('tracker', 'stitcher'))
    parser.add_argument('--browsers', default='chrome,firefox')
    parser.add_argument('--pages', default=PRIMARY, help='page directories (default: the primary pages)')
    parser.add_argument('--styles', default='notch,burst,continuous')
    parser.add_argument('--derived', action='store_true', help='also the derived sequences')
    parser.add_argument('--repeat', type=int, default=1)
    parser.add_argument('--reuse', action='store_true', help='score the results already in --out instead of replaying')
    parser.add_argument('--json', type=Path)
    args = parser.parse_args()
    rows = []
    pages = []
    for browser in args.browsers.split(','):
        root = args.corpus / browser
        if not root.is_dir():
            continue
        for name in args.pages.split(','):
            if (root / name).is_dir():
                pages.append((browser, root / name))
    foreign_pool = {}
    for browser, page_dir in pages:
        notch = style_dir(page_dir, 'notch')
        if notch:
            foreign_pool[(browser, page_dir.name)] = load_manifest(notch)[1]
    for browser, page_dir in pages:
        for style in args.styles.split(','):
            directory = style_dir(page_dir, style)
            if directory is None:
                continue
            manifest, frames = load_manifest(directory)
            if len(frames) < 2:
                continue
            viewport_height = manifest.get('viewport', {}).get('h', 0)
            variants = [('', frames)]
            if args.derived and args.engine == 'tracker':
                if style == 'continuous':
                    variants += [('every3', derived(frames, 'every3')), ('every6', derived(frames, 'every6'))]
                if style == 'notch':
                    variants += [('backwards', derived(frames, 'backwards')),
                                 ('downupdown', derived(frames, 'downupdown'))]
                    from PIL import Image
                    size = Image.open(frames[0]['path']).size
                    others = [f[len(f) // 2] for key, f in sorted(foreign_pool.items())
                              if key[0] == browser and key != (browser, page_dir.name)]
                    others = [f for f in others if Image.open(f['path']).size == size]
                    if others:
                        variants.append(('splice', derived(frames, 'splice', others[0])))
            for variant, sequence in variants:
                name = f'{browser}/{page_dir.name}/{style}' + (f'+{variant}' if variant else '')
                out = args.out / name.replace('/', '_').replace('+', '_')
                try:
                    results = run_replay(args.replay, sequence, out, args.engine, args.repeat, args.reuse)
                except RuntimeError as error:
                    rows.append({'sequence': name, 'error': str(error)})
                    print(f'{name}: {error}', file=sys.stderr)
                    continue
                results['_out'] = str(out)
                rows.append(score(name, sequence, results, page_dir, viewport_height, frames))
                print(json.dumps(rows[-1]), flush=True)
    if args.json:
        args.json.write_text(json.dumps(rows, indent=1) + '\n')
    print()
    print(f"{'sequence':44} {'eng':8} {'frm':>4} {'plc':>4} {'still':>5} {'skip':>4} {'err':>4} {'equal/reach':>11} {'all':>9} {'pin':>3} {'ms':>6} {'max':>6}")
    for row in rows:
        if 'error' in row:
            print(f"{row['sequence']:44} ERROR {row['error'][:80]}")
            continue
        reach = f"{row.get('bandsEqualReachable', '-')}/{row.get('bandsReachable', '-')}"
        bands = f"{row.get('bandsEqual', '-')}/{row.get('bands', '-')}"
        print(f"{row['sequence']:44} {row['engine']:8} {row['frames']:>4} {row['placed']:>4} {row['noMovement']:>5} "
              f"{row['skipped']:>4} {str(row['worstError']):>4} {reach:>11} {bands:>9} {str(row['pinnedRegions']):>3} "
              f"{row['msMean']:>6} {row['msMax']:>6}")
    return 0


if __name__ == '__main__':
    sys.exit(main())
