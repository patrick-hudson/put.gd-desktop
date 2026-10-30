# Shortcut keycap second-click cancellation (#731)

Code PR: https://github.com/patrick-hudson/put.gd-desktop/pull/743. Source
`44455848595f65d09ce6bf1365307d3b9ad3452a`, tree
`8e41ce22c3ed83914f080fe69ece873f0abfb2fe`. Issue #731 remains open for the
coordinator to close after merge. This records a bounded shortcut-settings fix,
not release certification for other desktops or features.

## Visible result

The boxes mark the active keycap and recording status. Arrows identify the
result after a second real mouse click on that same keycap. The screenshots
retain the native pixels; only the banner, boxes and arrows were added.

![Before: second click leaves recording active](https://raw.githubusercontent.com/patrick-hudson/put.gd-desktop/960323b0088ee6fb52d50d60be668007a6376fa0/docs/evidence/2026-10-30-shortcut-keycap-731/before.png)

The original issue's read-only `s703-r13` input-after frame comes from source
`40a95146b692dda82f0d5a07d11ce7ab9a260910`. The saved frame SHA-256
is `8e9118f9968d3b53e706d2140537bc684e20b5ce25acc946fdd34d50127ecfde`.
The native click reached the focused put.gd window, but the UI still said
“Press shortcut…” and native bindings stayed `recording`.

![After: second click cancels recording](https://raw.githubusercontent.com/patrick-hudson/put.gd-desktop/960323b0088ee6fb52d50d60be668007a6376fa0/docs/evidence/2026-10-30-shortcut-keycap-731/after.png)

The after frame is from the final source above, installed as a distro-linked
DEB in a fresh Mint 22.3 / Cinnamon 6.6.4 / X11 KVM overlay. It shows the saved
chord and “Recording canceled. The draft shortcut is unchanged.” The independent
native-binding samples confirm restoration; appearance alone is not that proof.
The build uses a supplied source archive: all 906 tracked inputs are verified,
and provenance remains `supplied` with `dirty: null`. See `result.json` for
hashes and exact identities.

## Cause and fix

The old `ShortcutEditor::startRecording` made the keycap button `NoFocus` and
focused its mouse-transparent, `NoFocus` QLineEdit child. Qt's spontaneous mouse
focus handling runs before the button receives its press and walks to a
focusable ancestor. In the actual settings scroll area it focused the
QScrollArea, sending FocusOut to the child. That canceled the recorder. The
button's subsequent `clicked` handler then saw no active recorder and started
it again. A programmatic `QPushButton::click()` does not exercise this path.

The fix keeps normal mouse focus on the actual keycap and watches that button's
FocusOut. A completed click on a different keycap switches rows. Cancellation
for that mouse-focus transfer is deferred through the release: restoring native
bindings hides the Paused labels, which otherwise moves the pressed target
before release. An abandoned drag still cancels; Escape/deactivation also
abandons a held row switch and prevents its later release from starting again.
Queued work and asynchronous acknowledgements retain generation guards.

This is shared Qt widget behavior. The recorder does not grab the pointer or
keyboard. The backend pause/resume code is unchanged: X11 clears passive grabs
and acknowledges keyboard release, GNOME applies empty extension bindings while
recording, and KDE calls KGlobalAccel `setInactive`. Each restores its requested
bindings on `setRecording(false)`. Backend contract and integration tests are part of
the full passing suite. No X11-specific workaround was added.

## Regression and full suite

`shortcutSecondMouseClickCancelsRecording` sends synthetic Qt mouse input
through the QWindow into a real QScrollArea. It checks both pending and completed
pause acknowledgements, saved chords, visible cancellation text, stale callbacks,
row switching with status-driven reflow, Escape, an abandoned drag, and Escape
while another keycap is held. Existing physical-key/chord tests remain in place.

The final test fails with the old widget substituted from nightly
`422183788394106528a55b7459964262e635ea36`: both data rows observe an unwanted
third pause/restore transition instead of exactly `[true, false]`. Restoring the
final source passes both rows plus the existing chord/Escape and cancellation
cases. `regression-old.log` and `regression-fixed.log` retain the actual outputs.
This is a controlled old-widget substitution, not a product build with a false
source identity. The exported product sources are hash-verified after restoration.

Full CTest on the Cinnamon fork passes **114/114 without retries** using the
normal dev preset, `PUTGD_TEST_TIME_SCALE=3`, resource locks and parallelism 4.
The complete suite result and parameters are in `ctest-summary.json`.

Earlier candidate attempts are preserved privately with their logs and JUnit:
a scale-3, parallel-12 run had one gui_settings assertion and one proton_contract
timeout (both passed one retry); a scale-1, parallel-12 run had failures in
operations, storage_contract, gui_history, proton_contract and proton_transport.
The final run uses the normal CI time scale with less contention and does not
retry any test. No timeout budget, assertion or frozen harness was weakened.

## Native proof

The unchanged `settings.shortcut_recording` recipe comes from
`feat/surface-recipes-settings` / PR #695, commit
`ef303b202936260ec1d61b1a1014cde785ba4f4e`. Its runner is separate from the
candidate app source. It exercises all six actions: Escape preserves the chord,
recording Ctrl+Alt+Shift+9 saves it, and a second click on the active keycap
cancels without changing it. Inputs use actual QMP mouse/key events in the
native desktop, with owned-process accessibility and input-recipient checks.

The final recipe receipt passes with no review flag. Its observer retains 444
samples covering 18 recording segments (three per action). A read-only observer samples
`putgd-cli ui.snapshot` from the recipe's own private resident socket; each
post-second-click action is native-bound to the saved chord and no entry remains
`recording`. `native-bindings.json` records the frame/sample pairing. An extra
native campaign switches directly between recording rows, records a new chord
on the target row, and verifies that the first row's saved chord stays intact.

The one remaining Cinnamon Boat fork performs the final build, full CTest and
native checks. It is bare metal with usable `/dev/kvm`; the guest stays visible
through its loopback VNC tunnel on the live wall. xdotool and python3-pyatspi are
staged only in the disposable overlay. No physical desktop or workstation VM is
used, and no cloud credentials or uploads are involved.

A preliminary GNOME 50 / Wayland / Qt 6.10.2 run of candidate `30aec91d` passed
second-click cancellation, Escape and chord capture, but exposed the row-reflow
switch failure that this final fix and GUI test address. Those failure frames
are retained. It is not final-head GNOME acceptance. After the coordinator's
sandbox-budget reduction, no further GNOME fork is started: this fix has no
separate GNOME input path or changed backend contract. KDE was inspected and
covered by integration tests, not a native desktop run.

## Retention and cleanup

Raw native frames, source manifests, build/package identities, complete logs,
recipe inputs/receipts, observer samples and initial failures are retained in
private `work/731` exports. Archives have SHA-256 indexes, verified restored raw
frames, run UUIDs and cleanup manifests; `result.json` records the final export.
No golden disk is booted or modified. Final Cinnamon overlay retirement and
sandbox deletion receipts are recorded after export verification.

The preliminary GNOME export was verified (130 files, 33 restored raw frames)
before its sandbox was deleted. Its guarded overlay cleanup command exited 1,
and deletion proceeded before a completed retirement receipt was confirmed.
That overlay retirement is explicitly incomplete, not passed; the enclosing
sandbox deletion completed as operation `bdop_36e4d01bf0234b29b7c19b889a35de86`.
