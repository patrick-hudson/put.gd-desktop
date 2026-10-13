# Evidence retention

Keep the evidence needed to explain what shipped and reproduce a failure. Test
source and saved test results are different: this policy archives results, not
coverage. Compression changes storage, never a pass/fail decision.

## What to keep

| Material | Retention |
| --- | --- |
| Merged acceptance summaries, source/tree and binary identities, workload parameters, evaluator inputs, original failures, review resolutions, production cleanup receipts | Keep readable and indexed in the repository when sanitized; retain referenced raw evidence |
| Cold source-transfer tar files and large repeated build/test logs | Lossless compression with original and compressed SHA-256, original path, size, mode and restore instructions |
| Latest baseline, active experiment, pending PR/release evidence | Keep directly usable until superseded and reviewed |
| Rebuildable object files and dependency caches | Separate lifecycle decision; verify no active build or installed runtime needs them before removal |
| VM disks, bases, overlays, protected login input, credentials | Excluded from evidence cleanup; use the lab lifecycle rules and verify backing references |
| Synthetic images/databases or diagnostic dumps | Private unless explicitly sanitized; compression does not sanitize content |

Age alone does not justify deletion. A failed attempt can explain the fix better
than another passing run. Keep its first useful error, source identity, command,
expected/actual result and the successful correction. Large repetitive logs can
be archived without retaining every copy inline. Never rewrite historic receipts
or relabel old measurements with a newer documentation commit.

## Find the accepted checkpoints

Start at [the evidence index](evidence/README.md), [verification](verification.md)
and [memory strategy](memory-strategy.md). Historical experiment documents remain
available, but their unmerged/open-gate statements describe their own revision.
The later source114 record includes replayable, sanitized archives and production
cleanup. Keep that curated record independent of private scratch directories.

## Archive cold local files

`tools/maintenance/archive_evidence.py` uses only Python's standard library. It
accepts explicit `.tar`, `.log` and `.xml` files, not recursive directory deletion. Before invoking it:

1. Confirm the workload is finished; inspect running builds, observers and open
   files. Do not compress files a producer or consumer is still using.
2. Record the selected paths and reason. Exclude pending work, credentials, disks,
   Git checkouts, reusable dependency installations and active benchmark inputs.
3. Use a private manifest outside source exports. Keep the summaries and any link
   index readable; consumers needing the old raw path must restore it first.

Example, from the repository root:

```sh
python3 tools/maintenance/archive_evidence.py archive \
  --root "$PWD" --manifest work/housekeeping/archive-index.json \
  work/old-experiment/source.tar
python3 tools/maintenance/archive_evidence.py restore \
  --root "$PWD" --manifest work/housekeeping/archive-index.json \
  work/old-experiment/source.tar
```

The archive is placed beside its original with `.gz` appended. Every compression
is read back and checked against the original SHA-256 before the original is
removed. The manifest records both hashes, sizes, original mode and modification
time. Changed files, symlinks, existing archives, protected paths and multi-link
files are rejected. Files that do not shrink stay untouched. Restoration verifies
both hashes and refuses to overwrite an existing file; it keeps the archive.
Run on an idle tree with one writer. The tool is not a concurrent archival service.
If interrupted, inspect the manifest and original/archive pair before resuming;
never delete an unindexed original merely because a `.gz` exists.

Tests: `python3 -m unittest discover -s tests/maintenance -v`.

## Storage reports and future passes

Record before/after allocated sizes and manifest totals separately: sparse VM
apparent sizes are not disk usage. Do not move or compress referenced VM bases.
Do not erase branches/worktrees that hold the only measured candidate or unmerged
changes. Current worktrees and open PRs need an ownership check before archival.

The October 12 housekeeping pass keeps committed raw evidence in place so its
existing links and replay hashes continue to work. It compresses selected cold
local transfer archives/logs instead. Its private selection and hash index live
under `work/housekeeping/`; these files may contain local paths and are not release
assets. Repeating the same selection is deliberately rejected rather than
silently overwriting the first archive.

## Duplicate screenshots

Screenshots are often already compressed. Recompressing each PNG saves little;
exact-byte deduplication is more useful. Compare SHA-256, not filename, perceived
similarity or image dimensions. Keep unique screenshots and the committed visual
acceptance/failure witnesses. Do not automatically discard different frames just
because the same test passed again.

For cold local duplicates, keep one image per SHA-256 in a private ZIP, with an
index mapping every original path to its hash, size, mode and timestamp. Verify
all archive members and durably save the index before removing any loose files;
rehash each original immediately before removal. A consumer requiring an original
path must restore it first. Compression/deduplication never changes pixel bytes.

The October 12 duplicate-image pass is recorded in the local
`work/housekeeping/screenshot-index.json`, `screenshots.zip` and
`screenshot-result.json`. The saved selection and `dedupe_screenshots.py`
execution record are private operational artifacts, not files to publish.
Do not rerun it on the same output: existing archives are deliberately refused. The index lists every removed
path; each ZIP member is named by that image's SHA-256. To restore one image:

1. Verify the ZIP's SHA-256 against `archive_sha256` in the index.
2. Find the exact original path in `groups`; its group key is the ZIP member/hash.
3. Read that member and verify its SHA-256 and original byte size.
4. Create the original file exclusively (never overwrite), write the bytes, and
   restore its recorded mode and modification time. Keep the ZIP and index.

VM/ISO images are outside this screenshot policy. No visual deduplication,
resampling, lossy compression or automated deletion of unique evidence is used.

### October 12 result

The local pass compressed 25 cold source-transfer archives and consolidated
1,098 duplicate screenshot paths into 432 exact images. Selected artifacts went
from 3,658,742,266 bytes to 2,644,019,624 bytes, saving 1,014,722,642 bytes before
small manifests. This is artifact-byte accounting, not total disk allocation;
the isolated documentation checkout also takes space. Private `summary.json`
records the totals. Two dependency XML inputs caught by the initial inventory
were immediately restored byte-for-byte and excluded from the final archive index.

All archives passed decompression/hash checks, and the screenshot ZIP was
independently read back after consolidation. Seven archive-helper safety tests
passed, as did the preserved source114 saved-evidence replay. No fresh desktop
or live-S3 tests were needed or claimed for this housekeeping change.
