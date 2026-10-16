# Proton shipping and final package acceptance

Issue #10. Replacement candidate `a5112bc55df93cfa42e0824f9a0a3d33fb7f9efa`,
clean tree `4d0a19c5c8c1eb3f5bd077a8d475039ec9170ead`, count 198,
`0.1.1-dev.198+ga5112bc55df9`. This is a local release candidate;
no tagged application release or hosted-CI result is claimed.

## Changes and preserved failures

The shipping integration is committed at `e736dd8`. Actual final-format testing
found an RPM dependency on a nonexistent Fedora package and a GNOME acquisition
failure. `691045b` corrects the RPM prerequisite and the native tray driver's
preparation-state expectation while requiring independent green upload/retry
observations. `a5112bc` filters Qt reader advertisements to `image/*`, preserving
the strict GNOME bridge. A real Qt metadata plugin regression fails before that
fix and passes after it. Nine transport checks and the bridge CTest pass.
The fixture is test-only and is excluded from production packages.

Prior `e736dd8` packages and desktop results remain historical. The failed Fedora
run invoked no SDK and its isolated session was returned. New package hashes and
new desktop receipts must be used for the replacement candidate.

The AppImage private dispatcher initially rewrote an already-qualified CLI path;
that setup attempt failed before any SDK operation. The corrected driver completed
the upload and five-minute measurement, then its final native-paste visibility
guard correctly rejected an obstructing Update Manager welcome window. After
foregrounding the independent fixture, a separate continuation passed the unchanged
guard, verified retained receipts and recorded the actual native paste. The original
failed receipts and successful measurement remain intact; no upload or measurement
was repeated or relabeled.

## Sources and shipping assets

The official JS SDK 0.22.2 / Bun 1.4.2 helper is unchanged:
`eeffe97c9c7f473895f790434dabafb13a018b8909a2f5215c3b769e7b7bb539`.
Its manifest is `cf5d47d458930cea461c33add5dab04f48b838e01bda6b7b8ceec738f08f62a9`.
The explicit GPL Crypto 2.0.1 source adoption and P3 preferred-source/relink
proofs remain applicable. The separate native trial remains unadopted.

