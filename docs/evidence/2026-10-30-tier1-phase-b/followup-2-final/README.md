# Tier 1 Phase B: live S3 and Proton

**Not accepted.** Correcting the fixture prefix made the real S3 origin uploads work. Authenticated GET hashes match the saved files, but copied public links return HTTP 403 in Firefox before deletion. Proton renders decrypted previews, while its real Download action produces no completed file within the bounded wait. No Proton remote hash is claimed.

| Desktop | Builder / guest placement | S3 | Proton |
| --- | --- | --- | --- |
| Cinnamon | d69 | F | F |
| GNOME | d69 | F | F |
| KDE | f44 | F | F |

P means the whole live recipe passed; F means a required proof failed; B means a prerequisite blocked execution. Aurora and recording were optional and were not added while the required recipient checks failed. All three required desktops used fresh builder overlays and the published **v26.1030.3203-nightly**, clean app **cc9d9410d6e982dd93d58bcfda82f112c44a82f6**, with matching catalog CI **41111404666**. No app or broker was compiled. [Published asset checksums](../followup-2/assets.json) were rechecked on d69.

| Proof | Follow-up 2 | Cumulative across campaigns |
| --- | ---: | ---: |
| Completed uploads | 8 | 15 |
| Verified remote hashes | 5 | 5 |
| UI revocations | 3 | 10 |
| UI deletions | 5 | 5 |
| Fresh unavailable browser observations | 6 | 8 |
| Confirmed baton carry-backs | 3 | 21 |
| Retired overlays | 3 | 9 |

This rerun also has **5 authenticated S3 origin GET 404 proofs** and **6 missing-key HEAD 404 preflights**. Public-serving status is separate from origin absence: the final S3 browser rows observed HTTP 403 before and after deletion. Those fresh 403 responses are recorded as unavailable observations; they do not prove that the public link worked before deletion. No cached CloudFront 200 is treated as an origin-deletion failure. Revocation and deletion proof is represented as counts only.

The [canonical schema-v2 coverage](../../../../tools/lab/surface_map/coverage/2026-10-30-surface-driver-tier1-live.json) has 36 guest-placement entries; [coverage.json](coverage.json) is the saved follow-up 2 coverage snapshot and differs from the canonical file (verified by SHA-256). [summary.json](summary.json) separates this rerun from cumulative counts. The preceding [final report](../followup-1-final/README.md), [coverage](../followup-1-final/coverage.json), [summary](../followup-1-final/summary.json) and audit indices remain unchanged historical snapshots. The initial campaign and first rerun raw receipts are preserved in their original directories. The earlier invalid-key and IAM-policy diagnoses are superseded by the parser and object-prefix findings below.

Current receipts and lifecycle proof:

- Cinnamon: [identity](../followup-2/cinnamon/setup/run-identity.json), [pattern-only pass](../followup-2/cinnamon/pattern/receipt.json), [first S3 selector refusal](../followup-2/cinnamon/s3a/receipt.json), [cancelled-before-upload reconciliation](../followup-2/cinnamon/s3-cancel/receipt.json), [final S3 row](../followup-2/cinnamon/s3b/receipt.json), [exact-row S3 deletion continuation](../followup-2/cinnamon/s3-delete2/receipt.json), [Proton row](../followup-2/cinnamon/protona/receipt.json), [baton](../followup-2/cinnamon/batons.json), [retirement](../followup-2/cinnamon/lifecycle/cin-retirement.json).
- GNOME: [identity](../followup-2/gnome/setup/run-identity.json), [final S3 row](../followup-2/gnome/s3b/receipt.json), [Proton row](../followup-2/gnome/protona/receipt.json), [baton](../followup-2/gnome/batons.json), [retirement](../followup-2/gnome/lifecycle/gno-retirement.json).
- KDE: [identity](../followup-2/kde/setup/run-identity.json), [final S3 row](../followup-2/kde/s3b/receipt.json), [Proton row](../followup-2/kde/protona/receipt.json), [baton](../followup-2/kde/batons.json), [retirement](../followup-2/kde/lifecycle/kde-retirement.json).

The original S3 attempts on GNOME and KDE retained their upload, authenticated hash, History deletion and origin-absence proofs. Their browser failure frame had navigated to about:blank, so their status-0 browser observations are invalid and are not counted as unavailable-link proofs. The second attempts preserve the URL only in Firefox RAM during a fully masked failure frame, then navigate afresh to the expected HTTPS provider. The positive observer's allowlisted booleans distinguish HTTP 403 from focus, naming, certificate and network errors.

