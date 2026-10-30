# Recording review surface recipes — round 2

Functional development evidence for code PR [#676](https://github.com/patrick-hudson/put.gd-desktop/pull/676)
and documentation PR [#697](https://github.com/patrick-hudson/put.gd-desktop/pull/697).
These boat guests do not establish desktop, release, A/V synchronization, latency,
or memory acceptance. Saved frames retain their visual-review requirement.

## Identities and scope

The recipes are `b6b3749ac9797d70920a08226522d6692dc82597`, incorporating core
`4b5e185ff9df2ae7dd578ae535da1202c58deb68`. The 31 review-owned interaction IDs
remain registered exactly once. No core driver, fixture, input, receipt, catalog,
or production application files were edited by this lane.

* Cinnamon: Linux Mint 22.3, native Cinnamon X11, clean application
  `b19a915b70ea2edef69a01be0bafd97cb0cbfa7d`, built and packaged on boat with the
  normal CMake presets and distro Qt. The later recipe changes do not change
  application source. The exact installed DEB hash is in [identities.json](identities.json).
* GNOME: Ubuntu 26.04.1, native GNOME Wayland, clean application
  `a18d79e762dfa4780522e25d6e3f63df1b3a2258`, from native Ubuntu 26.04 CI artifact
  `release-native-ubuntu26.04` in run `40969927486`. This artifact's successful
  installation and binary verification are distinct from the overall CI run,
  which failed. Its manifest is [gnome-ci-build-info.json](gnome-ci-build-info.json).
* Both use host/placement `boat`, destination `local`, synthetic content and fresh
  overlays. The named snapshots and golden disks were not modified. Newer
  Cinnamon v2 / GNOME v3 named snapshots were absent when the campaign selected
  `putgd-lab-cinnamon` and `putgd-lab-ubuntu-gnome-v2`.

The native GNOME package was used instead of the snapshot's Ubuntu 24.04-built
package. Probe installation, application installation and MCP setup were staged
in disposable overlays. The GNOME bootstrap key was pinned from #680's validator
evidence. Both guests remained on the live display wall during graphical work.

## Verification and remaining boundaries

The recipes check export bytes with ffprobe/ffmpeg or Pillow: actual container
and codec, trimmed duration, moving frames against a separately captured frozen
control, and opaque mask interiors at three retained times against the unmasked
source. CLI/MCP exports apply a two-second trim and a timed solid mask. Uploads
from review use only the isolated local output.

The [unit suite](unit-tests.txt) passes 121 tests, including the review recipe and
driver suites. It covers stale/null export acknowledgements, export path/revision
matching, dialog keyboard ownership, local MCP upload, timed mask commands and
decoder rejection cases. The separate [core regression suite](core-regressions.txt)
passed 140 tests. Unit mocks and the offscreen synthetic catalog are not graphical
proof.

Runtime control and dialog resolution blocks are coordinated in
[#672](https://github.com/patrick-hudson/put.gd-desktop/issues/672). A saved visible
control plus a zero-match refusal is a pre-input driver block, not evidence that
the application ignored delivered input. Broader CLI/MCP capture lifecycle
variants remain explicitly unexercised; review/edit/preview/save/upload evidence
does not silently pass those aggregate entries.

## Preserved initial attempts

* [Initial Cinnamon receipt](attempts/cinnamon-initial/receipt.json): 31 rows,
  1 passed / 1 failed / 29 blocked. The failure was the recipe sending chooser
  keyboard input while its guard still targeted the parent review. The fix
  explicitly focuses the chooser; discard/advice dialog keys receive the same
  ownership treatment. Control and dialog resolution refusals remain distinct.
* [Initial GNOME receipt](attempts/gnome-missing-gtksink/receipt.json): 19 rows
  before a controlled stop, all raw `failed`. The recording source covered the
  correct output but could not play because the GTK3 GStreamer sink was absent.
  The last row records the owned runner's SIGTERM, not an application failure.
  The [saved diagnostic](gnome-pattern-diagnostic.txt) shows the exact missing
  element and cover-player error. This prerequisite gap is
  [#784](https://github.com/patrick-hudson/put.gd-desktop/issues/784); a fresh overlay
  installs `gstreamer1.0-gtk3` and checks the element before the corrected run.

The original receipts and frames are preserved without relabelling their raw
verdicts. Final coverage uses the corrected complete runs and separately records
any justified setup/core classification.

## Lab lifecycle

Exactly two sandboxes were created and reused. All custom overlay retirements
use exported, SHA-256-verified evidence, explicit completion manifests and guarded
`lab.py cleanup`. The initial Cinnamon cleanup's canonical-root refusal is kept;
the corrected invocation supplied the existing external `work/lab-images` root.
No disk, golden or named snapshot was moved to bypass that check.

Only the exact owned GNOME runner PID was signalled after its command line, owner
and Linux start identity were verified. Both workstation lab-key agents were
stopped. Final cleanup receipts, complete-run counts and evidence hashes accompany
the summary; the wall entries and tunnels are removed only after collection.

The corrected GNOME setup removed the golden's legacy `0.1.1~dev.63` package,
installed the native CI DEB, and rebooted the owned overlay so GNOME loaded the
current put.gd extension. The GTK3 sink check passed before the full run. An early
post-reboot image still showed boot text; it is retained privately and is not
called desktop evidence. The later [console check](gnome-unlocked.png) visibly
confirms the unlocked GNOME desktop. The [GNOME-host unit run](gnome-unit-tests.txt)
also passed 121 tests. A staging check's initial shell-quoting failure and its
corrected heredoc command are preserved in the private evidence archive.

The corrected GNOME recording setup reaches a second, independent prerequisite:
[the real ScreenCast consent dialog](gnome-screencast-consent.png). Acquisition is
`portal`, capabilities report recording available, and the delivered
`recording.prepare` remains `preparing` with zero duration and no resident error.
The native session is active Wayland. The shared fixture waits for `ready` without
a consent input step; it reports a generic failed timeout. This is
[#791](https://github.com/patrick-hudson/put.gd-desktop/issues/791), not evidence of
an application regression. The canonical coverage classifies these shared-fixture
timeouts as blocked and preserves every original runner verdict. No manual grant,
portal bypass or third desktop attempt was used to turn them into passes.

A final recipe-only correction scopes destination-menu accessibility inspection to
visible, showing popup nodes in the private XDG family. `window: "review"` was a
literal title glob, not a window-role alias, and could never match the actual
review title. An unrelated footer label cannot satisfy the corrected popup check.
The [final unit suite](unit-tests-final.txt) passes 122 tests. The recorded desktop
runs are still the exact earlier recipe commit above: they block before this
query, so no new graphical pass or third desktop attempt is claimed for the fix.

## Complete-run counts

| Guest | Passed | Failed | Blocked | Issue |
| --- | ---: | ---: | ---: | --- |
| Mint Cinnamon X11 | 2 | 0 | 29 | #672 |
| Ubuntu GNOME Wayland | 0 | 0 | 31 | #791 |

These are classified development counts. GNOME's original runner receipt has 31 failed readiness timeouts; [classifications.json](classifications.json) records their issue-backed conversion to blocked. Cinnamon's raw and classified functional counts agree. [summary.json](summary.json) retains both sets and the full environments.

Cinnamon's passing interactions are `review.preview-error` and `mcp.recording-review`. Its CLI/MCP copy and explicit local-upload files passed actual decoding: 2.0 s, H.264/MP4, moving content, and the timed solid-mask interior checked at three source times against the original. Other formats and graphical mask/trim controls remain blocked; no all-format or whole-lane pass is claimed.

## Acceptance checklist and cleanup evidence

* Counts: all 31 owned IDs appear once per complete guest run in
  [summary.json](summary.json); every blocked row has #672 or #791 in derived coverage.
* Tests: [122 final unit tests](unit-tests-final.txt),
  [122 post-run boat-host tests](gnome/unit-tests-final.txt), and
  [140 separate core regressions](core-regressions.txt) pass.
* Coverage: the code PR commits schema-v2
  `tools/lab/surface_map/coverage/2026-10-30-surface-driver-review.json`, with host and
  placement `boat`, the actual guest names, and destination `local`. Its evidence
  links are pinned to the documentation commit.
* Overlay cleanup: the guarded retirements completed for
  [initial Cinnamon](cleanup/cinnamon-initial-retired.json),
  [final Cinnamon](cleanup/cinnamon-final-retired.json),
  [initial GNOME](cleanup/gnome-initial-retired.json) and
  [final GNOME](cleanup/gnome-final-retired.json).
* Export verification: [Cinnamon](cleanup/cinnamon-export-verified.json) records
  467 verified private evidence files; [GNOME](cleanup/gnome-export-verified.json)
  records 350. Their SHA-256 archives and restore index remain in private work,
  outside release source. This public packet has its own [hash index](sha256.json).
* Both sandbox deletions are confirmed completed:
  [Cinnamon](cleanup/cinnamon-deleted.json) and [GNOME](cleanup/gnome-deleted.json).
  The exact owned wall entries/tunnels were removed; both lab-key agents stopped.
* CI status for the final PR heads is recorded in their acceptance comments.
  No merge, issue closure, external review request, A/V gate, physical desktop or
  workstation build was performed. Graphical/release acceptance remains blocked.
