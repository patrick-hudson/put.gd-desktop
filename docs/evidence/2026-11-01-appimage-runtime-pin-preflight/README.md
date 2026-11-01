# AppImage runtime pin preflight — 2026-11-01

Companion to [code PR #1123](https://github.com/patrick-hudson/put.gd-desktop/pull/1123),
head `de0da60b4a43edb054c02e1106fe6016a4ce8176`, based on current `origin/nightly`
`cb0f368e6` when work began. The code PR only suppresses independent Renovate
updates to the AppImage lock's `microsoft/mimalloc` entry. The runtime update
stopped at the user's explicit durable-download prerequisite. No mirror was
created and no runtime, library, source archive, notice or debug-symbol pin changed.

## Candidate identity

The [continuous release](https://github.com/AppImage/type2-runtime/releases/tag/continuous)
was published on 2026-10-19 at 20:37:20Z, targeting
`fedfad51acc2860b2916a1eef32ba4a500ff31ef`. Its body links to successful
[push build 39320578745](https://github.com/AppImage/type2-runtime/actions/runs/39320578745),
whose `head_sha` is the same full commit. The x86-64 build and release-upload jobs
succeeded. Downloaded bytes match GitHub's reported asset digests:

| Asset | Asset ID | Bytes | SHA-256 |
| --- | --- | ---: | --- |
| `runtime-x86_64` | 657374641 | 944632 | `617aa77314d57c8b8e4fae9c3f0b35af7b30d5529cb69e98f64f92510ed98a46` |
| `runtime-x86_64.debug` | 657374685 | 1361136 | `193dd7abf4af95a5cb930ff9b7989524bf625049b1274ad03c702aba77326cc3` |

`strings runtime-x86_64` contains
`https://github.com/AppImage/type2-runtime/commit/fedfad5`. The matching release
target, successful build and embedded identifier establish the candidate's source
identity; the debug file also matches the runtime's `.gnu_debuglink` CRC.
See [release metadata](upstream-release.json), [build metadata](upstream-build.json)
and [byte inspection](inspection.json). These were read-only inspections of public
assets on the workstation; no application build or runtime execution occurred.

The upstream compare API for `8f39b89e2...fedfad51a` reports `ahead`, one commit
ahead and zero behind. The included
[8f39b89e2ac31e1640b3d3f7e9a5108e6ce805fa change](https://github.com/AppImage/type2-runtime/commit/8f39b89e2ac31e1640b3d3f7e9a5108e6ce805fa)
changes `mkdir_p` modes from 0755 to 0700 for extract-and-run directories.

## Why the runtime update stopped

The release API explicitly reports `immutable: false`. The runtime's
[workflow at the exact candidate commit](https://github.com/AppImage/type2-runtime/blob/fedfad51acc2860b2916a1eef32ba4a500ff31ef/.github/workflows/build.yaml)
uploads with the rolling pyuploadtool. The uploader's
[current source at ef4565f](https://github.com/TheAssassin/pyuploadtool/blob/ef4565f61f3854fa27e6ed112b62b01a864f7831/pyuploadtool/releases_hosting_provider/github_releases.py)
deletes an existing release when its target commit differs, recreates the tag,
and deletes existing same-name assets before upload even for the same commit.
This source inspection describes the replacement risk; it does not claim the
unpinned uploader downloaded by the historical run has this exact source identity.

The only upstream releases listed at inspection were `continuous`, `20251108`
and `old`. There was no fixed release containing the candidate commit. The build's
[Actions artifacts](upstream-artifacts.json) expire on 2027-01-17, so they are not
a durable fallback either. A numeric release-asset API URL would still name a
deletable asset, not preserve it after the uploader deletes the release or asset.

`tools/releases/portable_appimage.py:tools` downloads each lock entry's exact URL
with urllib and rejects a SHA-256 mismatch. That prevents accepting replacement
bytes, but a cold build fails after the continuous URL changes. A warm local cache
does not provide a durable public source for a clean build.
`tools/releases/provision-appimage.py` checks out source/licence commits; it does
not preserve the runtime binary. `refresh_pins.py:refresh_tools` expects upstream
release download URLs and does not supply an immutable archive service.

Options requiring a new decision are:

1. Obtain an upstream fixed release for `fedfad51acc…`, preferably with immutable
   assets, then pin its URL and measured digest.
2. Explicitly approve a project-controlled immutable copy of these exact upstream
   runtime/debug bytes, retaining original URLs, digests, build identity and notices.
   No such mirror was created or assumed by this work.
3. Scope a runtime build from pinned source and a pinned build environment, with
   its own reproducibility, library-identification and redistribution checks.
   This would change the requested prebuilt-asset approach.

## Validation and remaining gates

All seven existing `tests/ci/test_renovate_config.py` contracts pass for the code
change; `git diff --check` passes. The standalone Renovate validator was not
installed locally. CI also runs the Renovate workflow's strict validator.
The disabling rule matches only `custom.regex`, only
`packaging/appimage/tools.lock.json`, and only `microsoft/mimalloc`. No other
dependency tracking was disabled.

The shipped runtime remains `20251108` / `dd6cebed…`; its recorded mimalloc remains
2.1.7. No bundled library changed in this PR. Candidate library versions were not
re-derived after the durability stop, and are not inferred from Alpine versions.
The existing binary/strings/DWARF/`mi_version` identification method in
`packaging/appimage/runtime-source.json` and the
[earlier notices evidence](../2026-10-22-appimage-notices-417/README.md) remains the
required method once a durable replacement is available.

No CI-built replacement AppImage exists from this config-only change. Therefore
`audit_assets.py`, complete-source, SBOM and licence audits of a replacement,
Mint m74 `cinnamon-x11-appimage` and Ubuntu d69 `gnome-wayland-appimage`
`outcomes.region_upload` / `outcomes.recording_upload`, the manual 0700
extract-and-run check and `--appimage-extract` acceptance were not run. These gates
remain blocked, not passed. No builder or guest was touched, no desktop lease was
acquired, and no tile, tunnel or credential handling was needed.
