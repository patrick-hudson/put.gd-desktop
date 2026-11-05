# Surface input guards: 2026-10-30 development proof

Code: [PR #859](https://github.com/patrick-hudson/put.gd-desktop/pull/859),
`29d1462eb8a2c049be54ff3c259eaf66b457ff59`, based on
`97d335ec9f2bb6a9fb74fb81f3bf121ddb082760`.
This is development proof on Boat, not physical-desktop or release acceptance.
Five positive GNOME resize starts remain **blocked/unproven**.

## Identities and reproduction

Both hosts were hydrated, bare-metal/KVM Boat forks, each with one independently
owned QEMU overlay and a visible workstation wall tile. Immutable goldens were
not booted or modified. Every receipt identifies placement, sandbox, run, target
OS/session and observed clean app source. Target app source was
`6ad9f957e6544f30c5e45c08af2306db77a9ff64`, version
`26.1030.2481-dev+g6ad9f957e654` (unchanged snapshot package).

| Guest | Sandbox / overlay identity | Native stack |
| --- | --- | --- |
| Cinnamon | `bx_ftjc74at`, `core-r3-cin`, run `4cd3d592-2451-4ad3-ab17-2ba5e1d692e8` | Mint 22.3, Cinnamon 6.6.4, Muffin 6.6.1, X11, distro Qt 6.4.2 |
| GNOME | `bx_jrnr7ah6`, `core-r3-gnome`, run `68f9d2d3-bd17-40f7-86e9-33e0c3da2763` | Ubuntu 26.04, GNOME 50.1, native Wayland, distro Qt 6.10.2 |

Snapshots were `putgd-lab-cinnamon-v3` and `putgd-lab-ubuntu-gnome-v4`;
both TTLs were 14,400 seconds. See the platform files for binary SHA-256 values
and dynamic Qt linkage. Source transfer hashes are in `source-manifest.json`.
Per-run driver attribution, including older harness attempts, is recorded in
`source-attribution.json`. The final private proof branch was `b29a6f61d40afb1788ee3d2d229386d7c6d95865`:
it merged tray lane `ebce60530770f4fc58cec106c23c9e0f04b6486f` locally and
adapted its activation/wheel calls plus nested, owned page-state readback.
The code PR contains none of those tray recipe changes. All 55 core source/test
files in the manifest matched the final proof source byte for byte.

The runner used `--host boat --placement boat --run /home/user/v3 --package deb
--destination local --build 6ad9f957e6544f30c5e45c08af2306db77a9ff64`, each
sandbox's ID and native desktop profile, and the generated catalog in
`/home/user/e3/catalog`. Catalog generation used the ordinary CMake target on
Boat and private XDG directories. Its offscreen images are references, not
native desktop proof. Socket lengths were checked before launch.
The final synthetic live proof script is `live-proof.py`; local tray-only
adaptations are `tray-proof-adapter.diff`. All failed attempts remain in the
verified private archive indexed below; no payload, credential or private SSH identity is in this
committed packet.

Recipe selectors were `__main__.notification_cover,__main__.deadline_channel`
for Cinnamon `isolated-final`; final Cinnamon `header_twenty`/`core_demo` results
are in `final-smoke`; GNOME's focused command selected
`tray.activate,tray.menu-settings,tray.menu-history,tray.menu-destinations,tray.menu-change-output,tray.scroll-noop,core_demo`.
The full tray commands selected `tray`. Earlier corner/right-edge runs selected
`__main__.original_corner,__main__.resize_five`; the move positive control
selected `__main__.witness_move_control` alongside the first tray proof.
Selectors describe retained runs, not a claim that all passed.

## Results

| Item | Evidence and limits |
| --- | --- |
| #818 | `cinnamon-input-receipt.json`: authentic production failed-upload notification, persistent timeout 0. All Maximize/Minimize/Close calls raise `RecipeFailed` naming the Cinnamon notification and its summary/body/action. After closing that exact notification ID, all three effects succeed. Saved before/refused/maximized/minimized/closed frames are retained. |
| #838 | `cinnamon-repeat-demo-receipt.json`: 20 outward/return header gestures on one persistent SSH probe process at the final head; an earlier set of 20 also passed. `cinnamon-input-receipt.json`: a controlled 100 ms fresh accessibility probe times out with SSH still running, names request/budget/elapsed/pending reply, and later Escape is withheld with the original cause. Thus channel unavailability can be deliberate poisoning after an outstanding reply, not an SSH death. This does not establish the historical report's exact cause. Alarm/closed-stream regressions also retain request identity. |
| #843 | Original lower-right corner and one right-edge drag blocked because no new exact-window/PID resize-begin appeared. No resize or minimum-size success is claimed. A separate real header drag recorded Mutter MOVING begin/end for the exact client, proving the installed hook receives native grab signals. The v4 extension in that positive control is byte-identical to the final driver extension. Two unsuccessful edge approaches exhausted the campaign desktop-quirk attempt rule; five successful resize starts remain unproven. |
| #802 | Cinnamon full tray replay and final smoke `shortcut.region` pass with the private native selector's proven keyboard grab, including guarded Escape. Unit negatives cover foreign executable/PID, stale and ambiguous accessible witnesses. |
| #831 | Additive desktop double-click/wheel use fresh accessible geometry, foreground/occlusion and existing release code. GNOME targeted activation reaches the selected History page. Per-pulse cover changes and failed release stop unit inputs. Live wheel replay remains blocked by a fresh accessibility timeout (#805), so wheel desktop acceptance is unproven. |
| #837 | Actual GNOME wire observation found AT-SPI menu role 33 versus ATK menu role 32 for the same actor. Canonical role names bridge the enums. Full replay passes Shell context menu, About, output submenu and desktop capture; targeted replay verifies selected-page effects. Foreign PID, window, bounds and semantic-role mismatches refuse in units. |
| #767 focus | Plain KDE focus verifies without requesting unsupported raise; explicit raise remains a blocked contract. Unit test covers both. No new live KDE claim. |
| #767 X11 frame | Unit frame/client tree names the foreign frame's unique EWMH client instead of “unnamed X11 window.” |
| #767 activation | Unit injects a foreign foreground between settle observation and activation; no activation is dispatched. The immediate recheck is not a server-side atomic X transaction. |
| #767 staging | Host wrapper reuses `lab.verify_owned_guest` before reading and immediately before mutation. Units refuse real/golden targets and revalidate at the write boundary. Live witness version 4 was staged only in the owned GNOME overlay and verified ACTIVE after its controlled reboot. |
| #767 roles | Server `button` remains `button` in reported rows; `button`/`push button` and catalog classes match normally. Unit reports/matching tests pass. |
| #785 identity | Held-key and multi-head pointer units observe exactly one endpoint identity check per input. The direct events API still checks. |

All four `core_demo` functional rows pass on both guests. Redact's image
comparison is retained as visual review, including the cross-Qt reference
difference; a functional pass does not convert that score into visual acceptance.

The complete 57-row tray lane was run on each guest. Raw rows remain authoritative;
the lane is not accepted as a whole. Cinnamon has 7 passed, 9 failed and 41 blocked
rows. GNOME has 6 passed, 6 failed and 45 blocked rows. Many rows explicitly require fixtures unavailable in that lane (#670).
GNOME's raw replay predates the local nested selected-state readback correction;
its results and the later targeted rerun are stored separately. The final targeted rerun has seven passed interaction rows (four core demo rows, activation, History and Destinations) and three blocked rows (Settings, Change output and wheel), each naming a fresh accessibility timeout. No failed receipt
was edited into a pass. Recording media had already been removed by that lane's
fixture cleanup, so raw “recording stopped” success is not audio/video acceptance.

## Unit proof and retained failures

`surface-unit.xml` / `surface-unit.log`: **409 tests passed in 20.03 s** on the
Boat host with the final core and locally merged tray proof source. The two new
files contain 23 guard tests and 11 tray input tests. Baseline logs demonstrate:

- Original core: 19 of the first 21 cases fail (10 failures, 9 errors); two are
  positive controls. All 21 pass after the first change.
- Before additive tray support: all nine initial tray cases error; all pass after.
- Before canonical role bridging: the captured ATK/AT-SPI enum case fails.
- Before interrupted-channel diagnostics: the two added alarm/closed cases fail.

Earlier harness mistakes remain private and indexed: the real notification's
summary is “Completed with a problem” (failure is in its body); chrome labels
need live `text:` selectors; old queued recording notifications can cover a newer
banner; a 1 ms deadline may expire before dispatch. The final isolated run used
a controlled reboot of the same overlay, the exact owned banner ID, and a 100 ms
fresh probe with verified outstanding work. The shared Wayland `history_window`
query defect is tracked in [#850](https://github.com/patrick-hudson/put.gd-desktop/issues/850);
the tray selected-state readback defect is tracked in
[#861](https://github.com/patrick-hudson/put.gd-desktop/issues/861).

## Retention and cleanup

Raw archives remain private under this worktree's `work/r3/`, with every exported
file verified against the host SHA-256 inventory. The archive index gives paths,
hashes, restore commands and exact source attribution. The committed JSON
receipts are original, unmodified copies; the public packet checksum file covers
all committed members.

After export verification, each owned overlay is stopped, moved from its short
runtime directory to the canonical cleanup directory (same disk inode; original
state and migration receipt retained), then retired with `lab.cleanup_overlay`,
an explicit reviewed mixed-outcome completion manifest, and the golden-root
inventory. Only the disposable disk is removed. Cleanup receipts and sandbox
deletion receipts are retained alongside the results. Both delete requests were
confirmed by subsequent 404 `not_found` reads. The temporary wall entries and
forwarded-key agent were stopped. No sandbox remains to list on #865. The Boat TLS outage was
observed during collection; its API responded again at 10:30Z, before deletion.


The fixture-worker PR #855 was still open at collection. Its notification/SNI
parser and semantic selector additions were not imported early. When integrating
that PR, retain its QToolButton check-box and recentColorSwatch hints, and apply
`normalize_role(row['role'])` in the added catalog-candidate class filter, just
as the existing class-selector path does. No synthetic-file typing API was
exposed; #666 remains blocked without a raw-value bypass. Release and signal
critical sections owned by the other worker were not edited.
