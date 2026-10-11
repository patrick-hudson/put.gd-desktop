# Setup-result recovery replay

The focused tests use the production resident SetupService through the asynchronous
UiSession fixture, synthetic destination testers, temporary configuration paths,
and Qt offscreen widgets. They perform no S3 traffic or system credential writes.
The eviction case completes 65 setup jobs against the actual 64-result resident
cache before restoring the expired ID ahead of another accepted job.

Use the repository's normal documented dev configuration and AWS SDK provisioning.
The saved replay.sh accepts a checkout and a fresh absolute evidence directory.
Do not run it while another process owns that checkout's dev build cache.

For the exact red reproduction, create an isolated checkout of
4c687f5d55d69f1717bafd80f02d4771b5b3bac4 and apply red-test.patch there;
leave production files at that baseline. Run only the three focused functions.
The recorded red failed all three assertions before production edits. Its patch
was reconstructed after the run by removing the sole subsequent test addition:
the assertion that the test button stays disabled after a transient inspection
error. Final tests retain that stronger assertion.

The final green tree is identified by the owned source hashes in receipt.json;
implementation.patch includes all three owned-file changes. It is not a commit.
Full U11 acceptance, sanitizer integration and desktop evidence remain caller-owned.
