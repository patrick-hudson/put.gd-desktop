# UI catalog and reference images

`putgd-ui-catalog` (`tests/gui/ui_catalog.cpp`) renders every window, menu,
dialog, editor, review and overlay state from synthetic data, one PNG per
state, plus a `<state>.controls.json` inventory per capture and a
`manifest.json`. Since #708 the Ubuntu 26.04 CI leg compares every state,
apart from the few [exempt](#reproducible-renders) ones, with a reference image
kept in git, so a moved, clipped or recoloured control fails CI. The catalog is synthetic: it proves design and widget behaviour, not
desktop support (see [the visual language](visual-language.md)).

## Running it

```sh
QT_QPA_PLATFORM=offscreen putgd-ui-catalog OUTPUT_DIR      # dark
PUTGD_CATALOG_THEME=light QT_QPA_PLATFORM=offscreen putgd-ui-catalog OUTPUT_DIR
```

`PUTGD_CATALOG_RECORD_ONLY` and `PUTGD_CATALOG_EDITOR_ONLY` limit the run;
`PUTGD_TEXT_SCALE=1.5` renders at 150% text. `PUTGD_CATALOG_HANDSHAKE` is the
native capture mode for the lab drivers. A restricted, native or skipping run
is never a reference render: `manifest.json` records its `scope`, every image
it wrote and any `skippedCaptures`.

## The reference test

With `-DPUTGD_UI_CATALOG_REFERENCES=ON` (the 26.04 CI leg only; the 24.04
leg's Qt 6.4 draws text and edges differently) CTest registers:

| Test | What it checks |
| --- | --- |
| `ui_catalog_dark` | Every non-exempt dark state against `tests/gui/catalog-references/dark/` |
| `ui_catalog_light` | Every non-exempt light state against `tests/gui/catalog-references/light/` |
| `ui_catalog_text_150` | The catalog's own layout checks at 150% text; no images |

Each runs `tools/ci/check_catalog_references.py`, which renders offscreen and
keeps the catalog's own checks (stepper sizes, review footers, state waits, no
source reads). It fails when a state differs beyond the tolerance, when a
reference has no rendered state, and when a rendered state that is not exempt
has no reference. An exempt state must still render; it has no reference and
its pixels are not compared.
The tests run inside the Test step in the `cpu` pool, in parallel with the
rest of the suite. The surface map check reads the dark render afterwards.

### Tolerance

`tools/ci/image_diff.py` (shared with the lab drivers) compares RGBA pixels. A
pixel differs when any channel moves by more than 8. A state matches when at
most 24 pixels differ and no 8-connected group of differing pixels is wider or
taller than 6 px. Scattered rendering noise passes; a one-pixel shift of a
control, a clipped edge or a recolour does not.

### Reproducible renders

Two renders of one build must be pixel-identical, including on a loaded
runner. The checker and the catalog pin everything that would otherwise vary:

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

`IMAGE_EXEMPT` in `tools/ci/check_catalog_references.py` lists states that
must render but whose pixels are not compared, each with the issue that
removes the exemption. Today: `51a-settings-conflict-compact-update` (#778).

## When a UI change fails the test

An intended visual change fails `ui_catalog_dark`/`light` until its references
are updated. That is the point: every visible change is looked at once.

1. CI uploads the `ui-catalog-check` artifact: each render, its
   `catalog-check.json`, and `failures/<state>.{expected,actual,diff}.png`
   (magenta marks the changed pixels).
2. Check the diff images show what you meant to change, and nothing else.
3. Update the references from that run and commit them in the same PR:

   ```sh
   python3 tools/ci/update_catalog_references.py --run RUN_ID
   git add tests/gui/catalog-references
   ```

`update_catalog_references.py` rewrites only images that changed beyond the
tolerance, adds new states and deletes states the catalog no longer renders.
It refuses a render that is incomplete, restricted or skipped captures, so a
partial render cannot delete references. `--dry-run` lists the changes;
`--from DIR` takes a directory holding `dark/` and `light/` renders; `--catalog
BIN` renders now, which is only valid in an environment matching the CI leg
(an `ubuntu:26.04` container with the CI package list).

A new catalog state must be deterministic: use `save()`/`steadyGrab()`, wait
for the settled state through `tests/fixtures/Waits.h` rather than a fixed
delay, and never show the real clock, a temporary path or the build version.
Render it twice before adding it.
