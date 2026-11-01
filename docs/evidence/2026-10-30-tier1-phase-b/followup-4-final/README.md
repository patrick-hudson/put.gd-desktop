# Tier 1 Phase B: follow-up 4

**Cinnamon S3 passes with the default public-read ACL. Proton passes on all three desktops, with GNOME reused as authorized. GNOME and KDE S3 remain F for a recipient-verifier failure.** The ACL correction fixes public serving; no AWS policy, ownership, versioning or Block Public Access setting was changed. No app defect was isolated.

| Desktop | S3 | Proton |
| --- | --- | --- |
| GNOME Wayland / d69 | F | P, reused follow-up 3 |
| KDE Wayland / f44 | F | P |
| Cinnamon X11 / d69 | P | P |

Exactly one new attempt per requested destination ran: GNOME S3, KDE S3/Proton, Cinnamon S3/Proton. There were no additional uploads or complete reruns. The existing GNOME Proton original-byte proof uses the same exact published app build. No P-preview exception was needed.

| Proof | New this follow-up | Including reused GNOME Proton |
| --- | ---: | ---: |
| Uploads | 5 | 6 |
| Verified remote hashes | 5 | 6 |
| S3 public guest GET 200 + matching hash | 1 | 1 |
| Proton Firefox original-file hashes | 2 | 3 |
| History deletions | 3 | 3 |
| History revocations | 2 | 3 |
| S3 origin GET 404 | 3 | 3 |
| Fresh Proton unavailable-page proofs | 2 | 3 |

Revocation and deletion proof is recorded as counts only. All three S3 post-deletion browser observations were HTTP 200 from CloudFront cache. Those are informational; authenticated origin GET 404 is the deletion proof. Missing-key HEAD returned 404 before each S3 import/upload. All actual uploads were cleaned through History, including failed proof rows.

The fixture now combines `aclMode=public-read`, the CloudFront public base and a fresh isolated `acceptance/tier1-live/<uuid>/` prefix. The running app profile is read back before capture. All three S3 uploads passed the independent authenticated origin length/hash checks and loaded the copied image in guarded Firefox. GNOME and KDE then failed the older image-document page-script fetch/crypto proof before the new guest GET was reached. They remain F, with public hashes unproven, under driver issue #996; they are not silently promoted based on a different desktop.

The small verifier correction used on Cinnamon performs the requested independent bounded guest HTTP GET alongside the verified Firefox image load. It requires the acceptance URL shape, no redirect, HTTP 200 and exact saved bytes/SHA-256. Its working URL stays in the guest RAM process. Cinnamon's [S3 receipt](../followup-4/cinnamon/s3a/receipt.json) records both positive checks, real region input and zero-tolerance pattern-crop equality. The [GNOME](../followup-4/gnome/s3a/receipt.json) and [KDE](../followup-4/kde/s3a/receipt.json) failure receipts retain the missing-proof context and cleanup. #992's private-object fixture cause is superseded by this public-read result; the separate verifier failure remains explicit.

[KDE](../followup-4/kde/protona/recipient-proof.json) and [Cinnamon](../followup-4/cinnamon/protona/recipient-proof.json) retain Firefox's exact Download-list states, real target, completion/error/blocked/partial flags and decoded-preview comparison. The first Download click opens a confirmation; the real confirmation is clicked only after observing the list. Both actual completed originals match the saved PNG in guest RAM. GNOME's [reused proof](../followup-3/gnome/protona/recipient-proof.json) already proves the same behavior. Source URLs and arbitrary error messages are excluded. Proton preview comparisons are decoded RGBA at 600×280 with zero tolerance; original-file hashes, rather than preview exceptions, establish these P rows.

Every run installed published **v26.1030.3203-nightly**, clean app commit **cc9d9410d6e982dd93d58bcfda82f112c44a82f6**, matching catalog CI **41111404666**. Installation receipts and guest/golden identities are in each desktop's `setup/` directory. [Parameters](../followup-4/parameters.json) and [driver source hashes](../followup-4/driver-source.json) distinguish executed driver **692df12c** on GNOME/KDE from **49068834** on Cinnamon. The final correction was not rerun on GNOME/KDE, honoring the single-pass budget. No app or broker was compiled.

The unchanged #726 gate enforced builder-guest placement, exact build identity, pinned secret states/classifier, OCR self-test and full masking. Key set 2 stayed in builder memory and entered only through the bridge. Each Proton lend used its own builder's session under that builder's Proton lock, with source checks before and after. No account was signed in, signed out or created. No working URL, token, credential or session snapshot is in these artifacts.

App metadata, Firefox state, temporary files and download destinations stayed in guest RAM. Guest-only Downloads fallbacks were restored, browser handlers and clipboard leases were restored, and socket paths were checked before launch. Mint Update Manager autostart and exact owned applet PIDs were suppressed only in Cinnamon's overlay; its pre-run desktop was visibly clear. Each guest remained on the wall. Masked visuals remain beside every receipt; fully masked frames are not presented as visible-content proof. Private [preparation corrections](../followup-4/preparation-corrections.json) added no live attempt.

Both new Proton batons returned with source checks passing and guest entries cleaned. All three S3 bridge entries were cleaned. The desktop setup cleanup receipts, baton receipts, export/member-hash confirmations and guarded retirement manifests are under [followup-4](../followup-4). All three overlays were retired after verified export. The [d69](../followup-4/d69-lock-release.json) and [f44](../followup-4/f44-lock-release.json) desktop and guest locks were released with zero new host items, Proton locks free, and owned tunnels/tiles removed. Goldens and existing desktop work were preserved. No owned background job remains.

Validation: [tooling results](../followup-4/tests/summary.json) record 84 passed / 6 skipped across 90 builder cases on the initial driver, and 18 passed / 1 skipped across 19 final focused cases. The five Git pin/classifier provenance cases skipped in the isolated builder export passed separately in the clean owned worktree (5/5, 0.22 seconds). The Qt image decoder skip reflects the builder's absent PyQt6 and remains covered by the previous prepared-guest decoder test plus the actual Proton comparisons. These tests do not substitute for desktop proof.

The [canonical schema-v2 coverage](../../../../tools/lab/surface_map/coverage/2026-10-30-surface-driver-tier1-live.json) and [coverage.json](coverage.json), the saved follow-up 4 coverage snapshot (it differs from the canonical file, verified by SHA-256), each retain 36 guest entries: five new recipe results and the explicitly reused GNOME Proton result. [Summary](summary.json) separates new, reused and cumulative counts. The preceding [report and indices](../followup-3-final/README.md), all historical attempts, failure context and cleanup continuations are preserved. No issue was closed.

The combined [scan](scan.json), [mask index](frame-masks.json) and [SHA-256 index](sha256.json) cover all current and historical evidence. Private complete logs and bridge receipts remain on d69/f44 under t1r5e/t1r5export and in the ignored workstation work/tier1b archive. Only disposable overlays were retired.
