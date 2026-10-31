# Tier 1 Phase B: follow-up 3

**GNOME: S3 F, Proton P.** The Proton recipient download now completes and matches the saved PNG's original bytes. Its first Download click opened a confirmation; Firefox's own list was empty until the real confirmation was clicked. S3's running profile and copied link both use CloudFront, but the uploaded public object still returns HTTP 403. Full Phase B acceptance remains blocked.

| Desktop | Follow-up 3 S3 | Follow-up 3 Proton |
| --- | --- | --- |
| GNOME Wayland / d69 | F | P |
| KDE Wayland / f44 | Not rerun | Not rerun |
| Cinnamon X11 / d69 | Not rerun | Not rerun |

Exactly one GNOME attempt per destination ran. Expansion required both GNOME rows to pass, so KDE and Cinnamon retain their previous results. No P-preview exception was needed: Proton's original-file hash is proven.

| Proof | This follow-up |
| --- | ---: |
| Uploads | 2 |
| Verified remote hashes | 2 |
| History deletions | 1 |
| History revocations | 1 |
| S3 authenticated origin GET 404 | 1 |
| Proton fresh unavailable-page proof | 1 |
| Fresh public CloudFront status after deletion | 403, informational |
| Baton carried back / guest entry cleaned | 1 / 1 |
| Retired overlays | 1 |

S3's hash is an authenticated origin GET; Proton's is the actual Firefox download. CloudFront never returned the required public 200, so its public hash is not claimed. Its post-deletion status is informational and a cached 200 would not fail the row: origin GET 404 is the deletion proof. Revocation and deletion proof is recorded as counts only.

The [Proton recipient proof](followup-3/gnome/protona/recipient-proof.json) retains Firefox's exact Download-list states and decoded-preview comparison. Immediately after the first trusted click, the list was empty. After clicking the unique visible Download confirmation, it reported `succeeded=true`, `stopped=true`, `canceled=false`, `error=null`, no blocked or partial data, and 1491/1491 bytes at its configured guest-RAM target. No `.part` file remained. The original downloaded bytes and SHA-256 matched the saved file. The preview also matched at 600×280, no scaling and zero pixel tolerance. Source URLs and arbitrary error messages were excluded from Download-list evidence.

The S3 [receipt](followup-3/gnome/s3a/receipt.json) records the resolved CloudFront-base assertion and independent origin hash. A separate bounded [guest public GET](followup-3/gnome/setup/gno-s3-public-get-observation.json) read the exact uploaded receipt in RAM, verified that it uses CloudFront, and observed HTTP 403. The browser independently observed the expected host/name, focus/fullscreen readiness and HTTP 403, with no network or certificate error. The fixture already specified the CloudFront base; this rerun rejects direct S3 links instead of silently accepting them. No policy, ACL, versioning or existing user object was changed. S3 keys remain under the isolated acceptance prefix, with ACL omitted and missing-key HEAD 404 before import/upload.

Both [S3](followup-3/gnome/s3a/receipt.json) and [Proton](followup-3/gnome/protona/receipt.json) used real region input and matched the independently frozen pattern crop pixel for pixel. Each copied link was opened in guarded Firefox. History performed cleanup, followed by origin absence or a fresh unavailable recipient page. All saved visuals remain beside their receipts with mask provenance. Fully masked frames are not presented as visible-content proof.

Every run installed published **v26.1030.3203-nightly**, clean app commit **cc9d9410d6e982dd93d58bcfda82f112c44a82f6**, with matching catalog CI **41111404666**. The installed Ubuntu 26.04 DEB SHA-256 is **adfe9f221271b936b33dccd32b073da7b791745911a36e609c7abf22ab652aee**. [Installation](followup-3/gnome/setup/gno-install.json), [guest identity](followup-3/gnome/setup/run-identity.json), [parameters](followup-3/parameters.json) and [driver source hashes](followup-3/driver-source.json) identify the exact run. No app or broker was compiled. The executed implementation was 8c125ebc80a5beffc70fd090bf651e8534a2997b; final safety checks also ensure a completed file with the wrong hash can never become P-preview.

The unchanged #726 gate enforced builder-guest placement, exact build identity, pinned secret states/classifier, OCR self-test and masking. Key set 2 stayed in builder memory and entered the guest only through the bridge. Proton used the same builder's session through the bridge under its lock, with source checks before and after lending. No account was signed in, signed out or created.

App metadata, Firefox profiles, temporary files and download destinations stayed in guest RAM. The guest's empty Downloads fallback was temporarily redirected to owned RAM and restored afterward. No working URL, token, credential or session snapshot is in this evidence. Browser handlers and clipboard leases were restored. Socket paths were checked before app launch. The guest remained visible on the wall; the owned tile and tunnel were removed after retirement.

Validation: [tooling results](followup-3/tests/summary.json) include 79 passed / 1 skipped across 80 builder cases, 17 passed / 1 skipped across 18 final focused cases, and a passing real Qt PNG decoder test in the guest. The builder lacks PyQt6; the skipped image-decoder check was covered in the prepared guest. These checks do not substitute for desktop proof.

The [canonical schema-v2 coverage](../../../tools/lab/surface_map/coverage/2026-10-30-surface-driver-tier1-live.json) and [byte-identical copy](coverage.json) have 36 guest entries. Only GNOME was replaced in this follow-up; other desktop entries retain their historical failed results and are explicitly marked not rerun. [Summary](summary.json) separates this follow-up from cumulative counts. The [preceding report](followup-2-final/README.md), coverage, summary and audit indices are preserved unchanged. Earlier raw attempts, including failures and their cleanup continuations, remain in their original directories.

The [baton receipt](followup-3/gnome/batons.json) confirms carry-back and guest-entry cleanup; the [final source check](followup-3/gnome/setup/gno-proton-final-check-a.json) passed. The S3 [credential cleanup count](followup-3/gnome/credential-counts.json) is confirmed. [RAM cleanup](followup-3/gnome/setup/gno-ram-cleanup.json) and [retirement](followup-3/gnome/lifecycle/gno-retirement.json) passed after export/member-hash verification. The [desktop and guest locks](followup-3/d69-lock-release.json) were released with zero new host items, and the Proton lock was free. Goldens and existing desktop work were preserved. No owned background job remains.

Findings were updated without closing issues: #978 now has a full GNOME original-byte proof and the confirmed Download-menu cause; #992 still has a public CloudFront 403 despite the correct resolved base. The other desktops were not revalidated after this fix. P-preview remains a provisional, hash-unproven path requiring a matching decoded preview and exact Firefox Download-list state; it does not silently mark coverage passed, and it was not used here.

The combined [scan](scan.json), [mask index](frame-masks.json) and [SHA-256 index](sha256.json) cover current and historical evidence. Full private logs and bridge receipts remain on d69 under t1r4e/t1r4export and in the ignored workstation work/tier1b archive. Only the disposable overlay was retired.
