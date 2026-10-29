# Surface map per-builder evidence

Code: [PR #667](https://github.com/patrick-hudson/put.gd-desktop/pull/667), source `cf4f4525afc519a3e86f32fd7c1bdde3f55fda2a`, based on nightly `a18d79e762dfa4780522e25d6e3f63df1b3a2258`.

This is synthetic acceptance of the offline map page. No app build, lab desktop, host lock, VM, Proton or storage destination was used. Destination values are badges in synthetic receipts. These pictures establish map behavior, not desktop functionality. Browser, viewport, checks and image SHA-256 values are in [verification.json](verification.json).

## Original acceptance (source cf4f4525)

1. **V1 compatibility:** the original existing `_schema-example.json` is retained as [v1.json](v1.json). Before and after build output was `394 interactions, 0 passed, 0 partial, 0 failed, 0 blocked, 394 untested`. All 394 interaction verdicts and desktop summaries match. A nonempty synthetic v1 sample was also compared using the old and new Python implementations with equal summaries; [comparison receipt](v1-comparison.json). The page retains desktop chips/rollups and its default map design, with the additional host and Review controls.
2. **Two builders and filter:** [driver-v2.json](driver-v2.json) contains the same interaction on m74 and f44. Chromium loaded the generated file directly, selected Ubuntu GNOME/Wayland and Matrix, asserted both columns, then selected m74 and asserted f44 was absent. Annotated screenshots show both cells and the filtered result. Planned guest rows without receipts remain grey; the golden-specific rows refine the planned placement. [Browser assertions](browser-checks.json).
3. **Review:** the same run selected Review with the m74 filter and asserted the reason, both paths and local destination badge. Selecting f44 showed the empty Review state. No JavaScript errors were reported. Paths are synthetic; there are no actual captures at those paths.
4. **Placement inventory:** the model test checks all 25 planned `(desktop, host, placement)` rows in the task table. No desktop ids were missing. The Cinnamon golden is Mint 22.3, as documented by `docs/vm-lab.md` under “First Cinnamon golden disk”; its existing `mint-cinnamon-x11` id covers the VMs. Both Ubuntu GNOME profiles are declared on k72/aurora-amd. Rows describe planned placement, not proof of provisioned/accepted guests. Changes outside model desktops and notes were checked to be absent. Neon remains optional. Native Aurora uses the existing desktop id and now accepts the `source` package.
5. **Checks:** `python3 tools/lab/surface_map/surface_map.py check` passes (`9 desktops, 22 surfaces, 394 interactions`). `python3 -m pytest tests/desktop/test_surface_map.py` on Python 3.12.3 / pytest 9.2.0 gives **32 passed, 391 subtests passed, 1 failed**. The original nightly test also fails on `ModelContract::test_receipt_selectors_name_real_checks`: the model still maps `editorShowsImageAt100Percent` while the driver emits `editorFitsWholeImage`. [Issue #661](https://github.com/patrick-hudson/put.gd-desktop/issues/661) tracks this pre-existing gate. This lane was limited to model desktops/notes. Full-green suite acceptance is blocked; no test was skipped or weakened. The nightly job still consumes `tools/lab/surface_map/coverage/[0-9]*.json`; a regression assertion checks that numbered files match and `_schema-example.json` does not.

![Host filter and separate m74/f44 VM cells](hosts-annotated.png)

![Selecting m74 removes the f44 column](filtered-annotated.png)

![Review shows the reason and both frame paths](review-annotated.png)

## Reproduce the page

Use code commit `cf4f4525afc519a3e86f32fd7c1bdde3f55fda2a` and the synthetic input in this directory. Run from the repository root, with a new output directory:

```sh
python3 tools/lab/surface_map/surface_map.py import \
  --coverage /tmp/surface-host-demo.json \
  docs/evidence/surface-map-hosts/driver-v2.json
python3 tools/lab/surface_map/surface_map.py build \
  --coverage /tmp/surface-host-demo.json \
  --output /tmp/surface-host-demo \
  --label 'Synthetic host/filter demo — no desktop acceptance'
```

Open `/tmp/surface-host-demo/map.html` directly in a browser. Select Ubuntu GNOME/Wayland, then Matrix. Select m74 in Host, then Review. No server or network is required. For v1, build with this directory's `v1.json`; use the base commit and its original example file for the before result.

The workstation's system Python lacked pytest and ensurepip, so checks used a private temporary environment with the system Python and pytest/playwright installed through `uv pip`; no system packages or toolchain were replaced. Chromium was the existing `/usr/bin/google-chrome`. No C++ build or CTest was run for these Python/HTML changes.

## Independent-validator fixes (source 05a1ef26)

Code commit `05a1ef263fd764177e8164ff40987a9c23045dc2` fixes findings 1–6 on #667. The original screenshots above remain evidence for the original source; the original Review capture was re-annotated for finding 7 so box 2 and its number leave the `receipt:` line clear. The receipt pixel strip is unchanged from the raw capture, checked in [verification.json](verification.json).

Current validation is **36 passed and the single allowed pre-existing `test_receipt_selectors_name_real_checks` failure** ([#661](https://github.com/patrick-hudson/put.gd-desktop/issues/661), [#662](https://github.com/patrick-hudson/put.gd-desktop/issues/662)); the run with subtest reporting recorded 418 passing subtests. `surface_map.py check` passes: 9 desktops, 22 surfaces, 394 interactions. No tests were skipped or weakened. These checks used the existing temporary Python environment described above; no C++ builds, VMs or physical desktop tests were run.

| Finding | Fix and regression evidence |
| --- | --- |
| 1: unnamed guest failure hidden | Preserve every row backed by entries; remove only empty planned placeholders refined by named guests. `test_unnamed_guest_failure_keeps_its_row_beside_named_pass` round-trips existing v2 data and checks both cells. Chromium confirms both Matrix verdicts and the f44-filtered failed status. |
| 2: Matrix narrowed by desktop chip | Matrix retains every desktop column, with or without a chip; the host selector still filters hosts. Chromium checks all nine v1 columns with Mint selected, and other f44 desktop columns with Ubuntu selected. |
| 3: missing newest metadata erases evidence | Update metadata only for non-None values. `test_newer_host_verdict_preserves_nonempty_metadata` checks missing and explicit None fields, plus intentional False/empty/replacement values and timestamp ordering. |
| 4: optional desktop gates host status, marker lost | Rows carry model `required` values. `test_host_rows_preserve_required_for_planned_and_observed_cells` checks planned and observed rows. Chromium confirms optional neon's failed/wrong verdict does not gate d69's required rollup and its popover says `(optional)`. Explicitly selecting neon still shows its verdict, matching desktop-chip semantics without a host filter. |
| 5: stale usage | Top-level usage documents all five new mark flags, `source`, and optional import defaults supplied by receipt rows. |
| 6: native placement accepts guest | Shared row validation rejects native plus guest with `guest cannot be used with native placement`. `test_native_placement_rejects_guest_in_entry_and_mark` checks make_entry, CLI exit 2, error text, and no coverage file written. |
| 7: Review annotation covers receipt | Rebuilt annotations from the unchanged original screenshot, ending box 2 at y=293 and moving its number into empty space at the upper right. Receipt pixels y=298–315 match the raw capture exactly. |

[Browser assertions](fix-browser-checks.json) and [source, commands, checksums and limits](fix-verification.json) retain the exact source/base identities. V1 Map, Gaps and Matrix main text, verdict buttons/colours and headers match nightly `a18d79e762dfa4780522e25d6e3f63df1b3a2258` with both All required desktops and Mint selected. Comparison normalizes only the planned Aurora label change from `Aurora KDE/Wayland (AppImage)` to `Aurora KDE/Wayland`; this is semantic compatibility, not pixel-identical styling. Raw before/after captures and the comparison snapshots remain private in `/tmp/putgd-surface-fixes/`.

![V1 Matrix keeps all desktop columns with Mint selected](fix-v1-matrix-annotated.png)

![Unnamed f44 failure beside the named golden pass](fix-unnamed-annotated.png)

![Optional neon marker and required-only d69 rollup](fix-optional-annotated.png)

The scrolled Matrix screenshot also shows a pre-existing sticky-heading overlap: the heading offset is 58px even when the toolbar is taller. This was reproduced on nightly (70px toolbar) and the fix source (77px toolbar); [#689](https://github.com/patrick-hudson/put.gd-desktop/issues/689) tracks the layout follow-up. Accessible names independently verify the two Undo cell verdicts despite the partly covered headings.

To reproduce the new synthetic pages, use code commit `05a1ef263fd764177e8164ff40987a9c23045dc2`, export this evidence directory from the docs PR, and build with each of `fix-v1-nonempty.json`, `fix-unnamed-coverage.json`, and `fix-optional-coverage.json` as `--coverage FILE`, into separate fresh `--output DIR` directories. Open the generated HTML directly in Chromium at 1900×1000. For v1 compare the three views with All required desktops and Mint selected. For the unnamed case select f44 and Ubuntu, then Gaps/Matrix. For optional status select d69 and All required desktops, then open Undo's popover; select neon explicitly to inspect its failed/wrong verdict. No server is needed.
