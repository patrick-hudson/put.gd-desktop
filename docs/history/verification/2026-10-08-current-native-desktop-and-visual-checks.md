# Verification record

> **Historical record.** This is a dated run record, moved unchanged from [implementation evidence](../../testing/verification.md) on 2026-11-05 (#1222). It describes what that run found for the build it names, and its open or pending statements describe that time. "Above" and "below" refer to its neighbours on the old page, in the order of the [history index](../README.md#verification-run-records). For today, see the [current state](../../testing/verification.md#current-state). Do not use it as a work list.

## Current native desktop and visual checks

The custom Qt visual system has been inspected in installed Mint and Ubuntu
applications. Settings, activity and populated history use the same design.
An actual compact-history preview/expanded-controls failure was fixed; the installed
Mint witness fits 1000×700 with a visible preview. Offscreen GUI tests retain strict
layout and keyboard checks. Full visual scale/error/confirmation-state inspection
remains pending. Shortcut statuses now show readable per-action results with exact
JSON behind a keyboard-accessible advanced disclosure.

A fontless instrumented Qt run expanded history to 1045 pixels and failed its
unchanged assertion. The same thread-checked test passed with installed DejaVu fonts
selected through QT_QPA_FONTDIR. No layout assertion or race/leak filter was relaxed.

Ubuntu's native Qt6.10 build exposed a missing direct QElapsedTimer fixture include
and an unsigned public-GET check that spent six seconds discovering EC2 metadata.
The include is explicit and the SDK configuration disables unnecessary IMDS lookup,
as the configured S3 client already did. The original five-second synthetic probe
test then passed; its timeout was not increased. Native packaging also aligns the
desktop executable with the configured installation prefix.

Authenticated Mint, Ubuntu and Fedora images have been installed, booted into their
native desktop, powered down cleanly and checked/hashed. Tests use disposable qcow2
overlays. Kubuntu's one-time installation also completed, with fresh automatic Plasma
6.6.6/Wayland login and clean disk check. This establishes base provenance,
not full desktop support. Credentials are absent from base disks and Git.

GNOME50's removed grab API caused a genuine region-selector TypeError. The helper
now uses the public is_revoked API, cleans up failed setup, and preserves half-open
drag geometry. Native operation3ae7b7c4-aa27-40dd-92af-1de2fab42675 produced an80×60
image with expected pixels and an identical independent Qt paste. Desktop operation
8374ea3c-6d17-4b03-9677-aa8c4f8fb513 likewise produced1280×800 and identical paste.
The runner locates complete synthetic paint in a pre-action QMP image because Qt
on Wayland cannot report the global window origin. Active-window operation
e73d7b29-b38c-41b2-9395-4c2436dded50 produced662×480, with dimensions derived from
the pre-action paint and the fixture's native frame margins; independent paste matched.

A real clipboard import failure exposed use of an absent Meta.SelectionType.CLIPBOARD
enum. The installed Mutter18 typelib exposes SELECTION_CLIPBOARD. Correcting those
three references restored native text,96×64 image, and ordered two-file imports and
independent Qt paste. St.ClipboardType.CLIPBOARD remains correct and unchanged.
These are six named single-head local workflows, not complete GNOME certification.

Real selector cancellation exposed a durable pending copy status after acquisition
ended. A failing operation regression preceded the shared finish-path fix, which
settles pending failed/cancelled/interrupted results as no_result. The full operations
suite then passed Debug(4.59s), ASan/UBSan with leaks(5.06s), instrumented Qt TSan(6.75s),
and native Ubuntu(3.32s). Installed Escape/right-click cancellation now creates no
artifacts and preserves independently pasted synthetic text. QMP button press/release
must be separated so the compositor receives both transitions.
