# Tier 1 Phase B: final required-desktop results

**All six required cells pass.** Follow-up 6 ran one GNOME/d69 S3 attempt after suppressing update nags only in its fresh guest overlay. The pattern guard, origin hash, public recipient hash and History deletion proofs all passed. The other five same-build P results are retained from their cited attempts. Aurora was outside this closeout.

| Desktop | S3 | Proton |
| --- | --- | --- |
| GNOME Wayland / d69 | P, follow-up 6 | P, follow-up 3 |
| KDE Wayland / f44 | P, follow-up 5 | P, follow-up 4 |
| Cinnamon X11 / d69 | P, follow-up 4 | P, follow-up 4 |

| Proof | New follow-up 6 | Final six-cell matrix | Entire Phase B cumulative |
| --- | ---: | ---: | ---: |
| Uploads | 1 | 6 | 24 |
| Verified remote hashes | 1 | 6 | 14 |
| S3 public guest GET 200 + matching original hash | 1 | 3 | 3 |
| Proton Firefox original-file hashes | 0 | 3 | 3 |
| History deletions | 1 | 3 | 11 |
| History revocations | 0 | 3 | 13 |
| Authenticated S3 origin GET 404 | 1 | 3 | 11 |
| Fresh unavailable browser observations | 0 | 3 | 12 |
| Batons lent / returned | 0 / 0 | 3 / 3 | 24 / 24 |
| Retired disposable overlays | 1 | 5 distinct proof overlays | 16 |

Cumulative counts include historical failed proof attempts and cleanup continuations; they do not imply every earlier attempt passed. Historical unavailable-browser observations include earlier public-serving failures. The final matrix's three fresh unavailable recipient-page proofs are Proton revocations; S3 deletion is independently established by origin GET 404. Revocation and deletion proof is recorded as counts only. Reused results are not added to cumulative counts a second time.

Before launching put.gd, the fresh GNOME guest masked the update-notifier and GNOME Software service user autostart entries, stopped only their exact matching owned PIDs, and set GNOME Software `download-updates` and `allow-updates` to false. It attempted the Shell banner dismissal, waited for expiry and disabled notification banners only in this disposable overlay to prevent a pending nag racing the pattern. The [preparation receipt](followup-6/gnome/setup/gno-update-nag-suppression.json) records masked names, false settings and zero remaining matching processes. The golden lacked the GNOME Software schema; preparation added common data and staged only the schema XML from a SHA-256-verified official Ubuntu DEB in owned guest RAM. GSETTINGS_SCHEMA_DIR selected that official schema for the two real dconf Boolean writes/readbacks; its settings cache stayed in RAM and was removed at cleanup. No GNOME Software executable, compositor or portal package was installed. The [schema provenance](followup-6/gnome/setup/gno-schema-package.json) records the bounded correction; initial stopped-PID values were not retained when the first preparation stopped at the missing schema. No live attempt was added. The [native desktop readback](followup-6/gnome/setup/gno-native-config-readback.json) confirms the actual GNOME session settings and masks. The [saved pre-run frame](followup-6/gnome/setup/gno-ready.png) was inspected and clear before the single live attempt. No golden or builder desktop setting was changed. The full pattern/foreground guards remained intact. #997's preceding failure is preserved.

The [new GNOME receipt](followup-6/gnome/s3a/receipt.json) proves real mouse region input, a saved PNG matching the independently frozen pattern crop pixel for pixel at zero tolerance, authenticated origin HEAD length/GET SHA-256, copied image loading in guarded Firefox, and a bounded independent guest GET returning an unredirected HTTP 200 with matching original bytes/SHA-256. The working URL stayed inside the guest RAM process. The running app profile was read back as the default `public-read` ACL, CloudFront host and exact fresh isolated `acceptance/tier1-live/<uuid>/` prefix. Missing-key HEAD returned 404 before import/upload.

