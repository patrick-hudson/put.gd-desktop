# Action-sequence and crash-recovery fuzzing

`tools/lab/action_fuzz.py` drives a sanitizer build of the resident (`clang-asan`) through its session socket with long seeded random sequences, headlessly. It starts the resident under `dbus-run-session` with `QT_QPA_PLATFORM=offscreen`, private XDG roots and a loopback destination, so no desktop, keyring or real account is involved. Run it on a build server, not the workstation, and put `--run-dir` on disk: a run copies its payloads, and `/tmp` on a builder can be tmpfs.

```sh
dbus-run-session -- python3 -I tools/lab/action_fuzz.py --build build/clang-asan --seed 1 --steps 500 run
dbus-run-session -- python3 -I tools/lab/action_fuzz.py --build build/clang-asan --seed 1 --steps 60 --destination s3 crash
```

The destination is the custom HTTP uploader on `tests/fixtures/http_uploader_server.py` (default) or put.gd's S3 provider on the offline oracle `tests/fixtures/s3_server.py` (`--destination s3`; the 65 MiB fixture crosses the multipart threshold). Session credentials are stored with `setup.storeCredentials` and again after every restart.

## Modes

- `run` calls session methods in random valid and malformed orders: editor sessions and commands, uploads (also of folders, FIFOs, links, unreadable or vanishing files), History, deletion, operation control, settings, concurrent bursts, outside edits of `config.json`, an unwritable archive folder, helpers killed or frozen on their own, and graceful restarts.
- `faults` makes the OS and the network say no underneath the resident. It interleaves faults with a subset of the actions `run` uses (uploads, deletions, History, settings, operation control, editor sessions, bursts, restarts). See "Fault injection" below.
- `crash` SIGKILLs the resident and every process of its session at a milestone of an upload, deletion or settings write (and sometimes a second time while the restart is recovering), restarts it and checks recovery.

## What is checked

Each step: the resident lives; no sanitizer or log output. Every few steps: settings valid; History counts, filtered lists and rows agree. After every kill: `config.json` parses; `history.sqlite` (copied with its journal) passes `integrity_check`; journal recovery finishes; no operation or History row stays active; no upload is on the server twice or unknown to History; a deletion agrees with the receiver; on S3 a multipart upload with parts belongs to a journal put.gd still keeps. A retry after an unknown outcome may store a second copy on the custom uploader (it cannot look a file up), and a failed retry turns that unknown into failed. Neither is a finding.

A finding stops the run and is kept in `<run-dir>/finding.json` with `steps.jsonl`, the sanitizer logs and, for a hang, `stacks.txt` (eu-stack of every process). The seed and `--steps` replay the same sequence; timing still varies.

## Traps

- An ASan build starts helpers through Qt's fork fallback: a child waits for its parent before it execs. If the resident is killed at that moment the child lives on holding the listening socket and `resident.lock`, so the next resident exits 0 or stays initializing. The harness kills every process of the resident's session, not only its children.
- `/proc/PID/task/PID/children` lists only the children of the main thread; a worker thread's child is missing.
- `operation.recent` is bounded by the retention setting, so a harness cannot find every open editor there.

## Fault injection

`faults` mode (`--fault-classes fs,net,res`) injects what a kill cannot: errors. Every fault is chosen by the harness's seeded generator and logged (`steps.jsonl`, `logs/faultshim.log`, `logs/proxy.jsonl`). A seed and `--steps` reproduce the same fault plan only with the same fault options (`--fault-classes`, `--net-kinds`, `--epoch`, `--small-fs`, `--small-rt`, `--nofile` and the rest). Individual fault outcomes still vary between runs: the shim mixes the process ID into its generator, and timing decides which call a rule meets.

