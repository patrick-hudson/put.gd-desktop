# Verification record

> **Historical record.** This is a dated run record, moved unchanged from [implementation evidence](../../testing/verification.md) on 2026-11-05 (#1222). It describes what that run found for the build it names, and its open or pending statements describe that time. "Above" and "below" refer to its neighbours on the old page, in the order of the [history index](../README.md#verification-run-records). For today, see the [current state](../../testing/verification.md#current-state). Do not use it as a work list.

## Closed dropdown wheel input — issue #26 (2026-10-14)

The then-named `VisualStyle::ChoiceBox` was changed to ignore wheel input on the closed control,
so Qt can scroll its containing pane without changing the selection. Popup mouse
selection, keyboard arrows, and editable text entry retain their existing behavior.
A source audit found all current dropdowns in the Capture and Destinations settings
panes; History and Activity do not construct dropdowns.

On fresh `main` base `bc4215d`, the new regression initially failed for all 21 dropdowns.
The final focused run passes all 23 cases (25 QtTest results including setup/cleanup)
on distro Qt 6.4.2: each dropdown with and without focus, both wheel directions,
parent scrolling where space remains, and plain/editable deliberate selection.
QtTest's spontaneous-event marker and `qApp->notify` are required for the wheel
propagation check; `sendEvent` clears that marker. Earlier harness attempts are
retained under `/tmp/pg26-red`, `/tmp/pg26-red2`, `/tmp/pg26-green`, and `/tmp/p26v`.
The final focused command uses `XDG_RUNTIME_DIR=/tmp/p26v/r TMPDIR=/tmp/p26v/t
QT_QPA_PLATFORM=offscreen build/dev/test_gui_settings
closedDropdownWheelPreservesValueAndScrollsPane dropdownMouseAndKeyboardSelection`.
The fixed runtime path and equal-length templates for randomized test socket paths
passed the 107-byte preflight; the longest checked template is 77 bytes.

The standard development configure and full build passed using the existing pinned
AWS SDK and dynamically linked distro Qt. `ctest --preset dev` passed all 59 tests
in 446.92 seconds, including the full Settings, Activity, and History suites.
These are synthetic widget/core checks, not compositor or memory acceptance.
Current-candidate Cinnamon functional/memory acceptance and subsequent GNOME/KDE
regressions remain pending; no VM base was booted or changed for this slice.
