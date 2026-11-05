"""tools/ci/check_docs.py on synthetic trees, and the real tree against it."""
import contextlib
import importlib.util
import io
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tests/releases'))
import quiet_git  # noqa: E402,F401  (disables git background maintenance, #99)
sys.path.insert(0, str(ROOT / 'tools/releases'))
import source_archive  # noqa: E402


def load(name):
    spec = importlib.util.spec_from_file_location(name, ROOT / 'tools/ci' / (name + '.py'))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


check_docs = load('check_docs')
changes = load('changes')

VALID = {
    'README.md': '# Top\n\n[map](docs/README.md)\n',
    'AGENTS.md': '# Rules\n',
    'CLAUDE.md': '# Index\n',
    'docs/README.md': '# Map\n\n[release](release/README.md), [sources](readme/sources.md)\n',
    'docs/release.md': '# Stub\n\nMoved to [release](release/release.md).\n',
    'docs/verification.md': '# Stub\n\nMoved to [verification](testing/verification.md).\n',
    'docs/package-repositories.md': '# Stub\n\nMoved to [repositories](release/package-repositories.md).\n',
    'docs/cli.md': '# Stub\n\nMoved to [cli](automation/cli.md).\n',
    'docs/mcp.md': '# Stub\n\nMoved to [mcp](automation/mcp.md).\n',
    'docs/user-guide.md': '# Stub\n\nMoved to [guide](user/user-guide.md).\n',
    'docs/automation/README.md': '# Automation\n\n- [cli](cli.md)\n- [mcp](mcp.md)\n',
    'docs/automation/cli.md': '# CLI\n',
    'docs/automation/mcp.md': '# MCP\n',
    'docs/user/README.md': '# User\n\n- [guide](user-guide.md)\n',
    'docs/user/user-guide.md': '# Guide\n',
    'docs/release/README.md': '# Release\n\n- [release](release.md)\n- [repositories](package-repositories.md)\n',
    'docs/release/release.md': '# Release\n',
    'docs/release/package-repositories.md': '# Repositories\n',
    'docs/testing/README.md': '# Testing\n\n- [verification](verification.md)\n',
    'docs/testing/verification.md': '# Verification\n',
    'docs/history/README.md': '# History\n',
    'docs/readme/sources.md': '# Sources\n',
    'docs/evidence/README.md': '# Evidence\n\nSee [the map](../README.md).\n',
    'docs/evidence/live-s3-memory/source114/manifest.json': '{}\n',
    # A record's own links are not checked.
    'docs/evidence/cold-family-metadata-race/notes.md': '[gone](missing.md)\n',
}


