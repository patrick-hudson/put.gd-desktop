"""Compare the documented schema with fresh production settings; no desktop/cloud effects.

Use the locked tools/mcp environment and PUTGD_APP/PUTGD_CLI build paths.
"""
import copy
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest
from jsonschema import Draft202012Validator, ValidationError


class ConfigurationSchema(unittest.TestCase):
    def test_capture_defaults_and_explicit_exceptions(self):
        root = Path(__file__).resolve().parents[2]
        schema = json.loads((root / 'schemas/config.schema.json').read_text())
        validator = Draft202012Validator(schema)
        document = json.loads((root / 'config/development.example.json').read_text())
        document['captureDefaults'] = {'destination': 'local', 'profileId': None, 'publicationMode': 'default'}
        for action in document['actions'].values():
            action.update(destination='default', profileId=None, publicationMode='default')
        document['actions']['clipboard']['destination'] = 'local'
        self.assertTrue(validator.is_valid(document))
        missing = copy.deepcopy(document)
        del missing['captureDefaults']
        self.assertFalse(validator.is_valid(missing))
        for key, value in [('destination', 'default'), ('publicationMode', 'invalid'), ('secretKey', 'synthetic')]:
            invalid = copy.deepcopy(document)
            invalid['captureDefaults'][key] = value
            self.assertFalse(validator.is_valid(invalid))
        invalid = copy.deepcopy(document)
        invalid['actions']['region']['profileId'] = 'separate-reference'
        self.assertFalse(validator.is_valid(invalid))

    def test_upload_clipboard_modes_and_legacy_preferences(self):
        schema = json.loads((Path(__file__).resolve().parents[2] / 'schemas/config.schema.json').read_text())
        validator = Draft202012Validator(schema['properties']['preferences'])
        legacy = {'notifications': True, 'launchAtLogin': False}
        self.assertTrue(validator.is_valid(legacy))
        for mode in ('each_link', 'list'):
            self.assertTrue(validator.is_valid(legacy | {'uploadClipboardBehavior': mode}))
        for mode in ('latest', True, None, 42):
            self.assertFalse(validator.is_valid(legacy | {'uploadClipboardBehavior': mode}))

    def test_update_check_preferences(self):
        # #293: optional; check and channel each optional inside it.
        schema = json.loads((Path(__file__).resolve().parents[2] / 'schemas/config.schema.json').read_text())
        validator = Draft202012Validator(schema['properties']['preferences'])
        legacy = {'notifications': True, 'launchAtLogin': False}
        for updates in ({}, {'check': False}, {'channel': 'build'}, {'check': True, 'channel': 'nightly'}, {'channel': 'stable'}):
            self.assertTrue(validator.is_valid(legacy | {'updates': updates}))
        for updates in ({'check': 'yes'}, {'channel': 'beta'}, {'interval': 1}, True, None):
            self.assertFalse(validator.is_valid(legacy | {'updates': updates}))

    def test_appearance_theme_and_reserved_custom_colours(self):
        # #60: theme now; accent and overrides keep their shape for phase 2.
        schema = json.loads((Path(__file__).resolve().parents[2] / 'schemas/config.schema.json').read_text())
        validator = Draft202012Validator(schema['properties']['preferences'])
        legacy = {'notifications': True, 'launchAtLogin': False}
        for theme in ('system', 'dark', 'light'):
            self.assertTrue(validator.is_valid(legacy | {'appearance': {'theme': theme}}))
        full = {'theme': 'system', 'accent': None, 'overrides': {'dark': {}, 'light': {}}}
        self.assertTrue(validator.is_valid(legacy | {'appearance': full}))
        for appearance in ({}, {'theme': 'auto'}, {'theme': 'dark', 'accent': '#123456'},
                           {'theme': 'dark', 'overrides': {'dark': {'accent': '#123456'}, 'light': {}}},
                           {'theme': 'dark', 'overrides': {'dark': {}}}, {'theme': 'dark', 'colour': 'red'}):
            self.assertFalse(validator.is_valid(legacy | {'appearance': appearance}), appearance)

    def test_optional_proton_folder_name_keeps_legacy_settings_valid(self):
        schema = json.loads((Path(__file__).resolve().parents[2] / 'schemas/config.schema.json').read_text())
        validator = Draft202012Validator(schema['$defs']['protonSettings'])
        settings = {'accountRootUid': 'a' * 22 + '~' + 'b' * 22,
            'folderUid': 'a' * 22 + '~' + 'c' * 22, 'shareByDefault': True,
            'linkExpirationDays': 0, 'sharePasswordReference': None, 'requestTimeoutMs': 120000}
        self.assertTrue(validator.is_valid(settings))
        settings['folderName'] = 'Synthetic folder – 📁'
        self.assertTrue(validator.is_valid(settings))
        for name in (None, 42, '', 'x' * 256, '\x00', 'Synthetic folder\n', 'Synthetic\nfolder'):
            with self.subTest(name=name):
                self.assertFalse(validator.is_valid(settings | {'folderName': name}))

    def test_optional_recording_route_template_and_limits(self):
        root = Path(__file__).resolve().parents[2]
        schema = json.loads((root / 'schemas/config.schema.json').read_text())
        validator = Draft202012Validator(schema)
        document = json.loads((root / 'config/development.example.json').read_text())
        validator.validate(document)
        # Absent record action, template and limits stay valid (older documents).
        recorded = copy.deepcopy(document)
        recorded['actions']['record'] = {'destination': 'local', 'profileId': None, 'shortcut': 'Ctrl+Shift+9'}
        recorded['naming']['recordingTemplate'] = 'Rec-{yyyy}-{uuid}.{ext}'
        recorded['limits']['recordingAnimationSeconds'] = 15
        recorded['limits']['recordingAnimationEdge'] = 640
        validator.validate(recorded)
        inherited = copy.deepcopy(recorded)
        inherited['captureDefaults'] = {'destination': 'local', 'profileId': None, 'publicationMode': 'default'}
        inherited['actions']['record'] = {'destination': 'default', 'profileId': None, 'shortcut': ''}
        validator.validate(inherited)
        # A shortcut-only record entry keeps following the region route; a route needs both fields.
        keyed = copy.deepcopy(document)
        keyed['actions']['record'] = {'shortcut': 'Ctrl+Shift+9'}
        validator.validate(keyed)
        unpaired = copy.deepcopy(document)
        unpaired['actions']['record'] = {'destination': 'local', 'shortcut': ''}
        with self.assertRaises(ValidationError):
            validator.validate(unpaired)
        inherited.pop('captureDefaults')
        self.assertFalse(validator.is_valid(inherited))
        for field, value in (('recordingAnimationSeconds', 0), ('recordingAnimationEdge', 8), ('recordingAnimationSeconds', 'long')):
            invalid = copy.deepcopy(recorded)
            invalid['limits'][field] = value
            self.assertFalse(validator.is_valid(invalid), field)
        invalid = copy.deepcopy(recorded)
        invalid['naming']['recordingTemplate'] = '{bogus}'
        self.assertFalse(validator.is_valid(invalid))
        invalid = copy.deepcopy(recorded)
        invalid['actions']['record'] = {'destination': 'upload', 'profileId': None}
        self.assertFalse(validator.is_valid(invalid))

    def test_production_defaults_templates_and_legacy_fields(self):
        app = os.environ.get('PUTGD_APP')
        cli = os.environ.get('PUTGD_CLI')
        self.assertTrue(app and cli, 'Set PUTGD_APP and PUTGD_CLI to built production binaries')
        schema = json.loads((Path(__file__).resolve().parents[2] / 'schemas/config.schema.json').read_text())
        Draft202012Validator.check_schema(schema)
        validator = Draft202012Validator(schema)
        validator.validate(json.loads((Path(__file__).resolve().parents[2] / 'config/development.example.json').read_text()))
        with tempfile.TemporaryDirectory(prefix='putgd-schema-') as temporary:
            root = Path(temporary)
            runtime = root / 'runtime'
            runtime.mkdir(mode=0o700)
            environment = os.environ | {'QT_QPA_PLATFORM': 'offscreen', 'XDG_RUNTIME_DIR': str(runtime),
                'XDG_CONFIG_HOME': str(root / 'config'), 'XDG_DATA_HOME': str(root / 'data'), 'XDG_STATE_HOME': str(root / 'state'),
                'XDG_SESSION_TYPE': 'unspecified', 'XDG_CURRENT_DESKTOP': 'putgd-schema-fixture'}
            for key in ('DISPLAY', 'WAYLAND_DISPLAY', 'DBUS_SESSION_BUS_ADDRESS', 'SESSION_MANAGER'):
                environment.pop(key, None)
            socket = runtime / 'putgd/session.sock'
            subprocess.run([sys.executable, str(Path(__file__).resolve().parents[2] / 'tools/diagnostics/check_socket_paths.py'), '--socket', str(socket)], check=True, capture_output=True, text=True)
            with (root / 'resident.log').open('w') as log:
                resident = subprocess.Popen([app, '--background'], env=environment, stdout=log, stderr=log)
                try:
                    deadline = time.monotonic() + 10
                    while not socket.exists() and resident.poll() is None and time.monotonic() < deadline:
                        time.sleep(.02)
                    self.assertTrue(socket.exists(), 'Private resident did not start')
                    result = subprocess.run([cli, '--socket', str(socket), 'settings.get'], env=environment,
                        check=True, capture_output=True, text=True, timeout=20)
                    response = json.loads(result.stdout)
                    self.assertTrue(response['ok'])
                    document = response['result']['document']
                    errors = list(validator.iter_errors(document))
                    self.assertEqual([error.message for error in errors], [])
                    inherited = copy.deepcopy(document)
                    inherited['captureDefaults'] = {'destination': 'local', 'profileId': None, 'publicationMode': 'default'}
                    for action in inherited['actions'].values():
                        action.update(destination='default', profileId=None, publicationMode='default')
                    validator.validate(inherited)
                    checked = subprocess.run([cli, '--socket', str(socket), 'settings.validate', json.dumps({'document': inherited})], env=environment,
                        check=True, capture_output=True, text=True, timeout=20)
                    self.assertTrue(json.loads(checked.stdout)['ok'])
                    custom = copy.deepcopy(document)
                    custom['naming']['mode'] = 'template'
                    custom['naming']['fileTemplate'] = 'Share-{name}-{uuid}.{ext}'
                    custom['limits']['pendingImageBytes'] = 33554432
                    self.assertTrue(validator.is_valid(custom))
                    legacy = copy.deepcopy(document)
                    for field in ('fileTemplate', 'captureTemplate', 'imageTemplate', 'textTemplate'):
                        legacy['naming'].pop(field)
                    legacy['limits'].pop('pendingImageBytes')
                    legacy['naming']['mode'] = 'generated'
                    self.assertTrue(validator.is_valid(legacy))
                    for pattern in ('{unknown}.png', '{yyyy.png', 'x/{name}', 'x\\{name}', '', '.', '..', '{name}\n'):
                        with self.subTest(pattern=pattern):
                            invalid = copy.deepcopy(custom)
                            invalid['naming']['captureTemplate'] = pattern
                            self.assertFalse(validator.is_valid(invalid))
                    for value in (0, -1, 1.5, '67108864', 1099511627777):
                        with self.subTest(budget=value):
                            invalid = copy.deepcopy(custom)
                            invalid['limits']['pendingImageBytes'] = value
                            self.assertFalse(validator.is_valid(invalid))
                finally:
                    if resident.poll() is None:
                        resident.terminate()
                    try:
                        resident.wait(timeout=10)
                    except subprocess.TimeoutExpired:
                        resident.kill()
                        resident.wait(timeout=5)


if __name__ == '__main__':
    unittest.main()
