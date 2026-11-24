# Verification record

> **Historical record.** This is a dated run record, moved unchanged from [implementation evidence](../../testing/verification.md) on 2026-11-05 (#1222). It describes what that run found for the build it names, and its open or pending statements describe that time. "Above" and "below" refer to its neighbours on the old page, in the order of the [history index](../README.md#verification-run-records). For today, see the [current state](../../testing/verification.md#current-state). Do not use it as a work list.

## Storage-driven tray feedback — issue #33 (2026-10-14)

The candidate starts from pending PR #34 head `a4140ea`. Capture selection,
acquisition, local preparation and queue waits now stay blue. Destination checks,
uploads, reconciliation and sharing use scoped storage activity instead of the
active-operation count. S3 and Proton declare reliable upload phases, so their
uploads stay blue through setup/preparing and turn green on uploading/finalizing.
Providers without progress remain valid and use the request boundary.

Errors flash red/blue every 300 ms for three seconds using precise timers. A new
error restarts the interval; recovery restores green if another request remains
active, otherwise blue. Capture cancellation and deliberate clipboard supersession
produce no error pulse. Genuine local-copy errors, including failed retries, do.
The tooltip retains its operation/progress rows independently of color. Color-only
native updates emit icon changes without redundant tooltip signals.

The initial acquisition-color assertions failed against the supplied base before
production changes. Correctness review then found clipboard-supersession and
local-retry feedback defects, and the host review found an initial green event
before provider-reported upload preparation. All three added regressions failed
before their fixes (`review-red.txt`, 19.197 seconds) and passed afterward
(`review-green.txt`, 8.317 seconds). The final development rebuild and full
`ctest --preset dev` passed **59/59**, in **477.68 seconds**, after these fixes.
`git diff --check` passed. The preceding full run also passed 59/59 but predates
the review fixes.

Tests cover selection, deterministic preparation/queue waits with no storage
dispatch, provider preparation without even an intermediate active transition,
optional progress, success, cancellation, failed capture/preparation/upload,
notification-disabled feedback, local-save retries, deliberate deferred-copy
supersession, overlapping successful/failed uploads, pulse restart/recovery, native
wire pixels and host restart. These are offscreen, isolated D-Bus and Xvfb
contracts; mock wire peers are not compositor acceptance.

Logs and before/after failures remain in `/tmp/i33v`. The full-suite tested build
is `0.1.1-dev.175+ga4140ea053ed.dirty`; `build-info-tested.json`,
`binary-hashes-tested.json`, `candidate.patch` and `source-tested.json` identify
its actual bytes and uncommitted patch. It uses GCC 13.3, dynamic distro Qt 6.4.2
and the existing pinned official AWS SDK 1.11.907. The first configure needed
the SDK source/license path as well as its install prefix; both were reused
through ignored build-directory symlinks. Tests use private `XDG_RUNTIME_DIR`
`/tmp/i33v/r` and `TMPDIR=/tmp/i33v/t`; socket preflight passed 395 fixed paths
and equal-length randomized templates, maximum 77 UTF-8 bytes.

Preserved failed attempts include sandbox D-Bus binding denial, a coarse timer
that could recover late, and an overlap fixture that serialized same-name uploads.
The corrected run uses precise recovery timing, two upload slots and independent
generated targets; socket ownership and frozen performance harnesses are unchanged.
The external Anthropic review could not start because shared transport provisioning
returned npm ETARGET for `acpx@0.19.5`, before review-content egress. A separate
local adversarial reviewer completed the lens; all three code findings were applied.
The completed review and follow-up receipt are under
`/tmp/compound-engineering-1000/ce-code-review/20261014-172954-89b34b02/`.

The [native VM follow-up](https://github.com/patrick-hudson/put.gd-evidence/blob/874703b35562c732268c6fe0bc9b42614ffda928/records/2026-10-14-storage-tray-native.md) tested frozen
commit `061c2d9c569d` on Cinnamon 6.6.4/X11/Qt 6.4.2, GNOME 50.1/Wayland/Qt 6.10.2
and Plasma 6.6.6/Wayland/Qt 6.10.2. Each desktop passed twelve native
capture/clipboard/cancellation cases, six view opens/closes with normal helper
exit, and five rendered storage/error color checks. Cinnamon passed its required
functional and memory gates before the Wayland builds began. GNOME and KDE's
native development suites passed 60/60 and 59/59; their post-build tracked source
hashes matched all 5,998 files in the frozen archive.

Two serial five-minute post-use guest observations per desktop passed the 40 MiB
RSS/no-helper/no-swap/CPU gates. Peak RSS was 29.96 MiB on Cinnamon, 33.98 MiB on
GNOME and 31.05 MiB on KDE. The report retains exact identities, raw samples,
synthetic console frames and failed diagnostic attempts. The first KDE region
attempt had an SSH interruption and later helper timeout; its repeat and an
independent fresh-overlay first gesture passed, without a candidate code change.
These are bounded single-head compositor and post-use VM results. The offline S3
wire oracle does not establish cloud/Proton acceptance, sampled frames do not
certify each 300 ms repaint, and physical performance/full release gates remain
separate. All golden/backing disks were preserved; only fresh overlays were booted.
