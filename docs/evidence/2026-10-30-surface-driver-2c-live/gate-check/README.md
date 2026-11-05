# Live gate check after the third validation (2026-10-30)

Code PR [#726](https://github.com/patrick-hudson/put.gd-desktop/pull/726), driver
`a1b1f85535840770da417611a09e8d3cc1e070b2`. This is a short live check of the
secret-safe evidence gate through the production CLI (`tools/lab/surface_driver/run.py`).
It used synthetic values only, with no credentials and no Proton session.

| | |
| --- | --- |
| Boat sandbox | `bx_6hu56tnj` from `putgd-lab-cinnamon-v3` (bare metal, KVM); deleted ([receipt](lab/sandbox-deletion.json)) |
| Guest | Mint 22.3 Cinnamon/X11, run `88e0f156-f30f-4745-b6dc-3a5ac6d6378f`, golden `cinnamon-x11-probes-v3-session` (`23b3e375…`, unchanged) |
| App and catalog | lab-fixture DEB and putgd-ui-catalog built from `219e43f4090246618c2da3f7e64a2bf11292c0cf`; the catalog manifest records that commit, clean ([identity](lab/catalog-identity.json)) |
| OCR | tesseract 5.3.4 |
| Script | [`gate-live.sh`](gate-live.sh); output in [`gate-check.log`](gate-check.log) |

## Results

| Case | Result |
| --- | --- |
| Pin | The catalog's 29 secret-control states equal `tools/lab/surface_driver/secret-states.json`. |
| Correct configuration: guest placement, `--build 219e43f40902`, pinned catalog, real OCR, destination `s3` | Admitted. The resident reported `219e43f4…`. core_demo rows were blocked only because those recipes support `local` alone. The end-of-run audit passed ([receipt](admitted-receipt.json)). |
| `--build 2`, `--build 219e43f4` (8 hex), `--build 26.1030.2490-dev` | Refused before any target: `--build must be 12-40 hex characters of the exact commit`. No output directory created. |
| `--build ffffffffffff` (12 hex, another commit) | `catalog-build-mismatch` |
| Catalog with 8 of 9 destination/credential secret states removed, manifest edited to match | `catalog-secret-states-differ-from-pin` |
| A `tesseract` on PATH that replays the old fixed probe token | `ocr-self-test-failed` |
| Bypassing `run.main`: `DriverContext` constructed directly with a reduced catalog, native placement or a version build | Refused at construction (`catalog-secret-states-differ-from-pin`, `placement-not-disposable`, `catalog-build-mismatch`); nothing written. |
| Bypassing `run.main`: `run_recipes` given a context the gate did not admit | Refused before any receipt. |
| core_demo, `--destination local` | Exit 0. `editor.tool-text`, `editor.tool-redact`, `editor.undo` and `editor.redo` passed. Audit passed ([receipt](core-demo-local/receipt.json), [redact-tool frame](core-demo-local/redact-tool-frame.png)). |

## The first attempt

The [first attempt](gate-check-first-attempt.log), at driver `219e43f4`, also
refused the `ffffffffffff` case with `ocr-self-test-failed`: real tesseract
misread that run's random token.

Measured over 40 self-tests on this sandbox, 3 failed: 11 px digits were read
as letters (3→S, 4→A) or dropped.

`a1b1f855` changed the self-test in two ways:
- tokens use unconfusable letters only;
- each polarity may draw up to three fresh tokens, one of which must be read
  exactly.

After the change, 60 of 60 self-tests passed, and the case above is refused
for the catalog mismatch alone.

## Tests on this head

- Surface pytest: [450 passed](surface-pytest.log), with real tesseract.
- The validator's third-round suite: [17 passed, 17 failed, 1 skipped](validator-v3.log).
  - **Gate refusals:** all of them pass.
  - **`Gate::test_complete_catalog_admitted`:** fails by design. It expects an
    8-hex `--build` and a three-inventory catalog to be admitted, which the
    required ≥12-hex exact-commit rule and the pin refuse.
  - **Labels, ProbeRegistration and DesktopSafeRecorded (10 tests):** these
    construct `s3` contexts without a gate run, which is now refused at
    construction. With admission patched in they all pass, including the AKIA
    label, the single-row probe and `desktop_safe` recording fixed in this round.
  - **Unicode/zero-width, split-link and compressed-file cases:** belong to
    [#854](https://github.com/patrick-hudson/put.gd-desktop/issues/854).

The overlay was retired by guarded cleanup ([retire](lab/retire)). The wall
tile, tunnel and bootstrap agent were removed.