Cinnamon's current pixel-exact pattern diagnostic passed after masking the overlay-only mintupdate autostart entry; no matching PID required termination at setup. No current pattern occlusion was observed. The first S3 attempt stopped before drag on owned selector geometry. Its exact cancelled operation had zero items and no history artifact, so the retained bridge entry and owned RAM root could be safely cleared. This cleanup was not an upload or remote deletion. The second S3 attempt is retained separately. The pattern guard remains unchanged; if it fails, diagnostics now include stacking, class, PID, frame geometry, a fully masked frame, difference bounds and video color count.

Proton used the same builder's session via secret_bridge, under its Proton lock, with source checks before and after lending. The regular guarded Firefox profile lived in guest tmpfs, with service workers and IndexedDB enabled, automatic downloads configured, and no privacy.* overrides. Each decrypted preview matched the expected recipient page; runtime diagnostics confirmed a service-worker API and no download error, while the bounded completion wait observed no completed recipient file. Actual UI revocation and a fresh unavailable-page check followed each failure. No sign-in, sign-out or account creation occurred; revoked files remain private in their isolated test folders.

The unchanged #726 gate enforced builder guest placement, exact 12+ hex app identity, build-pinned secret states/classifier, OCR self-test and masking. App metadata, clipboard leases and browser profiles used guest RAM. No working share URL, token, credential or session snapshot is in this tree. The wall exposed each guest during graphical work; owned tiles and tunnels were removed afterward. Socket paths were checked before launch; no socket-ownership or artifact-reserve guard was weakened.

Tooling: **164 passed, 5 skipped across 169 cases** on Fedora, including the live outcomes, browser, masking and gate suites, followed by **13 passing focused checks** on the final History binding. The [test log and summary](../followup-2/tests) are retained. [First source checkpoint](../followup-2/driver-source-first.json), [final source hashes](../followup-2/driver-source-final.json) and [controller parameters](../followup-2/controllers) identify the executed driver. The browser correction was exercised by both second S3 attempts. Unit checks do not establish desktop acceptance.

Findings remain open:

- **#973:** the old parser selected the labelled Cloudflare key; corrected to exactly one bare AWS pair before other-provider headings, with bucket/region from set 1.
- **#975:** current fresh-overlay pattern checks pass; historical failures remain preserved without inventing an obstruction diagnosis.
- **#977:** exact owned QMessageBox title/alert-role and Qt 6.4 caption binding continue to confirm Proton revocation.
- **#978:** recipient Download still yields no file with regular browsing and service workers enabled on all three desktops. An app defect has not been isolated from recipient-browser interoperability.
- **#979:** Fedora font-path portability remains covered without changing production OCR or masks.
- **#988:** our fixture used an unauthorized root prefix. New keys stay under acceptance/tier1-live/ with ACL omitted; successful uploads prove that correction. No IAM or bucket policy was changed.
- **#991:** failure-frame navigation discarded the recipient URL; corrected with full masking and RAM-only URL retention.
- **#992:** S3 public links return HTTP 403 despite successful authenticated origin reads. This is a public-serving/configuration finding; an app defect has not been established.
- **#994:** Qt 6.4 omits the S3 delete button object name; exact owned-caption binding completed the retained upload’s UI deletion, authenticated origin absence and fresh public status.
- **#995:** the first continuation bootstrap refused a retained closed clipboard identity. Only after verifying restoration, source hash and absent PID was that closed record archived and a new guarded lease started; exclusive ownership checks remain unchanged.
- **#993:** Cinnamon's first S3 attempt refused owned selector geometry before capture; exact no-publication cleanup is retained. No guessed coordinates or guard bypass was used.

Every exported member hash was verified before guarded overlay retirement. All 3 lends have confirmed source replacement and guest-entry cleanup, with subsequent source checks passing. All S3 bridge imports have confirmed cleanup. [d69](../followup-2/d69-lock-release.json) and [f44](../followup-2/f44-lock-release.json) desktop/guest locks were released with zero new host items; Proton locks were free. Goldens and existing desktop work were preserved.

The combined [scan](scan.json), [mask provenance](frame-masks.json) and [SHA-256 index](sha256.json) cover the whole evidence tree, including historical receipts. Full private logs and bridge receipts remain on the builders under t1r3e/t1r3export and in the ignored workstation work/tier1b archive. Only disposable overlay disks were retired.
