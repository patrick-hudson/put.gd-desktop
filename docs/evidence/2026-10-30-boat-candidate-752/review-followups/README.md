# Review regressions for #753 / #754

[Exact source identities and fixture hashes](identity.json) bind this fake-only
campaign to code head `b02ad2dd4391725dc924a7218dad5dfc33fd3bc0` and reviewed
head `ca13f37828331c00f2731e91dd099133ca4d61cd`. No real Boat CLI, sandbox,
container build, or desktop run was used. Python contracts run on the workstation
in seconds; these results make no new package or desktop acceptance claim.

[All 46 controller tests pass](controller-tests.log). With site packages disabled,
[the same 46 tests pass with five explicit `requires python3-yaml` skips](without-yaml.log).
This includes the PKG_CONFIG recipe test and the other YAML-dependent contracts.

[Seven focused regressions all fail on the reviewed head](before.log):

| Finding | Regression | Before | Fixed |
| --- | --- | --- | --- |
| B1 | Expired legacy box + fresh live same-name box + unnamed neighbour | Deletes live peer | Deletes only expired ID |
| N1 | CLI killed at create timeout; accepted worker appears afterward | Reports verified cleanup with worker left behind | Recovers/deletes worker during 30-second grace |
| N2 | Fixture CPack summary and long description | Summary override missing | Both mark disposable test use |
| N3 | Size-scaled collection budget with 20 seconds of TTL left | Gives subprocess 600 seconds | Caps at 20; records explicit TTL failure |
| N3 | Production worker passes conservative TTL deadline | No TTL bound passed | Worker startup + TTL passed |
| N4 | Cleanup help | Claims name-based deletion | Says eligible orphans, preserves live peers |
| N5 | Failure after create ID printed | Waits 120 seconds | Completes deletion in 2 simulated seconds |

The [baseline runner](baseline.py) loads the current test suite against the exact
reviewed controller without editing that controller. For the older collector
signature only, its adapter drops the new `ttl_deadline` test input, exposing the
old 600-second timeout as a behavioural assertion failure. The worker deadline
case errors because the required argument was never passed; the other six fail
assertions. To reproduce in a checkout of the fixed code:

```sh
set -eu
old=$(mktemp -d /tmp/b752.XXXXXX)
mkdir -p "$old/tools/lab/boat"
git show ca13f37828331c00f2731e91dd099133ca4d61cd:tools/lab/boat/candidate.py > "$old/tools/lab/boat/candidate.py"
python3 docs/evidence/2026-10-30-boat-candidate-752/review-followups/baseline.py \
  --checkout "$PWD" --baseline "$old/tools/lab/boat/candidate.py"
python3 tests/desktop/test_boat_candidate.py -v
python3 -S tests/desktop/test_boat_candidate.py -v
```

## Independent reviewer scenarios

The originals under `/tmp/rev752/scratch_review/` were read before execution and
left unchanged. Inspected private copies changed only their hard-coded controller
and helper paths. Final runs used an immutable export of the pushed code, so later
worktree branch changes could not change the tested controller.

- [Scenario 1](scenario1.log): concurrent same-SHA workers preserve both live
  leases, even after TTL; dead leases and unknown expired boxes are deleted.
  Fresh legacy/unknown boxes, format neighbours, other SHA and unrelated names
  survive the sweeps. Each peer performs its own final cleanup.
- [Scenario 2](scenario2.log): cancellation at two seconds with visibility at five,
  timeout at four with visibility at seven, subsequent orphan cleanup, and expired
  legacy/fresh same-name peers. Both interrupted creates are deleted; only the
  fresh live legacy peer survives the legacy sweep.

The first attempt crossed a branch switch and its shell script was then rewritten
while still open, causing a shell parsing failure. Its [superseded output](superseded-attempt.log)
is retained as failed operational context. It is excluded from acceptance. The
complete frozen rerun above supersedes it; no real sandbox was involved.

## Historical fixture proof correction

The parent `fixture-proof.json` now includes `< fixture-container.sh`, saved-log
redirection, and `grep -Fxq FIXTURE_CONFLICT_PROOF_PASSED fixture-conflict.log`.
The script emits that sentinel and the retained log contains the exact sentinel
line. The command is explicitly labelled a reconstructed replay invocation; the
historical package installation was not repeated under the exhausted quota.
Its hashes remain unchanged. The new fixture summary is tested as a recipe
contract only; old package bytes are not presented as proof of the new summary.
