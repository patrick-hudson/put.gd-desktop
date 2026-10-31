# Private live credential bridge

These helpers provision **existing disposable Linux lab runs**, using their
strict SSH identity and the native desktop's DBus secret service. They never
change golden disks, call a storage service, sign out a Proton account, or print
secret material. Build and real-keychain integration are separate acceptance
gates; the Python tests use synthetic values only.

The standalone Qt executable uses system Qt 6.4.2+ and Qt6Keychain 0.14.2+,
dynamically linked. It writes S3 credentials as compact **binary** JSON
`accessKey`/`secretKey`/`sessionToken`, matching `src/secrets/CredentialStore.cpp`,
under the fixed `putgd` service. The compiled Bun executable uses `Bun.secrets`
under the fixed `ch.proton.drive/putgd` service, matching
`tools/proton/account.ts` and its pinned official CLI snapshot store. No
separately installed guest Bun or Node runtime is required.

Build commands (the host controller owns execution and package provisioning):

```sh
cmake -S tools/lab/secret_bridge -B build/lab-secret-bridge -G Ninja
cmake --build build/lab-secret-bridge
build/dependencies/proton-runtime/build-source/node_modules/@oven/bun-linux-x64/bin/bun build \
  --compile tools/lab/secret_bridge/proton.ts \
  --outfile build/lab-secret-bridge/putgd-lab-proton-secrets
```

Use the pinned Bun 1.4.4 artifact, then upload both executables to an owned
absolute path in each disposable guest. The Qt relay refuses a symlink, a
non-owned executable, or a group/world-writable executable. For Proton it locks
exactly `/tmp/putgd-proton-accounts-UID/SHA256(reference UTF-8).lock` with
`QLockFile`, `setStaleLockTime(0)` and `tryLock(2000)`, as the production SDK
adapter does. The lock directory must be owned, private, and free of symlinks.
The relay captures the Bun reply privately and bounds its input/output/time.
The Python wrapper gives each broker exchange 60 seconds, beyond the relay's
roughly 32-second Proton staged bound and 40-second two-job S3 bound, so it
does not preempt normal relay child cleanup. Its pipe input/output and outer
deadline remain bounded; delayed native secret-service behavior still needs
compiled broker acceptance.
This coordinates export and compare/replace with cooperating production SDK
operations; it does not claim an atomic keychain API or exclude other software
that writes the same entry without the production lock.

All broker operations are one JSON object on stdin, with no secret arguments.
S3 uses a Qt executable path. Proton uses the endpoint object
`{"qt":"/absolute/putgd-lab-secret-bridge","proton":"/absolute/putgd-lab-proton-secrets"}`.
The Python wrapper adds the fixed provider envelope; callers never select a
service namespace. S3 accepts `import` and `delete`; Proton accepts `export`,
`import`, `replace` and `delete`. Imports and deletes are restricted to fresh
`putgd-lab-UUIDv4` references. Import fails if an entry already exists. Proton
replace requires the entire exact expected baseline snapshot and fails if the
current entry differs. Delete removes only the local lab keychain entry.

The public `python3 -B tools/lab/provision_live_credentials.py` command reads
one bounded JSON request from stdin. Provision requests print only
`provisioned` or `provisioning_failed` status JSON. A fresh `receipt` path is
required to retain **nonsecret** provider, target, resolved run path, exact
reference, lifecycle state and optional S3 settings in a new mode-0600 file.
Receipt creation is exclusive, owned and durable before import. Receipt reads
and atomic status updates require an owned, singly linked mode-0600 regular
file, with no symlink in its path. Receipts contain no snapshots, credentials,
authentication output or file hashes. `import_pending` preserves the exact
cleanup reference after an ambiguous result; `import_confirmed` is recorded
only after the broker confirms import. A receipt's existence alone does not
prove success.
The S3 request fields are `provider`, `run`, `guestBroker`, `credentialsFile`
and `receipt`. `credentialsFile` is opened without following a
symlink, must be an owned mode-0600 regular file, and is never copied to a VM.
Only named colon-style access key, secret key, optional session token,
bucket/bucket name/bucketName, region and endpoint fields are parsed. Values
may be inline or on the next line; unknown text is never used
to guess a credential, region or endpoint. Endpoints with user info, query or
fragment are rejected before retaining settings, matching the production S3
contract. No input is printed or hashed.

Public cleanup consumes that same protected receipt and requires the exact
provider, resolved run and LAB reference. The fields are `op: "cleanup"`,
`provider`, `run`, `guestBroker`, `reference` and `receipt`; for example, the
controller constructs this request from the retained nonsecret receipt:

```python
cleanup_request = {
    "op": "cleanup", "provider": receipt_data["provider"], "run": receipt_data["run"],
    "guestBroker": guest_broker,
    "reference": receipt_data["credentialReference"]["reference"], "receipt": receipt_path,
}
```

Send it through the same stdin-only public command. It prints only `cleaned`
or `cleanup_failed` status JSON. The wrapper records `cleanup_pending` before
the exact local delete and `cleanup_confirmed` after its acknowledgement.
An absent entry is successful; retrying a confirmed cleanup is harmless.
Wrong provider, run, reference, permissions, symlinks and source-replace
receipts are rejected before deletion. A failed delete retains its exact
pending receipt for retry. Do not erase the receipt to represent cleanup.

