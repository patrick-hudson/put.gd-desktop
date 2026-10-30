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


BUILD = 'abcdef012345'
COMMIT = 'abcdef0123456789abcdef0123456789abcdef01'


# Two destination states, as in the real catalog: removing one of them, or one
# secret control, still leaves the page with secret controls.
SECRETS = {'01-history-list': ('historyLink',), '04-destinations-s3': ('credentialSecretKey', 'credentialAccessKey'),
           '18-destination-credentials-compact': ('credentialSecretKey',)}


def write_catalog(directory, build=COMMIT, dirty=False, pages=tuple(SECRETS), secret=True):
    """A synthetic putgd-ui-catalog with secret-control inventories for build."""
    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=True)
    captures = []
    for name in pages:
        controls = [{'name': 'pageAnchor'+name.split('-')[1], 'class': 'QLabel', 'rect': [0, 0, 10, 10]}]
        if secret:
            controls += [{'name': control, 'class': 'QLineEdit', 'rect': [5, 5+10*i, 20, 5]}
                         for i, control in enumerate(SECRETS.get(name, ()))]
        (directory/(name+'.controls.json')).write_text(json.dumps({'width': 100, 'height': 50, 'controls': controls}))
        captures.append({'file': name+'.png', 'controls': name+'.controls.json', 'width': 100, 'height': 50})
    captures.append({'file': '90-chrome.png', 'width': 100, 'height': 50})  # declares no inventory
    (directory/'manifest.json').write_text(json.dumps({'kind': 'synthetic-ui-catalog', 'captures': captures,
        'build': {'gitCommit': build, 'dirty': dirty, 'version': '26.1030.1-nightly'}}))
    return directory


def pinned(directory):
    """Pin a synthetic catalog's secret-control states (tests only; the real pin
    is tools/lab/surface_driver/secret-states.json)."""
    from contextlib import ExitStack
    from unittest.mock import patch
    from tools.lab.surface_driver import gate, redact
    states = gate.catalog_states(directory)[1]
    # The fixture's commit has no git objects: stand in for the build
    # commit's own pin and (unchanged) classifier, and the checkout's pin.
    classifier = (states, redact.SECRET_FIELD.pattern, redact.SECRET_FIELD.flags, gate.NOT_SECRET_CLASSES)

    class Pinned(ExitStack):
        def start(self):
            for item in (patch.object(gate, 'expected_secret_states', return_value=states),
                         patch.object(gate, 'build_classifier', return_value=classifier),
                         patch.object(gate, 'checkout_pin', return_value=states)):
                self.enter_context(item)
            return self

        def stop(self):
            self.close()

        def __enter__(self):
            return self.start()
    return Pinned()


def admitted():
    """Unit tests that build live contexts without a real gate run (no tesseract,
    synthetic catalog) admit them explicitly; production code cannot."""
    from unittest.mock import patch
    from tools.lab.surface_driver.context import DriverContext
    return patch.object(DriverContext, 'live_gate', staticmethod(lambda *a, **k: []))
