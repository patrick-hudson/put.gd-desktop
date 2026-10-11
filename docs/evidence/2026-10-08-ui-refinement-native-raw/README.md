# Native UI refinement replay preparation

This directory is scratch preparation only. No stop, build, install, launch, QMP input, or screenshot acceptance phase has been run by its preparer. Read-only localhost SSH inspected the **three existing disposable overlays**. The failed sandbox-only inventory and successful escalated inventory are separate files. No old payload or manual screenshot was opened.

Use `replay.py` only after the eight owned product/test paths have been committed. Supply that full commit explicitly. A Git archive never includes working-tree changes, untracked files, or local credential input. Every command attempt gets a new directory with exact argv, controlled input script, stdout/stderr, return code, and completion/failure receipt. Witness labels are unique and refusal prevents overwriting previous evidence. Build/install/launch failures require deliberate recovery and a distinct attempt/output; a partially completed phase never counts as acceptance.

The VM allowlist is fixed:

| Guest | Disposable overlay | SSH | Visible VNC |
|---|---|---:|---:|
| KDE/Wayland | `work/lab-runs/current-kde-a3d70dc/disk.qcow2` | 22234 | 5934 |
| GNOME/Wayland | `work/lab-runs/current-gnome-a3d70dc/disk.qcow2` | 22236 | 5936 |
| Cinnamon/X11 | `work/lab-runs/current-cinnamon-a3d70dc/disk.qcow2` | 22238 | 5938 |

Keep the display wall visible at http://127.0.0.1:8765/. No VM launch/stop/reboot, base-disk action, VNC redirection, or package/toolchain provisioning is part of this runner. The existing GNOME Shell remains active when the packaged extension hash equals its current installed hash. A mismatch aborts before install; plan visible Shell reload separately rather than rebooting reflexively.

## Exact freeze/stop/build/install sequence

From `/home/patrick/work/put.gd`, set `candidate_commit` to the frozen full commit. Choose a NEW ASCII alphanumeric/hyphen output name. Commands below are prepared, not executed:

```bash
candidate_commit=FULL_40_HEX_COMMITTED_UI_REVISION
replay=/home/patrick/work/put.gd/work/ui-refinement/native-replay/replay.py
replay_output=/home/patrick/work/put.gd/work/ui-refinement/native-replay/source43-attempt1
python3 "$replay" --output "$replay_output" freeze --commit "$candidate_commit"
python3 "$replay" --output "$replay_output" stop --guest kde
python3 "$replay" --output "$replay_output" stop --guest gnome
python3 "$replay" --output "$replay_output" stop --guest cinnamon
python3 "$replay" --output "$replay_output" guest-build --guest kde
python3 "$replay" --output "$replay_output" guest-build --guest gnome
python3 "$replay" --output "$replay_output" host-build
python3 "$replay" --output "$replay_output" install --guest kde
python3 "$replay" --output "$replay_output" install --guest gnome
python3 "$replay" --output "$replay_output" install --guest cinnamon
```

Stop refuses a resident whose PID, `/proc` start ticks, or live executable SHA-256 differs from `preparation-inventory-localhost.json`; deliberately refresh the read-only inventory if the live owner changes. It first requires no UI helper, zero active operations, and terminal ID/state values across **the entire** synthetic operation/deletion journal, not just bounded Activity. Stop targets the verified resident with pidfd SIGTERM, waits for normal disappearance, and checks that no resident/helper remains. It renames the entire old `/home/lab/putgd-lab` to `/home/lab/putgd-lab-before-ui-refinement-OUTPUT_NAME`, including prior manual capture, without traversing its payload files. No deletion occurs.

All three stop receipts must exist before any build starts. KDE/GNOME reuse `/home/lab/putgd-memory-build/sdk` and `/home/lab/putgd-memory-build/dependencies/aws-sdk-cpp`. Cinnamon has **no guest CMake/Ninja/ctest/SDK**; its package comes from the exact same archive via normal host `build/sdk` and `build/dependencies/aws-sdk-cpp`, matching the prior U11 approach. Builds use dev configure/build/ctest followed by release configure/build/cpack. Qt stays distro/dynamic through existing CMake configuration. No substitute SDK/toolchain is assembled.

Install first simulates `apt-get --no-download --no-remove install CANDIDATE_PACKAGE` and requires exactly one `Inst putgd` line and no removal. It then uses the guest’s existing authorized `sudo -n apt-get --no-remove -y install CANDIDATE_PACKAGE` with already installed dependencies. The local DEB is already copied and SHA-verified; the preceding exact-one-package simulation excludes dependency provisioning. The older Cinnamon apt cannot install local DEBs with `--no-download` (a preserved failed attempt reports a nonabsolute cache pathname), so that flag applies to simulation only. It does not run the lab install command's `apt-get update` or install development packages. Missing dependencies or sudo access abort. It verifies package manifest, installed commit/tree/count/version, executable SHA-256 and every executable `--version`; helper bytes are checked when visible views open. It backs up old `package.json`, `versions.json`, `diagnostics.json`, and `state.json` verbatim into `previous-live-markers`, then updates live package/version metadata accurately. Startup updates live diagnostics. Prior test artifacts and their recorded source-41 version stay untouched; do not copy/relabel them as candidate evidence.

