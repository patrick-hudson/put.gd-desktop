"""Cache pruning deletes only entries no run can restore."""
import importlib.util
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('ci_cache_prune', ROOT / 'tools/ci/cache_prune.py')
cache_prune = importlib.util.module_from_spec(spec)
spec.loader.exec_module(cache_prune)

NIGHTLY = 'refs/heads/nightly'
SDK = 'putgd-sdk-v1-' + 'ab' * 32
QT = 'install-qt-action-linux-6.17.0-1022-azure-desktop-linux_gcc_64-6.8.3-/home/runner/work/put.gd-desktop/Qt-==1.1.0-==3.3.*'


def cache(key, ref=NIGHTLY, hour=0, ident=None):
    return {'id': ident if ident is not None else hour, 'key': key, 'ref': ref,
            'created_at': f'2026-10-19T{hour:02d}:00:00Z', 'size_in_bytes': 1}


def deleted(caches, open_pulls=(), branches=('nightly', 'main')):
    return {(c['key'], c['ref']): reason for c, reason in cache_prune.plan(caches, set(open_pulls), set(branches))}


class Families(unittest.TestCase):
    def test_rolling_keys_group_by_prefix_and_exact_keys_do_not(self):
        self.assertEqual(cache_prune.family('ccache-linux-24.04-39266470843-1'), 'ccache-linux-24.04-')
        self.assertEqual(cache_prune.family('ccache-clang-asan-26.04-39266470843-2'), 'ccache-clang-asan-26.04-')
        self.assertEqual(cache_prune.family('ctest-cost-linux-26.04-39266470843-1'), 'ctest-cost-linux-26.04-')
        self.assertEqual(cache_prune.family('qt-asan-core-26.04-qt6.10.2+dfsg-7-clang21.1.8-7906bbcc033d33ba'),
                         'qt-asan-core-26.04-')
        pkgs = 'putgd-pkgs-v1-fedora44-' + 'ab' * 32 + '-'
        self.assertEqual(cache_prune.family(pkgs + '2026-W43'), pkgs)
        for key in (SDK, QT, 'ccache-linux-24.04', 'ccache-linux-24.04-latest', pkgs + 'latest'):
            with self.subTest(key=key):
                self.assertIsNone(cache_prune.family(key))


class Plan(unittest.TestCase):
    def test_only_the_newest_entry_of_a_family_on_a_ref_survives(self):
        caches = [cache('ccache-linux-24.04-1-1', hour=1), cache('ccache-linux-24.04-3-1', hour=3),
                  cache('ccache-linux-24.04-2-1', hour=2), cache('ccache-linux-26.04-2-1', hour=2)]
        self.assertEqual(deleted(caches), {
            ('ccache-linux-24.04-1-1', NIGHTLY): 'superseded by ccache-linux-24.04-3-1',
            ('ccache-linux-24.04-2-1', NIGHTLY): 'superseded by ccache-linux-24.04-3-1',
        })

    def test_a_newer_week_supersedes_the_package_snapshot(self):
        pkgs = 'putgd-pkgs-v1-debian13-' + 'cd' * 32 + '-'
        other = 'putgd-pkgs-v1-debian13-' + 'ef' * 32 + '-2026-W42'
        caches = [cache(pkgs + '2026-W42', hour=1), cache(pkgs + '2026-W43', hour=2), cache(other, hour=1)]
        self.assertEqual(deleted(caches), {(pkgs + '2026-W42', NIGHTLY): 'superseded by ' + pkgs + '2026-W43'})

    def test_refs_keep_their_own_newest_entry(self):
        pull = 'refs/pull/7/merge'
        caches = [cache('ctest-cost-linux-24.04-1-1', hour=1), cache('ctest-cost-linux-24.04-2-1', pull, hour=2),
                  cache('ctest-cost-linux-24.04-3-1', pull, hour=3)]
        self.assertEqual(deleted(caches, open_pulls=[7]), {
            ('ctest-cost-linux-24.04-2-1', pull): 'superseded by ctest-cost-linux-24.04-3-1'})

    def test_exact_key_caches_stay_for_idle_eviction(self):
        self.assertEqual(deleted([cache(SDK, hour=1), cache('putgd-sdk-v1-' + 'cd' * 32, hour=2), cache(QT, hour=3)]), {})

    def test_closed_pull_requests_and_deleted_branches_lose_every_cache(self):
        caches = [cache(SDK, 'refs/pull/5/merge', hour=1), cache('ctest-cost-linux-24.04-1-1', 'refs/pull/6/merge', hour=1),
                  cache('ccache-linux-24.04-1-1', 'refs/heads/gone', hour=1),
                  cache('ccache-linux-24.04-2-1', 'refs/heads/main', hour=2)]
        self.assertEqual(deleted(caches, open_pulls=[6]), {
            (SDK, 'refs/pull/5/merge'): 'closed pull request',
            ('ccache-linux-24.04-1-1', 'refs/heads/gone'): 'deleted branch',
        })

    def test_main_nightly_and_release_tags_lose_only_superseded_family_entries(self):
        for ref in (NIGHTLY, 'refs/heads/main', 'refs/tags/v0.1.1', 'refs/tags/v0.1.1-rc.1'):
            caches = [cache(SDK, ref, hour=1), cache(QT, ref, hour=1), cache('ccache-linux-24.04-1-1', ref, hour=1),
                      cache('ccache-linux-24.04-2-1', ref, hour=2), cache('ccache-linux-26.04-1-1', ref, hour=1)]
            with self.subTest(ref=ref):
                # Neither branch nor tag listings, nor open pull requests, can make these unreachable.
                self.assertEqual(deleted(caches, branches=('nightly',)), {
                    ('ccache-linux-24.04-1-1', ref): 'superseded by ccache-linux-24.04-2-1'})

    def test_a_branch_listing_without_the_trunk_deletes_nothing(self):
        with self.assertRaises(ValueError):
            cache_prune.plan([cache('ccache-linux-24.04-1-1')], set(), set())

    def test_same_second_entries_order_by_id(self):
        caches = [cache('ccache-linux-24.04-1-1', ident=10), cache('ccache-linux-24.04-1-2', ident=11)]
        self.assertEqual(deleted(caches), {('ccache-linux-24.04-1-1', NIGHTLY): 'superseded by ccache-linux-24.04-1-2'})


if __name__ == '__main__':
    unittest.main()
