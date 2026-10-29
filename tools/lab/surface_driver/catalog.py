"""The build's ui-catalog sidecars are the only source of control rectangles."""
import json
import math
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
    """Normalized RGB mean absolute error; dimension mismatch is never silently resized."""
    Image, ImageChops, ImageStat = image_modules()
    with Image.open(actual) as a, Image.open(reference) as b:
        if a.size != b.size:
            return 1.0
        return sum(ImageStat.Stat(ImageChops.difference(a.convert('RGB'), b.convert('RGB'))).mean) / (3 * 255)


def control_point(inventory, control, geometry, offset=None):
    """Catalog coordinates are Qt logical client units, not PNG device pixels.

    geometry is a live client rectangle in logical desktop coordinates. Convert
    to the selected head's framebuffer exactly once, after adding its origin.
    A resized layout needs a matching catalog state; proportional mapping is
    for uniformly scaled layouts only (nonuniform layouts are refused).
    """
    width, height = inventory['width'], inventory['height']
    sx, sy = geometry['width'] / width, geometry['height'] / height
    if not math.isclose(sx, sy, rel_tol=.015):
        raise RecipeBlocked('live window aspect differs from catalog; select a matching capture/layout')
    x, y, w, h = control['rect']
    dx, dy = offset if offset is not None else (w / 2, h / 2)
    scale = geometry.get('scale', 1)
    origin_x, origin_y = geometry.get('screen_x', 0), geometry.get('screen_y', 0)
    return (round((geometry['x'] - origin_x + (x + dx) * sx) * scale),
            round((geometry['y'] - origin_y + (y + dy) * sy) * scale))


class Catalog:
    def __init__(self, directory, profile):
        self.directory, self.profile = Path(directory), profile
        self.inventories = {p.name.removesuffix('.controls.json'): json.loads(p.read_text())
                            for p in sorted(self.directory.glob('*.controls.json'))}

    def reference(self, name):
        if Path(name).name != name:
            raise RecipeBlocked('catalog reference must be a capture name')
        path = self.directory / (name + '.png')
        if not path.is_file():
            raise RecipeBlocked('catalog reference missing: ' + str(path))
        return path

    def resolve(self, selector, window, capture=None):
        names = [capture] if capture else self.profile.get('catalog_windows', {}).get(window, [])
        if isinstance(names, str):
            names = [names]
        if not names:
            names = [name for name in self.inventories if window in name]
        found = []
        for name in names:
            inventory = self.inventories.get(name, {})
            for control in inventory.get('controls', []):
                if control_matches(control, selector) and control.get('enabled', True):
                    found.append((name, inventory, control))
        # Different states often have identical shared controls. Only collapse
        # identical layout + rectangle; never pick arbitrarily among real choices.
        unique = {(v[1]['width'], v[1]['height'], tuple(v[2]['rect'])): v for v in found}
        if len(unique) != 1:
            raise RecipeBlocked(f'catalog selector {selector!r} for {window!r} has {len(unique)} distinct matches; set a capture with use_catalog()')
        return next(iter(unique.values()))