KDE/GNOME/host builds may run concurrently after ALL old residents stop, with normal `functions.exec` yielding command sessions to the parent. Never start another machine or install missing tools. Review logs/JUnit and the source/build/install receipts before native testing.

## Visible startup and safe fixtures

Repeat for each guest (`guest=kde`, then `gnome`, then `cinnamon`):

```bash
guest=kde
python3 "$replay" --output "$replay_output" start --guest "$guest"
```

The first witness is the new synthetic-only setup dialog; restore an idle/locked session through its visible VNC console before counting any UI evidence. **Click its Cancel button visibly**, then observe helper exit. Do not treat Alt+F4 as Cancel: window Close retains draft state by design. Use `step` to click the observed Cancel coordinates, then `observe` for accepted revision/profile ID state. The startup screenshot/attempt remain evidence of any unexpected session lock or launch issue, not a passing view.

```bash
python3 "$replay" --output "$replay_output" step --guest "$guest" setup-cancel click OBSERVED_X OBSERVED_Y
python3 "$replay" --output "$replay_output" observe --guest "$guest"
python3 "$replay" --output "$replay_output" configure --guest "$guest" empty
```

`configure empty` saves validated local-only output for every action, no profiles, unbound shortcuts, disabled notifications/autostart, and `/home/lab/putgd-lab/archive`. `configure configured` adds exactly one synthetic S3 profile with session/no credential input, an `https://example.invalid` endpoint and public URL, while **every action remains local**. Saving a profile validates configuration only. Never click S3 setup, upload, or credential-storage actions.

`populate` requires every action local and no open helper, then performs three real native desktop actions in the fresh synthetic lab session. The resulting local-only operation IDs and history IDs are retained. This is actual native source/history behavior; there is no seeded mock ledger and no cloud request.

## Window sizing and visible QMP steps

Use the existing 1280×800 desktop; never silently redirect display output. The permitted compact alternative is a normal 1024×700 client window on that desktop, avoiding compositor display-layout changes.

Cinnamon supports exact owner-selected EWMH client resizing:

```bash
python3 "$replay" --output "$replay_output" resize --guest cinnamon settings-empty-compact 1024 700
python3 "$replay" --output "$replay_output" resize --guest cinnamon settings-empty-wide 1200 700
```

For Wayland, resize the visible window with QMP border/corner drags after inspecting the open screenshot. `drag` records the exact press, ten visible pointer movements, and release. Establish client geometry from the visible borders/native size indication; record the verified actual size in the manual visual review. A fullscreen 1280×800 display screenshot alone does **not** prove a 1024×700 client. If exact compact sizing cannot be established through visible interactions, mark that gate blocked. Do not substitute offscreen tests or infer geometry from requested drag coordinates.

