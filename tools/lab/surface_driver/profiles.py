"""Desktop policy, with deterministic recursive lane-fragment overrides."""
import copy
import json
from pathlib import Path

HERE = Path(__file__).resolve().parent


def merge(base, fragment):
    result = copy.deepcopy(base)
    for key, value in fragment.items():
        result[key] = merge(result[key], value) if isinstance(value, dict) and isinstance(result.get(key), dict) else copy.deepcopy(value)
    return result


def load_profile(desktop, directory=None):
    directory = Path(directory or HERE / 'profiles')
    if Path(desktop).name != desktop:
        raise ValueError('desktop must be an id, not a path')
    value = json.loads((directory / (desktop + '.json')).read_text())
    for fragment in sorted((directory / (desktop + '.d')).glob('*.json')):
        value = merge(value, json.loads(fragment.read_text()))
    if value.get('desktop') != desktop:
        raise ValueError('profile desktop does not match requested desktop')
    tolerance = value.get('catalog_tolerance')
    if isinstance(tolerance, bool) or not isinstance(tolerance, (int, float)) or not 0 <= tolerance < 1:
        raise ValueError('catalog_tolerance must be a normalized image distance in [0, 1)')
    return value
