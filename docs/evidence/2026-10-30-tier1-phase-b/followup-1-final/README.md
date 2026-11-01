# Tier 1 Phase B: live S3 and Proton

**Not accepted.** The corrected parser selects the real AWS key set 2. AWS nevertheless denied actual `PutObject` requests with HTTP 403, with both `public-read` and omitted ACLs. Proton share pages rendered, but Firefox did not produce recipient downloads. No remote hash is claimed.

The coordinator follow-up began on October 30 CDT and continued into October 31. The original campaign remains preserved separately; its diagnosis that the authorized AWS key was invalid was wrong. That parser selected the Cloudflare R2 labelled block instead.

| Desktop | Builder / fresh overlay | S3 | Proton | Rerun finding |
| --- | --- | --- | --- | --- |
| Mint 22.3 Cinnamon X11 | d69 / rc1 | F | F | Earlier Put was denied; final attempts stopped at the pattern-guard limit despite overlay autostart suppression and termination of the exact delayed Update Manager PID. Proton upload was revoked through History; recipient bytes remain unverified. |
| Ubuntu 26.04 GNOME Wayland | d69 / rg1 | B | F | AWS denied Put with ACL omitted. Both Proton attempts uploaded and were revoked; fresh Firefox navigation proved each link unavailable, but neither download completed. |
| Kubuntu 26.04 KDE Wayland | f44 / rk1 | B | F | AWS denied Put with either ACL mode. Both Proton uploads were revoked, including an exact-row continuation; recipient downloads failed. |

B denotes an authorization prerequisite; original failed receipts are unchanged. Aurora and recording were optional and were not added after these failures and retry limits.

| Proof | Initial campaign | Follow-up rerun | Cumulative |
| --- | ---: | ---: | ---: |
| Completed uploads | 2 | 5 | 7 |
| Verified remote hashes | 0 | 0 | 0 |
| UI-confirmed revocations | 2 | 5 | 7 |
| UI-confirmed deletions | 0 | 0 | 0 |
| Fresh unavailable-link proofs | 0 | 2 | 2 |
| Confirmed baton carry-backs / cleaned guest entries | 6 / 6 | 12 / 12 | 18 / 18 |
| Retired overlays | 3 | 3 | 6 |

Four refused S3 keys in the rerun were independently confirmed absent with authenticated GET before failed History/cache and bridge entries were cleared. These were definite `upload_put` / `AccessDenied` / HTTP 403 refusals, not successful uploads. No bucket policy, ACL, versioning or existing user object was changed. Revoked Proton files remain private in their isolated test folders. Revocation and deletion proof is reported as counts only.

The canonical [schema-v2 coverage](../../../../tools/lab/surface_map/coverage/2026-10-30-surface-driver-tier1-live.json) has 36 guest-placement entries. [coverage.json](coverage.json) is its byte-identical audited copy. [summary.json](summary.json) separates rerun and cumulative counts. The original [coverage](../initial-campaign/coverage.json), [README](../initial-campaign/README.md), [summary](../initial-campaign/summary.json) and audit files are preserved as historical snapshots; the original bad-key interpretation is superseded here and in #973.

Every guest installed **v26.1030.3203-nightly**, clean app commit **cc9d9410d6e982dd93d58bcfda82f112c44a82f6**, with its matching catalog from CI run **41111404666**. No app or broker was compiled. The same published native DEBs were used throughout:

| Guests | Asset | SHA-256 |
| --- | --- | --- |
| Cinnamon | putgd-26.1030.3203-nightly-ubuntu24.04-x86_64.deb | 2754650eb3c40d139630ff969bfc345d3ae182b457e8e660f7db73afff0a3cbe |
| GNOME, KDE | putgd-26.1030.3203-nightly-ubuntu26.04-x86_64.deb | adfe9f221271b936b33dccd32b073da7b791745911a36e609c7abf22ab652aee |

Exact guest identities, immutable golden checksums, ports, package installation, target sessions, observed running builds and socket paths are retained:

- Cinnamon: [identity](../rerun/cinnamon/setup/run-identity.json), [refused Put](../rerun/cinnamon/s3b/receipt.json), [exact-key reconciliation](../rerun/cinnamon/s3-reconcile/receipt.json), [final pattern failure](../rerun/cinnamon/s3d/receipt.json), [Proton capture](../rerun/cinnamon/protona/receipt.json), [successful UI revocation continuation](../rerun/cinnamon/qt64/receipt.json).
- GNOME: [identity](../rerun/gnome/setup/run-identity.json), [S3 refusal and absence](../rerun/gnome/s3a/receipt.json), [first Proton attempt](../rerun/gnome/protona/receipt.json), [final Proton attempt](../rerun/gnome/protonb/receipt.json).
- KDE: [identity](../rerun/kde/setup/run-identity.json), [first real Put refusal](../rerun/kde/s3b/receipt.json), [exact-key reconciliation](../rerun/kde/s3-reconcile/receipt.json), [ACL-omit refusal and absence](../rerun/kde/s3d/receipt.json), [first Proton attempt](../rerun/kde/protona/receipt.json), [second upload](../rerun/kde/protonc/receipt.json), [exact-row continuation](../rerun/kde/post/receipt.json).

