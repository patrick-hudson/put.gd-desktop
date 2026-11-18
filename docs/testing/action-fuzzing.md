# Action-sequence and crash-recovery fuzzing

`tools/lab/action_fuzz.py` drives a sanitizer build of the resident (`clang-asan`) through its session socket with long seeded random sequences, headlessly. It starts the resident under `dbus-run-session` with `QT_QPA_PLATFORM=offscreen`, private XDG roots and a loopback destination, so no desktop, keyring or real account is involved. Run it on a build server, not the workstation, and put `--run-dir` on disk: a run copies its payloads, and `/tmp` on a builder can be tmpfs.

```sh
dbus-run-session -- python3 -I tools/lab/action_fuzz.py --build build/clang-asan --seed 1 --steps 500 run
dbus-run-session -- python3 -I tools/lab/action_fuzz.py --build build/clang-asan --seed 1 --steps 60 --destination s3 crash
```

The destination is the custom HTTP uploader on `tests/fixtures/http_uploader_server.py` (default) or put.gd's S3 provider on the offline oracle `tests/fixtures/s3_server.py` (`--destination s3`; the 65 MiB fixture crosses the multipart threshold). Session credentials are stored with `setup.storeCredentials` and again after every restart.

## Modes

- `run` calls session methods in random valid and malformed orders: editor sessions and commands, uploads (also of folders, FIFOs, links, unreadable or vanishing files), History, deletion, operation control, settings, concurrent bursts, outside edits of `config.json`, an unwritable archive folder, helpers killed or frozen on their own, and graceful restarts.
- `faults` makes the OS and the network say no underneath the resident, between the same random actions as `run` (uploads, deletions, History, settings, operation control, restarts). See "Fault injection" below.
- `crash` SIGKILLs the resident and every process of its session at a milestone of an upload, deletion or settings write (and sometimes a second time while the restart is recovering), restarts it and checks recovery.

## What is checked

Each step: the resident lives; no sanitizer or log output. Every few steps: settings valid; History counts, filtered lists and rows agree. After every kill: `config.json` parses; `history.sqlite` (copied with its journal) passes `integrity_check`; journal recovery finishes; no operation or History row stays active; no upload is on the server twice or unknown to History; a deletion agrees with the receiver; on S3 a multipart upload with parts belongs to a journal put.gd still keeps. A retry after an unknown outcome may store a second copy on the custom uploader (it cannot look a file up), and a failed retry turns that unknown into failed. Neither is a finding.

A finding stops the run and is kept in `<run-dir>/finding.json` with `steps.jsonl`, the sanitizer logs and, for a hang, `stacks.txt` (eu-stack of every process). The seed and `--steps` replay the same sequence; timing still varies.

## Traps

- An ASan build starts helpers through Qt's fork fallback: a child waits for its parent before it execs. If the resident is killed at that moment the child lives on holding the listening socket and `resident.lock`, so the next resident exits 0 or stays initializing. The harness kills every process of the resident's session, not only its children.
- `/proc/PID/task/PID/children` lists only the children of the main thread; a worker thread's child is missing.
- `operation.recent` is bounded by the retention setting, so a harness cannot find every open editor there.

## Fault injection

`faults` mode (`--fault-classes fs,net,res`) injects what a kill cannot: errors. Faults come from the harness's seeded generator and are logged (`steps.jsonl`, `logs/faultshim.log`, `logs/proxy.jsonl`), so a finding replays from its seed and `--steps` (timing still varies).

- **File system** (`fs`): `tools/lab/faults/faultshim.c`, an `LD_PRELOAD` shim, fails `open`, `write`, `read`, `fsync`, `rename`, `truncate`, `fallocate`, `unlink`, `mkdir`, `copy_file_range` and `close` on a rule file the harness rewrites while the resident runs (a path filter, a probability, `ENOSPC`, `EIO`, `EACCES`, `EROFS`, `EMFILE`, `ENOMEM`, `EXDEV`, a short write, or a delay for a slow disk). It reaches `config.json`, `history.sqlite` and its journal, the operation journal and artifacts, the archive and the sources. With `--small-fs DIR` (a filesystem made by `tools/lab/faults/smallfs.sh`; put.gd keeps 1 GiB free, so make it a little bigger) the resident's config, data, state, cache and archive live on it and a filler file squeezes it to the margin just above the reserve, or below it.
- **Network** (`net`): `tools/lab/faults/fault_proxy.py` sits between put.gd and the fixture server and, per connection, resets before, during or after the request, drops or cuts the reply, stalls (before or after the answer), duplicates the reply, throttles, or adds latency and jitter; the fixtures' own rules (lost replies, 4xx/5xx) combine with it. The custom uploader fixture runs with `PUTGD_HTTP_FIXTURE_TRUST_HOST=1` so its file and deletion addresses stay behind the proxy.
- **Resources** (`res`): the open-file limit drops under the resident for a moment with an upload starting (`prlimit`); `--small-rt DIR` puts the runtime directory on a tiny filesystem; `--nofile N` and `--memory-max 600M` (a `systemd-run --user --scope`) limit it from the start.

Every `--epoch` steps (and at the end) the harness lifts every fault and requires: the resident alive and its journal recovery finished; no sanitizer output; `config.json` parsing and `history.sqlite` passing `integrity_check`; no operation or row active; no row uploaded that the server lacks, nothing on the server that History does not know, no second copy without an unknown outcome or an explicit retry in between; an upload that was unknown and whose copy is on the server still unknown (#1572); and, with space back, a fresh upload, a settings write and a History read all working again.

`tools/lab/faults/faultrun.sh BUILD OUT FIRST LAST STEPS http|s3 fs,net,res` runs a range of seeds, each on a freshly made small filesystem; the filesystem a finding leaves is copied into the run folder (`sfs-left`).

Traps: the shim needs `ASAN_OPTIONS=verify_asan_link_order=0` (set by the harness); a job started with `&` from a shell inherits SIGINT ignored, which the harness resets; Qt refuses a symbolic link as the runtime directory; an editor session waits for its user for ever, so the epoch check cancels them first.
