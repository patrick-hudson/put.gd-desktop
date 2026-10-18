---
title: CI/CD Pipeline Redesign - Plan
type: feat
date: 2026-10-17
supersedes: 2026-10-12-1657-feat-stable-nightly-release-channels-plan.md (branch/coordination model only)
execution: code
status: phase 1 implemented
---

# CI/CD pipeline redesign

## Goal

One run per event, jobs chosen by what changed, `nightly` as the trunk every
PR lands on, a scheduled nightly channel, and a promotion PR from `nightly`
into `main` that cuts a release. Remove the three-workflow coordination layer
that exists today only because jobs live in separate workflow files.

Version identity, tag grammar, package audits, the collector, the publisher's
draft/verify/publish logic and the physical acceptance gates are unchanged.
This plan changes *when* and *how often* they run, not *what* they prove.

## What happens today (measured 2026-10-10 to 2026-10-17)

| Workflow | Event | Runs | Avg wall time |
| --- | --- | ---: | ---: |
| Build | pull_request | 65 | 15.0 min |
| Build | push (main/nightly) | 41 | 14.4 min |
| Linux release (dispatched by coordinator) | workflow_dispatch | 17 | 17.7 min |
| Linux release (packaging-path PRs) | pull_request | 30 | 8.7 min |
| Nightly coordinator | push + schedule | 22 | 0.4 min |

175 runs, about 2,100 wall-clock minutes, 17 merges. Every merge to `main`
produced three runs and a public "nightly" prerelease; 14 of them
from the last three days are on the Releases page.

Problems, in order of cost:

1. **The branch model is inverted.** `nightly` never diverges from `main`; it
   is fast-forwarded *from* `main` on every push and has never been a PR
   target (all 28 merged PRs targeted `main`). "Nightly" releases are really
   per-merge releases of `main`.
2. **Cross-workflow coordination.** Because packaging lives in `release.yml`
   and tests in `build.yml`, the release run's `ci` job polls the Actions API
   for up to two hours waiting for a Build run with the same SHA (16 minutes in
   run 39001997868). `nightly.py`, `github_evidence.py` and the publisher's
   producer-binding flags exist only to make this safe: roughly 500 lines of
   tooling plus 400 lines of tests, and a 2-hour timeout failure mode.
3. **No path awareness.** A docs-only PR (for example #53, four commits) runs
   the full Ubuntu 24.04/26.04 test matrix plus Windows and macOS compiles.
4. **Repeated work inside every job.** Measured on run 39001983521, Linux
   24.04 (15:52 total): `apt-get` 1:22, clone and verify the AWS SDK source
   1:24 *on a cache hit*, build+test 9:37, a second Release-preset compile
   2:37, plus the same Python contract suites that also run in all seven
   other jobs. The CMake presets pin `jobs: 2`; public-repo Linux runners
   have four vCPUs.
5. **Cache coupling.** `sdk_cache.py` hashes `.github/workflows/build.yml`
   into the SDK cache key, so any workflow edit rebuilds the AWS SDK on every
   platform (3 to 5 minutes each).
6. **No protection.** Neither branch has rulesets or required checks, so
   nothing enforces that CI passed before a merge or that `main` only moves by
   PR.


## Decisions (user, 2026-10-17)

| Question | Decision |
| --- | --- |
| Nightly cadence | Daily schedule plus manual runs; no nightly when `nightly` HEAD is already published |
| Prune old nightly tags and prereleases | No; every nightly tag and prerelease is kept |
| ccache for application code | Yes, for the Linux test builds |
| Default branch | `nightly` |
| macOS | Not built in CI for now; Windows remains the portable compile |
| Feature PR merge style | Unchanged: merge commits |
| Scope | Phase 0 and phase 1, starting from current `main` (d1b6c83) |

## Design as implemented

Three refinements to the proposal came out of implementation:

1. **Promotion cuts a candidate, not a stable release.** A failed acceptance on a
   stable draft would have consumed `vX.Y.Z` permanently, because tags are never
   moved. Every push to `main` now cuts the next `vX.Y.Z-rc.N`; stable is a manual
   Release run that requires a candidate tag on the same commit.
2. **No automated push to `nightly`.** Rulesets require a pull request and the
   `CI` check on both branches, so a workflow token cannot fast-forward `nightly`
   or push a version bump without a bypass. Promotion does not need `nightly` to
   contain `main`'s merge commits. The post-release bump is an ordinary pull
   request, and `nightly.py plan` refuses to publish while a stable tag exists for
   the current base, so it cannot be forgotten.
3. **Skip means "already published".** The schedule skips when nightly HEAD's tag
   is a complete, verified public nightly. A failed attempt on unchanged source is
   retried on the next run rather than silently skipped.

### Events

| Event | Runs | Result |
| --- | --- | --- |
| Pull request (any base) | `contracts` always; `linux` + Windows `portable` when compiled inputs change; `packages` when packaging, CMake, Proton or CI inputs change | one required `CI` check |
| Push to `nightly` | everything in `ci.yml` | nothing published |
| Schedule 05:23 UTC or manual `nightly.yml` | `plan`; if changed: CI + packages on the new tag, then `publish` | one nightly prerelease |
| Push to `main` | `tag` (next rc), CI + packages on the tag, `draft` | candidate draft |
| Manual `release.yml -f channel=stable` | `tag` (`vBASE` on a candidate commit), CI + packages, `draft` | stable draft |
| Publication of candidate or stable | `publish.py --publish --acceptance` after exact-asset acceptance | public release |

### Files

- Workflows: `ci.yml`, `package.yml`, `nightly.yml`, `release.yml`. `build.yml` is gone.
- `tools/ci/changes.py` selects jobs; `tools/ci/contracts.py` runs every Python
  suite once; `tools/ci/ctest_preflight.py` holds the former inline preflight;
  `tools/ci/sdk-source.sh`, `provision-linux-sdk.sh` and
  `provision-portable-deps.sh` replace inline SDK recipes.
- `tools/releases/nightly.py plan` replaces `pin`/`prepare`/`execute`/`build-mode`/
  `release-ci`. `tools/releases/release_tag.py` chooses candidate and stable tags.
  `tools/releases/promote.py` drafts or opens the promotion pull request.
- `tools/releases/github_evidence.py` became `github_api.py`; cross-workflow
  producer verification and dispatch are removed. `publish.py` records the
  workflow run that built the assets instead of producer IDs.
- SDK cache keys hash recipes, not workflow files. Linux test prefixes carry the
  SDK license files, so cache hits skip the SDK clone. Package builders still
  clone and verify it.

### Repository settings

- Default branch `nightly`.
- Rulesets on `nightly` and `main`: pull request required, `CI` status check
  required, force-push and deletion blocked. Repository admins may bypass only
  through a pull request merge.
- Auto-merge allowed. "Delete head branch on merge" stays off, because promotion
  and back-merge pull requests use `nightly` and `main` as heads.

### Rollout record

Phase 1 lands as one pull request into `nightly`, then: default branch flip,
rulesets, a manual nightly (publishes), a second manual nightly (skips), and a
promotion pull request whose merge cuts `v0.1.1-rc.1` as a draft. Run IDs and
timings are recorded in `docs/ci.md` once observed.

### Phase 2 (not started)

Prebuilt builder images to remove the per-job package install, and a cached
SDK source tree for the package builders. Self-hosted runners stay off for pull
requests from a public repository.
