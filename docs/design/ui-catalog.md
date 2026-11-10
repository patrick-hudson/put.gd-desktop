# UI catalog

`putgd-ui-catalog` (`tests/gui/ui_catalog.cpp`) renders every window, menu,
dialog, editor, review and overlay state from synthetic data, one PNG per
state, plus a `<state>.controls.json` inventory per capture and a
`manifest.json`. The Ubuntu 26.04 CI leg keeps one complete dark render for
human inspection, the surface map and exact-build nightly desktop admission.
There is no pixel-comparison gate or committed reference-image set. The catalog
is synthetic: it proves design and widget behaviour, not desktop support
(see [the visual language](visual-language.md)).

An inventory row for a field carries its `echo` mode, and `sensitive: true` when the
field is masked or its widget sets the `putgdSensitive` property (a body text kept
secret, #1361). The surface driver's secret-control gate counts such a row beside the
controls whose names match its pattern, and compares every capture with
`tools/lab/surface_driver/secret-states.json`. A change that adds such a control to a
capture regenerates that pin in the same commit:
`python3 -m tools.lab.surface_driver.gate write-pin build/dev/ui-catalog/dark`.

## Running it

```sh
QT_QPA_PLATFORM=offscreen putgd-ui-catalog OUTPUT_DIR      # dark
PUTGD_CATALOG_THEME=light QT_QPA_PLATFORM=offscreen putgd-ui-catalog OUTPUT_DIR
```

`PUTGD_CATALOG_RECORD_ONLY` and `PUTGD_CATALOG_EDITOR_ONLY` limit the run;
`PUTGD_TEXT_SCALE=1.5` renders at 150% text. `PUTGD_CATALOG_HANDSHAKE` is the
native capture mode for the lab drivers. A restricted, native or skipping run
is never a complete CI render: `manifest.json` records its `scope`, every image
it wrote and any `skippedCaptures`.

## The render-only test

With `-DPUTGD_UI_CATALOG_RENDER=ON` (the Ubuntu 26.04 CI leg only), CTest
registers `ui_catalog_render`. It runs `tools/ci/check_catalog_render.py`,
which renders offscreen and keeps the catalog's own checks (stepper sizes,
review footers, state waits, no source reads). A nonzero renderer exit,
unreadable manifest, restricted or native run, skipped capture, empty image
inventory or missing listed image fails the check.

The test runs in the `cpu` pool alongside the rest of the suite. It produces
`build/dev/ui-catalog/dark` with its `manifest.json`, control inventories and
images. The surface map and secret-control gate check that directory afterwards.
CI uploads it as `ui-catalog` with missing files treated as an error; the nightly
desktop checker downloads the exact-build artifact for live gate admission.
`catalog-check.json` records render status and timing.

### Reproducible renders

The checker and catalog retain stable synthetic inputs for inspection:

- **Clock:** libfaketime stops the wall clock at 2026-06-15 12:00:00 UTC
  (monotonic time runs, so Qt timers work).
- **Paths:** the catalog's data folders are `/tmp/putgd-catalog-<variant>`,
  under a lock, because Settings shows them.
- **Build identity:** the catalog links `tests/gui/catalog_build_identity.cpp`,
  a fixed version, instead of the commit's.
- **Captures:** `steadyGrab()` waits for History's thumbnails, draws the busy
  indicator at a fixed phase with a steady text cursor, and grabs until two
  frames 150 ms apart are identical. `reveal()` scrolls from the top once
  pending layout settles.

## History states

The stock catalog fixture has eight History items. A second seeded history of 68
images over six days, with two failed uploads, renders the states that need
several pages and days: `history-pages` (heading with its count, sidebar counts,
day sections and the pager), `history-pinned-day` (a pinned day heading),
`history-details-closed` (the pane closed with a selection), `history-selection-header`
(the selection bar in the header), `history-page-continued` (page 2 starting
with a `· continued` day), `history-filter-chips` and `history-filter-chips-more`
(one line, and two lines with `+N more` in the 1060 × 680 window) and
`history-gallery-days` (gallery captions and day sections). `history-in-progress`
shows the `In progress` section; `47-history-filtering` the blank count while a
query loads and `43-history-no-matches` the `0 of N items` note.

## Reviewing a UI change

Inspect the published `ui-catalog` artifact when changing the UI. Moved,
clipped or recoloured controls no longer fail an automatic pixel comparison;
human review of the rendered states is required. There is no reference-update
command or regeneration commit. A failed render must be fixed before the
surface map and nightly consumers can use the catalog.

A new catalog state must be deterministic: use `save()`/`steadyGrab()`, wait
for the settled state through `tests/fixtures/Waits.h` rather than a fixed
delay, and never show the real clock, a temporary path or the build version.
Render it twice before adding it.
