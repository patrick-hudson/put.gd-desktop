# Releasing a version: agent runbook

Follow this procedure when the user asks for a release, for example "release
0.1.1", "cut a release" or "ship 0.2.0". [Releases](release.md) explains the
branches, channels and assets; this page is the order of work. Do the steps in
sequence and stop where a step says to.

A version tag is permanent and the version is compiled into every package. So
each version gets exactly one tag, and every set of bytes that ships needs its
own acceptance receipt. Nothing below moves, deletes or retags a `v*` tag.

## Who decides what

| Action | Who |
| --- | --- |
| Open pull requests, watch runs, rerun failed jobs, run acceptance | The agent |
| Merge any pull request into `nightly` or `main` | The user, unless they told you in this conversation to merge it and your permissions allow it |
| Cut the stable tag, publish a candidate, publish a stable release | The agent, after telling the user the tag, commit and acceptance result and getting a yes, unless the user said to proceed without asking |
| Choose the acceptance path (below) | The user; default to candidate-first when they have not said |
| Mark a blocked or failed gate as passed | Nobody |

**Acceptance paths.** *Candidate-first* (default) accepts and publishes the
candidate, then cuts stable and accepts the stable draft: two campaigns, and a
failure costs only a new candidate number. *Single campaign* cuts stable as soon
as the candidate draft exists and accepts only the stable draft: one campaign,
but a failure burns the version and the release moves to the next patch.

## 1. Establish the target version

```sh
git fetch origin --tags
git show origin/nightly:CMakeLists.txt | grep -m1 'project(putgd VERSION'
gh api repos/patrick-hudson/put.gd-desktop/git/matching-refs/tags/vX.Y.Z --jq '.[].ref'
```

| Situation | Do |
| --- | --- |
| Requested version equals `nightly`'s base and `vX.Y.Z` does not exist | Continue. |
| No version given | Use `nightly`'s base and tell the user which version you are releasing. |
| Requested version is higher than the base | Open a pull request into `nightly` titled `chore(release): prepare X.Y.Z` that changes only `project(putgd VERSION ...)`. Continue after it merges. |
| Requested version is lower than the base, or `vX.Y.Z` already exists | Stop and ask. An older line is released only through the hotfix path below. |

Then check that the source is ready:

- The latest `CI` run for `nightly` HEAD passed: `gh run list --workflow ci.yml --branch nightly --limit 1`.
- List open pull requests into `nightly` and tell the user which are not
  included. Do not wait for them unless the user asks.
- `docs/verification.md` names any acceptance gate already known to be blocked.

## 2. Promote nightly into main

```sh
python3 tools/releases/promote.py
python3 tools/releases/promote.py --open
```

The first command previews the title, the change list and the version's
`CHANGELOG.md` section. The second opens two pull requests: `changelog: vX.Y.Z`
into `nightly`, which adds that section, then the `release: vX.Y.Z` pull request
into `main`. Show the user the changelog pull request so they can curate its
entries, and ask them to merge it (an ordinary merge into `nightly`). Then wait for
the promotion's `CI` check; it now includes `CHANGELOG.md`. Fix a failure with an
ordinary pull request into `nightly`; the promotion pull request picks it up. Ask
the user to merge the promotion with a merge commit.

## 3. Check the candidate draft

The merge starts the Release workflow on `main`: `tag`, then `ci`, then `draft`.

```sh
gh run list --workflow release.yml --branch main --limit 1
gh release view vX.Y.Z-rc.N --json isDraft,isPrerelease,assets --jq '{draft: .isDraft, prerelease: .isPrerelease, assets: (.assets | length)}'
```

Expect a draft prerelease with 12 assets, tagged on `main` HEAD. If a job fails
for an infrastructure reason, use "Re-run failed jobs" on that run. If the source
is at fault, fix it on `nightly` and promote again; the next push cuts `rc.N+1`.

On the single-campaign path, skip to step 6.

## 4. Run acceptance on the candidate

Work in a private, ignored directory and test the exact draft bytes:

```sh
tag=vX.Y.Z-rc.N
git fetch origin --tags
mkdir -p "work/releases/$tag"
gh release download "$tag" -D "work/releases/$tag/assets"
(cd "work/releases/$tag/assets" && sha256sum --check --strict SHA256SUMS)
```

Coverage and placement come from [the acceptance matrix](acceptance-matrix.md).
Execution rules, including parallel Cinnamon/GNOME/KDE rows, physical gates,
socket-path checks, the visible display wall and saved visuals, come from
`AGENTS.md`. For each row:

- Install the asset that [Releases](release.md#formats-and-build-environments)
  assigns to that distro. Use `tools/lab/lab.py install --run RUN --deb DEB` in a
  fresh VM, or `tools/lab/physical.py HOSTCFG.json install DEB|RPM` on a physical
  desktop. The lab tools do not install an RPM in a VM or an AppImage anywhere;
  install those by hand in the session and record the exact commands. Where a
  distro has no native asset, use the AppImage and record that choice.
- Run the functional scenarios, the editor drivers and the memory gates listed
  in [the VM lab](vm-lab.md), with synthetic payloads and isolated destinations.
- For native packages, also check install over the previous published release or
  nightly, upgrade and removal. For the AppImage, follow the checklist under
  [Acceptance boundary](release.md#acceptance-boundary).

Record the result:

1. Raw evidence stays under `work/releases/$tag/`.
2. Write a sanitized `docs/evidence/YYYY-MM-DD-$tag-acceptance.md` and `.json`
   with exact identities, rows, results and blocked gates, then add an entry to
   `docs/verification.md`. Land them through a pull request into `nightly`.
3. Write the receipt that publication checks. Start it as `pending`:

```sh
python3 - "$tag" "work/releases/$tag" <<'PY'
import json, subprocess, sys
from pathlib import Path
tag, root = sys.argv[1], Path(sys.argv[2])
sums = dict(reversed(line.split('  ', 1)) for line in (root / 'assets/SHA256SUMS').read_text().splitlines())
commit = subprocess.check_output(['git', 'rev-parse', tag + '^{commit}'], text=True).strip()
(root / 'acceptance.json').write_text(json.dumps({
    'tag': tag, 'gitCommit': commit, 'status': 'pending', 'assetSha256': sums,
    'evidence': 'docs/evidence/YYYY-MM-DD-' + tag + '-acceptance.md', 'rows': []}, indent=2) + '\n')
PY
```

Fill in `rows` with each distro, session, asset and result. Set `status` to
`passed` only when every required row passed on these exact bytes. If any gate
failed or is blocked, leave it `pending`, report to the user and stop. A failure
means fixing on `nightly` and starting again from step 2 with the next candidate.

## 5. Publish the candidate

Get the user's yes, then publish from a clean checkout of the tag:

```sh
repo=$PWD
git worktree add "../putgd-$tag" "$tag"
(cd "../putgd-$tag" && python3 tools/releases/publish.py --tag "$tag" \
  --directory "$repo/work/releases/$tag/assets" --publish \
  --acceptance "$repo/work/releases/$tag/acceptance.json")
git worktree remove "../putgd-$tag"
```

It runs the tag's own publisher, re-downloads the draft, checks every byte
against the receipt and makes the release public. A candidate stays a prerelease;
a stable release becomes Latest.

## 6. Cut stable

Confirm that `main` HEAD is still the accepted candidate's commit:

```sh
git ls-remote origin refs/heads/main
git rev-parse "vX.Y.Z-rc.N^{commit}"
```

If they differ, something else reached `main` and a newer candidate was cut;
accept that one first. Otherwise get the user's yes, then:

```sh
gh workflow run release.yml --repo patrick-hudson/put.gd-desktop --ref main -f channel=stable
```

Expect a draft for `vX.Y.Z` that is not a prerelease, with 12 assets. From the
moment this tag exists, scheduled nightlies with new commits fail until step 8.

## 7. Accept and publish stable

The stable assets are rebuilt with the stable version, so their bytes differ from
the candidate's. Repeat step 4 with `tag=vX.Y.Z`, writing a new receipt and
evidence record, then step 5 with the same tag. Confirm the result:

```sh
gh api repos/patrick-hudson/put.gd-desktop/releases/latest --jq .tag_name
```

## 8. Start the next version

Open a pull request into `nightly` titled `chore(release): start X.Y.Z+1
development` that changes only `project(putgd VERSION ...)` in `CMakeLists.txt`,
normally to the next patch. Ask the user to merge it; nightlies resume after it
merges. Do not open it before step 6: until stable is cut, a fix may still need a
new candidate of the current version.

## 9. Report

Tell the user, with links:

- The candidate and stable tags, their release pages and Release run IDs.
- Where each receipt and evidence record is, and the evidence pull request.
- Any gate that was blocked, and what remains open.
- The next-version pull request.

## Other cases

**Hotfix of the released line.** Branch from `origin/main`, make the fix, raise
the patch in `project(putgd VERSION ...)`, and open a pull request into `main`.
Its merge cuts that version's first candidate; continue from step 3. Afterwards,
open a pull request merging `main` into `nightly` so the fix is kept. In a
version conflict, `nightly` keeps its own higher version.

**A run fails after a draft was partly uploaded.** Rerun the failed job of the
same run; the publisher keeps matching uploads and adds missing ones. A new run
rebuilds packages and cannot resume another run's draft. Inspect that draft and
ask the user before deleting an unpublished draft.

**The nightly reports that a stable tag already exists.** Step 8 has not merged.

**Never:** publish without a passed receipt for the exact bytes, accept evidence
from a different tag, substitute a VM or XWayland result for a physical or native
gate, or put credentials or login values in receipts, evidence or logs.