History deleted the exact newly owned object; authenticated origin GET returned 404 afterward. Fresh post-deletion CloudFront navigation observed status **200**, informational because edge caching is independent of origin deletion. No bucket policy, ownership, versioning, Block Public Access setting or existing user object was changed. The public-read fixture correction and guest-GET verifier now have passing S3 proofs on all three required desktops.

The other final proof sources are [KDE S3](followup-5/kde/s3a/receipt.json), [Cinnamon S3](followup-4/cinnamon/s3a/receipt.json), [GNOME Proton](followup-3/gnome/protona/recipient-proof.json), [KDE Proton](followup-4/kde/protona/recipient-proof.json) and [Cinnamon Proton](followup-4/cinnamon/protona/recipient-proof.json). All Proton rows prove real completed Firefox original-file hashes, exact Download-list states and matching decoded-preview pixels with zero tolerance; none uses P-preview. Their History revocations and fresh unavailable recipient pages passed.

Every final-table cell uses published **v26.1030.3203-nightly**, clean app commit **cc9d9410d6e982dd93d58bcfda82f112c44a82f6**, matching catalog CI **41111404666**. The new Ubuntu 26.04 DEB SHA-256 is **adfe9f221271b936b33dccd32b073da7b791745911a36e609c7abf22ab652aee**. Installation and exact guest/golden identities are retained in the new desktop's `setup/` directory. [Parameters](followup-6/parameters.json) and [driver source hashes](followup-6/driver-source.json) identify executed head **3c6ff4feacebea84f40e818d25463b747daccd11**, unchanged implementation **4906883475816701ba5a697d96c997273eeda884**. No app or broker was compiled.

The unchanged #726 gate enforced builder-guest placement, exact build identity, pinned secret states/classifier, OCR self-test and full masking. Key set 2 stayed in builder memory and entered the guest only through the bridge. No Proton session/check/lend/account action was used in follow-up 6 and no Proton lock was taken. Previously returned same-builder batons and passing source checks remain in their original proof directories. No account was signed in, signed out or created.

App metadata, Firefox profiles, temporary files and download fallbacks stayed in guest RAM. Browser handlers and clipboard leases were restored, owned RAM roots removed, the empty Downloads fallback restored and the S3 bridge entry cleaned. Socket lengths were checked before app launch. The guest was visibly unlocked and kept on the display wall; saved masked visuals remain beside the receipt. Fully masked frames are not presented as visible-content proof.

The overlay was exported and every exported member hash verified on the workstation before guarded retirement. Its `lifecycle/` directory retains the retirement receipt. The [d69 desktop/guest locks](followup-6/d69-lock-release.json) were released with zero new host items; owned tunnel/tile removed. Goldens, host evidence and existing desktop work were preserved. No owned background job remains.

The driver did not change in this closeout. Retained [tooling results](followup-4/tests/summary.json) include 84 passed / 6 skipped across 90 builder cases, 18 passed / 1 skipped across 19 focused cases and five separately passing Git pin/classifier provenance cases. The Qt decoder skip is covered by the prepared-guest test and actual Proton comparisons. These unit checks do not substitute for desktop proof.

The [canonical schema-v2 coverage](../../../tools/lab/surface_map/coverage/2026-10-30-surface-driver-tier1-live.json) and [identical copy](coverage.json) have **36 passed guest entries**. Only the six GNOME S3 entries were replaced; the other 30 retain their same-build P results. [Summary](summary.json) separates new, accepted-matrix and cumulative counts. The previous [follow-up 5 report and indices](followup-5-final/README.md), earlier failures and cleanup continuations remain unchanged. Findings were updated without closing any issue. No app defect was isolated by this final run.

The combined [scan](scan.json), [mask index](frame-masks.json) and [SHA-256 index](sha256.json) cover current and historical evidence with no working URL, token, credential or session snapshot. Private complete logs and bridge receipts remain on d69 under t1r7e/t1r7export and in the ignored workstation work/tier1b archive. Only the disposable overlay was retired.
