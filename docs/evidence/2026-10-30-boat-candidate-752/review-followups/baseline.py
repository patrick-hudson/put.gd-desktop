import importlib.util, sys, unittest
from pathlib import Path
import argparse
parser=argparse.ArgumentParser()
parser.add_argument('--checkout',type=Path,required=True)
parser.add_argument('--baseline',type=Path,required=True)
args=parser.parse_args()
root=args.checkout.resolve()
def load(name,path):
 spec=importlib.util.spec_from_file_location(name,path)
 module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module);return module
suite=load('candidate_tests',root/'tests/desktop/test_boat_candidate.py')
before=load('candidate_before',args.baseline)
before.ROOT=root
suite.candidate=before
names=['test_orphan_sweep_deletes_only_expired_legacy_id_with_shared_name',
       'test_timeout_kill_has_grace_to_recover_create_visible_after_kill',
       'test_fixture_package_has_test_only_summary_and_long_description',
       'test_collection_size_budget_is_capped_at_ttl_and_expiry_is_explicit',
       'test_collection_worker_passes_conservative_ttl_deadline',
       'test_cleanup_help_describes_eligible_orphans_and_live_peers',
       'test_reported_create_id_skips_remaining_create_window_on_failure']
# Adapt only the test invocation to the old signature: the baseline collector
# ignores the new TTL input, exposing its actual unbounded subprocess budget.
original=before.collect_artifacts
def uncapped(*args,ttl_deadline=None,**kwargs): return original(*args,**kwargs)
before.collect_artifacts=uncapped
result=unittest.TextTestRunner(verbosity=2).run(unittest.TestSuite(suite.CandidateContracts(n) for n in names))
assert len(result.failures)+len(result.errors)==len(names), 'Every regression must fail on reviewed head'
print('All 7 regression cases fail on ca13f378 as expected.')
