# Surface driver 2c: live secret-safe evidence (2026-10-30)

Code PR: [#726](https://github.com/patrick-hudson/put.gd-desktop/pull/726), final head
`238257fa89c0a76f322ed4cc6b88c061131d7a70`. This replaces the 2026-10-29
directory as the evidence for that PR; the older directory stays as history of
head `c3db0e8d`, which failed its second independent validation.

These are boat runs with synthetic values only. No real S3, B2 or Proton
credentials and no Proton session were used. Uploads went to the driver's
loopback S3 fixture with fake public URL bases. This is evidence-layer proof,
not desktop release acceptance.

## What was planted, and where

Values, all fake and never working:
- a put.gd-style share link;
- a Backblaze B2 friendly link;
- a B2 S3-endpoint presigned URL with `X-Amz-Credential`, `X-Amz-Signature`,
  `X-Amz-Security-Token`-style parameters;
- a custom-endpoint link (`objects.sd726.internal:9443`, a configured endpoint host);
- a `pmtok_` token;
- an AKIA-shaped access key ID and a secret key.

Channels:

| Channel | How |
| --- | --- |
| History rows | Real uploads through production `editor.submit` to the loopback S3 fixture, with the public URL base switched to the share, B2 and custom bases. |
| Notifications | The app's own notifications. Plus every value sent as an owned `Notify` and through `notify-send`. Cinnamon used both banner positions (top-right default, then `bottom-notifications`); GNOME has one. |
| Clipboard | The presigned URL set with `xclip`/`wl-copy` and read by `clipboard_text()`. |
| Tooltip / foreign window | A non-owned GTK window showing the presigned, B2 and custom links over the History window. |
| Settings secret field | Destinations → Credentials, typed through real QMP key input (Tab focus). |
| Exceptions | A registered token in `RecipeFailed`, scheme-less B2 and custom links in `RecipeFailed`, and `Authorization: Bearer` plus the presigned URL in a `RuntimeError`. |

Driver: [`driver/live.py`](driver/live.py). It reads the values from a
private file, so the raw values are not committed. Since #726's gate round
(third validation), constructing its live `DriverContext` runs the
secret-safe evidence gate and records `live_admitted`; the runs below predate
that enforcement. A later short live check of the gate itself is recorded in
[`gate-check/`](gate-check/README.md).

## Runs

| | Cinnamon (Mint 22.3, X11) | Ubuntu 26.04 GNOME (Wayland) |
| --- | --- | --- |
| Boat sandbox | `bx_nhk7d37m` from `putgd-lab-cinnamon-v2` | `bx_kn72kkcv` from `putgd-lab-ubuntu-gnome-v3` |
| Guest run | `7adcf68d-63c6-48db-92b2-08491dcf61bc` | `0189a37a-5e7a-46f9-b203-f4c0ef2bbb43` |
| Golden (unchanged before and after) | `518e3e45…2e48` (`cinnamon-x11-probes-v2`) | `349698ea…c656` (`gnome-wayland-probes-v2`) |
| App package | lab-fixture DEB of `e02f46e2`, `putgd_26.1030.2487~dev+ge02f46e2b204`, sha256 `bea6d574…` | ubuntu26.04 candidate DEB of `ccc3f98b` (`tools/lab/boat/candidate.py --artifact ubuntu26.04`), `putgd_26.1030.2464~dev+gccc3f98bf374`, sha256 `28c804f9…` |
| Driver source | `7577c2e6` | `238257fa` (final) |
| Catalog | putgd-ui-catalog of `e02f46e2`; manifest records that build (192 captures) | same inventories |
| Overlay | GNOME witness and put.gd extension enabled in the overlay only; reboot after the package upgrade | mintUpdate stopped by exact PID 2670 |

The two package builds differ only in Python tooling and tests; the C++ is
unchanged since `7e2d7ef6`. The published nightly package was refused: it
lacks this PR's `putgd-cli --params-stdin` (attempt g1).

Between the Cinnamon driver `7577c2e6` and the final head there are two
commits:
- `f0210884` adds Wayland-only placement of fields scrolled out of view.
- `238257fa` keeps shell panels in declared desktop frames.

Unit tests cover both commits on X11 as well.

## Results

Each [`summary.json`](cinnamon/summary.json) holds verdicts and frame
statistics. Each [`run/`](cinnamon/run) directory holds the driver's own
receipt, coverage, frame masks and command log.

| Check | Cinnamon | GNOME |
| --- | --- | --- |
| Planted value in any written byte (raw, %-encoded, hex, base64 at 3 offsets, url-safe base64), [leak check](cinnamon/leakcheck.json) | none in 50 files | none in 38 files ([GNOME](gnome/leakcheck.json)) |
| End-of-run audit (`secret_scan`) | passed: 46 files, 41 PNGs OCR-audited | passed: 34 files, 29 PNGs |
| Coverage export | scanned, then published (`published: true`) | same |
| Notification bodies read and registered | 4/4 per position (top and bottom) | 10/10 |
| Clipboard presigned URL read and registered | yes | yes |
| Credentials typed and masked | secret key ([frame](cinnamon/frames/03-destinations-credentials-typed.png)); the earlier full pass typed both fields | access key ID and secret key ([frame](gnome/frames/03-destinations-credentials-typed.png)) |
| Fully black frames | only the undeclared desktop frames (live policy) | same |
| History / Destinations / Settings live frames | 40–60 % masked, UI readable | 20–46 % masked, UI readable |
| Recipe failures | token `[redacted]`; scheme-less links replaced by derivatives; `RuntimeError` reduced to its type | same |

Frames (all from the live, sensitive context):

- Cinnamon:
  - [History](cinnamon/frames/00-history-list.png);
  - [Destinations](cinnamon/frames/02-destinations.png);
  - [Settings](cinnamon/frames/04-settings.png);
  - [foreign link window over History](cinnamon/frames/05-history-under-foreign-window.png);
  - [undeclared desktop](cinnamon/frames/06-desktop-foreign-window.png);
  - [declared desktop](cinnamon/frames/07-desktop-foreign-window-declared.png);
  - [top banner](cinnamon/frames/09-top-share-desktop-declared.png);
  - [bottom banner](cinnamon/frames/24-bottom-b2-desktop-declared.png).
- GNOME:
  - [History](gnome/frames/00-history-list.png);
  - [Destinations](gnome/frames/02-destinations.png);
  - [Settings](gnome/frames/04-settings.png);
  - [foreign link window over History](gnome/frames/05-history-under-foreign-window.png);
  - [declared desktop](gnome/frames/07-desktop-foreign-window-declared.png) (top bar, dock and tray kept; foreign window and banner area masked).

Known cost: on the 1280×800 Cinnamon guest the static notification areas cover
the right 520 px of a full-width window. [#833](https://github.com/patrick-hudson/put.gd-desktop/issues/833)
tracks masking only drawn banners.

## Live gate

Live destinations are refused through the production CLI before any target is
touched ([gate-check.log](cinnamon/tests/gate-check.log)):

| Case | Blockers |
| --- | --- |
| Physical builder desktop (`--placement native --host-profile`) | `placement-not-disposable` |
| Empty catalog | `catalog-missing` |
| OCR that exits 0 and reads nothing | `ocr-self-test-failed` |
| Catalog of another build | `catalog-build-mismatch` |
| Boat or guest placement, exact-build catalog, real tesseract 5.3.4 | none: admitted |

Physical desktops stay refused until [#828](https://github.com/patrick-hudson/put.gd-desktop/issues/828)
(clipboard managers, browser history, link revocation) is designed and built.

## Tests on the final head

Run in the Cinnamon sandbox, with tesseract installed so the real-OCR cases ran:

- [surface pytest](cinnamon/tests/surface-pytest-final.log): 430 passed (`238257fa`).
- The independent validator's suite ([log](cinnamon/tests/validator-final.log)): 20 passed, 1 failed.
  - The failure, `test_b3_small_notification_text_link_masked`, draws its text
    at y=40 on a 40 px canvas, so every image is one solid colour.
  - The same banners with the text inside the canvas (11, 12 and 13 px, both
    polarities, share, scheme-less B2 and registered B2) pass in
    `test_surface_live_evidence.F1OwnedWindowOnly.test_real_ocr_reads_small_banner_text`.
- Contracts:
  - [release](cinnamon/tests/contracts-release.log), [proton](cinnamon/tests/contracts-proton.log), [lab](cinnamon/tests/contracts-lab.log), [observer](cinnamon/tests/contracts-observer.log) and [evidence](cinnamon/tests/contracts-evidence.log) all exit 0 (`f0210884`).
  - CLI ctest (`cli_integration` plus the session and clipboard integrations): [5/5](cinnamon/tests/ctest-cli.log).
  - MCP integration: [6/6](cinnamon/tests/mcp.log).

## Attempts and cleanup

[`attempts.json`](attempts.json) lists every live attempt, what failed and the
commit that fixed it. The failures were frames masked too much, or blocked
cases, never a leak. Byte-level leak checks found no planted value in
attempts c1, c3, c4, c5 and g2–g7. g1 wrote no frames. c2's directory was
deleted with its sandbox before a byte check.

Cleanup:
- **Overlays:** retired by guarded cleanup with explicit completion manifests
  ([Cinnamon](cinnamon/lab/retire), [GNOME](gnome/lab/retire)); goldens unchanged.
- **Sandboxes:** both lab sandboxes and the candidate worker deleted
  ([Cinnamon](cinnamon/lab/sandbox-deletion.json), [GNOME](gnome/lab/sandbox-deletion.json),
  [candidate](gnome/lab/candidate-ubuntu26.04-receipt.json)).
- **Workstation:** wall tiles, tunnels and the bootstrap agents removed.
