# Graphite design feasibility — 2026-10-26

This receipt covers the standalone Qt visual prototype used to develop the
[Graphite visual language](../visual-language.md). It does **not** establish
production feature parity, compositor support or release acceptance.

## Scope and references

The user requested a modern desktop interface with dark mode first, a complete
light mode, a palette other than blue, coherent labels and controls, and a design
that could be built with the existing C++/Qt stack. The user then authorized
implementation through an unmerged PR, with Opus 5.5 implementing and the design
thread reviewing the language and integrated screens.

The existing app catalogue at `work/ui-fleet-20261026/catalogue/` contained
7,766 screenshots across 11 desktop profiles at audit time. Its captured source
was `6b7949f5cab8ee9d9564d5ebb4f2cee81881cbf8`. Representative screens were inspected;
this is not a claim to have visually inspected every image. The prototype's
`feature-audit.json` maps 14 catalogue categories and 23 reference anchors.

The requested references were inspected at these revisions:

| Reference | Revision | Design input |
| --- | --- | --- |
| [ApexShot](https://github.com/apex-shot/apexshot) | `9065656db3c4f910a1ee17c0f269be7b8cf3074c` | Compact capture controls, canvas emphasis, contextual properties |
| [LinkShot](https://github.com/ZeroDay-Labz/linkshot) | `d29d1030a77c0ae72f91814f950cf62ba672b18f` | Explicit capture modes, named tools and dedicated properties |

Neither reference application was installed or run. A LinkShot repository editor
image has “Snagit Editor” in its title; it was treated as a repository-published
reference, not independently verified LinkShot runtime evidence. No reference
application code was ported into the prototype.

## Frozen prototype identity

| Field | Recorded value |
| --- | --- |
| Host | `m74`, Linux Mint 22.3 |
| Desktop | Cinnamon; version command reports 6.6.9 |
| Session | `c89`, X11, `DISPLAY=:0`, active `lightdm-autologin` session |
| Qt | 6.4.2, dynamically linked distribution libraries |
| Build | C++20, CMake `dev` preset, Ninja |
| Executable SHA-256 | `83ac68bd59fe35cdcf6231d9efbebfdfc4a8155296cfd66e79563db775254553` |
| Copied production components | `9dda39b7685a79aeaa4902561938bf3c94e076c1` |
| Source-manifest SHA-256 | `9405e02ca6e76ff50217bdf46eece2388813219684cbdd94964870c0e5209655` |

The copied editor canvas/document and IconArt components were unchanged. The
prototype uses synthetic history, recording frames and destination data. It has
no capture backend, media encoder, provider access, credential persistence or
upload pipeline. Some secondary controls are visual affordances only.

The private source/evidence root is
`work/design-language-20261026/qt-refined/`. The corresponding builder directory
is `/home/patrick/work/putgd-qt-refined-20261026` on m74. `RUN.txt` records build,
capture and test commands; `evidence/source-manifest.json` records source hashes.
Every native case retains its command, socket-path preflight, binary hash,
session metadata, widget image, visible root-window crop and RFB desktop image.
These private artifacts are retained outside release source exports.

## Recorded results

| Check | Result | Private evidence under `qt-refined/evidence/` |
| --- | --- | --- |
| Offscreen widget, layout and semantic checks | 142 passed | `exercise-final-counts/checks.json` |
| Visible native prototype states | 40 passed: 20 in each theme, one executable SHA | `native-final-v3/` |
| Root and RFB pixel comparison | Every final case matched the Qt widget pixels | Each case's `visible-pixels.json` |
| Real RFB pointer/keyboard checks | 5 passed | `native-final-input-v3/checks.json` |
| Final token text/surface contrast calculation | 48 passed; minimum 4.555:1 | `final-contrast.json` |
| Desktop cleanup and release | No owned GUI process left; released 07:51:46 UTC | `final-release-receipt.json` |

The native input checks exercised drawing on the production canvas, exact undo
and redo pixel restoration with equivalent cleared-selection states, changing
appearance, and closing the owned study window/backdrop. Earlier move/resize and
maximize/restore checks used a different prototype binary and are separately
identified; they are not counted as proof for the frozen final executable.

The contrast calculation covers readable content/status token pairs, not
whole-app accessibility. The final production specification darkens light-mode
success to `#386A4B`; the frozen prototype used `#3A6D4E` on unselected surfaces.
This documented token refinement did not change the frozen executable.

The derived summary originally carried a manually entered Cinnamon 6.6.4 label.
`final-build-and-session.txt` reports 6.6.9. The derived summary is corrected and
`metadata-correction.json` supersedes that label in retained raw input receipts.
No source, executable, pixels, check outcomes or failure records changed.

## Preserved failed and superseded attempts

`native-final-attempt1/` was rejected because the fullscreen backdrop obscured
the app. `native-final-v2/` was superseded after an empty/search count correction;
one launch also overlapped executable linking and failed with Permission denied.
The final capture series used one frozen executable throughout.

`native-final-input/` read a startup receipt too early. The next attempt,
`native-final-input-v2/`, compared different selection overlays and sampled the
appearance change before RFB repainted. The third attempt waited for readiness,
cleared selection consistently and allowed an 800 ms repaint interval. All
attempts and their explanations remain preserved; none of the failures is counted
as a pass.

## Production acceptance boundary

Production implementation is tracked in
[code PR #596](https://github.com/patrick-hudson/put.gd-desktop/pull/596), with
[documentation PR #595](https://github.com/patrick-hudson/put.gd-desktop/pull/595)
to merge afterward. Native production acceptance is recorded separately from
this prototype receipt.

The production integration must independently pass current-source widget and
workflow tests, supported minimum sizes, large text, DPR, complete keyboard and
popup behavior, and the declared compositor functional and memory gates with
saved native visuals. Recording review must include its native custom header in
geometry checks. Production previews, tests and PR checks must identify their own
source and build rather than reuse this prototype's results.

The implementation PR and its linked documentation PR remain unmerged for user
review. This receipt does not waive any gate in the
[acceptance matrix](../acceptance-matrix.md).
