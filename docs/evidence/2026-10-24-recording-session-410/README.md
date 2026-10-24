# Recording session contract — issue #410

Companion documentation [PR #550](https://github.com/patrick-hudson/put.gd-desktop/pull/550)
for [issue #410](https://github.com/patrick-hudson/put.gd-desktop/issues/410),
covering graceful cancel/discard shutdown, `action.invoke`'s recording `mode`
and `stopReason` after a command stop. The code and documentation PRs target
`nightly`; merge [code PR #551](https://github.com/patrick-hudson/put.gd-desktop/pull/551)
before this companion. The [recording contract](../../recording.md)
retains private review and explicit Upload as the only admission path.

## Evidence status

**Pending.** No current-candidate build, regression, desktop or memory result
has been accepted in this report. [The JSON ledger](summary.json) keeps those
gates explicit. This documentation PR stays draft until the execution owner
supplies exact-commit receipts and the report owner reviews them, including
saved Cinnamon, GNOME and KDE visuals and separate functional/memory gates.
The older reproduction and test counts in #410 describe that issue's original
source; they are not results for the new candidate.

The candidate code identity is `b11c3ba6d64a60ace215aeb8ef9ac9cd8418f142`,
tree `f9d1b5e55bb37d76c2522b6229b52faa5af7b6bb`. This identifies the proposed
implementation, not a tested build. The shutdown guard follows envelope/ready
validation, the session boundary confines `mode` to `record`, and the stop-reply
guard preserves an already-finalized review when a budget event wins the race.
The regression source checks that race and the later duplicate stop separately.

## Required receipts

- Clean source commit/tree, build manifests and running executable hashes;
  exact host, distribution, desktop/compositor, session type/ID and Qt platform.
- Regression failures on the unfixed source and passing checks on the fixed
  source, with commands, test names, exit status and repetition counts. Fixture
  or offscreen checks establish the session/protocol contract only.
- Per-desktop `open`/`toggle`, command stop reason, ready/recording cancel and
  review Discard checks, graceful helper exits, private staging/admission
  results, and an independent memory window with duration, limits and host load.
- Saved complete desktop frames for decisive actions, failures and completion;
  file hashes and report-owner visual review. A live wall tile or SSH log alone
  is insufficient. Keep raw bulk evidence private.
- Socket path preflight, physical desktop lock/heartbeat/release receipts where
  applicable, owned-process cleanup and VM retirement/export receipts if used.

Preserve failed attempts and corrected commands. A missing or failed gate stays
pending or failed; this record does not certify unrelated release profiles,
physical performance, package lifecycle or published assets. Retention follows
[the evidence policy](../../evidence-retention.md).
