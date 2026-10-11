# Live S3 acceptance

Use an isolated synthetic destination and the production resident, CLI and S3
module from one clean versioned build. Record their hashes, package version,
desktop/compositor versions and actual resident PID/start time. Keep graphical
actions visible through the lab VNC display. A provider probe alone does not
establish resident, clipboard or History acceptance.

Credentials are protected local input. Supply them through the production
credential interface without placing their values in command arguments, saved
configuration, logs or evidence. Configuration records credential references
only. Never commit `credentials.txt`. Record the credential mode, not its values.

1. Save the original settings document and accepted/observed revision. Configure
   one test profile and route only the tested action to it. Validate and save with
   the current revision; preserve other actions and shortcuts. Use a unique
   synthetic filename and a prefix allowed by the test credentials. A root-only
   credential cannot write a nested test prefix.
2. Put synthetic text on the clipboard with a registered native desktop fixture.
   Verify a native paste before invoking the production clipboard action once.
   Retain the returned operation ID. Inspect that operation until terminal and
   join its artifact, History, publication and provider receipt identities.
3. Fetch the actual public URL with a bounded unsigned request and compare its
   exact length and digest with the synthetic source. Do not save payload bodies.
   Independently paste into the native receiver to verify automatic URL copying;
   an internal `copied` status alone is insufficient.
4. Open actual History and select the uploaded item. Reset and independently
   verify the clipboard first, then click **Copy link** and paste into the native
   receiver. Check the result against the receipt URL. Retain a visible success
   witness. A CLI `history.copyLink` call does not prove the GUI button.
5. Use `delete.prepare` with that exact History ID. Review its receipt, object
   identity, concurrency token/version, local effects and cache notice. Pass the
   identical ordered IDs and returned acknowledgement to `delete.confirm` once.
   Retain the deletion ID and inspect it: require the exact item's removed status
   and provider progress `done`, rather than just a completed aggregate state.
   Verify History retains the receipt and disables the removed link. Never retry
   an unknown outcome blindly or delete by a broad bucket listing.
6. Restore the original document using a fresh revision; stop on a conflicting
   settings change. Reread and verify document equality, accepted/observed revision
   equality and valid configuration. Close the view normally and stop only the
   registered fixture generation. Preserve operation and History records.

Restoring settings does not clear session credentials. End the resident normally
before a credential-free cold run. Record cold, post-view, post-capture and
post-upload memory separately using [the frozen memory protocol](testing-memory.md).
A single `smaps_rollup` snapshot is a locator, not a five-minute acceptance run.
The provider module remains pinned after first use, so cold savings do not prove
post-upload savings. Origin deletion also does not establish browser/CDN cache
invalidation.

The [source55 Cinnamon evidence](evidence/2026-10-09-live-s3-cinnamon.json) records
one passing live workflow and its distinct 49.14 MiB post-cloud diagnostic.
Use fresh IDs for each reproduction; historical deletion IDs are evidence only.
See [CLI](cli.md), [S3](s3.md), [credentials](credentials.md) and
[VM lifecycle](vm-lab.md) for the production commands and environment setup.
