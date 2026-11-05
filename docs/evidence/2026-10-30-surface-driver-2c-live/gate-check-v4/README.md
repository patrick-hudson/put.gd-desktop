# Live gate check after the fourth validation (2026-10-30)

Code PR [#726](https://github.com/patrick-hudson/put.gd-desktop/pull/726).
Driver, app and catalog are all at `38b0b965ec79b25bc5868052a33bdad15f0cdb81`:
the branch after the core sync #869 and the re-pinned catalog. The run used
synthetic values only, with no credentials and no Proton session.

| | |
| --- | --- |
| Boat sandbox | `bx_v5jp6qqv` from `putgd-lab-cinnamon-v3`; deleted ([receipt](lab/sandbox-deletion.json)) |
| Guest | Mint 22.3 Cinnamon/X11, run `17b31863-c971-46b0-be13-092aebf2fc91`, golden `cinnamon-x11-probes-v3-session` (`23b3e375…`, unchanged) |
| Package | `putgd_26.1030.2754~dev+g38b0b965ec79` ([install](lab/c900-install3.json)) |
| Catalog | putgd-ui-catalog of the same commit; its manifest's `build` is the real `build-info.json` ([identity](lab/catalog-identity.json)), 28 secret-control states |
| Script | [`gate4-live.sh`](gate4-live.sh); output in [`gate-check.log`](gate-check.log) |

## Results

| Case | Result |
| --- | --- |
| Correct configuration: guest placement, exact build, the build's pinned catalog, real OCR, destination `s3` | Admitted. The resident reported a clean `38b0b965…`; the audit passed ([receipt](admitted-receipt.json)). |
| Edited local pin: `gate write-pin` on a catalog without its 6 credential states | `driver-pin-differs-from-build, catalog-secret-states-differ-from-pin` |
| One control (`credentialAccessKey`, the only one in `04-destinations-s3`) removed from both the catalog and the local pin | Refused with the same two blockers. The checkout's own `check-catalog` agreed with the edited pin; the gate used the build commit's pin. |
| A build whose committed pin differs from the driver's: catalog and `--build` both `3d9cb9db` | `driver-pin-differs-from-build, catalog-secret-states-differ-from-pin` |
| `DriverContext` with `placement='boat'` on a physical (builder-like) target | `placement-unproven` |
| `placement='guest'` on a physical target | `physical-desktop-refused` |
| `placement='boat'` on a copied run directory with no owned QEMU | `placement-unproven` |
| `placement='guest'` or `'boat'` on the real owned QEMU guest | Proven and admitted. |

No refused case created an output directory.

## The pin change

The core sync (#869) changed the catalog. Captures now reveal from the top
(c4c77d48), so `52-destinations-conflict` renders at the bucket fields and
no longer shows the credential fields.

At `3d9cb9db`, the boat render therefore differed from the 29-state pin.
`38b0b965` re-pins 28 states, and CI's 26.04 job checks that pin against its
own render.

## Tests

- **Surface pytest:** [476 passed](surface-pytest.log), including `tests/ci/test_changes.py` and the real-OCR cases.
- **The validator's fourth-round suite:** 25 passed, 4 failed, with `V4_CATALOG` set to this render ([log](validator-v4.log)).
  - **Placement proof (2 tests):** `test_resident_reporting_other_build_is_refused_before_settings_or_recipes` and `test_admitted_context_other_destination` use fake targets. Those are now refused earlier, at construction, as `placement-unproven`.
  - **Unreadable pin:** `test_pin_unreadable_refused` points the checkout pin at a missing file. It is refused as `driver-pin-differs-from-build`, because the pin now comes from the build commit.
  - **Hard-coded count:** `test_real_ci_catalog_is_admitted_against_committed_pin` admits the catalog, then expects 29 states; there are now 28.

The overlay was retired by guarded cleanup ([retire](lab/retire)). The wall
tile, tunnel and bootstrap agent were removed.
