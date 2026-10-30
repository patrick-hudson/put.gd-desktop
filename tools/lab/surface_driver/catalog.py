"""The build's ui-catalog sidecars are the only source of control rectangles."""
import json
import fnmatch
import re
from pathlib import Path
from tools.lab.surface_map.surface_map import control_matches
from .api import RecipeBlocked


def image_modules():
    try:
        from PIL import Image, ImageChops, ImageStat
        return Image, ImageChops, ImageStat
    except ImportError as error:
        raise RecipeBlocked('PNG frames and catalog comparison require the system python3-pil package on the driver host') from error


def compare(actual, reference, tolerance):
    """RGB mean absolute error in normalized window coordinates.

    Both crops cover the client, so compare at their common (smaller) size.
    This also removes device-pixel-ratio differences. Layout/content changes
    still contribute to the distance; normalization never authorizes input.
    """
    Image, ImageChops, ImageStat = image_modules()
    with Image.open(actual) as a, Image.open(reference) as b:
        size = (min(a.width, b.width), min(a.height, b.height))
        a, b = (im.convert('RGB').resize(size, Image.Resampling.LANCZOS) for im in (a, b))
        return sum(ImageStat.Stat(ImageChops.difference(a, b)).mean) / (3 * 255)


def control_point(inventory, control, geometry, offset=None):
    """Catalog coordinates are Qt logical client units, not PNG device pixels.

    geometry is a live client rectangle in logical desktop coordinates. Convert
    to the selected head's framebuffer exactly once, after adding its origin.
    A resized layout needs explicit edge anchors. Even equal aspect ratios
    do not prove that Qt's fixed-width controls scale with the window.
    """
    width, height = inventory['width'], inventory['height']
    x, y, w, h = control['rect']
    anchors = control.get('anchors', {})
    for axis, extent, live_extent in (('x', width, geometry['width']), ('y', height, geometry['height'])):
        delta = live_extent - extent
        if delta and axis not in anchors:
            raise RecipeBlocked('live window size differs from catalog; require live controls or explicit layout anchors')
        anchor = anchors.get(axis, 'start')
        if anchor not in ('start', 'center', 'end'):
            raise RecipeBlocked('invalid catalog layout anchor: ' + str(anchor))
        shift = delta * {'start': 0, 'center': .5, 'end': 1}[anchor]
        if axis == 'x':
            x += shift
        else:
            y += shift
    dx, dy = offset if offset is not None else (w / 2, h / 2)
    scale = geometry.get('scale', 1)
    origin_x, origin_y = geometry.get('screen_x', 0), geometry.get('screen_y', 0)
    return (round((geometry['x'] - origin_x + x + dx) * scale),
            round((geometry['y'] - origin_y + y + dy) * scale))


def control_labels(control):
    """Visible and accessible names are distinct, equally valid selectors."""
    values = [control.get(k, '') for k in ('text', 'accessibleName', 'toolTip', 'placeholderText')]
    values += control.get('accessibleNames', [])
    return list(dict.fromkeys(v.split('\t')[0].replace('&&', '\0').replace('&', '').replace('\0', '&')
                              for v in values if v))


def semantic_matches(control, selector):
    if control_matches(control, selector):
        return True
    if selector.startswith('text:'):
        return selector[5:] in control_labels(control)
    if selector.startswith('text-glob:'):
        return any(fnmatch.fnmatchcase(label, selector[10:]) for label in control_labels(control))
    return False


class Catalog:
    def __init__(self, directory, profile):
        self.directory, self.profile = Path(directory), profile
        aliases = json.loads((Path(__file__).with_name('profiles') / 'control-names.json').read_text())
        aliases.update(profile.get('control_names', {}))
        self.inventories = {p.name.removesuffix('.controls.json'): json.loads(p.read_text())
                            for p in sorted(self.directory.glob('*.controls.json'))}
        for inventory in self.inventories.values():
            for control in inventory.get('controls', []):
                control['accessibleNames'] = list(dict.fromkeys(control.get('accessibleNames', []) + aliases.get(control.get('name'), [])))

    def reference(self, name):
        if Path(name).name != name:
            raise RecipeBlocked('catalog reference must be a capture name')
        path = self.directory / (name + '.png')
        if not path.is_file():
            raise RecipeBlocked('catalog reference missing: ' + str(path))
        return path

    def _matches(self, selector, window, capture=None):
        names = [capture] if capture else self.profile.get('catalog_windows', {}).get(window, [])
        if isinstance(names, str):
            names = [names]
        if not names:
            names = [name for name in self.inventories if window in name]
        found = []
        for name in names:
            inventory = self.inventories.get(name, {})
            for control in inventory.get('controls', []):
                if semantic_matches(control, selector):
                    found.append((name, inventory, control))
        return found

    def candidates(self, selector, window, capture=None):
        """Semantic hints for live resolution; never select a runtime menu by coordinates."""
        found = [control for _,_,control in self._matches(selector, window, capture)]
        # Qt 6.4 does not export objectName. Recent swatches are runtime-created
        # and cannot be enumerated in a fixed catalog. The application's stable
        # ID encodes the exact QColor and makeSwatch's accessible name.
        recent = re.fullmatch(r'recentColorSwatch_([0-9a-f]{6})', selector)
        if not found and window == 'editor' and recent:
            found = [{'name':selector, 'class':'QToolButton',
                      'accessibleName':'Color #'+recent[1],
                      'toolTip':'Recent custom color #'+recent[1]}]
        return found

    def resolve(self, selector, window, capture=None):
        found = [v for v in self._matches(selector, window, capture)
                 if v[2].get('enabled', True) and v[2].get('visible', True)]
        # Different states often have identical shared controls. Only collapse
        # identical layout + rectangle; never pick arbitrarily among real choices.
        unique = {(v[1]['width'], v[1]['height'], tuple(v[2]['rect'])): v for v in found}
        if len(unique) != 1:
            raise RecipeBlocked(f'catalog selector {selector!r} for {window!r} has {len(unique)} distinct matches; set a capture with use_catalog()')
        return next(iter(unique.values()))
