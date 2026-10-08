# Implementation evidence

Evidence below records observed checks, not the remaining release scope. The full
contract is [issue #1](https://github.com/patrick-hudson/put.gd-desktop/issues/1).

| Unit / slice | Observed evidence | Remaining gate |
| --- | --- | --- |
| U1 bootstrap | Mint 22.3, GCC 13.3, Qt 6.4.2, QtKeychain 0.14.2; AWS 1.11.907 plus 15 pinned submodules built; Debug contracts and no-test Release build passed; ASan/UBSan contracts passed outside process sandbox | Authored CI jobs have not run on Ubuntu 26.04, Windows, or macOS; TSan configured only |
| U2 configuration service | Test-first missing-service/link failures; explicit regression failures observed for null folder patches and unwritable lock creation before their fixes; all three current test executables passed in Debug and ASan/UBSan after the fixes | Provider validator integration, development example, GUI/CLI round trips, operation/history snapshots and remaining U2 services |
| U3 S3 | SDK serializes conditional headers in a local test | Signed requests, full offline fixture, secrets, synthetic destination acceptance and all deletion cases |
| U4–U10 | Not implemented or verified yet | Native workflows, UI/history, real desktop lab, GNOME/KDE adapters, MCP, packaging and release evidence |

Configuration tests cover fresh setup without an implicit cloud profile, private atomic
writes, stale revisions, retained accepted settings after malformed/missing/oversized files,
repeated external replacements, actual periodic discovery when the parent directory was
initially missing, own-write notification deduplication, strict root types/keys/references,
cooperative lock contention, nullable patch values, and an unwritable parent directory.
All use temporary synthetic configuration; they access no clipboard, screen, cloud,
or secret store. Offscreen tests are not compositor acceptance evidence.
