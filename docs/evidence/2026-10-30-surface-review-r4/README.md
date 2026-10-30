# Review lane R4: implementation ready, replay gate blocked

Code PR #941 targets `feat/surface-driver-core`. **The R4 graphical campaign did
not complete and is not accepted.** The new coverage rows are campaign holds under
[#943](https://github.com/patrick-hudson/put.gd-desktop/issues/943), not claims that
31 interactions executed on each desktop.

| Desktop | R2 passed/failed/blocked | R4 coverage passed/failed/blocked |
| --- | --- | --- |
| Cinnamon X11 | 2/0/29 | 0/0/31 — #943 |
| GNOME Wayland | 0/0/31 | 0/0/31 — #943 |
| KDE Wayland | Not run | 0/0/31 — #943; consent prerequisite #822 |

The unchanged stopped-attempt checkpoints have different, incomplete counts:

| Guest | Raw passed/failed/blocked | Interpretation |
| --- | --- | --- |
| Cinnamon | 0/13/2, 15 rows | Two selector refusals, five prerequisite deadlines, eight controlled-interruption failures |
| GNOME | 0/0/31 | Termination placeholders written by the runner; not 31 completed UI tests |
| KDE | 0/0/16, 16 rows | Consent refuses input because no unique enabled Share button is resolved |

[summary.json](summary.json) separates these raw counts from gate coverage.
No application regression is established by these attempts.

## Changes and observed boundaries

The lane adopts `recording-lifecycle` for production CLI/MCP prepare, selection,
start, pause/resume, overlay, stop/private-review and cancellation, removing their
stale unconditional blockers. Advice also uses the authentic export-failure fixture.
Cinnamon exposed a catalog-tooltip versus live-accessible-name mismatch. The
corrected selectors use stable names such as Playback speed, Play, Add mask and
Output format, plus the actual Restore label and a scoped destination button.
Only review recipes and their tests change; core and application source do not.

The actual first Cinnamon/KDE recipe source is `57f9f9600b80b31189fb0a0710d981ec16145f2e`;
GNOME uses the later selector correction recorded in [identities.json](identities.json).
All guests install the published `v26.1030.3203-nightly`: ubuntu24.04 DEB for
Cinnamon, ubuntu26.04 for KDE/GNOME, with verified SHA256SUMS and the matching
CI 41111404666 catalog. **No application compilation occurred.** All work used
three disposable boat forks; no physical desktop, A/V, memory or latency gate ran.

GNOME's staged witness was disabled after reboot. The initial UnknownMethod
refusals are a staging failure. It was subsequently enabled and version 4 ACTIVE
plus the Windows interface verified ([state](diagnostics/witness-enabled-after-reboot.txt),
[interface](diagnostics/witness-interface.txt)); no corrected GUI pass is claimed.
KDE's unresolved consent selector remains tracked in
[#822](https://github.com/patrick-hudson/put.gd-desktop/issues/822).

The shared blocker is sustained CPU in `redact.py:spellings` while publishing
receipts, outside the recipe action deadline. Verified SIGTERM requests did not
finish; exact owned PID/start/argv checks preceded the eventual pidfd SIGKILLs.
[Python stack](diagnostics/a-python-stacks.txt), [native stack](diagnostics/a-native-stacks.txt),
[synthetic reproducer](diagnostics/redactor-repro.py), [diagnostic result](diagnostics/redactor-repro.txt).
No redaction fence was disabled or core file patched to manufacture a pass.

## Evidence, checks and cleanup

Both corrected review/driver suites pass **134 tests** on
[Cinnamon](tests/cinnamon-semantic.txt) and [GNOME](tests/gnome-semantic.txt).
The initial suites passed 133 on all three hosts, including [KDE](tests/kde-initial.txt).
Unit tests do not establish graphical acceptance. Exact-head CI is recorded in the PR comments.

Cinnamon's first fixture captured moving/frozen clips (motion about 47.64/0) and
retained three private media files with verified hashes. No new trimmed/masked
export pass was reached in R4. Raw run trees and clips remain private because the
runner's final secret scan did not finish; public content is limited to selected
metadata, source-only stacks, test logs and manually inspected synthetic visuals.
[Archive verification](cleanup/cinnamon-export-verified.json) records the retained
49 Cinnamon files; KDE has 41 and GNOME 40. Private restore instructions and all
operational failures/corrections remain in the work archive.

All three overlays were retired after verified export using explicit failed-run
completion manifests. The initial running-writer cleanup refusal is retained;
the corrected sequence resets the owned VM before guarded preview/retirement.
[Cleanup receipts](cleanup/) include completed retirements and sandbox deletions.
Owned wall entries/tunnels and the workstation lab-key agent were removed.
Named snapshots, goldens and other sessions were preserved. No merge, issue closure
or external review request was performed.

At setup the lane coverage contained R2 rows only, with no R3 GNOME rows to merge.
Historical R2 receipts remain in the existing immutable evidence packet. Current
R4 rows explicitly say campaign-gate and retain the open blocking issue.
