# Proton Drive storage

Proton Drive is a provider in the shared upload, retry and history pipeline. The
Qt app in a Proton-enabled build uses a privately packaged executable built
from the official Drive SDK and pinned official account/CLI sources. Users of
that build do not install a CLI, Bun or Node.js. This is a third-party integration, not officially supported by Proton.
put.gd ships and maintains it as an experimental production integration.
The 2026-10-15 product decision accepts upstream's readiness position; ongoing
compatibility, packaging and user recovery are put.gd responsibilities.
See [building and verification](#building-and-verification) for build checks,
[release gates](#release-gates) for ongoing acceptance requirements and the
[maintenance procedure](#maintenance-procedure) for SDK updates and migrations.
The [2026-10-15 audit](history/proton-drive-release-gates.md) preserves the earlier
integration decisions and source-specific receipts.

The unmodified official CLI cannot supply the upload receipt this application
needs: its pinned upload command discards completion node/revision IDs and
prints aggregate transfer counters. Its sharing password is an argv option.
The private helper therefore calls the same official SDK directly and retains
the official CLI/account foundation for authentication. It does not reimplement
Proton cryptography. This replaces the initial CLI-only candidate.

In Destinations, add a Proton Drive destination. Non-secret edits save automatically.
Choose Connect account and finish sign-in in the browser. Authentication stays in the desktop secret service
under put.gd's own namespace; it does not use the official Drive CLI's session.
Choose a top-level folder in My files or explicitly create a new put.gd folder,
and the selection saves automatically. Settings retain its display name alongside
the folder ID so the picker still shows the selected folder after reopening.
The Refresh folders button beside Upload folder updates a renamed folder’s display
name; uploads use the ID. Queries show Loading folders while working and preserve
the known account email and selection, including when switching destinations.
Existing configurations without a name keep working. Account setup never uploads a file.

Connect stores the session in the secret service's default collection (the
Secret Service `default` alias). A secret service without one refuses the write,
for example gnome-keyring with no login keyring and no prompt to create a default
one. Connect then fails with "The private keychain is unavailable."
(`keychain_unavailable`), and token refreshes report the same error. Create or
unlock a default keyring, then connect again. KWallet and KeePassXC secret
services were not tested; see [the round-11 evidence](evidence/2026-10-20-round11-proton/README.md).

Use **Test connection** to check that the saved account and selected upload folder
are still accessible. A successful check satisfies the current session's
untested-destination warning when choosing **Make default**. This check is
read-only: it does not verify upload permission, file transfer, or share-link
creation. Its result states those limits explicitly.

Proton enforces each plan's per-file size limit on the server; the pinned SDK sets
none of its own, so import does not reject large files. A file over the limit fails
with `destination_file_too_large_for_plan` (service error 200003), naming the file's
size and the plan limit, rather than `quota_exceeded`, which means the drive or a
sharing limit is full. After either refusal the helper asks Proton whether the file's
name is still free in the folder. Unfinished drafts hold their names too, so a free
name means nothing was created and the item fails; Retry then uploads again without
reconciling. A taken name or a failed check leaves the item unknown until Retry
reconciles it. On a free 5 GiB plan, tested live on 2026-10-20, Proton refused every
oversized file at draft creation with the storage quota (`quota_exceeded`, service
error 200002) before any per-file limit, from 5.5 GiB to 100 TiB, so 200003 was not
observed; see [the round-10 evidence](evidence/2026-10-20-round10-proton/README.md).

Sharing is the destination default. Clear that choice for private uploads, or
use Settings → Capture → Per-action exceptions to override publication per action. Private upload
creates no public link and leaves the clipboard unchanged. Existing folder and
member access still applies. Shortcuts remain global app/action settings.

Advanced settings set new-link expiration (0–365 days; 0 means no expiration)
and an optional password. Store the password explicitly in the secret service;
configuration and receipts contain only its reference. Use no link password
clears that reference for future links. Password entry is excluded from retained
UI state and CLI arguments. Old receipts keep their original account, folder,
credential and sharing settings when destinations are edited.

History shows a link's expiry as Proton confirmed it, "Link expires on …".
Once it passes, the row says "Link expired on …" and Copy link and Open in
browser are off. `history.detail` carries the expiry as `shareExpiresAt` and
`share.inspect` as `expiresAt`. Inspecting an expired link never recreates it.

The Proton destination's Advanced link settings include **Request deadline** in
seconds (default 120; range 1–86,400, or 24 hours). It is saved as `requestTimeoutMs` in that
profile's settings. This bounds each helper request, including verification,
encryption and transfer; browser sign-in has a separate ten-minute deadline.
Changes apply to new operations. Explicit Retry may adopt a longer upload/reconciliation deadline
from the current profile only when the provider, account reference, account root
and folder still match. Its attempt budget is saved separately as
`operation.requestTimeoutMs`; the accepted destination, settings and receipt
identity remain unchanged. Lowering a profile deadline does not shorten an
already accepted retry budget. A deadline interruption
reports its reason and budget and preserves an unknown remote outcome.

Retries inspect the original publication before uploading again. Proton hides
unfinished drafts from completed-file listings. If the SDK explicitly reports
an unfinished upload collision and a complete listing proves no completed file
for that candidate, the configured rename policy can choose a new name without
removing the old draft. Unknown or incomplete listings remain unknown. Upload
and reconciliation requests use a stable client UID derived from the account
reference and publication ID, so subsequent attempts can recognize their own
new drafts. Drafts left by older builds are preserved.

History includes current activity through acquisition, preparation, upload, sharing and
clipboard results. Proton reports real SDK transferred bytes as encrypted
blocks finish, with no matching total; the current-operation row therefore shows bytes rather
than an estimated percentage. Counters can include overhead and decrease during
SDK retries. Other providers may report a matching total for percentage display.
The Lift tray uses the same byte observations and acquired source size for an
explicitly labelled estimate when there is no transfer total. This estimate
stops at 99% pending finalization/receipt; it does not change the SDK counters,
the activity row's accounting, or publication evidence.
A full byte count does not establish a completed file or a usable link: those
still require the confirmed receipt and sharing result. Notifications follow
the saved final result, independently of progress updates.

Shared results are Proton share-page URLs, including their complete access
fragment. They are not direct file/embedding URLs. URLs are sensitive: history
and explicitly requested clipboard/API results retain them; diagnostics and
helper errors do not include them.

History can create a link for a private upload, inspect uncertain sharing, or
revoke URL access without another upload. Revocation leaves the file, member
access and downloaded copies in place. Each sharing mutation has a durable
operation ID and intent before the helper starts. Interrupted results suppress
copied links and require inspection; startup does not repeat mutations. A
stale upload completion or retry cannot restore a revoked URL. If inspection
finds no link after an uncertain create, the original create intent and its
expiry/password references remain pending; a subsequent explicit retry creates
the link without reuploading. Inspection of a previously shared expired link or
an explicitly revoked link does not restore it. A failed journal commit stops
polling, clears any unconfirmed URL and permits explicit inspection after the
journal becomes writable.

Uploads are create-only. Name preflight uses the SDK's encrypted name-availability
query rather than decrypting every file in the destination. The SDK's atomic
name collision result supports rename or skip; overwrite is rejected. Exact completion node/revision IDs and an
authenticated encrypted publication/checksum marker establish ownership during
reconciliation. Missing requires a complete successful scoped listing; denial
is uncertain. Sharing checks the recorded account, folder, name, node, revision
and marker. SDK link mutations have no atomic revision precondition, so an
external revision change can race the mutation; a fresh post-check suppresses
an unconfirmed URL. Do not treat a link as an immutable revision URL.

Safe remote deletion is unavailable because the SDK does not expose atomic
revision-guarded deletion. The app rejects deletion of a recorded Proton file;
use link revocation or manage deletion in Proton. The helper never empties Trash.

All helper operations for the same keychain reference serialize across put.gd
processes, including token refresh. Each helper loads its own copy of the session,
and Proton rotates the refresh token when it is used. If two helpers refreshed
the same session, the one holding the stale token would be refused and the
official account code would sign the account out and remove it from the
keychain. Live, three parallel helpers finished three uploads 1.7–2.7 times
faster than one at a time, so concurrent Proton uploads remain a tracked design
item (#176) that needs refresh coordinated across helpers first. Helper requests use bounded stdin/stdout;
passwords and sessions never use argv. Each helper request keeps at most 64
SDK crypto-key entries in memory to avoid repeated parent-key decryption. Entity
metadata stays uncached, so identity/revision checks still read from the service.
The key cache is discarded with the helper process and is never persisted.
Timeouts/cancellation can leave an uncertain mutation, which the journal reconciles instead of hiding with another upload. SDK transport retries
are separate from application-level operation replay.

## Building and verification

See [the helper provisioning instructions](../tools/proton/README.md). The current
runtime lock supports Linux x86-64. Provision before configuring with
`cmake --preset dev -DPUTGD_BUNDLE_PROTON=ON`; use the ordinary build and test
presets. CMake checks the artifact, current helper sources, dependency lock, pinned
versions and notices against the manifest, then compiles its hash into the
adapter. Changing any of those inputs requires explicit reprovisioning. Installation uses a private libexec path, manifest and license inventory.
An app built without this bundle reports runtime unavailability on use.

Local contract tests use synthetic fixtures and prove shared pipeline/journal
behavior, not Proton service or desktop acceptance. The bundled helper's own
verification is in [VERIFICATION.md](../tools/proton/VERIFICATION.md). Live testing
requires an explicitly connected account and a new isolated synthetic test
folder. Never put a user's session or credentials into golden images, source
exports or evidence. For the tray-progress campaign, the user explicitly
authorized copying only put.gd's Proton session into disposable VM keychains.
Use the [private credential bridge](../tools/lab/secret_bridge/README.md), keep
rotating snapshots in host memory, serialize use of the copied session, and
carry refreshed state back under the production account lock. Remove only the
local lab keychain entry; signing out would invalidate the shared remote session.
The builders' lab sessions below do this for any test. See the
[tray-progress replay](testing-tray-progress.md) for the isolated live
test path. Release acceptance requires parallel Cinnamon, GNOME and KDE
functional/memory checks, authenticated helper memory, recovery, redistribution
notices and exact shipping-asset evidence under the
[acceptance matrix](acceptance-matrix.md) and [release runbook](release-runbook.md).
The [2026-10-15 register](history/proton-drive-release-gates.md) records the earlier audit.
Upstream approval is not required for production shipment.
See [integration verification](history/proton-drive-verification.md) for the historical
build, test and review checkpoints.

## Release gates

The integration maintainer owns P1, P2 and P5; the release maintainer owns P3, P4
and P6. The same person may perform both roles. For each affected release, retain
a dated pass, failure or blocker with the exact source, runtime and package
identity. An observed live incompatibility blocks affected publication. Upstream
endorsement or SDK general availability is not a gate.

| Gate | Current requirement |
| --- | --- |
| P1 — Recovery and recipient evidence | Verify authentication/refresh/reconnect, interruption/reconciliation/retry and sharing protection on isolated synthetic items. Preserve accepted operation and receipt identities. Verify unsigned-in protected/unprotected recipient bytes, expiry and revocation. Distinguish controlled service failures from observed live behavior; do not induce service-wide limits. |
| P2 — Authenticated process-family resources | Measure first use, small/large upload/share, cancellation/retry, repeated use and post-upload idle across the resident, UI and all Proton descendants. Retain RSS/PSS/private bytes, CPU, swap and helper lifetimes. Keep the separate 40 MiB post-use idle and applicable desktop memory gates; there is no approved hard active-memory ceiling. Confirm local-only/S3 work does not launch Proton. |
| P3 — Redistribution and rebuild materials | Review notices, pinned source provenance and corresponding-source/relink materials for changed runtime inputs. Preserve the source catalog and verify an extracted-source rebuild; fixture-only install checks do not prove shipping-package contents. |
| P4 — Shipping pipeline and supported assets | Provision and test the pinned Linux x86-64 helper explicitly. Audit the bundled runtime/manifest/hash, notices, redistribution files and source catalog in native packages and AppImage, and verify the corresponding-source delivery against its hashes. Packaged users need no separate runtime. |
| P5 — SDK and service compatibility | Perform the [maintenance procedure](#maintenance-procedure), including upstream migration checks, pinned updates, compatibility verification and recovery/rollback limits. Do not fabricate an extra SDK update as evidence. |
| P6 — Exact candidate and package acceptance | Freeze source/runtime identities, run the applicable app/helper/protocol checks, install actual shipping packages and retain hashes and visible native functional/memory evidence on Cinnamon/X11, GNOME/Wayland and KDE/Wayland. Run desktop checks in parallel as required by the current acceptance matrix. |

The [acceptance matrix](acceptance-matrix.md) and
[release runbook](release-runbook.md) define the complete package/desktop gates;
[verification](verification.md) records observed results and remaining application
work. The [2026-10-15 audit](history/proton-drive-release-gates.md) credits its
completed feature gates only to their dated sources and workloads. Its old
issue-closure and local-merge notes are historical, not an open work list for the
shipping integration.

## Maintenance procedure

1. **Ownership and cadence.** The integration maintainer reviews upstream SDK,
   account foundation, Crypto and Bun releases/security and migration notes
   before each stable release, when a nightly changes Proton runtime
   inputs, and whenever an announced required update or observed incompatibility
   occurs. Record the check date,
   references, pinned versions, compatibility result and next action in the
   release receipt. This is a manual responsibility; no monitor was created.
2. **Pinned updates.** Change the upstream commit, runtime/dependency locks and
   any guarded source patches together. Reprovision explicitly, review notices
   and source fingerprints, and verify the helper protocol, version/hash mismatch
   failures, clean install and extracted-source rebuild. Do not float dependencies
   or replace a public release artifact in place.
3. **Compatibility checks.** Test browser authentication and refresh, private
   upload, anonymous protected/unprotected download equality, sharing history,
   interruption/reconciliation and old journal/config/receipt compatibility.
   Confirm fresh account/folder/node/revision proof with the upgraded SDK.
   Preserve credential references and accepted operation identities; record any
   explicit reauthentication needed. Apply the P2/P6 [release gates](#release-gates)
   to the changed candidate.
4. **Migration or incident.** A required-update response or newly incompatible
   service stops affected publication with a useful update/reconnect result.
   Preserve local archives, journals, unknown outcomes and offline history for
   explicit inspection/retry after the compatible release. Verify this behavior
   rather than assuming the current generic error proves it. Do not automatically
   replay mutations or redirect work to S3. Publish a new tested put.gd version.
5. **Rollback.** Retain the prior verified package/runtime and test compatibility
   of its journals/configuration before offering downgrade. After a server crypto
   migration an old SDK may be unusable; do not promise rollback restores service.
   Keep user data and recovery identities intact while preparing a forward fix.

## Lab sessions on the builders

Each lab builder (f44, d69, k72, m74, aurora-amd) holds one put.gd Proton
session, signed in once by Patrick with the dedicated test account
phudson2@gmail.com. Later sessions
test against it without a new sign-in: on the builder itself, or in a VM on that
builder that borrows it. Every refresh rotates the session's refresh token, so a
session is a baton with one holder at a time, and the refreshed state goes back to
the builder. That holder takes the builder's **Proton lock**:

```sh
python3 tools/lab/desktop_lock.py acquire d69 --resource proton --owner NAME --session ID --purpose TEXT --minutes 30
```

The Proton lock is separate from the desktop lock: a test can hold the session
without the desktop, or the desktop without the session. It has the same
heartbeat, extend, release, break and history receipts. Hold it for anything that
uses the builder's session: the lab profile's app, `check`, `lend` or `recover`.
The `proton_lab.py` commands below take it themselves when it is free, and reuse it
when the same session already holds it.

Aurora was added on 2026-10-29 with an independent sign-in. On that Atomic host,
use `~/.local/bin/putgd-proton-lab` for the commands below: its installed adapter
uses canonical `/var/home/patrick/putgd-lab/` paths and the prepared native app.
The [Aurora builder guide](remote-builders.md#aurora-amd-fedora-45-builder)
records the build, keyring and keep-alive setup. Its host has QEMU and `dpkg`
for guests, so it lends its session to VMs like the other builders.

Every builder can lend its session to a VM it hosts. k72 joined on 2026-10-30,
after VT-x was enabled in its firmware
([#659](https://github.com/patrick-hudson/put.gd-desktop/issues/659)). The goldens each builder lends to are in
[the builders' Proton sessions](remote-builders.md#proton-sessions-on-the-builders).

### Where each session lives

| | |
| --- | --- |
| Lab profile | `~/putgd-lab/proton/` with its own `config/`, `data/` and `state/` (`XDG_CONFIG_HOME`, `XDG_DATA_HOME`, `XDG_STATE_HOME`); destination `proton-lab` |
| Keychain entry | service `ch.proton.drive/putgd`, name `putgd/proton/lab-<host>`, in the builder's default Secret Service collection |
| Registry | `~/putgd-lab/locks/proton-registry.json`, next to the lock: host, reference, app, keyring, last check and its outcome, nothing secret |
| App | on f44, d69, k72 and m74: nightly 26.1023.1058 extracted into `~/putgd-lab/proton/app/` (AppImage on f44/m74, debian13 or ubuntu26.04 .deb on d69/k72, until [#518](https://github.com/patrick-hudson/put.gd-desktop/issues/518) is fixed); on aurora-amd: the installed native source build recorded in its registry |
| Bridge | `~/putgd-lab/proton/bridge/` holds the host relay and Bun 1.4.2 broker. `guest-bridge/` holds the relay built against Qt 6.4.2 on m74, for guests, and the same broker: one build, byte-identical on every builder (relay `12b70c49…`, broker `31a76c8d…`). The registry's `guests` entry records both hashes. |
| Keep-alive | `~/.config/systemd/user/putgd-proton-keepalive.{service,timer}`, running a copy of the lab tools in `~/.local/libexec/putgd-proton/` |

The default collection is a **blank-password lab keyring**: gnome-keyring
`putgd lab` on f44, d69 and m74, KWallet `putgdlab` on k72 and KWallet
`putgd lab` on aurora-amd.
- **Why blank:** the builders log in automatically, and a password-protected
  keyring stays locked after every boot, so the session would fail with
  `keychain_unavailable` until someone typed the password.
- **Decision:** Patrick chose this on 2026-10-23 for the test account on lab
  machines, accepting that the session is stored unencrypted on disk.
- **Earlier keyrings:** each builder's earlier keyrings and wallets are kept, still
  password-protected. New secrets from other apps now go to the lab keyring.
- **Repairing it:** `proton_lab.py lab-keyring` creates the collection and makes it the
  default. `proton_lab.py keychain` proves it accepts a synthetic entry without a
  prompt. Aurora's KWallet 6.31 uses the standard Secret Service interface;
  its collection was created through that interface and passes the synthetic
  check. The legacy bootstrap's compatibility fix is tracked in
  [#647](https://github.com/patrick-hudson/put.gd-desktop/issues/647); preserve the
  existing collection and use the installed Aurora adapter.

### Commands

`tools/lab/proton_lab.py` runs on the builder, from a checkout of `nightly` there.
The keep-alive copy in `~/.local/libexec/putgd-proton/` also works for `status`
and `check`.

| Command | What it does |
| --- | --- |
| `status` | session state and the action to take, registry, Proton lock, running lab app, bridge files |
| `check [--new-folder]` | headless: inspects the account and runs Test connection; with `--new-folder`, first creates a new isolated folder and selects it. Records the outcome in the registry |
| `lend RUN --session ID --owner NAME --purpose TEXT -- COMMAND` | lends the session to one disposable VM run on this builder for COMMAND (see below) |
| `recover RECEIPTS --session ID --owner NAME` | finishes an interrupted lend from its receipts folder; the VM must be running |
| `open` / `close --session ID` | under the desktop lock, shows the lab profile's Settings on the desktop (for a sign-in) and quits it |
| `install-app FILE`, `seed HOST` | extracts an AppImage or .deb into the profile; creates the profile and its destination |
| `lab-keyring`, `keychain`, `install-keepalive`, `keepalive` | the keyring and keep-alive set-up above |
| `register-guests --bootstrap-key KEY --golden PROFILE GOLDEN KNOWN_HOSTS ...` | records in the registry's `guests` entry what `lend` can boot: the guest bridge's hashes, and the bootstrap key path and each golden's path, SHA-256 and host-key pins |

### Lending the session to a VM

Launch the VM from a golden overlay on the same builder (`lab.py launch`), and
install put.gd in it (`lab.py install`). Then:

```sh
python3 tools/lab/proton_lab.py lend "$run" --session ID --owner NAME --purpose TEXT -- \
  python3 tools/lab/proton_lend_smoke.py
```

Under the builder's Proton lock, `lend`:
1. Copies the guest brokers into the VM and gives the VM the same blank lab keyring.
2. Exports the builder's session through the [private credential bridge](../tools/lab/secret_bridge/README.md)
   and imports it under a new `putgd-lab-UUID` name.
3. Runs COMMAND with `PUTGD_LAB_RUN`, `PUTGD_LAB_PROTON_REFERENCE` and
   `PUTGD_LAB_PROTON_RECEIPTS` set.
4. Quits the guest's put.gd, collects the guest copy and carries it back to the
   builder's entry (an exact compare-and-replace under the production account lock,
   with `reconcile_carry_back` after an ambiguous reply).
5. Deletes only the guest entry.

Receipts go in `~/putgd-lab/proton/receipts/<time>-<run>/` and hold names and states,
never session material. `lend` also records its outcome in the registry as
`lastLend`. The smoke test creates a new folder, runs **Check account and folder**
against it and requires the `account-folder` scope. It then uploads a synthetic
image privately into that folder, and creates, inspects and revokes its link. With
`--pictures` it saves the guest's Destinations and History. It has these options:
- `--browser --hold N`: pauses before and after revoke, so the live link and its
  share page can be framed.
- `--revoke HISTORY_ID`: settles a link left by an earlier run; use it with
  `lend --guest-reference` and that run's guest name.

Keep the VM running until `lend` returns: its copy may be the only current one. If
`lend` fails while a copy is outstanding, it leaves the Proton lock held and blocked
("lent copy outstanding"). Boot the same overlay again if it stopped, run `recover`
with the receipts folder, then check. `recover` releases the lock it finishes.

### When a session is lost

`status` and `check` name the state in the registry. Patrick signs in again only on
the builder that lost it.

| State | Meaning | What to do |
| --- | --- | --- |
| `signed_out` (`authentication_required`) | signed out, password changed, revoked in Proton's security settings, or a refused refresh; the official code then deletes the keychain entry | Patrick signs in again on that builder: `open` under the desktop lock, **Connect account**, then `close` and `check --new-folder` |
| `keychain_unavailable` | the default collection is missing or locked | `keychain`, then `lab-keyring` if needed; the session may still be valid, so check again before asking for a sign-in |
| `needs_new_reference` | the entry exists but holds no usable session | Patrick signs in again on that builder |
| `service_error` | Proton or the network failed; the session is not known to be lost | check again later |

Never sign out, revoke or copy a builder's session yourself. Signing out from one
copy ends the session for every copy.

### Idle expiry and the keep-alive

The pinned SDK (0.22.2, `incubating/account/js/src/apiClient.ts`) tracks no session
expiry:
- After a 401 it refreshes once; a refused refresh deletes only the local entry.
- Its API types document no idle limit for refresh tokens.

The weekly keep-alive is therefore insurance. It runs one `check` under the Proton
lock each Wednesday at 04:30 local time, with up to 15 minutes' random delay, and
catches up after the builder was off. That check refreshes the session once its
access token has expired. It is skipped, not queued, while the lock is held or the
lab app is open. To disable it, run
`systemctl --user disable --now putgd-proton-keepalive.timer` on that builder.

A guest that refreshed the session was carried back live for the first time on
2026-10-29: d69's and m74's lends reported `rotated: true`, the carry-back was
`replace_confirmed`, and each builder's `check` passed afterwards
([#527](https://github.com/patrick-hudson/put.gd-desktop/issues/527)). Evidence:
[the 2026-10-23 lab sessions](evidence/2026-10-23-proton-lab-sessions/README.md) and
[lending on every builder, 2026-10-29](evidence/2026-10-29-proton-guest-bridge/README.md).
