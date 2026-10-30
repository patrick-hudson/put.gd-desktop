"""Synthetic desktop and catalog fixtures for live-evidence tests (no real desktop)."""
import json
from pathlib import Path

# Small notification area in the top-right corner of the synthetic 120x80 head.
EVIDENCE = {'notification_areas': [{'anchor': 'top-right', 'width': 10, 'height': 10}],
            'window_list_panels': [{'anchor': 'bottom', 'height': 6}],
            'browser': {'title': 'Firefox$', 'chrome_height': 12}}


def owned_window(rect=(0, 0, 120, 80), window_id='10', stack=0, pid=7):
    return {'id': window_id, 'title': 'put.gd', 'pid': pid, 'owned': True, 'rect': list(rect), 'stack': stack}


class FakeDesktop:
    """Answers DriverContext._probe for layout/accessible/viewState-free frames."""
    def __init__(self, windows=None, rows=None, page=None):
        self.windows = windows if windows is not None else [owned_window()]
        self.rows = rows if rows is not None else []
        self.calls = []

    def __call__(self, name, query):
        self.calls.append((name, query))
        if name == 'layout':
            return {'windows': [dict(w) for w in self.windows]}
        if name == 'accessible':
            base = [{'window': 'put.gd', 'role': 'frame', 'name': 'put.gd', 'rect': [0, 0, 120, 80], 'objectName': ''}]
            return base + [dict(r) for r in self.rows]
        raise AssertionError('unexpected probe '+name)


def control(object_name, rect, text='', role='text', window='put.gd'):
    return {'window': window, 'role': role, 'name': object_name, 'objectName': object_name, 'text': text, 'rect': list(rect)}


def write_catalog(directory, build='abcdef', dirty=False, pages=('01-history-list', '04-destinations-s3'), secret=True):
    """A synthetic putgd-ui-catalog with secret-control inventories for build."""
    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=True)
    captures = []
    for name in pages:
        controls = [{'name': 'pageAnchor'+name.split('-')[1], 'class': 'QLabel', 'rect': [0, 0, 10, 10]}]
        if secret:
            controls.append({'name': 'historyLink' if 'history' in name else 'credentialSecretKey',
                             'class': 'QLineEdit', 'rect': [5, 5, 20, 5]})
        (directory/(name+'.controls.json')).write_text(json.dumps({'width': 100, 'height': 50, 'controls': controls}))
        captures.append({'file': name+'.png', 'controls': name+'.controls.json', 'width': 100, 'height': 50})
    (directory/'manifest.json').write_text(json.dumps({'kind': 'synthetic-ui-catalog', 'captures': captures,
        'build': {'gitCommit': build+'0'*(40-len(build)), 'dirty': dirty, 'version': '26.1030.1-nightly'}}))
    return directory
