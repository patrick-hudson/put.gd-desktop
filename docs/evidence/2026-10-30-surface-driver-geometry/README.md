# Cinnamon geometry and run-hygiene acceptance

Code PR [#736](https://github.com/patrick-hudson/put.gd-desktop/pull/736), core
`02c99c0fe65ae12ac3b9008a0d35cd52f86a9af8`, tested with the desktop worker's
`fe423c0192373424bde6414b5eb65a94629f52a9` probe and its `focus`,
`start_notifications`, and `notifications` context methods. This composition is
recorded by file SHA-256 in [acceptance.json](acceptance.json); it is not a claim
that the peer branch has merged.

The native Mint 22.3 / Cinnamon 6.6.4 X11 guest ran on boat bare metal sandbox
`bx_y3ym9kye`, forked from `putgd-lab-cinnamon`, with KVM, 3072 MiB, two CPUs and
two QEMU display heads. Run UUID: `b18d6a21-e105-403b-b6d7-9d34ce8ba0fc`.
Distro Qt was 6.4.2. The installed DEB was built from application commit
`550a3b808d5a485ad3033df4128e8e0542d91e19`; version and package checksum are in
the JSON. Guest displays stayed on the live wall throughout the campaign.

| Gate | Observed result |
| --- | --- |
| History at default 1320×880, catalog 1440×960 | Gallery objectName, List text/accessible alias, search and clear all passed. |
| Live catalog comparison | History no-matches distance 0.0118535, tolerance 0.025, **reviewed-ok**. |
| Recording bar | Real format, record, and cancel clicks passed for a 960×1080 half-screen region and a 1920×1080 full-screen region. Live bars were 789×52 and 797×52, both on head 1; synthetic catalog bar is 795×52. An additional 960×540 region passed on head 0. |
| Two-head QMP drag | At 1280×800 + 1024×768, requested `[320,200,640,400]`, actual `[320,200,640,400]`; 0 px error. |
| Preflight | Missing xdotool, xclip and python3-pyqt6 blocked before resident launch; provisioning produced an empty missing list. |
| Hygiene | Own notification retained; foreign and spoofed native-app notification bodies absent. Text, HTML and custom clipboard MIME restored after fixture copy. Successful private roots removed. Forced deadline left X11 button mask 0. |
| Tests | 68 Python driver tests; three CTest contracts passed. All three core_demo functional recipes passed on Cinnamon. The redact demo's synthetic-content comparison still requests review. |
| Guard performance | 12 characters: 19.919 → 1.441 s, 24 → 0 new SSH sessions. 12-step drag: 18.291 → 1.114 s, 26 → 0 new sessions. Fresh geometry/foreground observations still run per point/character over the persistent channel. |

The timing comparison precedes the peer's popup-observer-only change and uses
the same native X11 witness protocol. A final repeat with fixed-head points
blocked after the window relocated outside that head; that failure is retained.

These are original native screenshots:

- [History client crop](history-1320x880.png)
- [Half-screen recording bar](bar-half-ready.png)
- [Full-screen recording bar](bar-full-ready.png)
- [Two-head recording result](twohead-ready.png)
- [Destination actions menu](destination-menu.png)

Two issue-level gates remain open. For #718, two managed CSD resize attempts
produced one exact 1440×960 result and one safe abort when the foreground guard
reported Desktop. `WM_NORMAL_HINTS` has minimum 1060×680 and no maximum;
`_NET_FRAME_EXTENTS` is absent. This rules out a declared maximum but does not
establish the WM/grab cause. Per the campaign rule, no third resize was attempted.
Default-size History needs no resize. For #712, the correct Destination actions
menu visibly opens at 1320×880, but its Rename item remains absent from the
accessibility result, so the full rename recipe fails; this overlaps the existing
menu issue #675 and the desktop worker's scope.

Private bulk evidence retains successful and failed receipts, all saved visuals,
functional artifacts, exact scripts and composed sources, the tested DEB, build
logs, and checksummed QMP witnesses with a verified restore index. Clipboard
bytes and guest identity private keys are excluded. The code PR acceptance
comment records the export and retirement receipts. No physical desktop,
workstation build/VM/GUI driver, S3 credentials, or existing named snapshot was
used or modified.

After core advanced with #734, the geometry branch merged `bb3012c0` and reached
`37217c45b1db8d8e8500770d96cdd562e9ae1347`. The exact branch source passed all
97 driver tests on boat. A fresh disposable guest overlay then reran all three
core_demo recipes with that head plus the same frozen desktop peer; all passed
functionally, including the newly merged cleanup between recipes. The native
[text-tool frame](core-demo-text.png), [merged check identities](merged-check.json),
and [original campaign retirement summary](retirement.json) are retained here.
The original live gates above keep their original source identities.

Issues #705, #714 and #709 remain open for the coordinator to close after
independent validation and merge. #712's Rename item belongs to the
2a-desktop/#675 runtime-menu work. #718's unresolved resize guard is handed
back in geometry/WM scope after the two-attempt limit. Provisioning is a step
only: the coordinator schedules a new golden/snapshot after merge.