The six sealed P3 source packets are publicly delivered by the source-only
[release](https://github.com/patrick-hudson/put.gd-desktop/releases/tag/proton-sources-0.22.2-crypto2.0.1).
Both public parts, the reconstructed archive, and every inner packet were byte
verified. The helper's frozen catalog contains build-time P6-pending flags;
current exact-hash acceptance is recorded externally rather than changing tested
artifact bytes to label them accepted.

The replacement curated application archive is 3,560,725 bytes, SHA-256
`5bed7267da2a0d83f12025f1c36a4bc719e48bab848d456db3839a4d82a94ce4`.
An actual extracted production rebuild passed and reproduced the exact helper
and manifest. All six executables matched the supplied source identity.

## Native and format evidence

All three installed-DEB native workloads passed: local-only control, account
inspection, initial/normal uploads, cancellation after real SDK progress, explicit
same-operation retry, three repeated batches, independent native paste and clean
restart with retained receipts. Both same-used-resident 300-second idle windows
passed the unchanged gates on every desktop. Sampled helper generations were
cross-checked against actual process birth, exit and wait traces.

| Native desktop / Qt | Sampled active RSS / PSS / private MiB | Idle RSS MiB, windows 1 / 2 | Idle CPU % of one core, windows 1 / 2 | Settled-repeat RSS growth MiB |
| --- | --- | --- | --- | --- |
| Cinnamon/X11 / 6.4.2 | 200.438 / 156.484 / 150.539 | 29.258 / 29.258 | 0.0267 / 0.0100 | 0.449 |
| GNOME/Wayland / 6.10.2 | 235.098 / 159.656 / 150.586 | 31.621 / 31.621 | 0.0100 / 0.0133 | -0.133 |
| KDE/Wayland / 6.10.2 | 295.109 / 157.398 / 148.656 | 30.867 / 30.867 | 0.0233 / 0.0167 | 0.011 |

The actual Fedora RPM and mounted AppImage passed live Proton upload/progress,
Activity, native paste and restart/retained-receipt checks. The AppImage
measurement includes its reparented FUSE process. It is a separate one-window
format diagnostic under the existing approved format-overhead exception.
Its whole-format quiet peak was 38.316 MiB RSS,
with 0.0033% of one core and no guest-process swap.
The strict numerical 40 MiB result was **pass**; the native
gate was not changed and this format-policy result is not a native-budget pass.

GNOME used a fresh normal Shell login and verified the active system-extension
path, packaged source hash and visible current companion behavior. The driver
missed its actual x1158 tray position; original captures were retained alongside
explicit supplementary idle/upload/retry captures. KDE also retains a fresh
retry hover after Activity changed focus. These are actual visible consoles,
not offscreen widget or synthetic-bus proof.

All forty confirmed test share links were revoked against their exact retained
history/receipt targets. Files and existing member access were retained. Every
isolated SDK session was returned to the host and its copied guest reference
removed before its VM was retired. All test VMs were shut down after use.

The initial host workload caused pressure; after the user reported it, unused
VMs were stopped and remaining builds were capped at two CPUs and serialized
with a single active test VM. No frozen observer or socket-ownership rule changed.

| Audited shipping artifact | Bytes | SHA-256 prefix |
| --- | --- | --- |
| cinnamon-deb | 50,360,486 | `8382885202fd4eaf` |
| ubuntu26-deb | 50,590,058 | `8613d914eebf9247` |
| fedora44-rpm | 38,083,685 | `e9825030748f5314` |
| appimage | 98,089,464 | `4b40af363d87bbce` |

Full hashes, source identities, source/rebuild commands, native window summaries
and format-policy outcomes are in the [machine-readable receipt](2026-10-15-proton-p4-p6-acceptance.json).
Private indexed raw packets are retained under
`work/evidence/proton-p6-mime-fixed-20261015/`; capability URLs, auth contents
and payloads are excluded from the committed summary.

The capacity-bounded native workload uses two 1 MiB initial files, two 2 MiB
normal-upload files, two 4 MiB cancellation/retry files, and three pairs of
256 KiB repetitions. Account free space at freeze was 58,832,188 bytes.
The unchanged observers measure the whole product family, helper drain and two
five-minute idle windows on the same used resident. These smaller uploads are
not a throughput or active-peak comparison to the earlier P2 large-file workload.
There is no arbitrary active-RSS rejection ceiling. Native idle still requires
RSS at most 40 MiB, CPU below 0.5% of one core, no swap and no surviving helpers.
Fixture, observer, CLI driver and compositor processes are excluded; product UI
and SDK helpers are included. This does not measure the GNOME Shell heap.

Current Cinnamon's signed-out recipient download delivered exactly 1,048,576
bytes with SHA-256 `fbbab289f7f94b25736c58be46a994c441fd02552cc6022352e3d86d2fab7c83`,
matching the original artifact. The browser reported its transfer completed;
the saved downloaded file was independently hashed.

## Maintenance and scope

P5 credits the actual Crypto provenance/source transition and its source,
rebuild, notice, protocol and installation checks. The newly found distro
interop failure also demonstrates the maintenance obligation; no artificial
additional SDK upgrade was performed.

No S3 credentials, configuration, tests or service operations are part of this
campaign. Whole-application compilation includes the unchanged SDK/module.
No remote test files were deleted; all forty exact confirmed test links were revoked.
P1 retains its documented controlled-only response cases. Four upstream
Crypto/Bun type diagnostics remain; a clean full type-check is not claimed.
Feature acceptance does not certify physical performance, U9/U10, every desktop
geometry/action/lifecycle combination, or an unrun tagged application release.

The KDE disposable lab keyring used the user-completed passwordless setup.
This proves the recorded namespace/return behavior, not production keyring
security policy.

P4 and P6 are complete for this recorded candidate and workload. P5 is credited
as requested. The wrap-up changes remain local until merged; close #10 with that
merge and this receipt. This does not assert a tagged application release or
current-main acceptance.