The two saved GNOME negative-browser visuals are [first](../rerun/gnome/protona/frames/outcomes_live.proton_region_upload/06-browser-link-unavailable.png) and [final](../rerun/gnome/protonb/frames/outcomes_live.proton_region_upload/06-browser-link-unavailable.png). Their target observations require the expected host, a complete document, unavailable/missing text and no decrypted media. All other saved visuals, including failures, remain beside their receipts with mask provenance. Fully masked frames are not presented as browser-content proof. Two preflight metadata entries have no exported startup PNG; the aggregate index lists existing frames only. Retained synthetic PNGs are byte-identical to their saved-file hashes.

The final driver is **32ab5d38312e25a9f3cbf2bd8d08e0e9310eee9f**; [source hashes](../rerun/driver-source.json) identify its files. It adds a strict single bare AWS-pair parser, explicit app keychain unlock, exact owned alert-title/role confirmation, Qt 6.4 caption binding, bounded recipient observation/download and guest-side original-byte comparison, S3 image hashing in Firefox, fresh negative navigation, and one frame-capture timeout retry. S3 refused-Put reconciliation requires both the exact new key and authenticated GET 404. Positive S3/browser hash paths remain unproven live. [Controllers](../rerun/controllers) retain reproducible setup and continuation parameters. The reconciliation controller remains in the private source archive, indexed by its [checksum](../rerun/controllers/private-source-index.json), because its SDK argument syntax triggers the conservative evidence scanner.

The unchanged #726 gate enforced builder guest placement, the exact 12+ hex app identity, build-pinned secret states/classifier, real OCR self-test and masking. App metadata and guarded Firefox profiles used guest tmpfs; no working share URL, token, credential or session snapshot is included here. Browser handlers and clipboard leases were restored. The wall exposed every guest during graphical work, and owned guest tiles/tunnels were removed after retirement. Socket lengths were checked before app launch; no socket ownership or artifact reserve check was weakened.

Tooling checks: **237 passed, 5 skipped across 242 cases**, followed by **73 passing focused tests on the final browser code**. Fedora's font/browser suite also passed **137 with 5 skips**. [Logs and hashes](../rerun/tests/summary.json) are retained. A transient parameter-name error in the new browser preference was caught by focused tests and corrected before GNOME browser setup. Earlier failed unit logs remain in the private archive. These tooling checks do not establish desktop acceptance.

Findings remain open:

- **#973:** wrong labelled R2 section selected; corrected to exactly one bare AWS pair before Backblaze/Cloudflare, with bucket/region from set 1. No alternate key was substituted.
- **#974:** original small runtime filesystem hit the app reserve; corrected with guest `/dev/shm`, preserving the reserve.
- **#975:** Cinnamon pattern guard still failed after the overlay-only Update Manager actions; stopped at the retry limit.
- **#977:** exact owned QMessageBox alert confirmation and Qt 6.4 caption binding were exercised successfully.
- **#978:** recipient pages rendered but bounded real downloads produced no file. The final GNOME rows completed UI revocation and independent unavailable-page proof despite this failure. An app defect has not been isolated from browser/download interoperability.
- **#979:** Fedora font-path portability fixed without weakening production OCR/masking.
- **#986:** unnecessary GetBucketLocation preflight required an unavailable permission; removed in favor of the explicit shared region.
- **#987:** bridged S3 keychain reference needed explicit app unlock; fixed. The unrelated synthetic/multipart setup probe was removed.
- **#988:** actual PutObject authorization denied, including with ACL omitted; exact refused keys confirmed absent. This is separate from #973.
- **#989:** browser setup parameter-name error caught by unit tests and corrected.

All twelve rerun lends have confirmed source replacement and guest-entry cleanup: [Cinnamon](../rerun/cinnamon/batons.json), [GNOME](../rerun/gnome/batons.json), [KDE](../rerun/kde/batons.json). Subsequent source `check` calls passed on both builders: [d69](../rerun/gnome/setup/gno-proton-final-check-b.json), [f44](../rerun/kde/setup/kde-post-final-check.json). No account was signed in, signed out or created. All seven S3 bridge imports have confirmed cleanup: [d69 counts](../rerun/d69-credential-counts.json), [f44 counts](../rerun/f44-credential-counts.json).

Every export passed its scan and every member hash was verified before guarded overlay retirement: [Cinnamon](../rerun/cinnamon/lifecycle/cin-retirement.json), [GNOME](../rerun/gnome/lifecycle/gno-retirement.json), [KDE](../rerun/kde/lifecycle/kde-retirement.json). Completion manifests retain failed outcomes. Goldens were preserved. Desktop and guest slot locks were released after cleanup: [d69](../rerun/d69-lock-release.json), [f44](../rerun/f44-lock-release.json). Both Proton locks were free; existing desktop work was preserved.

The aggregate [scan](scan.json), [frame masks](frame-masks.json) and [SHA-256 index](sha256.json) cover this combined evidence tree. Original audit snapshots retain their original relative paths. Full private logs, executed source checkpoints and bridge receipts remain on the builders under `/home/patrick/t1e`, `/home/patrick/t1r2e`, `/home/patrick/t1export`, `/home/patrick/t1r2export` and `/home/patrick/t1b`, and in the workstation's ignored `work/tier1b` archive. Only disposable overlay disks were retired.
