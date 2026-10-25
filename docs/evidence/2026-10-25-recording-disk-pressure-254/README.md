# Recording preservation under competing disk writes — #254

Code: [PR #572](https://github.com/patrick-hudson/put.gd-desktop/pull/572),
`25b36396ea408c53ff2b0d2daa9572a2faad9cd6`, based on nightly
`80ffa2e6c0d34223d2cc0f31254f2e60ac2bd247`.

**This is bounded filesystem and Xvfb integration evidence, not native desktop
acceptance.** Current-source Cinnamon, GNOME and KDE functional/memory gates
remain pending with the parent lab coordinator. Windows is outside this task.
No issue closure or release acceptance is claimed.

## Reproduction and result

The issue began as a code-inspection hypothesis. On Fedora 44 (`f44`,
`192.168.68.70`), the baseline production resident/CLI/recording helper captured
a synthetic moving ball on an independent Xvfb display. A separate Python
writer filled a disposable 1536 MiB tmpfs mounted **only** over this resident's
private recording staging. The journal, logs, configuration and host filesystem
remained outside that mount. Baseline run `d254b1` ended with generic
`recording_failed`, “No space left on the resource”, an unavailable recording
session and a failed operation with no items. No review opened.

On the candidate, the same production path samples free space and stops to
private review before the paced writer finishes. The writer continues to zero
free bytes on that disposable filesystem. After removing only its filler,
preview, local MP4 export and full FFmpeg decode succeed. The local archive
stays empty until a separate explicit Upload call. That Upload is local-only
and completes without a network destination.

| Final-source run | Scenario | Result |
| --- | --- | --- |
| `d254f2` | Separate paced writer fills 1536 MiB mount to zero | `budget` / `disk_space`; review, preview, export/decode and explicit local Upload pass |
| `d254p1` | Same pressure while paused | Same preserved review and local output |
| `d254m1` | Exactly 1216 MiB volume: 1 GiB system reserve plus minimum 192 MiB admission | Stays recording; explicit user stop, review and export pass |
| `d254n1` | Normal pause/resume and explicit stop | `stopReason: user`; export/decode and local Upload pass |
| `d254r1` | Writer reduces free space to 1200 MiB, above the 1184 MiB threshold, then releases it | No automatic stop; explicit user stop and export pass |
| `d254c1` | 2 MiB max-file setting, 1 MiB working budget, synthetic snow | `budget` / `max_file_bytes`; review and export pass |
| `d254e1` | Freeze only this helper, rapidly fill its mount to zero, resume it | `recording_disk_full`, `reason: disk_space`, `finalized: false`; failed operation, no review claim or uploaded items |
| `d254f3` | Repeat disk pressure with official-SDK MCP inspection | MCP `get_recording` agrees with CLI stop reason and all stop figures; preview/export/decode and explicit local Upload pass |

All eight final-source runs report successful unmount cleanup. Their manifests,
binary SHA-256 values, stop/operation results and MCP receipts are in
[summary.json](summary.json). Earlier `d254f1` is explicitly identified as an
intermediate `f4565f22` run; it is not substituted for the final source.

The initial baseline used a rapid writer; the final positive pressure protocol
paces 8 MiB writes by 50 ms. The abrupt-race case deliberately removes that
spacing and freezes only its own recording helper. These are different cases,
not a claim that periodic sampling prevents an arbitrary zero-space race.

## Safety and identities

Each run creates a new mode-0700 directory under `/tmp/d254*`. Before launching
the resident it runs `tools/diagnostics/check_socket_paths.py --ui-output RUN`;
all resolved runtime/session socket paths fit Linux's 107-byte limit. Before any
fill, the runner verifies the exact mountpoint, `tmpfs` type, disposable source
name, device ID and capacity. The writer independently verifies its open file's
device ID and filesystem capacity. It never follows a host free-space target.
Only its filler is removed; the mount is unmounted after owned processes exit.
No golden images, VM overlays or physical desktop sessions are used.

The host uses dynamically linked distro Qt 6.11.2, GStreamer 1.28.8, FFmpeg 8.0
libraries, C++20 and the existing official pinned AWS SDK. Preset configuration
uses the Fedora SDK's `lib64` runtime location and its existing SDK source for
the required license. Configure attempts and logs are retained, including the
initial missing runtime/license path failures; no substitute toolchain was made.

The saved visuals below were inspected: they contain only the independent
synthetic Xvfb display and recording UI, with no physical desktop or user data.

![Baseline returns to the synthetic display without review](pictures/baseline-failure-xvfb.png)

![Candidate opens private review with the live disk-pressure notice](pictures/review-xvfb.png)

## Threshold and tests

The [shared reserve](../../../src/core/DiskSpace.h) keeps the existing 1–4 GiB,
5%-of-capacity rule. The [live threshold](../../../src/record/RecordingStorage.h)
uses `2 * max(clipBytes, 64 MiB) + 32 MiB` above that reserve. The working clip
already consumed its portion of free bytes; only the two future copies are
counted here. The additional 32 MiB is conservative drain headroom for encoded
queues and Matroska finalization, not an export-size promise or a reservation
against other writers. It stays below the third 64 MiB admission budget, leaving
32 MiB of usable space at the minimum admission boundary. The 1 GiB minimum
system reserve remains separate and is counted once.

`recording_model_contract`, `recording_service_integration` and
`recording_helper_integration` pass 3/3 on the exact final source; see
[i254-focused-final.log](i254-focused-final.log). Coverage includes threshold
boundaries, minimum admission, recovery before threshold, unknown free space,
overflow-safe estimates, preserving the triggering sample after space recovers,
private review/export behavior, failure journal details, existing stop races and
helper shutdown/cancellation. The first service run failed because its assertion
expected exact details while the journal adds `step: null`; the assertion now
checks the meaningful fields and the whole suite passes. The failure log remains
in the private retention directory.

No idle-resident timer or media dependency was added. Sampling belongs only to
the active transient recording pipeline and stops with it. The native functional
and memory gates are still required to measure the deployed behavior.

## Reproduction and retention

The exact evolving lab harness is retained privately on f44 at
`/home/patrick/work/i254-evidence/`, including source snapshots per later run,
logs, clips, synthetic screenshots, parameters, identities and cleanup results.
[SHA256.json](SHA256.json) indexes that private directory; it is not a claim that
all indexed files are committed here. Nothing was deleted or compressed.

[reproducer.py](reproducer.py) and [mcp_inspect.py](mcp_inspect.py) snapshot the
final lab recipe. They are builder-specific evidence scripts, not a release gate
or an installer. They require a fresh private run path, distro Xvfb/GStreamer/
FFmpeg, authorized `sudo -n mount`, and the locked MCP environment. The captured
paths are the real paths used on f44. For example:

```sh
nice -n 10 python3 /home/patrick/work/i254-pressure.py \
  --build /home/patrick/work/i254/build/dev --run /tmp/d254f3
# Other independent runs use --paused, --scenario minimum|normal|recovery|budget,
# or --exhaust. Never reuse an existing run directory.
```

The parent thread `3939c6ca-6ea9-488c-aa60-ef4898cfcb50` owns native lab allocation,
quiet-host admission and the required parallel current-build desktop gates.
