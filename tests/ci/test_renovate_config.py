"""Renovate's regex managers keep seeing every pin they manage (.github/renovate.json, #420)."""
import json
from pathlib import Path
import re
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[2]
CONFIG = json.loads((ROOT / '.github/renovate.json').read_text())


def python_pattern(text):
    """Renovate's /…/ file patterns and (?<name>…) groups, in Python's syntax."""
    return re.compile(text.replace('(?<', '(?P<'))


def managed_files(manager):
    patterns = [python_pattern(pattern.strip('/')) for pattern in manager['managerFilePatterns']]
    tracked = subprocess.check_output(['git', '-C', str(ROOT), 'ls-files'], text=True).splitlines()
    return sorted(path for path in tracked if any(pattern.search(path) for pattern in patterns))


class RenovateConfig(unittest.TestCase):
    def test_updates_target_nightly_and_never_merge_themselves(self):
        self.assertEqual(CONFIG['baseBranchPatterns'], ['nightly'])
        self.assertFalse(CONFIG['automerge'])
        self.assertFalse(CONFIG['platformAutomerge'])
        self.assertLessEqual(CONFIG['prConcurrentLimit'], 5)
        # The workflow token cannot push .github/workflows changes (docs/ci.md).
        self.assertEqual(CONFIG['enabledManagers'], ['custom.regex', 'npm', 'pep621'])

    def test_every_regex_manager_matches_in_every_file_it_names(self):
        for manager in CONFIG['customManagers']:
            files = managed_files(manager)
            self.assertTrue(files, manager['managerFilePatterns'])
            for path in files:
                with self.subTest(file=path, match=manager['matchStrings']):
                    text = (ROOT / path).read_text()
                    found = [match for pattern in manager['matchStrings'] for match in python_pattern(pattern).finditer(text)]
                    self.assertTrue(found)
                    for match in found:
                        self.assertTrue(match['currentValue'])

    def test_every_release_tool_and_source_pin_is_managed(self):
        managers = CONFIG['customManagers']
        for name in ('packaging/appimage/tools.lock.json', 'packaging/sbom/tools.lock.json'):
            lock = json.loads((ROOT / name).read_text())
            seen = {match['depName'] for pattern in managers[0]['matchStrings']
                    for match in python_pattern(pattern).finditer((ROOT / name).read_text())}
            self.assertEqual(seen, {tool['name'] for tool in lock['tools']}, name)
        lock = json.loads((ROOT / 'packaging/appimage/tools.lock.json').read_text())['buildInputs']
        pinned = {item['repository'].removeprefix('https://github.com/').removesuffix('.git')
                  for item in [*lock['qtLicenseSources'], *(value for value in lock.values() if isinstance(value, dict))]}
        text = (ROOT / 'packaging/appimage/tools.lock.json').read_text()
        seen = {match['depName'] for match in python_pattern(managers[1]['matchStrings'][0]).finditer(text)}
        self.assertEqual(seen, pinned)

    def test_windows_pins_stay_with_the_windows_rewrite(self):
        for manager in CONFIG['customManagers']:
            for path in managed_files(manager):
                self.assertNotIn('portable', path)
                self.assertFalse(path.startswith('.github/workflows/'), path)

    def test_post_upgrade_task_is_the_allowed_refresh_command(self):
        rules = [rule for rule in CONFIG['packageRules'] if 'postUpgradeTasks' in rule]
        self.assertEqual(len(rules), 1)
        self.assertEqual(rules[0]['postUpgradeTasks']['commands'], ['python3 tools/maintenance/refresh_pins.py'])
        workflow = (ROOT / '.github/workflows/renovate.yml').read_text()
        allowed = re.search(r"RENOVATE_ALLOWED_COMMANDS: '(.*)'", workflow)[1]
        for command in rules[0]['postUpgradeTasks']['commands']:
            self.assertTrue(any(re.fullmatch(pattern, command) for pattern in json.loads(allowed)))

    def test_dependabot_keeps_the_workflow_actions_current_on_nightly(self):
        text = (ROOT / '.github/dependabot.yml').read_text()
        self.assertEqual(re.findall(r'package-ecosystem: (\S+)', text), ['github-actions'])
        self.assertIn('target-branch: nightly', text)
        self.assertIn("patterns: ['*']", text)
        self.assertIn('dependency-name: ilammy/msvc-dev-cmd', text)

    def test_bundled_component_majors_and_the_proton_helper_wait_for_approval(self):
        approved = [rule for rule in CONFIG['packageRules'] if rule.get('dependencyDashboardApproval')]
        names = {name for rule in approved for name in rule.get('matchPackageNames', [])}
        self.assertTrue({'qt/**', 'FFmpeg/FFmpeg'} <= names)
        self.assertTrue(any(rule.get('matchFileNames') == ['tools/proton/**'] for rule in approved))


if __name__ == '__main__':
    unittest.main()