Prepared primitives (coordinates are obtained from that guest's actual latest synthetic witness, not guessed/copied across desktops):

```bash
python3 "$replay" --output "$replay_output" show --guest "$guest" settings settings-empty-open
python3 "$replay" --output "$replay_output" step --guest "$guest" settings-empty-sized drag X1 Y1 X2 Y2
python3 "$replay" --output "$replay_output" step --guest "$guest" settings-add click ADD_X ADD_Y
python3 "$replay" --output "$replay_output" step --guest "$guest" settings-add-cancel click CANCEL_X CANCEL_Y
python3 "$replay" --output "$replay_output" step --guest "$guest" settings-advanced click ADVANCED_X ADVANCED_Y
python3 "$replay" --output "$replay_output" step --guest "$guest" state-witness shot
python3 "$replay" --output "$replay_output" close --guest "$guest" normal-close
```

`show` verifies native helper PID, start, parent resident, and helper binary hash. `close` uses visible Alt+F4 and requires helper PID=0, normal exit code=0, resident identity unchanged and tray available. Only use it while the intended owned view is foreground and no auxiliary window/dialog is foreground. Clicking Settings Cancel uses `step`, then `observe`; a discarded draft must not change accepted configuration.

## Required case matrix for EACH compositor

Every required state needs both a wide desktop witness and compact normal-window witness. Keep unique labels and corresponding commands; visual acceptance remains pending until the actual images are reviewed.

1. Saved local-only, **empty profiles** Settings: prominent Add S3 destination with inactive editor/credential cards hidden, Save locally, and bottom actions reachable. Capture before Add, after Add (draft editor displayed), then click Cancel. `observe` before/after Cancel must show same accepted revision and zero profiles; reopen and show the empty state again. Repeat wide/compact.
2. One configured **synthetic** profile Settings: simple destination edit state and expanded Advanced settings. Check region/bucket/URL/control sizing, navigation visibility, normal fonts, help affordances and reachable footer. Expand/collapse must preserve the draft. Capture both widths; Cancel any unsaved edit before changing fixture via CLI.
3. Populated History: close every helper, `populate`, `show history`. Capture list with no selection, then select a known first row by visible click. Check inspector appears, selected item corresponds to retained synthetic history ID, Name/date/upload/local/remote columns fit, text/details remain readable, and compact stacked/wide side-by-side inspector behavior works. Capture selected compact, selected wide, selected compact again to show stability, then normal close/reopen. Do not click remote delete/history-clear/cache-clear actions.
4. Activity: create a known synthetic clipboard-text operation through the independent Qt fixture below. `show activity`, select that recorded parent row and separately its child row, use visible full-ID tooltips plus retained CLI operation/item IDs to identify the selection, and click the actual **Copy result** button. Paste into the independent fixture and require exact known synthetic text plus `copyStatus=copied`. Capture parent/child selection at wide/compact. The tree ID column is abbreviated; it provides no invented full-ID copy control. Full IDs are retained in receipts and verified via production inspect. Avoid relying on Ctrl+C from the abbreviated tree cell.
5. Close each view normally, verify helper exit, resident unchanged, and visible tray remains. `trayAvailable` is only diagnostics; include final native screenshot showing rendered tray, and mark any tray/desktop visual limitation explicitly.

Run the existing current-source native matrix before the view refinement wave:

```bash
python3 "$replay" --output "$replay_output" matrix --guest "$guest"
```

This reuses `tools/lab/lab.py` for twelve real native scenarios: Qt region/window/desktop, Qt clipboard text/image/files, three region cancellations, and GTK clipboard text/image/files. The installed resident/source/helper checks surround every case. Each scenario gets a NEW short persistent `work/lab-runs/ui43-GUEST-TIMESTAMP-INDEX` host control-directory proxy (QMP socket pathname under 104 bytes) with a copy of current state/package metadata and symlinks to only the **existing** overlay's QMP socket/SSH identity. It launches no VM. Generic lab screenshots/results/versions are written inside that new proxy, so prior run-directory test artifacts are untouched. Scenario evidence preserves actual operation IDs, local artifact/paste verification, and visible QMP witnesses. The lab helper's recorded process/window lifecycle is unchanged. Close the synthetic fixture normally before subsequent view work and reapply `configure empty` or `configured` as needed after its fixture shortcuts/settings.

Independent Activity result-copy sequence:

```bash
# Start while no Activity helper is open; a NEW synthetic fixture directory only.
python3 "$replay" --output "$replay_output" fixture --guest "$guest" start result-fixture-ready
# Focus fixture visibly and establish the exact known synthetic clipboard text.
python3 "$replay" --output "$replay_output" step --guest "$guest" synthetic-text-source key ctrl alt shift t
python3 "$replay" --output "$replay_output" fixture --guest "$guest" invoke-text clipboard-text-complete
python3 "$replay" --output "$replay_output" show --guest "$guest" activity activity-open
# Click known parent row; hover ID column for the actual full-ID tooltip.
python3 "$replay" --output "$replay_output" step --guest "$guest" activity-parent-selected click ROW_X ROW_Y
python3 "$replay" --output "$replay_output" step --guest "$guest" activity-parent-id hover ID_X ID_Y
python3 "$replay" --output "$replay_output" step --guest "$guest" activity-parent-copy-result click COPY_RESULT_X COPY_RESULT_Y
# Focus visible Qt fixture (Alt+Tab or observed click), then paste.
python3 "$replay" --output "$replay_output" step --guest "$guest" result-fixture-focused key alt tab
python3 "$replay" --output "$replay_output" step --guest "$guest" result-pasted key ctrl v
python3 "$replay" --output "$replay_output" fixture --guest "$guest" verify-result-paste result-parent-paste-verified --selection parent
```

Repeat after returning to Activity and selecting the known child row, with unique labels and `--selection child`. Capture the parent tooltip/full operation ID and corresponding child selection. The fixture records native clipboard MIME/text; verification requires exact equality with `putgd synthetic clipboard text` and inspects the recorded full parent operation ID for `copyStatus=copied`. Selection identity still needs visible image/tooltip review. Close the foreground fixture visibly (Alt+F4), then foreground Activity before the runner's helper `close`. Do not signal or broadly pkill windows/processes as a substitute for GUI close.

## Receipts and constraints

Make a NEW `visual-review.json` per compositor after inspection, containing full candidate commit/version, source/archive/package/install/binary hashes, resident/helper identities, exact witness labels/hashes, actual client sizes, case outcomes, full parent/item IDs plus actual copied-result match, successful helper close events, and blocked gates. Never mark `passed` from procedure success alone. Source-41 prior native/performance evidence is unchanged and is not newly established by this bounded refinement wave.

The runner was syntax compiled and `--help` exercised locally only. No native acceptance phases were executed. Coordinates, compact Wayland geometry, and complete GUI coverage still require the root's visible execution/review. If a view's draft retention prevents clean fixture transition, use its visible Cancel/Reload behavior and new witness labels; do not edit the product or a private saved-state file to manufacture a desired screenshot.

Failed initial matrix attempts used excessively deep control paths and hit the Unix socket pathname limit before capture. They remain failure receipts. Recovery uses fresh short persistent control directories and records their exact paths; it neither starts VMs nor overwrites the failed directories.
