# Releasing a version: agent runbook

Follow this procedure when the user asks for a release, for example "cut a
release", "ship stable" or "release what's on nightly". [Releases](release.md) explains the
branches, channels and assets; this page is the order of work. Do the steps in
sequence and stop where a step says to.

A version tag is permanent and the version is compiled into every package. So
each version gets exactly one tag, and every set of bytes that ships needs its
own acceptance receipt. Nothing below moves, deletes or retags a `v*` tag.

Versions are calendar versions, `YY.MDD.N` (#373, [versioning.md](versioning.md)).
A stable release's version is the UTC date and commit count of `main`'s merge
commit. It can't be chosen, and it's known only once the promotion merges. There
is no release-candidate channel: acceptance runs on the stable draft itself.

## Who decides what

| Action | Who |
| --- | --- |
| Open pull requests, watch runs, rerun failed jobs, run acceptance | The agent |
| Merge any pull request into `nightly` or `main` | The user, unless they told you in this conversation to merge it and your permissions allow it |
| Publish a stable release | The agent, after telling the user the tag, commit and acceptance result and getting a yes, unless the user said to proceed without asking |
| Mark a blocked or failed gate as passed | Nobody |

A failed acceptance costs only that version: the fix goes into `nightly`, and the
next promotion is a new commit with a new version.

## 1. Check the source

```sh
git fetch origin --tags
python3 tools/releases/promote.py
```

The preview names the nightly version being promoted (`release: promote nightly
YY.MDD.N`), the change list and `CHANGELOG.md`'s `[Unreleased]` section. Tell the
user the stable version isn't known yet: it will be the merge commit's date and
count, a little higher than that nightly's `N`.

Then check that the source is ready:

- The latest `CI` run for `nightly` HEAD passed: `gh run list --workflow ci.yml --branch nightly --limit 1`.
- List open pull requests into `nightly` and tell the user which are not
  included. Do not wait for them unless the user asks.
- `docs/verification.md` names any acceptance gate already known to be blocked.

## 2. Promote nightly into main

```sh
python3 tools/releases/promote.py --open
```

It opens two pull requests:

- `changelog: next stable release` into `nightly`, which adds the
  `[Unreleased]` section;
- `release: promote nightly YY.MDD.N` into `main`.

Show the user the changelog pull request so they can curate its entries, and ask
them to merge it (an ordinary merge into `nightly`). Then wait for the
promotion's `CI` check; it now includes `CHANGELOG.md`. Fix a failure with an
ordinary pull request into `nightly`; the promotion pull request picks it up. Ask
the user to merge the promotion with a merge commit.

## 3. Check the stable draft

The merge starts the Release workflow on `main`: `tag`, then `ci`, then `draft`.
The `tag` job's summary and `release-tag` artifact name the version.

```sh
gh run list --workflow release.yml --branch main --limit 1
tag=$(gh release list --limit 5 --json tagName,isDraft --jq '[.[] | select(.isDraft)][0].tagName')
gh release view "$tag" --json isDraft,isPrerelease,assets --jq '{draft: .isDraft, prerelease: .isPrerelease, assets: (.assets | length)}'
```

Expect a draft for `vYY.MDD.N` that is not a prerelease, with the 16 assets
listed in [Releases](release.md#github-downloads-and-source), tagged on `main`
HEAD. Check that `vYY.MDD.N` is exactly `main` HEAD's date and count:
`git rev-parse "$tag^{commit}"` must equal `git ls-remote origin refs/heads/main`.
If a job fails for an infrastructure reason, use "Re-run failed jobs" on that
run. If the source is at fault, fix it on `nightly` and promote again; the next
push to `main` is a new version.

## 4. Run acceptance on the stable draft

Work in a private, ignored directory and test the exact draft bytes:

```sh
tag=vYY.MDD.N
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
means leaving the draft unpublished, fixing on `nightly` and starting again from
step 2; the next promotion is a new version. The failed tag stays unreleased, and
the release notes and CHANGELOG skip it.

## 5. Publish stable

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
against the receipt and makes the release public. A stable release becomes
Latest. Confirm it:

```sh
gh api repos/patrick-hudson/put.gd-desktop/releases/latest --jq .tag_name
```

## 6. Sign the stable repositories

Sign the stable apt and dnf repositories, which only the user's stable key can do
([package-repositories.md](package-repositories.md)). Within ten minutes of publication
the signer on d69 stages the release. Ask the user to run, on the workstation:

```sh
python3 tools/releases/sign_stable.py --tag vYY.MDD.N
```

gpg-agent and minisign ask for the passphrases, so the user runs it. The signer
publishes on its next run. Check that `https://pkg.put.gd/update/v1/stable.json`
names `vYY.MDD.N` and that `receipts/vYY.MDD.N.json` says `published`.

## 7. Name the CHANGELOG section

```sh
python3 tools/releases/promote.py --finalize "$tag" --open
```

It opens `changelog: vYY.MDD.N` into `nightly`, which renames `[Unreleased]` to
`[YY.MDD.N] - <publication date>`. Ask the user to merge it. Nothing else starts
the next version: nightlies continue with the next commit's date and count.

## 9. Report

Tell the user, with links:

- The stable tag, its release page and Release run ID.
- Where the receipt and evidence record are, and the evidence pull request.
- Any gate that was blocked, and what remains open.
- The CHANGELOG pull request.

## Other cases

**Hotfix of the released line.** Branch from `origin/main`, make the fix and open
a pull request into `main`. Its merge tags that commit's version and stages its
draft; continue from step 3. Afterwards, open a pull request merging `main` into
`nightly` so the fix is kept. No version is bumped anywhere.

**A run fails after a draft was partly uploaded.** Rerun the failed job of the
same run; the publisher keeps matching uploads and adds missing ones. A new run
rebuilds packages and cannot resume another run's draft. Inspect that draft and
ask the user before deleting an unpublished draft.

**The nightly refuses a version that wouldn't sort after the newest nightly.**
`nightly` HEAD's committer date is older than the last nightly's. Find out how
that commit reached `nightly` before doing anything else.

**Never:** publish without a passed receipt for the exact bytes, accept evidence
from a different tag, substitute a VM or XWayland result for a physical or native
gate, or put credentials or login values in receipts, evidence or logs.