A one-time Proton request has `provider`, `run`, `guestBroker`, `hostBroker`,
`sourceReference` and `receipt`. The exact source reference must come
from the user-approved host put.gd profile, not an official CLI account or a
guessed keychain name. A raw snapshot is validated as an opaque JSON string
with `session.uid`, `session.accessToken` and `userKeyPassword`, preserving
the official serialization and any extension fields. Broker export stdout
must go directly into the wrapper's private pipe: never invoke export in a
terminal or redirect it to a log/file. Both brokers refuse export when stdout
is a terminal or regular file; this check cannot identify what a downstream
pipe reader later chooses to do with its input, so the controller still owns
the private pipe destination.

On the lab builders, use `tools/lab/proton_lab.py lend` instead of driving the
chain by hand. It takes the builder's Proton lock (`desktop_lock.py --resource
proton`), provisions its signed-in session into one VM run, runs the test
command, then collects, carries back and cleans up exactly as below, and
`recover` finishes an interrupted lend from its receipts. The lab section of
`docs/proton-drive.md` describes it.

For a campaign that uses and refreshes the copied Proton session, retain a
`ProtonSessionChain` in **one host process**. Import the module normally and:

```python
chain = ProtonSessionChain(host_broker, exact_host_profile_reference)
reference = chain.provision(run_a, guest_broker_a, fresh_receipt_a)
# Run the authorized guest helper/app work; never use the same remote session
# concurrently on the host or another guest during this step.
chain.collect(run_a, guest_broker_a, reference)
chain.carry_back(fresh_save_receipt_a)
chain.cleanup(run_a, guest_broker_a, reference)
next_reference = chain.provision(run_b, guest_broker_b, fresh_receipt_b)
# Run guest B's authorized work, then collect and carry back before cleanup.
chain.collect(run_b, guest_broker_b, next_reference)
chain.carry_back(fresh_save_receipt_b)  # Exact host baseline comparison under the production lock.
chain.cleanup(run_b, guest_broker_b, next_reference)
```

`provision(..., reference=NAME)` reuses an earlier guest's exact
`putgd-lab-UUIDv4` name instead of a new one, after that entry was cleaned up.
put.gd binds a history entry's link operations to the reference it uploaded
with, so this lets that guest finish its own operations, such as revoking its
test link. The import still refuses an entry that exists.

Every `provision` and `carry_back` now requires a distinct fresh receipt path,
including guarded replacement of the source entry. The replacement receipt
names the source reference but is never eligible for deletion. Every collect,
even one that returns identical bytes, sets `chain.current_saved` false and
retains the guest entry until a carry-back and its durable status updates are
confirmed. Cleanup refuses that state. A host baseline conflict or ambiguous
reply retains the guest and the current snapshot. Do not blindly retry a
source replacement whose acknowledgement was lost. Call
`chain.reconcile_carry_back(failed_save_receipt)` first. It privately exports
the source under the production lock and compares the entire opaque snapshot
exactly. An exact current-snapshot match confirms the save and returns `True`;
an exact original-baseline match records `replace_not_applied`, returns `False`
and permits a new carry-back with a fresh receipt. Any other bytes retain the
pending operation and guest copy as a conflict. Repeated CAS, new provisioning
and guest/host cleanup are blocked until this reconciliation succeeds. No
snapshots or comparison hashes are written to the receipt, and no unguarded
overwrite is provided.

`chain.carry_back(fresh_host_receipt, new_lab_reference=True)` instead creates
a new host LAB reference and returns its nonsecret name. Its exact reference
and pending/confirmed state survive controller exit in the receipt. Use
`chain.cleanup_host(reference)` for exact local cleanup while that chain is
alive. A confirmed host copy that is the saved current session cannot be
removed until a subsequent source carry-back confirms the same current
snapshot. A pending host import can be cleaned because no helper may start
after an unconfirmed import. When the controller has exited, the public
cleanup fields for a host copy are `op: "cleanup"`, `provider: "proton"`,
`hostBroker`, `reference` and `receipt`. Its receipt must explicitly permit
cleanup; source-replace receipts always refuse it.

Start guest SDK/app work only after `provision` returns successfully. A failed
guest import leaves `chain.active` and its receipt pending, so collection and
SDK work are forbidden. `chain.abandon(run, guest_broker, chain.active)`
deletes only that exact pending LAB entry, tolerates an absent entry, and
clears the active slot without changing `chain.current`. A prior collected
snapshot can also be carried back while this import remains pending. An
abandon failure retains the pending reference; a confirmed import cannot be
abandoned and must be collected and carried back.

After controller exit, a pending guest import may use public cleanup directly.
To recover a confirmed chain guest, stop provider work and construct a fresh
chain from the exact approved host source, then call
`chain.recover(receipt_path, reference, run, guest_broker)`. This privately
exports the confirmed guest copy, marks it collected and requires a fresh
source carry-back receipt before cleanup. For a confirmed host LAB copy use
`chain.recover(receipt_path, reference)` and then carry back before
`cleanup_host`. Recovery accepts only the exact protected receipt and a fresh
controller; it does not start SDK work or infer references. Restore only the
latest known copy from the serialized campaign, and keep host/guest provider
work quiescent throughout recovery.

The one-time Proton import is for copying without subsequent session-refreshing
work. Use the chain for all SDK/app work that can refresh the session. These
APIs never serialize snapshots into configuration, receipts or logs. Local
cleanup never calls logout/signout/disconnect: copied snapshots share the
same remote session.

The controller owns destination/folder selection, unique synthetic live test
folders, application operation serialization, guest keychain unlock, visible
console evidence and campaign lifecycle. No automatic cloud requests, secret
logging, config overwrite, package installation or keychain fallback is
provided by this unit.

```sh
python3 -B tests/desktop/test_live_credentials_lab.py
```

Passing these tests establishes host parsing, redaction, bounded transport,
reference constraints and session handoff contracts only. It does not prove
compiled broker behavior, live credentials, a real keychain or desktop support.
