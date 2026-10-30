# Editor zoom shortcuts after menus close (#806)

Code: [PR #825](https://github.com/patrick-hudson/put.gd-desktop/pull/825), head
`382a623c5ad0a14d53bc3efbd3ce1264d16d12b7`. Merge this documentation after the code.
This is a behaviour fix; it introduces no visible UI redesign. These saved frames
document the observed zoom result.

The Zoom menu's `WidgetShortcut` actions competed with separate editor
`WindowShortcut` registrations when focus returned to the Zoom tool button.
Nightly's GUI regression reports ambiguous shortcut overloads. One shared QAction
per zoom command, associated with the editor window, removes that ambiguity while
preserving platform bindings and Ctrl+=. The shortcut settings and global shortcut
implementation are unchanged.

## Builds and widget contracts

Both builds ran on boat `bx_agpb9q4z`, a large fork of `putgd-build-base-v3`, with
the normal dev presets, dynamically linked distro Qt, and the official AWS SDK.

| Build | Clean source | Full CTest, retries disabled |
| --- | --- | --- |
| Nightly `26.1030.2536-dev+gfdc52414043a` | `fdc52414043a78b17148ed31f73fdf5b4573af5f` | 114/114, 141.05 s |
| Head `26.1030.2537-dev+g382a623c5ad0` | `382a623c5ad0a14d53bc3efbd3ce1264d16d12b7` | 114/114, 138.21 s |

The new test-only regression patch was injected into nightly for the red test:
Escape then Ctrl+1 fails; the saved-copy menu control passes. The patch was removed
before the clean nightly build, full suite and DEB. At the head, all 30 combinations
pass: Zoom and Saved copy actions menus × Escape, outside click and choosing an
item × Ctrl+0, Ctrl+1, Ctrl++, Ctrl+= and Ctrl+-. The additional window-scope control
passes too (33 QTest results including setup/cleanup, zero failures).

The GUI test uses condition-based waits and preserves the focus returned by the
menu. Its explicit offscreen window reactivation models the issue's foreground
check; it asserts that the menu's tool button still has focus. It never selects a
canvas tool to make the shortcut work. These are widget contracts, not native
desktop proof.

Full CTest used `putgd-ci`, new private short TMPDIRs, time scale 3, parallel 12 and
the documented resource pool. It invoked plain `ctest --preset dev`, without the
retry wrapper or `--repeat`. The suite includes shortcut keyboard and Settings
coverage. Exact commands, identities, package hashes and case outcomes are in
[results.json](results.json).

## Native Cinnamon replay

The separate large boat sandbox `bx_mq3wnphn` came from `putgd-lab-cinnamon-v3`.
Its fresh KVM overlay was `e8890770-09e0-4a65-bdb5-7608c6877ff1`, backed by the
immutable Cinnamon probes-v3 golden. Mint 22.3 Cinnamon/X11 ran through Qt xcb at
1920×1080, visible on the workstation wall through port 6806 during the run.
Both clean DEBs had `PUTGD_LAB_FIXTURE=OFF`; each app session used a new private
configuration, runtime, data, state and cache directory with only local synthetic
output. The document was 640×320 and the editor client was 900×620.

For both builds, real QMP input clicked Zoom, dismissed it with Escape, confirmed
native EWMH foreground focus without raising the window or selecting a tool, and
sent Ctrl+1 once. `editor.inspect` read:

| Build | Before | After one Ctrl+1 | Native foreground |
| --- | --- | --- | --- |
| Nightly | fitted 98% | fitted 98% | active and target window `69206022` |
| Head | fitted 98% | 100%, not fitted | exact active/target identity in results.json |

Nightly failure, sandbox `bx_mq3wnphn`, app `fdc52414043a`:

![Nightly remains at Fit 98% after Escape and one Ctrl+1](https://raw.githubusercontent.com/patrick-hudson/put.gd-desktop/90b519b03ec338c9be9632507c67a6cc3a362cd9/docs/evidence/2026-10-30-editor-zoom-shortcuts-806/nightly-result.png)

Head pass, same sandbox/overlay, app `382a623c5ad0`:

![Head reaches 100% after Escape and one Ctrl+1](https://raw.githubusercontent.com/patrick-hudson/put.gd-desktop/90b519b03ec338c9be9632507c67a6cc3a362cd9/docs/evidence/2026-10-30-editor-zoom-shortcuts-806/head-result.png)

The head additionally passed all five zoom keys after both Escape and outside-click
dismissal: ten native QMP cases. Native item-selection coverage is **inconclusive**:
the extra driver sent Home/Down/Return intending to select 50%, but inspection still
reported Fit 98%, so it stopped before sending a zoom shortcut. This is tracked in
[#827](https://github.com/patrick-hudson/put.gd-desktop/issues/827). Choosing-item
shortcut coverage and the other editor menu are established by the GUI regression.
No further desktop-quirk attempts were made after the campaign's two-attempt bound.

## Retention and cleanup

Raw JUnit, LastTest logs, registered suites, socket preflights, CMake caches,
toolchain and build identities, clean DEBs, drivers, native receipts, frames and
failure context are retained privately under
`work/campaigns/editor-shortcuts-806-20261030/` in the canonical checkout. Exported
archives and DEBs were checked against their recorded SHA-256 values.

The initial offscreen activation/saved-menu fixture corrections, CTest traversal
startup refusal, and native AT-SPI menu-observation failure are retained alongside
the corrected results. The extra native item-selection precondition is retained as
a failed/inconclusive campaign result; it is not relabelled as a passing native row.
The fresh overlay is retired through guarded cleanup with a reviewed completion
manifest, and both disposable sandboxes are deleted. Cleanup and deletion receipts
are indexed in results.json. Named snapshots and immutable goldens are preserved.

This is scoped bug evidence. It does not establish physical hardware, performance,
memory, another compositor, or full release acceptance.
