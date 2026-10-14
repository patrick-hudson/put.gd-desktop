"""Compare the documented schema with fresh production settings; no desktop/cloud effects.

Use the locked tools/mcp environment and PUTGD_APP/PUTGD_CLI build paths.
"""
import copy
import json
import os
from pathlib import Path
import subprocess
import tempfile
import time
import unittest
from jsonschema import Draft202012Validator


class ConfigurationSchema(unittest.TestCase):
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
                'XDG_CONFIG_HOME': str(root / 'config'), 'XDG_DATA_HOME': str(root / 'data'),
                'XDG_SESSION_TYPE': 'unspecified', 'XDG_CURRENT_DESKTOP': 'putgd-schema-fixture'}
            for key in ('DISPLAY', 'WAYLAND_DISPLAY', 'DBUS_SESSION_BUS_ADDRESS'):
                environment.pop(key, None)
            socket = runtime / 'putgd/session.sock'
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
