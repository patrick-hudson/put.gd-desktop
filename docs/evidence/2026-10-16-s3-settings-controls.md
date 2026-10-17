# Local S3 settings controls — 2026-10-16

The user explicitly approved changing the saved-secret contract: Show may reveal
an unlocked saved S3 secret access key. The subsequent request adds visible
connection-test success and actionable errors.

## Cause and behavior

The secret field contained only a masked stored-value placeholder. Its old Show
handler changed the echo mode of an empty input without retrieving a saved value.
The new explicit `setup.revealCredentialSecret` read validates the saved S3
profile and accepted/observed revision, then returns only its cached secret key.
It does not unlock the keychain; the existing explicit Unlock action remains
required when credentials are not loaded. CLI export of this method is rejected.

The UI displays saved secrets read-only and discards them on Hide, destination or
provider changes, page hide, close, reload and credential clearing. Request serials
reject obsolete replies. Revealed values do not become credential drafts,
configuration, history, setup records, diagnostics or checkpoints. Typed secrets
retain ordinary Show/Hide behavior.

Test connection previously wrote success only to the hidden Settings status label.
Progress, errors and explicit success now appear below the destination title.
Changing destination clears the old result; a result for an older destination or
settings revision is qualified. Success still requires the existing synthetic
upload, exact public GET and guarded cleanup checks.

S3 errors now provide fixed, sanitized guidance for recognized authentication,
token, clock, bucket, region, ACL, permission and transport failures. Permission
advice identifies the failed operation and relevant read/write/version/cleanup
permissions. A HEAD 403 remains ambiguous rather than being called a bad key.
Public-link failures report concurrent cleanup failure too. No raw SDK response
text is added to messages or diagnostics, and no bucket policy is changed.

## Verification

Product source is based on `af8c035a1364def01be430e97d12f47f92358563`.
The product-only patch SHA-256 (`git diff -- src`) is
`0588b64d5c2636a48b8447451ed376cf49b75087a5bb00570bf3fd8e78f3e0ff`.
The local build reports `0.1.1-dev.221+gaf8c035a1364.dirty` on
Mint 22.3 / distro Qt 6.4.2. Private evidence retains per-file source hashes,
build-info, executable/module hashes, JUnit and the complete CTest log.

- `cmake --preset dev` and `cmake --build --preset dev` pass.
- The saved-secret regression first failed with an empty field after Show; it
  passes with Hide, typed-secret behavior, no draft/checkpoint export, destination
  switching, page hide, clearing, missing credentials and delayed reply checks.
- The connection-status regression first failed because visible destination
  feedback was empty after successful completion. The corrected focused GUI run
  passes both new cases and the two existing uncertain-inspection recovery cases
  (6 QtTest outcomes including setup/cleanup).
- All eight actionable-error rows failed against the original S3 messages or
  classifications. The corrected isolated SDK/worker fixture run passes those
  eight rows and all four synthetic connection/public-link/cleanup rows
  (14 QtTest outcomes including setup/cleanup).
- The initial sandbox S3 fixture could not bind its loopback port. The same
  command was rerun outside the sandbox with synthetic credentials only; no
  production S3 endpoint or user secret store was contacted.
- `TMPDIR=/tmp/sr.bQh12j ctest --preset dev --output-junit /tmp/sr.bQh12j/dev.xml`:
  **59/59 passed**, 648.26 seconds. The private short temporary root keeps the
  checked socket templates at 43–67 UTF-8 bytes (six-character Qt temporary
  suffixes), below the 107-byte limit. The UI seam tests use in-process dispatch;
  the S3 fixture uses localhost TCP and worker pipes.

Scoped code/test review corrected saved-value unsaved messaging, stale success
under another destination, exact-version IAM advice and two test assertions that
did not observe the claimed boundary. No unresolved review finding remains.
The unrelated pre-existing `tools/qemu-grid/` directory is excluded from this fix.

## Acceptance limits

These are automated widget/core and synthetic SDK transport checks. No graphical
lab campaign or current-candidate Cinnamon functional/resident-memory gate was
run, and no GNOME/KDE expansion was attempted. Current-version native desktop,
memory and wider release gates remain pending; earlier desktop evidence is not
relabeled as evidence for this change. No VM was created or retired. The user
requested staging only and deferring the VM suite until another job finishes;
this change is not committed or pushed.
