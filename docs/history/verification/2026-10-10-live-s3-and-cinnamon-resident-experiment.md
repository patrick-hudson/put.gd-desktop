# Verification record

> **Historical record.** This is a dated run record, moved unchanged from [implementation evidence](../../testing/verification.md) on 2026-11-05 (#1222). It describes what that run found for the build it names, and its open or pending statements describe that time. "Above" and "below" refer to its neighbours on the old page, in the order of the [history index](../README.md#verification-run-records). For today, see the [current state](../../testing/verification.md#current-state). Do not use it as a work list.

### Live S3 and Cinnamon resident experiment — 2026-10-10

The [source55 live S3 record](https://github.com/patrick-hudson/put.gd-evidence/blob/874703b35562c732268c6fe0bc9b42614ffda928/records/2026-10-09-live-s3-cinnamon.json)
uses the clean `0.1.1-dev.55+g033b73fb58a3` resident, commit
`033b73fb58a38f853bd79b1902d88117e723a122`, in the Cinnamon/X11 VM.
One synthetic 30-byte clipboard text upload passed unsigned public-byte
verification and automatic URL paste into an independent native receiver.
The actual History **Copy link** button passed after a clipboard reset;
guarded deletion removed the exact owned test object and left its History
record marked Removed. Original settings were restored, the UI exited normally,
the owned fixture was cleaned up and the resident exited normally. Earlier
AccessDenied attempts remain failure evidence; they no longer describe the
outcome of this specific successful workflow. Follow
[testing-live-s3.md](../../testing/live-s3.md) with fresh synthetic object identities.

The single post-cloud diagnostic was 49.14 MiB RSS. It was neither cold nor a
frozen memory workload and does not establish post-upload memory acceptance.
This earlier text workflow did not prove cloud behavior of the later resident experiment, all content
types/collision/failure cases, GNOME/KDE remote history or MCP cloud parity.
No browser/CDN invalidation or credential-memory zeroization was tested.

The isolated clean resident experiment was `0.1.1-dev.57+g2a02118ffc7b`, commit
`2a02118ffc7bcf3f4643b83f83d5dd7594a8207f`. Cinnamon's implementation is unchanged
by its GNOME startup/argument corrections. It passed twelve native local
capture/clipboard cases, with Qt and GTK independent paste, plus current tray,
simple/advanced settings, native GTK folder dialog/font integration, transparent
live selection, History preview and Activity parent/item result-copy checks.
Each Activity copy oracle first replaced the clipboard with an unrelated image.
Root inspected all 21 actual current-version witnesses. The native functional
receipt is SHA256 `6dacb8b11072b61c0141dd24235139a707918b9de23a56828a4415cc3c05d981`.
Failed attempts remain preserved. Two early confirmed-icon tray gestures showed
no popup; attribution remains unresolved. A controlled gesture opened the native
menu and Settings, and the menu reopened after normal Settings close.

The authorized one-pair physical Cinnamon regression measured:

| Workload | Actual result |
| --- | --- |
| Region-selection timing | 113.61 ms p95 over 30 measured invocations; all 35 warmup/measured operations cancelled with no result |
| Cold idle | 24.62 MiB peak RSS over 300.19 seconds; measured CPU 0.0% of one core; one process throughout and adequate sample coverage |
| Three empty-view cycles | 25.19 MiB worst settled RSS / 16.64 MiB PSS; 51.53 MiB peak open PSS |
| Separate normal-exit supplement | Three actual normal-zero helper exits; 25.10 MiB worst settled RSS / 16.59 MiB PSS; 51.66 MiB peak open PSS |

The original view run omits the final generation's exit status. The separate
unchanged-workload supplement adds three read-only queries and proves all three
normal exits; it retains the original result and labels its observer noise.
Its 27 synthetic safety contracts also pass. After cleanup, a fresh physical
inventory found no owned app/helper/capture/fixture/benchmark or known observer
processes, and the serial slot was released.

The local resident expansion gate (`work/gnome-native-boundary/cinnamon-candidate57/physical/cinnamon-expansion-gate.json`, private and unavailable in this checkout)
has SHA256 `41041a41048f0cfb34d8c7ee514d6cb065c76c97b2d1f635c2f748b9b80123d3`;
root rehashed its 81 references. These raw receipts remain local work artifacts.
This establishes the approximately 40 MiB Cinnamon cold/empty-view regression
target and its timing guard, with native fonts/dialogs preserved. It does not
establish post-S3, post-capture or populated-history retained-memory bounds,
a permanent plateau, mixed-display acceptance, the complete agent workflow and release acceptance, or that experiment's
GNOME/KDE acceptance. GNOME comparison/confirmation and KDE work remain separate;
their memory failures must not roll back the passing Cinnamon changes.

The GNOME resident experiment subsequently passed all fifty registered guest tests, the twelve
native capture/clipboard/cancellation cases and the local focused UI gates on
its verified Core route. Two separate 300-second post-use observations both
peaked at 32.77 MiB RSS, with 61 valid samples each and worst CPU 0.0334% of one
core. The [structured record](https://github.com/patrick-hudson/put.gd-evidence/blob/874703b35562c732268c6fe0bc9b42614ffda928/records/2026-10-10-gnome-core57-memory.json)
pins exact source, workload, process and original result hashes. The approved
30-minute extension completed the fresh empty-profile cold confirmation at
26.95 MiB peak RSS and 0.0067% of one CPU core over 300.000 seconds. All 61
samples passed coverage and exact identity checks. Independent numeric and
lifecycle review confirmed the external 5.119-second warmup, normal shutdown
and unchanged populated profile. Both preceding orchestration failures reached
no samples and remain preserved. The experiment was retained in its clean isolated
checkout and was not integrated at this checkpoint. This established the scoped
GNOME VM cold/post-local-native target, with Cinnamon implementation unchanged.
GNOME remote History, KDE and post-S3 memory were still open at that point; see
[the replay procedure](../testing-gnome-memory.md).
