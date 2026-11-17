# Action-sequence and crash-recovery fuzzing

`tools/lab/action_fuzz.py` drives a sanitizer build of the resident (`clang-asan`) through its session socket with long seeded random sequences, headlessly. It starts the resident under `dbus-run-session` with `QT_QPA_PLATFORM=offscreen`, private XDG roots and a loopback destination, so no desktop, keyring or real account is involved. Run it on a build server, not the workstation, and put `--run-dir` on disk: a run copies its payloads, and `/tmp` on a builder can be tmpfs.

```sh
dbus-run-session -- python3 -I tools/lab/action_fuzz.py --build build/clang-asan --seed 1 --steps 500 run
dbus-run-session -- python3 -I tools/lab/action_fuzz.py --build build/clang-asan --seed 1 --steps 60 --destination s3 crash
```

The destination is the custom HTTP uploader on `tests/fixtures/http_uploader_server.py` (default) or put.gd's S3 provider on the offline oracle `tests/fixtures/s3_server.py` (`--destination s3`; the 65 MiB fixture crosses the multipart threshold). Session credentials are stored with `setup.storeCredentials` and again after every restart.

## Modes

- `run` calls session methods in random valid and malformed orders: editor sessions and commands, uploads (also of folders, FIFOs, links, unreadable or vanishing files), History, deletion, operation control, settings, concurrent bursts, outside edits of `config.json`, an unwritable archive folder, helpers killed or frozen on their own, and graceful restarts.
- `crash` SIGKILLs the resident and every process of its session at a milestone of an upload, deletion or settings write (and sometimes a second time while the restart is recovering), restarts it and checks recovery.

## What is checked

Each step: the resident lives; no sanitizer or log output. Every few steps: settings valid; History counts, filtered lists and rows agree. After every kill: `config.json` parses; `history.sqlite` (copied with its journal) passes `integrity_check`; journal recovery finishes; no operation or History row stays active; no upload is on the server twice or unknown to History; a deletion agrees with the receiver; on S3 a multipart upload with parts belongs to a journal put.gd still keeps. A retry after an unknown outcome may store a second copy on the custom uploader (it cannot look a file up), and a failed retry turns that unknown into failed. Neither is a finding.

A finding stops the run and is kept in `<run-dir>/finding.json` with `steps.jsonl`, the sanitizer logs and, for a hang, `stacks.txt` (eu-stack of every process). The seed and `--steps` replay the same sequence; timing still varies.

## Traps

- An ASan build starts helpers through Qt's fork fallback: a child waits for its parent before it execs. If the resident is killed at that moment the child lives on holding the listening socket and `resident.lock`, so the next resident exits 0 or stays initializing. The harness kills every process of the resident's session, not only its children.
- `/proc/PID/task/PID/children` lists only the children of the main thread; a worker thread's child is missing.
- `operation.recent` is bounded by the retention setting, so a harness cannot find every open editor there.
