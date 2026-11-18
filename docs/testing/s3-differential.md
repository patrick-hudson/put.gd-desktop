# S3 differential test

`tools/lab/s3_differential.py` runs one seeded sequence of uploads, deletes and existence checks through the real resident against several S3-compatible servers and compares what put.gd reports with what each server holds. Real servers differ on multipart, odd keys, error codes, ETags, listing permission and conditional writes, and put.gd has to behave correctly on all of them. [Live S3 acceptance](live-s3.md) proves one destination end to end; this finds the differences between destinations.

It is a lab tool, not a CI job: it needs podman and a loopback port per server, and the cloud columns need the builder's `~/Documents/creds.txt`. Run it on a build server, never on the workstation. The harness reads the credentials into memory; never put their contents on a command line or in a log, diagnostic, result file or exported evidence.

## Servers

`tools/lab/s3_servers.py` starts and stops the local servers, each in a podman container named `s3diff-<name>` with a named volume of the same name, bound to `127.0.0.1` only, with one fixed synthetic key pair:

| Name | Image | Port |
| --- | --- | --- |
| `minio` | `cgr.dev/chainguard/minio` (MinIO's own images are gone from quay.io and Docker Hub) | 19001 |
| `versitygw` | `ghcr.io/versity/versitygw`, POSIX backend | 19002 |
| `garage` | `docker.io/dxflrs/garage:v2.1.0` | 19003 |
| `seaweedfs` | `docker.io/chrislusf/seaweedfs`, `mini` | 19004 |
| `rustfs` | `docker.io/rustfs/rustfs` | 19005 |
| `zenko` | `docker.io/zenko/cloudserver` (an old Scality CloudServer, in memory) | 19006 |

The offline oracle `tests/fixtures/s3_server.py` is the seventh server and starts by itself. Ceph RGW is left out: its demo image is not published any more. `aws`, `aws1` and `r2` use the builder's credentials in memory (key set 2 for `aws`; `aws1` is key set 1, which has no `ListBucket`, and is used for the listing-denied case only). Their prefix is unique to the run, each case deletes exactly the objects it made and aborts its own multipart uploads, and the run ends by listing its prefix again. Before it writes anything, a cloud run refuses a bucket whose versioning is enabled, suspended or unreadable, and then runs no cleanup at all. Cleanup and the final check cover both the run's prefix and its leading-slash twin (`/<prefix>`). A delete or abort that fails, an upload listing the server won't give, or anything still listed at the end makes the command exit 1: investigate and remove what is left before calling the run clean.

Use a short, new, private output directory, such as `/tmp/s3d-<run>`. The resident's socket ends up at `<out>/work/<server>/r/rt/putgd/session.sock`, and AGENTS.md requires checking the UTF-8 length of every such resolved path before launch: reject anything over 107 bytes.

```sh
python3 -I tools/lab/s3_servers.py start minio versitygw garage seaweedfs rustfs zenko
dbus-run-session -- python3 -I tools/lab/s3_differential.py --build build/release --out /tmp/s3d-1 run \
    --servers oracle,minio,versitygw,garage,seaweedfs,rustfs,zenko
python3 -I tools/lab/s3_differential.py --out /tmp/s3d-1 report
python3 -I tools/lab/s3_servers.py stop minio versitygw garage seaweedfs rustfs zenko
```

`--cases` picks cases (`cases` lists them), `--max-mib` and `--sizes` bound the size ladder, and a cloud run should pass `--sizes s1,s-over`. The resident runs headless under the private bus with session credentials, so no desktop and no keyring is involved. Every request of a local server goes through `tools/lab/s3_fault_proxy.py`, which relays HTTP/1.1, records what put.gd sent (method, path, status, conditional and checksum headers) and can lose a reply after the server acted, drop or delay a request, answer with an error, or alter one byte of a body.

## Cases

Object names (spaces, `+`, `%`, `?`, `#`, `:`, `@`, unicode in two normalisation forms, bidi and zero-width characters), long names and keys at 1024 bytes, prefix shapes (trailing and doubled slashes, a leading slash, dots), case collisions, the size ladder (1 byte, either side of the 64 MiB multipart threshold, five parts, 21 parts), the three collision policies, a name twice in one batch, History deletion (normal, already gone, replaced by someone else, twice), lost replies to PUT, to the multipart completion and to DELETE, failing parts and completion and a cancel in the middle of a multipart upload (nothing may be left incomplete, and Retry must finish a kept upload), a part altered in transit, another client creating the key between the check and the conditional PUT, content type and the publication metadata the server keeps, the stored link fetched without credentials, `public-read`, wrong regions, path and virtual-host addressing, a refused name check (`403` without `ListBucket`), and thirty one-file operations for connection failures.

Per case the run records, for every item: operation state, upload status, error code and `httpStatus`/`serviceCode`/`requestStage`, receipt key and link; and from the server, independently of put.gd, every key under the case's prefix with size, SHA-256, content type, user metadata and ETag, plus incomplete multipart uploads. A harness check flags what put.gd's documents promise and the server contradicts (an item reported uploaded under a key the server does not hold, a stored object that differs from its file, a failed item whose object is stored, a foreign object overwritten, an upload left incomplete). `report` lines the servers up per case; a difference that is only a server's own behaviour (a key length limit of a file-system backed server, a region it insists on) is for the reader to classify.

## What the first run found (2026-11-17, nightly `b60a58c0d`, AWS SDK 1.11.924)

Eight destinations: the oracle, MinIO (Chainguard build, release 2026-11-16), versitygw 1.11.0, Garage 2.1.0, SeaweedFS (4.54), RustFS, Zenko CloudServer, plus AWS S3 and Cloudflare R2. Findings, each with its issue:

- [#1581](https://github.com/patrick-hudson/put.gd-desktop/issues/1581): on Garage, History deletion refuses put.gd's own object (`changed`, nothing sent). Garage returns a version ID on every write but no version header on a versioned HEAD.
- [#1582](https://github.com/patrick-hudson/put.gd-desktop/issues/1582): an `objectPrefix` with `//`, a leading `/` or dot segments gives a receipt and link that name a key the SDK and libcurl never sent (AWS S3 and every other server).
- [#1583](https://github.com/patrick-hudson/put.gd-desktop/issues/1583): Garage and Zenko ignore `If-None-Match` and `If-Match` on `PutObject`; the no-overwrite guarantee is silently absent and Test connection cannot tell.
- [#1584](https://github.com/patrick-hudson/put.gd-desktop/issues/1584): the Minisign verifier refuses CRLF line endings the reference accepts.

Clean on every server: names with spaces, `+`, `%`, `?`, `#`, unicode in two normalisation forms, bidi and zero-width characters (stored byte-exact under the key put.gd reports, and the link opens them where the bucket can be made public), case collisions, keys at the 1024-byte limit (put.gd shortens the name to fit), all three collision policies, a name twice in a batch, the size ladder with composite multipart ETags, deletion of an object that is normal, already gone, replaced by someone else or deleted twice (on every server but Garage until #1581 is fixed), lost replies to PUT, to the multipart completion and to DELETE, failing parts, a failing completion and a cancel in the middle of a multipart upload (Retry finishes the kept upload and nothing stays incomplete), a part altered in transit (refused by the server's digest check, nothing stored), and a refused name check (no PUT follows).

Server behaviours worth knowing when something fails on one of them, none of them put.gd's:

- Garage and the oracle cannot verify the SigV4 signature of a key with `:`, `@`, `$`, `,`, `&` or `=` in it (the SDK sends them unencoded and signs them encoded; AWS and the other servers normalise). The name check answers a body-less `403`, which put.gd shows as `s3_permission_denied` with IAM guidance.
- Zenko refuses keys of 1000 bytes and more and serves nothing anonymously; versitygw takes only `us-east-1`; Garage only `garage`.
- A HEAD with the wrong region is a body-less `400` on AWS's regional endpoint, so put.gd can only show the general `s3_request_failed` guidance (which names the region) there; `s3_region_mismatch` needs the error document a PUT gets or the global endpoint's `301`.
- On a host loaded by other work, a TLS connection to AWS now and then fails (`s3_transport_unknown`, "The secure connection to S3 failed"); on an idle host 34 of 34 uploads and 30 single-file operations passed.

## Minisign

`tools/lab/minisign_differential.py` checks put.gd's verifier (`src/cli/Minisign.cpp`) against the reference `minisign` tool: genuine signatures of seven messages (empty included, #1561) with seven trusted comments in both the prehashed (`ED`) and legacy (`Ed`) forms, wrong keys, tampered messages, hand-made mutations of the signature and key files, and seeded byte-level mutations, each verified by both and compared. `tools/lab/minisign_verify_driver.cpp` is the batch front end to the verifier. No input is accepted by put.gd for a message or comment other than the one that was signed. put.gd is laxer than the reference about framing that no signature covers (the untrusted comment's prefix, unpadded or whitespace-trimmed base64, the key line placed before the comment) and, as of the CRLF fix, no stricter on line endings.

Run it with a build that has `libputgd_minisign.a` (build the `test_minisign` target first), the reference `minisign` on `PATH` or given with `--minisign`, and a new empty `--work` directory:

```sh
python3 -I tools/lab/minisign_differential.py --build build/release --work /tmp/msd-1 --seed 1 --mutations 1500
```

Each case where the two tools disagree is printed as a `DIFFERENCE` line, followed by a JSON summary (`put_gd_accepts_more` counts cases put.gd accepts and the reference refuses), and the command then exits 1.
