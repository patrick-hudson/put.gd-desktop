# Tier 1 Phase B: follow-up 5 closeout attempt

**KDE S3 now passes. GNOME S3 remains F because a shell-banner obstruction tripped the pattern guard before capture or upload.** Exactly one S3 recipe attempt ran per requested desktop, with the unchanged corrected guest-GET verifier. The other four same-build P results are retained. Full six-cell acceptance is still incomplete; no app upload defect was isolated by these runs.

| Desktop | S3 | Proton |
| --- | --- | --- |
| GNOME Wayland / d69 | F, follow-up 5, pattern guard | P, follow-up 3 |
| KDE Wayland / f44 | P, follow-up 5 | P, follow-up 4 |
| Cinnamon X11 / d69 | P, follow-up 4 | P, follow-up 4 |

| Proof | New follow-up 5 | Entire Phase B cumulative |
| --- | ---: | ---: |
| Uploads | 1 | 23 |
| Verified remote hashes | 1 | 13 |
| S3 public guest GET 200 + matching original hash | 1 | 2 |
| Proton Firefox original-file hashes | 0 | 3 |
| History deletions | 1 | 10 |
| History revocations | 0 | 13 |
| Authenticated S3 origin GET 404 | 1 | 10 |
| Fresh unavailable browser observations | 0 | 12 |
| Missing-key HEAD 404 before S3 import/upload | 2 | 12 |
| Batons lent / returned | 0 / 0 | 24 / 24 |
| Retired disposable overlays | 2 | 15 |

Cumulative counts include historical failed proof attempts and cleanup continuations; they do not imply every earlier row passed. Historical unavailable-browser observations include earlier public-serving failures. Revocation and deletion proof is recorded as counts only. Reused results are not added to cumulative counts a second time. The five currently passing cells prove five remote hashes, two S3 public hashes, three Proton original-file hashes, two deletions, three revocations, two origin-404 and three fresh unavailable Proton pages.

The [GNOME failure receipt](../followup-5/gnome/s3a/receipt.json) records zero capture/upload/hash/delete actions. One read-only missing-key HEAD returned 404, and its one-time S3 bridge entry was cleaned during teardown. The [precredential desktop](../followup-5/gnome/setup/gno-ready.png) visibly contains a Software Updater banner; it should have been dismissed before launching this attempt. The [failure diagnostic](../followup-5/gnome/s3a/pattern-occlusion.json) reports outside-pattern obstruction bounds [376,0,903,113], consistent with a GNOME shell banner. Its window list contains only Desktop Icons 1 (gjs, PID 2414, stack 0) and the owned fullscreen fixture (python3, PID 6890, stack 1), both [0,0,1280,800]; the chosen pattern rectangle is [320,240,640,320]. The exact banner content at failure was not retained unmasked. A masked failure frame is retained beside the diagnostic. No guard was weakened. This distinct failure is #997; no second attempt was made under the explicit one-attempt limit.

The [KDE receipt](../followup-5/kde/s3a/receipt.json) proves real mouse region input and a saved PNG matching the independently frozen crop pixel for pixel, with zero tolerance. Authenticated origin HEAD/GET verified length and SHA-256. The exact copied public link rendered in guarded Firefox, and a bounded independent guest GET returned an unredirected HTTP 200 with matching original bytes/SHA-256. The working URL stayed in the guest RAM process. The resolved app profile used `public-read`, the CloudFront host and a fresh isolated `acceptance/tier1-live/<uuid>/` prefix.

History deleted the exact owned KDE object and authenticated origin GET returned 404. Fresh CloudFront navigation afterward observed cached 200, informational; the subsequent saved-frame focus lookup encountered a title-space normalization mismatch, also informational. The origin-404 proof is decisive. No bucket policy, ownership, versioning, Block Public Access setting or existing user object was changed. #996's guest-GET correction is now verified on KDE and Cinnamon; GNOME did not reach the recipient phase in this attempt.

The retained proof sources are [Cinnamon S3](../followup-4/cinnamon/s3a/receipt.json), [GNOME Proton](../followup-3/gnome/protona/recipient-proof.json), [KDE Proton](../followup-4/kde/protona/recipient-proof.json) and [Cinnamon Proton](../followup-4/cinnamon/protona/recipient-proof.json). All Proton rows prove real completed Firefox original-file hashes, exact Download-list states and zero-tolerance decoded-preview matches, without P-preview. Their History revocations and fresh unavailable recipient pages passed.

All final-table cells use published **v26.1030.3203-nightly**, clean app commit **cc9d9410d6e982dd93d58bcfda82f112c44a82f6**, matching catalog CI **41111404666**. The new Ubuntu 26.04 DEB SHA-256 is **adfe9f221271b936b33dccd32b073da7b791745911a36e609c7abf22ab652aee**. Exact installation, guest and golden identities are retained in each new desktop's `setup/` directory. [Parameters](../followup-5/parameters.json) and [driver source hashes](../followup-5/driver-source.json) identify executed head **8a7ea94eec76c77a1a7a1da08c3a8eb38de282bc**, unchanged implementation **4906883475816701ba5a697d96c997273eeda884**. No app or broker was compiled.

The unchanged #726 gate enforced builder-guest placement, exact build identity, pinned secret states/classifier, OCR self-test and full masking. Key set 2 stayed in builder memory and entered each guest only through the bridge. No Proton session/check/lend/account action was used in follow-up 5 and no Proton lock was taken. Previously returned same-builder batons and passing source checks remain in their original proof directories. No account was signed in, signed out or created.

App metadata, Firefox profiles, temporary files and download fallbacks stayed in guest RAM. Browser handlers and clipboard leases were restored, owned RAM roots removed, empty Downloads fallbacks restored and both S3 bridge entries cleaned. Socket lengths were checked before app launch. Both guests were visibly unlocked and kept on the wall; saved masked visuals remain beside every receipt. Fully masked frames are not presented as visible-content proof.

Both overlays were exported and every member hash verified on the workstation, then retired through guarded completion manifests. Their `lifecycle/` directories retain retirement receipts. [d69](../followup-5/d69-lock-release.json) and [f44](../followup-5/f44-lock-release.json) desktop/guest locks were released with zero new host items; owned tunnels/tiles removed. Goldens, host evidence and existing desktop work were preserved. No owned background job remains.

The driver did not change in this closeout. Retained [tooling results](../followup-4/tests/summary.json) include 84 passed / 6 skipped across 90 builder cases, 18 passed / 1 skipped across 19 focused cases and five separately passing Git pin/classifier provenance cases. The Qt decoder skip is covered by the prepared-guest test and actual Proton comparisons. These unit checks do not substitute for desktop proof.

[coverage.json](coverage.json), the saved follow-up 5 coverage snapshot, has 36 guest entries: **30 passed, 6 failed**. The [canonical schema-v2 coverage](../../../../tools/lab/surface_map/coverage/2026-10-30-surface-driver-tier1-live.json) has since been updated by follow-up 6 and differs from this snapshot (verified by SHA-256). Only the 12 GNOME/KDE S3 entries were replaced; other same-build P results remain. [Summary](summary.json) separates new and cumulative counts. The previous [follow-up 4 report and indices](../followup-4-final/README.md), earlier failures and cleanup continuations are preserved unchanged. No issue was closed.

The combined [scan](scan.json), [mask index](frame-masks.json) and [SHA-256 index](sha256.json) cover current and historical evidence with no working URL, token, credential or session snapshot. Private logs and bridge receipts remain on d69/f44 under t1r6e/t1r6export and in the ignored workstation work/tier1b archive. Only disposable overlays were retired.
