# Using put.gd

Everyday use of the app, its failures and its storage destinations.

## Guarantees

- Every service and CLI failure has a `code`, a source-free `message` and bounded `details`. Ordinary diagnostics never contain source contents, filenames, history URLs, authorization values or credential references.
- Exit 0 from `putgd-cli` does not mean an accepted upload finished; inspect the operation ID.
- The S3 adapter never changes ACL selection, bucket policy, ownership, retention, versioning or DNS.
- S3 requests verify HTTPS certificates. Plain HTTP is accepted only for a literal loopback address.
- Backblaze B2 uses B2's native API, because B2's S3-compatible API has no conditional writes.
- A custom uploader's header, parameter, argument and body values are secrets unless a row says otherwise. An imported `.sxcu` token is held in the Settings window until it is stored in the keychain or the session cache; it never reaches `config.json`, a checkpoint, a log, History or a diagnostics package.
- A custom uploader's server names each upload, so put.gd cannot check whether one arrived. A lost reply is shown as unknown, and only an explicit Retry sends the file again.

## Documents

- [User guide](user-guide.md): the main window, outputs, destinations, shortcuts and everyday use.
- [Troubleshooting](troubleshooting.md): start from the production CLI, then work through common problems.
- [Structured failures and recovery](errors.md): every failure code and what to do about it.
- [S3 adapter](s3.md): S3 and S3-compatible destinations, addressing, transfers and version safeguards.
- [Backblaze B2 destination](b2.md): setting up and using B2 through its native API.
- [Custom uploader (HTTP) destination](custom-uploader.md): your own server or any service with a ShareX `.sxcu` file, its placeholders, secrets, deletion and limits.
- [Proton Drive](proton-drive.md): the provider, its build checks, release gates and lab sessions.
