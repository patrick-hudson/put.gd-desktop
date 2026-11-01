# Tier 1 Phase B: live S3 and Proton, 2026-10-30

**Not accepted.** S3 is blocked by rejected key set 2 credentials. Proton recipient verification failed. Two real region captures completed publication and were revoked through History, but neither has a successful recipient download/hash or independent unavailable-link check. The campaign ran on October 30 CDT; UTC receipts extend into October 31.

| Desktop | Builder / fresh guest | S3 | Proton | Finding |
| --- | --- | --- | --- | --- |
| Mint 22.3 Cinnamon X11 | d69 / bc1 | B | F | S3 rejected key set 2; Proton stopped before publication on the disk reserve, then the independent pattern guard with Mint Update Manager present. |
| Ubuntu 26.04 GNOME Wayland | d69 / bg1 | B | F | Region pixels, saved file and completed upload observed; recipient Firefox timed out; History UI revocation completed. |
| Kubuntu 26.04 KDE Wayland | f44 / bk1 | B | F | Region pixels, saved file and completed upload observed; recipient Firefox timed out; a bounded continuation revoked the same upload through History. |

Aurora was optional and was not run. Recording was not added to these failing region rows.

| Proof | Count |
| --- | ---: |
| Completed live uploads | 2 |
| UI-confirmed revocations | 2 |
| UI-confirmed remote deletions | 0 |
| Independently verified remote hashes | 0 |
| Independently verified unavailable links | 0 |
| Session lends / confirmed carry-backs / cleaned guest entries | 6 / 6 / 6 |
| Retired overlays | 3 |

Revocation leaves the synthetic Proton files private in their isolated test folders. No working link, token, credential, or session snapshot is included in this evidence. Deletion and revocation proof is reported as counts.

The canonical schema-v2 coverage is [the guest coverage file](../../../../tools/lab/surface_map/coverage/2026-10-30-surface-driver-tier1-live.json). Its audited byte-identical copy is [coverage.json](coverage.json). The first Cinnamon S3 attempt recorded a generic SDK failure; consolidated coverage reports the identified credential prerequisite as B. Original attempt receipts remain unchanged.

Every guest installed **v26.1030.3203-nightly**, exact clean app commit **cc9d9410d6e982dd93d58bcfda82f112c44a82f6**. The matching ui-catalog came from CI run **41111404666**. No app or broker was compiled. Native DEBs and their release digests:

| Guests | Asset | SHA-256 |
| --- | --- | --- |
| Cinnamon | putgd-26.1030.3203-nightly-ubuntu24.04-x86_64.deb | 2754650eb3c40d139630ff969bfc345d3ae182b457e8e660f7db73afff0a3cbe |
| GNOME, KDE | putgd-26.1030.3203-nightly-ubuntu26.04-x86_64.deb | adfe9f221271b936b33dccd32b073da7b791745911a36e609c7abf22ab652aee |

The exact VM IDs, immutable golden paths/checksums, QEMU process identities, ports, target distro/session, observed running build, socket paths and functional failures are retained in each desktop's setup identity and receipts:

- Cinnamon: [identity](../cinnamon/setup/run-identity.json), [S3 attempt](../cinnamon/s3a/receipt.json), [first Proton attempt](../cinnamon/protona/receipt.json), [second Proton attempt](../cinnamon/protonb/receipt.json).
- GNOME: [identity](../gnome/setup/run-identity.json), [S3](../gnome/s3a/receipt.json), [Proton](../gnome/protona/receipt.json).
- KDE: [identity](../kde/setup/run-identity.json), [S3 prerequisite attempt](../kde/s3a/receipt.json), [S3 credential result](../kde/s3b/receipt.json), [first Proton attempt](../kde/protona/receipt.json), [publication attempt](../kde/protonb/receipt.json), [existing-upload continuation](../kde/post/receipt.json).

