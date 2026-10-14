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

Use the pinned Bun 1.4.2 artifact, then upload both executables to an owned
absolute path in each disposable guest. The Qt relay refuses a symlink, a
non-owned executable, or a group/world-writable executable. For Proton it locks
exactly `/tmp/putgd-proton-accounts-UID/SHA256(reference UTF-8).lock` with
`QLockFile`, `setStaleLockTime(0)` and `tryLock(2000)`, as the production SDK
adapter does. The lock directory must be owned, private, and free of symlinks.
The relay captures the Bun reply privately and bounds its input/output/time.
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
one bounded JSON request from stdin and prints only `provisioned` or
`provisioning_failed` status JSON. Supply a fresh `receipt` path to retain its
**nonsecret** generated reference and S3 settings in a new mode-0600 file.
This receipt is written before import, so it preserves the cleanup reference
if the SSH result is ambiguous; its existence alone does not prove success.
The S3 request fields are `provider`, `run`, `guestBroker`, `credentialsFile`
and optional `receipt`. `credentialsFile` is opened without following a
symlink, must be an owned mode-0600 regular file, and is never copied to a VM.
Only named colon-style access key, secret key, optional session token,
bucket/bucket name/bucketName, region and endpoint fields are parsed. Values
may be inline or on the next line; unknown text is never used
to guess a credential, region or endpoint. Endpoints with user info, query or
fragment are rejected before retaining settings, matching the production S3
contract. No input is printed or hashed.

A one-time Proton request has `provider`, `run`, `guestBroker`, `hostBroker`,
`sourceReference` and optional `receipt`. The exact source reference must come
from the user-approved host put.gd profile, not an official CLI account or a
guessed keychain name. A raw snapshot is validated as an opaque JSON string
with `session.uid`, `session.accessToken` and `userKeyPassword`, preserving
the official serialization and any extension fields. Broker export stdout
must go directly into the wrapper's private pipe: never invoke export in a
terminal or redirect it to a log/file. Both brokers refuse export when stdout
is a terminal or regular file; this check cannot identify what a downstream
pipe reader later chooses to do with its input, so the controller still owns
the private pipe destination.

For a campaign that uses and refreshes the copied Proton session, retain a
`ProtonSessionChain` in **one host process**. Import the module normally and:

```python
chain = ProtonSessionChain(host_broker, exact_host_profile_reference)
reference = chain.provision(run_a, guest_broker_a)
# Run the authorized guest helper/app work; never use the same remote session
# concurrently on the host or another guest during this step.
chain.collect(run_a, guest_broker_a, reference)
chain.cleanup(run_a, guest_broker_a, reference)
next_reference = chain.provision(run_b, guest_broker_b)
# Run guest B's authorized work, then collect before cleanup.
chain.collect(run_b, guest_broker_b, next_reference)
chain.carry_back()  # Exact host baseline comparison under the production lock.
chain.cleanup(run_b, guest_broker_b, next_reference)
```

`carry_back(new_lab_reference=True)` instead creates a new host LAB reference
and returns its nonsecret name. The caller owns later local cleanup of that
entry. A host baseline conflict is a failure, never an unguarded overwrite;
retain the chain in memory while resolving it. A failed guest import retains
its nonsecret reference in `chain.active`/`chain.owned`, since a failed SSH
reply may be ambiguous. The controller must inspect that reference and decide
the cleanup; do not retry with guessed references or discard refreshed
snapshots before carrying them forward. Each provision must be followed by
collection before another VM is provisioned. These APIs never serialize
snapshots into the provisioning configuration, receipts or logs. Local
cleanup must not call logout/signout/disconnect: copied snapshots share the
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