- **File system** (`fs`): `tools/lab/faults/faultshim.c`, an `LD_PRELOAD` shim, fails `open`, `write`, `read`, `fsync`, `rename`, `truncate`, `fallocate`, `unlink` and `mkdir` from a rule file the harness rewrites while the resident runs. A rule has a path filter, a probability and an action: `ENOSPC`, `EIO`, `EACCES`, `EROFS`, `EMFILE`, `ENOMEM`, `EXDEV`, a short write, or a delay for a slow disk. The shim can also fail `copy_file_range`, `sendfile` and `close`, but the harness's generator never emits those rules. Faults reach `config.json`, `history.sqlite` and its journal, the operation journal and artifacts, the archive and the upload sources.
- **Only the resident is faulted.** The harness names the resident's executable (`PUTGD_FAULT_EXE`). The shim binds to the first process of that executable and its PID, and removes `LD_PRELOAD` and its rule path from that process's environment, so the image, editor, recording and upload helpers start without it. A fork, or a helper that still gets the shim, has a different PID and gets no fault. Each epoch check fails the run if the shim log names any other PID.
- **Small filesystem** (`--small-fs DIR`): the resident's config, data, state, cache and archive live on a filesystem made by `tools/lab/faults/smallfs.sh`, and a filler file squeezes it to the margin just above put.gd's 1 GiB free-space reserve, or below it. Make it a little bigger than the reserve (`faultrun.sh` uses 1300m). The harness refuses `DIR` and writes nothing there unless it is a fresh mount that `smallfs.sh` made: a mount point of its own that matches the record `smallfs.sh` wrote next to it (`DIR.smallfs`), no larger than 4 GiB, and empty. A plain folder or the host's own disk is refused, so a squeeze never fills a real disk. The filler is created with `O_EXCL|O_NOFOLLOW` and never opened by name again, so a link planted at `.filler` is not followed.
- **Network** (`net`): `tools/lab/faults/fault_proxy.py` sits between put.gd and the fixture server. Per connection it resets before, during or after the request, drops or cuts the reply, stalls (before or after the answer), duplicates the first chunk of the reply, throttles, or adds latency and jitter. The fixtures' own rules (lost replies, 4xx/5xx) combine with it. The custom uploader fixture runs with `PUTGD_HTTP_FIXTURE_TRUST_HOST=1`, so its file and deletion addresses stay behind the proxy. A plan holds for the life of its connection, so lifting network faults also resets every open connection with a fault plan and waits for it to end.
- **Resources** (`res`): the open-file limit drops under the resident for a moment while an upload starts (`prlimit`). `--small-rt DIR` puts the runtime directory on a tiny `smallfs.sh` filesystem, checked the same way. `--nofile N` and `--memory-max 600M` (a `systemd-run --user --scope`) limit the resident from the start. UBSan's invalid-vptr report (it cannot open the files it checks types with) is expected only in the part of the resident log written while the harness held the resident at its open-file limit; any other runtime error is a finding.

Every `--epoch` steps, and at the end, the harness clears the transient faults (the shim's rules, the proxy profile and its faulted connections, the filler, the open-file limit) and checks that the resident recovered:

- The resident is alive and its journal recovery has finished. If it aborts during the check (an open-files abort or an OOM kill the harness caused), it is restarted and checked again, and three aborts in a row are a finding.
- There is no sanitizer output, `config.json` parses, and `history.sqlite` passes `integrity_check`. A live resident's database is read through SQLite's backup API.
- No operation or row is still active.
- No row says uploaded while the server has no copy of it, and nothing is on the server that History does not know. The server holds no more copies of a file than the run's upload attempts explain: one per History row, one per accepted `operation.retry`, and one per row that was ever unknown.
- The unknown-outcome check (#1572) flags a row that was unknown and is now failed or cancelled while the server still holds its content and its receipt has no direct URL.
- With space back, a fresh upload ends uploaded, its receipt names a file the server stored with the probe's bytes, and a settings write and a History read work.

### Running seeds

`tools/lab/faults/faultrun.sh BUILD OUT FIRST LAST STEPS http|s3 fs,net,res` runs a range of seeds. Each seed gets a new run folder, `OUT/runs/f-DEST-SEED`, and freshly mounted small filesystems, `OUT/sfs` and, with `res`, `OUT/srt`.

- Use a new, short, private, absolute `OUT`. A Unix socket path has at most 107 bytes. Before launching, check each resolved socket path against 107 UTF-8 bytes: `OUT/srt/putgd/session.sock` with `res`, otherwise `OUT/runs/f-DEST-SEED/r/rt/putgd/session.sock` for the longest seed number.
- A mount or unmount that fails stops the whole run (status 100). The script unmounts only the filesystems it mounted itself and checked against their records, after every seed and on every exit, Ctrl+C included. `smallfs.sh umount` refuses a mount, or a loop image, that it did not make.
- When a seed finds something, the script first copies what was left on the small filesystems into `sfs-left` and `srt-left` in the run folder. That copy is best-effort and leaves out the filler. Then it unmounts them and checks the unmount.
- Ctrl+C or SIGTERM stops the harness through its one cleanup path: the resident, its helpers and the receiver run in sessions of their own and are stopped by the harness, which signals only processes whose pidfd it holds and whose environment carries the run's token.

Traps: the shim needs `ASAN_OPTIONS=verify_asan_link_order=0` (set by the harness). Qt refuses a symbolic link as the runtime directory, so `--small-rt` is the mount itself. An editor session waits for its user for ever, so the epoch check cancels every editor first.
