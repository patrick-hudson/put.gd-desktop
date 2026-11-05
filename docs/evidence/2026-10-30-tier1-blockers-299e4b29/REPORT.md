# Tier 1 Phase A tooling repair, 2026-10-30

Local synthetic destinations only. These are Boat development results, not physical/native acceptance or release gates. Phase B did not run. No application build was used for these lab runs; only the published assets were installed. Every run installed the SHA256SUMS-verified published nightly `v26.1030.3203-nightly`, app commit `cc9d9410d6e982dd93d58bcfda82f112c44a82f6`, tree `47d8ee85fb481fde7f557aff8f1c01e378f17ad3`; the catalog came from official CI run `41111404666` at that same app commit. The three Debian-family guests used DEBs; Aurora used the AppImage.

## Results

P = passed the outcome oracle; F = failed; B = tooling blocked. Window/desktop is one recipe with two independent pixel checks. Saved/uploaded bytes are compared by SHA-256; captures are compared pixel-for-pixel with an independently frozen pattern crop at scale 1. Recording compares moving versus frozen clips. Browser checks real navigation and image content after real keyboard paste. Delete uses real UI confirmation, prepared exact IDs/effects, then independent HTTP 404.

| Desktop | Region → upload | Window + desktop | Recording → upload | Edit → upload | Link → browser | History → delete |
| --- | --- | --- | --- | --- | --- | --- |
| Cinnamon X11 | P | P | P | F (#958) | P | P |
| GNOME Wayland | P | P | P | P | P (Mozilla fallback) | P |
| KDE Wayland | P | P | owned by the #822 worker | P | P (Mozilla fallback) | P |
| Aurora Wayland | B (KWin permission) | B (KWin permission / notification) | B (consent path; #822) | B (notification occlusion) | P (Mozilla fallback, a3) | P (a3) |

Cinnamon used two full runs. The first completed its recipe invocation but the final evidence receipt failed because the local final audit still required OCR; that failed attempt is retained. The second passed its final audit and exposed shifted arrow/rectangle origins in the editor pixel oracle (#958). The oracle was not relaxed. Full-run live wall visibility could not be proved because the initial SSH tunnel did not persist; independent guest VNC/QMP frames are retained. The later targeted visible region/browser replay passed both flows, including the exact 600×280 crop and useful full-screen browser content, without a third full run.

GNOME used two full runs and two browser-only diagnostic/retest runs. Full a2 passed all other recipes with a persistent wall tunnel; b3 exposed Snap's private-runtime mismatch and refused before paste. b4 passed with Mozilla 159.0 and the private profile's accessibility enabled. Both failed Snap and initially inaccessible Mozilla attempts are retained. The helper's MIME restoration passed hash checks on every attempt.

Aurora had one startup preflight refusal before any recipe input because its dependency layer had not activated, followed by two full outcome invocations (`a2`, `a3`) and a targeted affected-flow retry (`a4`). The legacy PAM-wallet fixture failed before input in a2 and was removed. Under the unique-bus fixture, a3 passes browser/delete and preserves the existing native owner/collection/default-alias metadata. Its real region drag reaches publication but KWin denies screenshot permission. The resulting persistent failure notification blocks the next pattern/editor checks; those are environment/occlusion blocks, not editor pixel failures. The targeted a4 was started after a normal visible reboot and staged the existing AppImage entry link/native service cache. It still reaches the same KWin rejection (`c7afd166-2fd8-4b4f-8b73-47061f6b91e6`); that ineffective staging experiment was removed from the code PR. Browser navigation and MIME restoration pass again. The two-attempt desktop rule stopped further permission work; [#961](https://github.com/patrick-hudson/put.gd-desktop/issues/961) owns the remaining capture-result gate.

The original ksecretd PID/owner, collection-set hash and default-alias hash match exactly before/after a3 and before/after a4. Reboots naturally create a new native owner; each pair compares within one boot. No existing wallet/item value was read. `a11y.json` records Aurora's actual `IsEnabled`-only property set. Tests also cover `ScreenReaderEnabled` present. Distro PyQt6/GStreamer/GNOME-keyring packages were layered only in the overlay and activated with foreground `sudo systemctl reboot`; KWin, Plasma, Qt and portal versions stayed unchanged. The updater remained masked.

The shipped keyring implementation matches the a3 fixture path; a4's additional registration code is retained only as failed experimental evidence. Each archive includes per-file driver hashes and profile fragments. The main identity of older Cinnamon uses commit `7fe83a0d`; its visible replay uses captured `e8be7a90` files. GNOME/KDE include bounded intermediate script transfers; their file manifests are authoritative. This is not a claim that every desktop ran one clean final harness checkout, or that native release gates passed.

## Changes

- #942: native X11 root-child cover mapping uses measured active outputs, exact installed helper/PID and accessible identity, independently of the changing selection rectangle. Anonymous Wayland covers require the owned packaged helper and unique exact undecorated size. The GNOME witness binds the production extension's exact selector actor to its active request and authenticated bridge sender. Every region drag step rechecks pointer occlusion. Foreign, ambiguous and stale request matches remain refused.
- #952: local fake-data evidence keeps useful frames and masks secret fields. It skips the image/OCR audit, including receipt finalization, coverage export and standalone re-audit; text/metadata redaction and mask manifests remain. Live destinations keep the full #726 layer and default image auditing. Regression tests cover the split.
- #951: Snap's accessible Firefox process loses the private XDG runtime required by the existing ownership test. The guard is preserved. `tools/lab/stage_firefox.py --run RUN` stages the official non-Snap Mozilla archive in an authenticated overlay, verifies its official SHA-512 list and records archive/binary hashes. A copied driver profile uses the receipt's `browser.command` and `browser.profile_root`; no golden or system browser/default is replaced. The #680 helper now enables accessibility only in its disposable profile. Mozilla 159.0 archive SHA-256 `54536fcd120af484d4c56272a5a1cf4af57b9c713d1a3b1e775da192dd541fcd`, binary `de3f1f70c111692fce304c7af83a7bf2e6f4bc8033191ea8b8ccc25f1aa122ed`.
- #954: Plasma staging reads the actual accessibility property dictionary, enables only `IsEnabled`, and verifies every other reported property/key is unchanged. Both presence and absence of `ScreenReaderEnabled` are tested.
- #955: modern ksecretd does not export the legacy pamOpen API. The failed attempt is retained and that approach was removed. The guest-only fixture launches a distro GNOME keyring with private storage and an exact PID/start-time lease, authenticates its unique D-Bus connection, and sets libsecret’s `SECRET_SERVICE_BUS_NAME` only for the private resident. The desktop keeps its real bus and ksecretd owner; no existing wallet, alias or item is opened or modified. The existing unowned-name KDE path is preserved. AppImage fixture helper shims use the verified extracted runtime.
- KDE recording is owned by the #822 worker. The earlier guarded run retained its chooser failure frame and sent no consent click. This task does not change recording acquisition, `fixtures/consent.py` or the PipeWire/runtime environment in `target.py`.

## Validation

Broad remote Python suite: **1,083 passed, seven skipped** in 49.94s. After removing an ineffective AppImage registration experiment, the final 27 focused selector/privacy/keyring/raw-walk checks passed in 0.134s. GNOME witness signal lifecycle and selector-reference/sender/occlusion checks passed. After merging the recording worker’s upstream change, 67 focused/adversarial checks passed in 3.813s. CI exposed one older test that still assumed a local destination used OCR; it now explicitly exercises the live policy, without changing production privacy code. Earlier setup failures, old import-hook failures and the raw-walk unit’s accidental real session-bus call are retained in the private validation logs. The raw-walk fixture was corrected without changing any production time budget. These unit checks are not desktop proof; the graphical receipts above supply it.

## Evidence and cleanup

Private evidence root: `/home/patrick/work/put.gd/work/campaigns/surface-driver-20261029/tier1-fix-299e4b29`. Full raw local-fixture screenshots, recording controls, exact source-file hashes, operation/history IDs, package receipts, capacity/socket checks, failure context and cleanup receipts remain there. Source was transferred as scripts; the retained per-file hashes describe the exact files in each run rather than claiming every intermediate transfer was a clean Git checkout.

Every overlay retirement followed local archive SHA-256 and all-member manifest verification. Goldens and named snapshots were preserved. Sandboxes and persistent tunnel services created for this task were deleted/stopped, and only this task's wall entries were removed.

| Export | Exact VM run ID | Verified members | Archive SHA-256 |
| --- | --- | --- | --- |
| cinnamon | `a1e73f6c-4fd8-4159-b725-820b281d1677` | 453 | `df15bfcab87af4bca369ef662598c78495b857c46d19df04d02f1037bd7758c1` |
| gnome | `826789cb-397b-46b9-aacc-f39b9d024217` | 376 | `df5cbf3402433b9830e163f61c24d943b83de66a8d9b93cc6a8272c2f0a52209` |
| kde | `3ecf33b9-ec72-4877-b294-98a994c19e0d` | 267 | `0de2b6c3c150d0533ee5bdec9f473d9ec9053717410959bbacad0218fda7eb36` |
| cinnamon-visible | `7b747462-54dd-4c56-93b1-17f2708fb097` | 185 | `45b2d9f709fa2e5d98c6a7cdacda66e720d13a6017e5d06a574ca0581c353bb0` |
| aurora | `ef6e7128-6c00-497e-85dd-78c24e9a6bdf` | 492 | `1162cc18112d03d54398161bb4913253e17f7bf8c18d3f134369664c28476920` |

The full archive manifests and every member were checked locally before retirement. `cleanup.json` and `deletion-final.json` in each desktop folder prove guarded overlay retirement and completed sandbox deletion. Five disposable sandboxes were created, with at most two active; the oversize default GNOME allocation failed before creating a sandbox. Named snapshots and immutable sources remain intact. Task SSH-agent/tunnel services and wall entries were removed.

The code PR is [#962](https://github.com/patrick-hudson/put.gd-desktop/pull/962), final code head `48ab5c2c8bbecf74f914dc04b430b5b9f9bd27dd`. This evidence PR targets `feat/surface-driver-core` separately and must merge after the code. No issue was closed and no external review was requested.

## Saved visuals

These unchanged source frames and synthetic uploaded PNGs are pinned to the asset commit. Their hashes are in `files.sha256.json`; `region-crop-proof.json` independently rechecks the magenta-bounded pattern crop at zero pixel tolerance.

Cinnamon's visible replay: exact uploaded crop and useful private browser content.

![Cinnamon region crop](https://raw.githubusercontent.com/patrick-hudson/put.gd-desktop/060126db181e2fc8109728592d63ed914137afd2/docs/evidence/2026-10-30-tier1-blockers-299e4b29/cinnamon-visible/region-uploaded.png)
![Cinnamon browser](https://raw.githubusercontent.com/patrick-hudson/put.gd-desktop/060126db181e2fc8109728592d63ed914137afd2/docs/evidence/2026-10-30-tier1-blockers-299e4b29/cinnamon-visible/browser-content.png)

GNOME and KDE exact uploaded crops; corresponding full frozen patterns and receipts are beside each image.

![GNOME region crop](https://raw.githubusercontent.com/patrick-hudson/put.gd-desktop/060126db181e2fc8109728592d63ed914137afd2/docs/evidence/2026-10-30-tier1-blockers-299e4b29/gnome/region-uploaded.png)
![KDE region crop](https://raw.githubusercontent.com/patrick-hudson/put.gd-desktop/060126db181e2fc8109728592d63ed914137afd2/docs/evidence/2026-10-30-tier1-blockers-299e4b29/kde/region-uploaded.png)

Aurora's saved failure and passing browser content. The capture reaches terminal publication failure; no uploaded region result exists.

![Aurora KWin refusal](https://raw.githubusercontent.com/patrick-hudson/put.gd-desktop/060126db181e2fc8109728592d63ed914137afd2/docs/evidence/2026-10-30-tier1-blockers-299e4b29/aurora/region-permission-failed.png)
![Aurora browser](https://raw.githubusercontent.com/patrick-hudson/put.gd-desktop/060126db181e2fc8109728592d63ed914137afd2/docs/evidence/2026-10-30-tier1-blockers-299e4b29/aurora/browser-content.png)