class Tree(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='pgdocs-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        subprocess.run(['git', '-C', str(self.root), 'init', '-q'], check=True)

    def problems(self, changed=None, removed=(), untracked=None):
        files = {**VALID, **(changed or {})}
        for name in removed:
            del files[name]
        for name, content in {**files, **(untracked or {})}.items():
            path = self.root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(content)
        subprocess.run(['git', '-C', str(self.root), 'add', '--', *files], check=True)
        return check_docs.check(self.root)

    def assertOneProblem(self, problems, *fragments):
        self.assertEqual(len(problems), 1, problems)
        for fragment in fragments:
            self.assertIn(fragment, problems[0])


class Links(Tree):
    def test_the_valid_tree_passes(self):
        self.assertEqual(self.problems(), [])

    def test_a_broken_link_names_the_path_and_the_old_path_table(self):
        self.assertOneProblem(self.problems({'docs/testing/verification.md': '# V\n\n[ci](../ci.md)\n'}),
                              'docs/testing/verification.md:3', 'docs/ci.md', '2026-11-docs-layout.md')

    def test_every_living_document_is_checked(self):
        for name in ('README.md', 'AGENTS.md', 'CLAUDE.md', 'docs/README.md', 'docs/evidence/README.md',
                     'docs/history/README.md', 'docs/readme/sources.md'):
            with self.subTest(name=name):
                self.assertOneProblem(self.problems({name: VALID[name] + '[gone](missing.md)\n'}), name + ':', 'broken link')

    def test_a_link_to_an_untracked_file_fails_and_to_a_tracked_directory_passes(self):
        self.assertOneProblem(self.problems({'AGENTS.md': '[x](local.md)\n'}, untracked={'local.md': '# Local\n'}), 'local.md')
        self.assertEqual(self.problems({'AGENTS.md': '[x](docs/release/) [y](docs/testing)\n'}), [])

    def test_absolute_links_fail(self):
        self.assertOneProblem(self.problems({'AGENTS.md': '[x](/docs/README.md)\n'}), 'absolute link', 'relative')

    def test_external_code_and_fenced_targets_are_skipped(self):
        text = ('[a](https://example.com/x.md) [b](mailto:a@example.com) `[c](missing.md)`\n\n'
                '```\n[d](missing.md)\n```\n\n~~~~\n```\n[e](missing.md)\n~~~~\n')
        self.assertEqual(self.problems({'AGENTS.md': text}), [])

    def test_reference_definitions_html_images_and_titles_are_links(self):
        for text in ('[a][r]\n\n[r]: missing.md\n', '<a href="missing.md">x</a>\n', '<img src="missing.png">\n',
                     '![image](missing.png)\n', '[a](missing.md "title")\n', "[a](missing.md 'title')\n",
                     '[a](<missing.md>)\n'):
            with self.subTest(text=text):
                self.assertOneProblem(self.problems({'AGENTS.md': text}), 'broken link', 'missing')


class Anchors(Tree):
    def test_github_slugs(self):
        cases = {'Foo & Bar': 'foo--bar', 'Sharing the lab: rules': 'sharing-the-lab-rules', 'snake_case name': 'snake_case-name',
                 '`code <x>` here': 'code-x-here', '**Bold** and *em* and _under_': 'bold-and-em-and-under',
                 '[Link text](target.md) now': 'link-text-now', 'Über: Größe': 'über-größe', 'v1.2 (beta)': 'v12-beta',
                 'A &amp; B': 'a--b', 'Trailing closed ##': 'trailing-closed'}
        for heading, expected in cases.items():
            with self.subTest(heading=heading):
                self.assertIn(expected, check_docs.anchors(f'## {heading}\n'))

    def test_repeats_setext_html_and_fences(self):
        found = check_docs.anchors('# Same\n\n## Same\n\nTitle\n=====\n\n<a name="explicit"></a>\n\n```\n# Fenced\n```\n\n| a |\n---\n')
        self.assertLessEqual({'same', 'same-1', 'title', 'explicit'}, found)
        self.assertNotIn('fenced', found)

    def test_a_rule_after_a_fenced_block_is_not_a_setext_heading(self):
        found = check_docs.anchors('Not a title\n```\ncode\n```\n---\n')
        self.assertNotIn('not-a-title', found)

    def test_anchors_resolve_across_files_and_in_the_same_file(self):
        text = '# Here\n\n[a](docs/release/release.md#the-section) [b](#here) [c](docs/release/release.md#The-Section)\n'
        target = {'docs/release/release.md': '# Release\n\n## The section\n', 'AGENTS.md': text}
        self.assertEqual(self.problems(target), [])

    def test_a_missing_anchor_fails(self):
        self.assertOneProblem(self.problems({'AGENTS.md': '[a](docs/release/release.md#nope)\n'}), 'missing anchor', 'slug')
        self.assertOneProblem(self.problems({'AGENTS.md': '# Here\n\n[b](#nope)\n'}), 'missing anchor', 'AGENTS.md:3')

    def test_anchors_on_other_file_types_are_not_read(self):
        self.assertEqual(self.problems({'AGENTS.md': '[a](tool.py#L3)\n', 'tool.py': 'pass\n'}), [])


class Indexes(Tree):
    def test_a_document_missing_from_its_folder_readme_fails(self):
        self.assertOneProblem(self.problems({'docs/release/new.md': '# New\n'}), 'docs/release/new.md', 'docs/release/README.md')

    def test_a_folder_without_a_readme_fails(self):
        self.assertOneProblem(self.problems({'docs/topic/one.md': '# One\n'}), 'docs/topic/README.md is missing')

    def test_readme_artwork_folder_is_indexed_by_the_map(self):
        self.assertOneProblem(self.problems({'docs/README.md': '# Map\n'}), 'docs/readme/sources.md', 'docs/README.md')

    def test_a_topic_document_may_be_linked_from_the_entry_document(self):
        readme = '# Release\n\n- [release](release.md)\n- [repositories](package-repositories.md)\n- [topic](topic.md)\n'
        part = {'docs/release/topic/part.md': '# Part\n', 'docs/release/topic/deep/more.md': '# More\n'}
        entry = '# Topic\n\n[part](topic/part.md) [more](topic/deep/more.md)\n'
        self.assertEqual(self.problems({**part, 'docs/release/README.md': readme, 'docs/release/topic.md': entry}), [])

    def test_a_topic_document_may_be_linked_from_the_readme(self):
        readme = '# Release\n\n- [release](release.md)\n- [repositories](package-repositories.md)\n- [part](topic/part.md)\n'
        self.assertEqual(self.problems({'docs/release/README.md': readme, 'docs/release/topic/part.md': '# Part\n'}), [])

    def test_an_unlinked_topic_document_fails(self):
        readme = '# Release\n\n- [release](release.md)\n- [repositories](package-repositories.md)\n- [topic](topic.md)\n'
        self.assertOneProblem(self.problems({'docs/release/README.md': readme, 'docs/release/topic.md': '# Topic\n',
                                             'docs/release/topic/part.md': '# Part\n'}),
                              'docs/release/topic/part.md', 'docs/release/topic.md')

    def test_a_topic_document_with_no_entry_document_fails(self):
        self.assertOneProblem(self.problems({'docs/release/topic/part.md': '# Part\n'}), 'docs/release/topic/part.md', 'entry document')

    def test_an_entry_document_must_itself_be_in_the_readme(self):
        self.assertOneProblem(self.problems({'docs/release/topic.md': '# Topic\n\n[part](topic/part.md)\n',
                                             'docs/release/topic/part.md': '# Part\n'}),
                              'docs/release/topic.md', 'docs/release/README.md')

    def test_history_subfolders_use_the_history_readme(self):
        plan = {'docs/history/plans/plan.md': '# Plan\n'}
        self.assertOneProblem(self.problems(plan), 'docs/history/plans/plan.md', 'docs/history/README.md')
        self.assertEqual(self.problems({**plan, 'docs/history/README.md': '# History\n\n[plan](plans/plan.md)\n'}), [])
        # The topic rule does not apply there.
        entry = {'docs/history/README.md': '# History\n\n[plans](plans.md)\n', 'docs/history/plans.md': '[plan](plans/plan.md)\n'}
        self.assertOneProblem(self.problems({**plan, **entry}), 'docs/history/plans/plan.md', 'docs/history/README.md')


class TopLevel(Tree):
    def test_another_document_directly_in_docs_fails(self):
        self.assertOneProblem(self.problems({'docs/new.md': '# New\n'}), 'docs/new.md', 'docs/<folder>/')

    def test_a_stub_must_link_to_its_new_location(self):
        for stub, text in (('docs/release.md', '# Stub\n'), ('docs/verification.md', '# Stub\n\n[elsewhere](README.md)\n'),
                           ('docs/package-repositories.md', '# Stub\n')):
            with self.subTest(stub=stub):
                self.assertOneProblem(self.problems({stub: text}), stub, check_docs.STUBS[stub])
                self.problems()

    def test_a_removed_stub_fails(self):
        self.assertOneProblem(self.problems(removed=['docs/release.md']), 'docs/release.md', 'restore')

    def test_files_that_are_not_documents_are_allowed(self):
        self.assertEqual(self.problems({'docs/diagram.svg': '<svg/>\n'}), [])


class Evidence(Tree):
    def test_anything_new_under_evidence_points_at_the_evidence_repository(self):
        for name in ('docs/evidence/2026-11-05-new/README.md', 'docs/evidence/live-s3-memory/other.json',
                     'docs/evidence/receipt.json', 'docs/evidence/live-s3-memory/source1140/manifest.json'):
            with self.subTest(name=name):
                self.assertOneProblem(self.problems({name: '{}\n'}), name, 'put.gd-evidence',
                                      'python3 tools/maintenance/evidence.py publish <dir>')
                (self.root / name).unlink()
                subprocess.run(['git', '-C', str(self.root), 'rm', '-q', '--cached', '--', name], check=True)

    def test_the_kept_directories_pass(self):
        self.assertEqual(self.problems({'docs/evidence/live-s3-memory/source114/test/replay.py': 'pass\n',
                                        'docs/evidence/cold-family-metadata-race/fixtures/a.json': '{}\n'}), [])


class Components(Tree):
    def test_a_component_the_archive_drops_fails(self):
        for part in ('agents', 'plans', 'tests', 'test', 'fixtures', 'reviews', 'evidence', 'context', 'work', 'build'):
            with self.subTest(part=part):
                name = f'docs/release/{part}/asset.png'
                self.assertOneProblem(self.problems({name: 'x\n'}), f'docs/release/{part}:', 'rename', 'source_archive.py')
                (self.root / name).unlink()
                subprocess.run(['git', '-C', str(self.root), 'rm', '-q', '--cached', '--', name], check=True)

    def test_each_bad_directory_is_reported_once(self):
        self.assertOneProblem(self.problems({'docs/release/plans/a.png': 'x\n', 'docs/release/plans/b.png': 'x\n'}), 'docs/release/plans:')

    def test_a_file_named_like_a_component_passes(self):
        readme = '# Release\n\n- [release](release.md)\n- [repositories](package-repositories.md)\n- [build](build.md)\n- [agents](agents.md)\n'
        self.assertEqual(self.problems({'docs/release/README.md': readme, 'docs/release/build.md': '# Build\n',
                                        'docs/release/agents.md': '# Agents\n'}), [])

    def test_the_existing_history_directories_pass(self):
        index = '# History\n\n[plan](plans/plan.md) [review](reviews/review.md)\n'
        self.assertEqual(self.problems({'docs/history/README.md': index, 'docs/history/plans/plan.md': '# Plan\n',
                                        'docs/history/reviews/review.md': '# Review\n'}), [])

    def test_paths_outside_docs_are_not_this_rules_business(self):
        self.assertEqual(self.problems({'tests/plans/a.txt': 'x\n'}), [])

    def test_the_list_matches_the_source_archive_filter(self):
        self.assertEqual(check_docs.ARCHIVE_DROPPED, source_archive.EXCLUDED_COMPONENTS)


class CommandLine(Tree):
    def run_main(self):
        output = io.StringIO()
        with contextlib.redirect_stdout(output), mock.patch.object(sys, 'argv', ['check_docs.py', '--root', str(self.root)]):
            return check_docs.main(), output.getvalue()

    def test_exit_status_and_output(self):
        self.problems()
        code, output = self.run_main()
        self.assertEqual(code, 0)
        self.assertIn('OK', output)
        self.problems({'AGENTS.md': '[x](missing.md)\n'})
        code, output = self.run_main()
        self.assertEqual(code, 1)
        self.assertIn('AGENTS.md:1', output)
        self.assertIn('FAILED: 1 problems', output)


class RealTree(unittest.TestCase):
    def test_the_repository_passes(self):
        self.assertEqual(check_docs.check(ROOT), [])

    def test_package_input_documents_reach_the_archive_and_select_every_job(self):
        documents = {name for name in source_archive.EXACT_PATHS if name.startswith('docs/')}
        self.assertEqual(documents, {'docs/release/build.md', 'docs/user/proton-drive.md'})
        self.assertEqual(documents, {name for name in changes.PACKAGING_FILES if name.startswith('docs/')})
        files = check_docs.tracked(ROOT)
        for name in documents:
            with self.subTest(name=name):
                self.assertIn(name, files)
                self.assertTrue(source_archive._included(name))
                self.assertTrue(changes.select([name])['packages'])


if __name__ == '__main__':
    unittest.main()
