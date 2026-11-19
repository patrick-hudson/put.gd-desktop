"""CI images: each job runs the image its profile names, and a tag changes with anything it is built from."""
import datetime
import json
from pathlib import Path
import re
import shutil
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/ci'))
import ci_images  # noqa: E402

WORKFLOWS = ROOT / '.github/workflows'


class CiImages(unittest.TestCase):
    def setUp(self):
        self.config = ci_images.load()

    def test_native_builders_start_from_their_matrix_images(self):
        # Lab tools rebuild a package from the matrix image, so the two must be one base.
        workflow = (WORKFLOWS / 'package.yml').read_text()
        rows = re.findall(r'- name: (\S+)\n\s+(?:#.*\n\s+)*format: \w+\n\s+image: (\S+)', workflow)
        self.assertEqual(len(rows), 5)
        for name, image in rows:
            self.assertEqual(self.config['profiles']['native-' + name]['base'], image, name)

    def test_every_container_job_runs_a_known_ci_image(self):
        ci = (WORKFLOWS / 'ci.yml').read_text()
        package = (WORKFLOWS / 'package.yml').read_text()
        named = set()
        for text in (ci, package):
            named |= {a or b for a, b in re.findall(r"images\)(?:\.([a-z0-9-]+)|\['([a-z0-9.-]+)'\])", text)}
        named |= {'native-' + name for name in re.findall(r'- name: (\S+)\n\s+(?:#.*\n\s+)*format:', package)}
        named |= {'linux-' + version for version in re.search(r"distribution: \[(.*)\]", ci).group(1).replace("'", '').split(', ')}
        self.assertEqual(named, set(self.config['profiles']))
        # Only arch-install, which installs the package on a clean base, starts from a plain distro image.
        plain = re.findall(r'^    container: (\S+)$', ci + package, re.M)
        self.assertEqual([image.split('@')[0] for image in plain], ['archlinux:base'])
        for line in re.findall(r'docker run .*', ci + package):
            self.assertIn('"$IMAGE"', line)

    def test_a_tag_changes_with_its_inputs_base_and_week(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for path in {p for name in self.config['profiles'] for p in ci_images.inputs(self.config, name)} | {
                    'packaging/appimage/tools.lock.json'}:
                (root / path).parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(ROOT / path, root / path)
            before = {name: ci_images.tag(self.config, name, '2026w47', root) for name in self.config['profiles']}
            self.assertEqual(len(set(before.values())), len(before))
            self.assertTrue(all(tag.startswith(name + '-2026w47-') for name, tag in before.items()))
            self.assertNotEqual(ci_images.tag(self.config, 'pipewire', '2026w48', root), before['pipewire'])
            with (root / 'tools/releases/install-native-deps.sh').open('a') as stream:
                stream.write('\n')
            after = {name: ci_images.tag(self.config, name, '2026w47', root) for name in self.config['profiles']}
            changed = {name for name in before if before[name] != after[name]}
            self.assertEqual(changed, {name for name in before if name.startswith('native-')} | {'appimage'})
            lock = json.loads((root / 'packaging/appimage/tools.lock.json').read_text())
            lock['buildInputs']['qt'] += '.1'
            (root / 'packaging/appimage/tools.lock.json').write_text(json.dumps(lock))
            self.assertNotEqual(ci_images.tag(self.config, 'appimage', '2026w47', root), after['appimage'])
            self.assertEqual(ci_images.tag(self.config, 'pipewire', '2026w47', root), after['pipewire'])

    def test_the_week_is_the_iso_week(self):
        self.assertEqual(ci_images.week(datetime.date(2026, 11, 19)), '2026w47')
        self.assertEqual(ci_images.week(datetime.date(2027, 1, 1)), '2026w53')


if __name__ == '__main__':
    unittest.main()
