# Issue #5: Linux release acceptance audit

Date: 2026-10-13. Issue: [#5](https://github.com/patrick-hudson/put.gd-desktop/issues/5).

This audit starts from main commit `b1ed4ae6c2128ad189889b61c18798994ebf210c`,
source tree `292c9e298a1e4d8a83edafa83c7cc5e1c4ad2187`, commit count 144.
It validates the issue's older claims before proposing changes. This is an
acceptance record, not authorization to publish a release or a statement that
all U9/U10 gates passed.

## Reproduced failures and corrections

| Case | Observed baseline | Correction and verification |
| --- | --- | --- |
| Lab launch on a new SSH forwarding port | Strict SSH could not authenticate because `known_hosts` pinned another endpoint; the runner had already allocated and booted the disposable guest | Check the selected endpoint with `ssh-keygen -F` before allocation. Regression failed before the fix. Review added plain/hashed positive-entry cases and caught the default-port-22 spelling; both default-port cases failed before correction and all 17 lab contracts then passed. Actual negative CLI preflight returned an actionable error without allocating a run. A corrected positive launch authenticated and booted a fresh Cinnamon overlay on port 22432, then stopped it; this proves launch only, not graphical acceptance. Strict host-key checking remains enabled. |
| Documented MCP suite on Python 3.14.7 | All forwarding assertions reached cleanup, but unconditional socket unlink raised `FileNotFoundError`; newer asyncio removes the listener socket on close | Make cleanup tolerate an already-removed socket, retain the unavailable-session assertion, and check socket length before binding. All three tests passed on both Python 3.14.7 and distro Python 3.12.3. |

Python's listener cleanup behavior was added in 3.13; see the official
[asyncio Unix-server documentation](https://docs.python.org/3.14/library/asyncio-eventloop.html#asyncio.loop.create_unix_server).
The MCP verification used the production CLI built as
`0.1.1-dev.143+g0f930a4b89bc`, whose embedded source tree is exactly the baseline
tree above. These are synthetic transport contracts, not graphical acceptance.

The integrated working branch passed `cmake --preset dev`,
`cmake --build --preset dev`, and all 57 `ctest --preset dev` tests in 437.43s.
The independent clean-baseline development build passed all 57 tests in 401.44s.
Build identities and original failures remain in the private run receipts.

## Fresh-agent production walkthrough

The worker began without prior chat history or earlier private campaign scripts,
read the committed instructions, built official AWS SDK 1.11.907 with its 15
pinned submodules using two build jobs, and independently built the clean source.
The release build passed all 57 tests in 400.29s. A separate production package
was configured with tests, lab fixtures and physical benchmark drivers disabled;
the independent paste fixture was retained outside that package.

On Cinnamon/X11, the first region operation
`afa26b3c-655a-480b-b168-b19c3f14add3` completed its local save but suppressed the
automatic copy with an empty operation error. An unchanged repeat,
`a0491df9-9215-441e-9c16-3ca46f2a64f7`, passed automatic copy, the complete 80×60
pixel oracle and independent native paste. The repeat does not explain or fix
the first failure. Both receipts and witnesses were retained. Three later unchanged-runner probes
(one existing profile, two fresh profiles) also passed with passive XFixes
ownership metadata tracing; the observer never requested clipboard payloads.
Their operation IDs are `139501d7-149b-45ef-99bc-2a71d71dac21`,
`8fa4a3e6-c50e-49fb-a25f-5e6542b238d8`, and
`61d1ae59-2f5a-432f-b59a-b25f8860f828`. These passes do not identify the original
cause. No speculative clipboard change was made, and the original unexplained
failure blocks expansion to sibling desktop profiles.

A probe setup attempt failed while replacing a still-running independent fixture.
That attempt invoked no capture. Its receipt is preserved; the corrected driver
stops only the fixture at its exact owned executable path before replacement.

The production resident was exercised through all 12 documented MCP tools using
the official SDK. CLI and MCP views of the upload and deletion records matched.
One unique synthetic S3 object (30 bytes) was published, fetched with matching
SHA-256, copied through both operation and History routes, and pasted into the
independent native fixture. Exact prepared deletion effects were inspected;
an incorrect acknowledgement was rejected; the exact selection then completed
with `removed` and provider phase `done`.

| Identity | Value |
| --- | --- |
| Upload operation | `e38fd33e-5866-4402-975e-491a2b37366b` |
| History record | `ce044873-534a-4581-b6dd-4b50a5911ecb` |
| Deletion operation | `50b8e5c2-7c44-4da5-ba8d-c98faad4c8cd` |

A deliberate malformed external configuration produced `config_invalid`, retained
the accepted document, and recovered after correction and reload. A stale patch
revision was rejected. An MCP notification-preference edit was visibly reflected
in the open Settings window at the recorded accepted revision.

Credential preparation used the source-derived, owner-only Settings session API
with encrypted SSH stdin. This is test setup, not a documented raw-credential
CLI/MCP feature: the public CLI intentionally rejects that operation and directs
credential entry to Settings. No key values were stored in test configuration or
logs. Full U9 branch coverage, GUI History/deletion parity and current compositor
release acceptance are not inferred from tool discovery or these bounded cases.

## Published package baseline

The upgrade baseline is the published nightly
[`v0.1.1-dev.133.20261013004314+gb0ed0461c600`](https://github.com/patrick-hudson/put.gd-desktop/releases/tag/v0.1.1-dev.133.20261013004314%2Bgb0ed0461c600),
commit `b0ed0461c600de74e0a924057a59722f2b1851a3`.
All eight entries in its outer checksum list matched the downloaded files;
the checksum file also matched GitHub's asset digest. The combined ZIP contained
its seven underlying assets and an inner checksum list, with identical underlying
bytes. The Ubuntu 24.04 DEB passed the production native audit: seven program
identities, runtime module and private SDK hashes, dependency resolution,
64 installed inventory entries, and license notices.

The audited Ubuntu 24.04 DEB SHA-256 is
`f709f199165c9372ad0a7f0c4dee34020a8661b342d36717c34136378d1cd092`.
These archive/package checks do not establish installed desktop behavior.
Both source 133 and source 144 already use history schema 5. Upgrading between
them tests state preservation; it must not be described as exercising a schema
migration. Existing history tests separately cover legacy migration and rollback.

## Installed Cinnamon upgrade and populated History

All installed checks used a disposable overlay of the sealed Cinnamon base,
with its localhost VNC display kept visible. The normal-home application profile
was absent before this lifecycle test. The older published source133 DEB was
installed, customized with local-only actions and a synthetic unused keychain
reference, and used to create local capture operation
`7e01688f-843a-4961-be3f-35742bc9fcc9`; capture and automatic copy completed.

With the resident stopped, its database was backed up and populated to 10,000
records. The added records use an unavailable fixture provider, invalid example
URLs and synthetic deletion outcomes; they represent no cloud uploads or deletes.
The old resident successfully loaded the populated ledger before upgrade.

After stopping the old resident, installation of the clean source144 production
DEB preserved every normal-home configuration, data and output file byte for byte.
The new resident returned the same settings document, final history page, removed
records and retained capture operation. Both versions use schema 5; this is
upgrade preservation evidence, not migration evidence. The live process hash was
verified against the installed app, so an older surviving process was not counted
as a successful upgrade.

The upgraded installed History window visibly showed 10,000 entries, paged to
entries 51–100 and filtered the fixture name `09900` to one record with the expected
removed state. These observations establish bounded VM functionality, not a
physical-machine responsiveness or memory budget.

| Artifact | SHA-256 |
| --- | --- |
| Current production DEB | `6fb8197b837b350e7a01f5bd5b52ec19172d19dce72e616b024322028115e441` |
| Old resident bytes | `382f76a9bf3987630a3389f4103f5a5a4077e4a5d3160fc48190a96ad6388f7c` |
| New resident bytes | `a7825869d977364425179939cd76b8634c8e1cf4610bd063f2246cd7b3b48006` |

## Login, removal and memory boundaries

A five-minute production-resident observation after the fresh CLI/MCP/live-S3
walkthrough passed at **27.410 MiB** peak RSS plus swap. After upgrading the
populated ledger and browsing History, a separate unchanged 300.142-second
observer run collected 61 samples and passed at **30.949 MiB** peak RSS plus swap.
Both windows had zero swap, one resident, no UI process and no escaped helper.
These are bounded Cinnamon VM results, not physical-machine performance evidence.

The first logout/login attempt retained the SSH-started resident's PID and start
ticks, so it was rejected as autostart proof. The corrected attempt stopped that
resident before logout. Login through the visible console then started a new
installed resident with the expected package hash, preserved normal-home settings
and working journal. Settings opened during startup; this result does not certify
background-only login behavior. The protected login input stayed only in memory
and was typed through the focused masked console field.

Package removal deleted every inventoried package-owned file. Configuration,
credential references, the 10,000-record ledger and local outputs remained byte
identical before removal and after the next real login. No put.gd process started
on that login. The managed per-user autostart file remained but was inactive with
the executable absent. This does not establish literal removal of every per-user
integration file; no package script was added to traverse or delete user homes.

## Remaining acceptance

Issue #5 remains open. This audit does not complete U9/U10 or certify a release.
The following gates still lack passing evidence for the candidate:

- Explain or reproduce the initial Cinnamon automatic-copy suppression before
  expanding to new GNOME/KDE acceptance runs.
- Establish background-only login behavior and resolve the contract boundary for
  the retained inactive per-user autostart entry.
- Run native package/helper install, upgrade, removal and live configuration cases
  on the remaining declared desktops. Earlier bounded #3/#4 receipts remain valid
  for their own source identities and workloads only.
- Complete the plan's remaining fresh-agent/GUI parity and display geometry cases,
  including mixed displays; physical Cinnamon capture latency and populated-history
  performance; upload bursts and induced slow-network workloads.
- Complete the exact release-asset license/lifecycle checks before publication.

The native package evidence above applies to clean baseline source144. The
workflow fixes and documentation on this branch do not change application runtime
code, and their later Git identity must not be substituted into those receipts.

## Related acceptance

- [Issue #3 closure](https://github.com/patrick-hudson/put.gd-desktop/issues/3#issuecomment-6149342985): cold capture could not be reproduced on the recorded current builds. Its local-only tests do not prove which earlier change resolved the report.
- [Issue #4 closure](https://github.com/patrick-hudson/put.gd-desktop/issues/4#issuecomment-6152173663): current-source bounded live-S3, clipboard/history/deletion and five-minute post-use memory checks across Cinnamon, GNOME and KDE, plus the earlier source114 matrix. Preserve each record's exact identities and workload limits.
- Main [Build 38438946727](https://github.com/patrick-hudson/put.gd-desktop/actions/runs/38438946727): Linux verification and Windows/macOS compilation. The latter remain compile-only platforms.

## Evidence handling

Private commands, failure logs, package audits and graphical receipts are retained
under `work/issue5/` on the test host. This directory is not a release or review
artifact and must not be uploaded wholesale. Cloud tests use synthetic payloads,
isolated names and exact receipt-based cleanup; local credentials and protected
login inputs stay out of logs and committed evidence. Lab bases stay immutable;
only owned disposable overlays may be changed.

The test resident and independent fixtures were stopped. The lifecycle VM is
powered off; its private overlay and exported evidence are retained for the
unresolved first-use investigation. No golden disk or unrelated VM was changed.

The positive-launch probe VM was also stopped. After checking every Linux disk
backing reference and live QEMU argument, its unreferenced disposable overlay was
removed. Its launch, socket-preflight and cleanup receipts remain readable.