Saved visuals include [Cinnamon before live use](../cinnamon/setup/unlocked-before-live.png), [GNOME before live use](../gnome/setup/unlocked-before-live.png), [KDE before live use](../kde/setup/unlocked-before-live.png), [GNOME History after cleanup](../gnome/protona/frames/outcomes_live.proton_region_upload/04-history-after-remote-cleanup.png), and [KDE History after cleanup](../kde/post/frames/__main__.finish/02-history-after-remote-cleanup.png). All live failure frames and their mask provenance remain beside their receipts; fully masked frames are identified as such and are not browser-content proof. The two retained synthetic PNGs remain byte-identical to their saved-file hashes.

The live contexts passed the unmodified #726 guest-placement, exact-build catalog, build-pinned secret-state/classifier and real OCR self-test gates. Per-attempt scans and the desktop export scans passed. The final aggregate scan and file manifest cover the README, coverage, screenshots, test receipts and lifecycle evidence. App metadata and guarded Firefox profiles used guest tmpfs. Browser MIME handlers and clipboard leases were restored; the receipts retain their results. The wall exposed both builders' guests throughout graphical work; the owned guest tiles and tunnels were removed at retirement.

The initial runtime tmpfs was below the app's normal artifact disk reserve. Its failed operations were independently reconciled as zero items and zero History rows before cleanup. The corrected commands used fresh `/dev/shm` roots with the same app reserve and socket ownership checks. All complete app/QMP socket paths were checked before launch. GNOME's first launch also refused an unpinned SSH port before allocating an overlay; the corrected launch used existing pinned port 22222. Failed attempts remain retained.

Findings remain open:

- **#973:** key set 2 rejected with `InvalidAccessKeyId`; no S3 upload attempted. Key set 1 was not substituted.
- **#974:** initial RAM profile below the normal disk reserve; corrected without changing the app's reserve.
- **#975:** Cinnamon pattern guard failure with Mint Update Manager present; stopped at the retry limit.
- **#977:** driver required a QDialog role for Proton's QMessageBox confirmation. Exact owned-title resolution was exercised by the KDE continuation and GNOME revocation; the original failure is retained.
- **#978:** recipient Firefox/content verification failed on KDE and GNOME. Remote bytes and independent link unavailability remain unproven. An app defect has not been isolated from this failure.
- **#979:** unchanged banner-OCR unit test assumes Debian font locations and errors on Fedora. The production gate supports Fedora's font location and admitted the actual live context. The final Debian-controller suite passed.

The narrow recipe implementation reuses the Phase A capture oracle and the History/routing approach from draft #687. Final driver code is **5a3c11af98501ba2da13168736ea74fa49a1cf04**. Final ownership, payload-read and public-S3-link safeguards have unit coverage; these failed live attempts do not establish graphical acceptance of that final driver revision. A private checkpoint of the executed GNOME driver is indexed by [its file hashes](../gnome-executed-driver-sha256.json). The bounded KDE continuation source is [retained here](../controllers/kde-post.py); it creates no new upload.

Final controller checks: **234 passed, 5 skipped** across 239 cases, including the added live outcome fault tests, browser contracts, driver contracts, the #726 evidence suite and the existing Phase A pixel oracles. [Test summary](../tests/summary.json) and individual logs are retained. These are tooling checks, separate from the F/B desktop results.

All six batons have confirmed source replacement and guest-entry cleanup: [Cinnamon](../cinnamon/batons.json), [GNOME](../gnome/batons.json), [KDE](../kde/batons.json). Both builders' subsequent `check` calls passed, using their registered session-health apps; guest acceptance used the nightly identified above. No account was signed in, signed out or created.

Evidence was exported and every member hash verified before guarded retirement: [Cinnamon receipt](../cinnamon/lifecycle/retirement.json), [GNOME receipt](../gnome/lifecycle/retirement.json), [KDE receipt](../kde/lifecycle/retirement.json). Completion manifests retain failed outcomes. Immutable goldens remain intact. Guest slot locks were removed and desktop cleanup checks passed before release: [d69](../d69-lock-release.json), [f44](../f44-lock-release.json). Both Proton locks were free after carry-back. Existing desktop work was preserved.

Full private setup/attempt logs, original cold evidence and controller sources remain under each builder's `/home/patrick/t1e`, `/home/patrick/t1export` and `/home/patrick/t1b`, and the workstation's `work/tier1b`. Only disposable overlay disks were retired.
