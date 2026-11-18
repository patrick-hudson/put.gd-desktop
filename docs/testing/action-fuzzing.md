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

`faults` mode (`--fault-classes fs,net,res`) injects what a kill cannot: errors. Every fault is chosen by the harness's seeded generator and logged (`steps.jsonl`, `logs/faultshim.log`, `logs/proxy.jsonl`). A seed and `--steps` reproduce the plan only roughly, and only with the same fault options (`--fault-classes`, `--net-kinds`, `--epoch`, `--nofile` and the rest). Some random draws depend on live state (which rows, operations and editors exist when a step runs). The proxy assigns plans by connection order, and the shim mixes the process ID into its generator. So the same seed can meet different faults, and timing decides which call a rule meets.

- **File system** (`fs`): `tools/lab/faults/faultshim.c`, an `LD_PRELOAD` shim, fails `open`, `write`, `read`, `fsync`, `rename`, `truncate`, `fallocate`, `unlink` and `mkdir` from a rule file the harness rewrites while the resident runs. A rule has a path filter, a probability and an action: `ENOSPC`, `EDQUOT`, `EIO`, `EACCES`, `EROFS`, `EMFILE`, `ENOMEM`, `EXDEV`, a short write, or a delay for a slow disk. The shim can also fail `copy_file_range`, `sendfile` and `close`, but the harness's generator never emits those rules. Faults reach `config.json`, `history.sqlite` and its journal, the operation journal and artifacts, the archive and the upload sources.
- **A full disk is a shim rule.** It is writes, truncates and allocations failing with `ENOSPC` or `EDQUOT`. The harness mounts, fills and unmounts nothing. A known gap follows from that: put.gd's own free-space checks (the 1 GiB reserve, `disk_space_insufficient`) see the host's real free space, so the margin just above the reserve is not exercised.
- **Only the resident gets syscall faults.** The harness names the resident's executable (`PUTGD_FAULT_EXE`) and starts it through `sh -c 'PUTGD_FAULT_PID=$$; exec …'`, so the marker is the resident's own PID. The shim binds only in a process of that executable whose PID matches the marker; with no marker it never binds. The bound resident removes `LD_PRELOAD` and the rule path from its environment, so the image, editor, recording and upload helpers start without the shim. A fork, or an exec with a copy of the first environment, has a different PID or no marker, and gets no fault. Each epoch check fails the run if the shim log names any other PID. This covers the shim only: the proxy faults every connection to the destination, helpers' included.
- **Network** (`net`): `tools/lab/faults/fault_proxy.py` sits between put.gd and the fixture server. Per connection it resets before, during or after the request, drops or cuts the reply, stalls (before or after the answer), duplicates the first chunk of the reply, throttles, or adds latency and jitter. The fixtures' own rules (lost replies, 4xx/5xx) combine with it. The custom uploader fixture runs with `PUTGD_HTTP_FIXTURE_TRUST_HOST=1`, so its file and deletion addresses stay behind the proxy.
- **Lifting network faults**: a connection takes its plan and registers in one step, under the lock a profile change takes. Every profile change starts a new generation. Clearing the profile retires every open connection of an earlier generation whose plan is not a plain pass: it resets both sockets, wakes its stalls and waits up to 15 s for it to end. A connection that does not end in that time is a finding, so no old fault reaches the recovery check.
- **Resources** (`res`): the open-file limit drops under the resident for a moment while an upload starts (`prlimit`). `--nofile N` and `--memory-max 600M` (a `systemd-run --user --scope`) limit it from the start.

Every `--epoch` steps, and at the end, the harness clears the transient faults (the shim's rules, the proxy profile and its faulted connections, the open-file limit) and checks that the resident recovered:

- The resident is alive and its journal recovery has finished. If it aborts during the check (an open-files abort or an OOM kill the harness caused), it is restarted and checked again; three aborts in a row are a finding.
- There is no sanitizer output, and `config.json` parses.
- `history.sqlite` passes `integrity_check` on a snapshot taken through SQLite's backup API. A database that gives no snapshot within 60 s is a finding.
- No operation or row is still active.
- No row says uploaded while the server no longer holds a copy, and nothing is on the server that History does not know.
- The server holds no more copies of a file than the run's upload attempts explain: one per History row, one per accepted `operation.retry` without a scope, and one per row that was ever unknown.
- The unknown-outcome check (#1572) flags a row that was unknown and is now failed or cancelled while the server still holds its content and its receipt has no direct URL.
- A fresh upload ends uploaded, and its receipt names a file the server holds with the probe's bytes: the HTTP receipt on the address put.gd was given, the S3 receipt with its version ID when it has one. A settings write and a History read work too.

What a clean run does not prove:

- **Sanitizer noise is filtered.** UBSan's report about a valid object it cannot type-check (a "does not point to an object of type" error whose note says "invalid vptr") is ignored when the resident wrote it while the harness held the resident at its open-file limit. That window ends when the limit is restored. Any other runtime error is a finding.
- **Deletes can explain a missing copy.** A row whose deletion was attempted may have no copy on the server.
- **`--known-journal-stuck` hides #1579.** It counts rows left uploading after a journal write fault (#1579) instead of stopping the run on them.
- **A start under a fault can end the run.** Without `--clean-restarts`, a restart under an active shim rule can end the run with a startup finding.

### Running seeds

`tools/lab/faults/faultrun.sh BUILD OUT FIRST LAST STEPS http|s3 fs,net,res` runs a range of seeds, each in a new run folder, `OUT/runs/f-DEST-SEED`.

- Use a new, short, private, absolute `OUT`. A Unix socket path has at most 107 bytes. Before launching, check `OUT/runs/f-DEST-SEED/r/rt/putgd/session.sock` against 107 UTF-8 bytes for the longest seed number.
- A finding stops that seed, and the loop goes on; the exit status is the number of findings.
- Ctrl+C or SIGTERM stops the harness through its one cleanup path, which also covers setup, and the loop stops with it. The resident and the receiver run in sessions of their own, so the signal never reaches them; the harness stops both:
  - **The resident** is signalled through a pidfd opened when it started. The process and its pidfd are published together (a signal is held until both are set). The resident is reaped only after its session is clear, so its PID and its session ID cannot be reused while the harness still acts on them.
  - **Helpers** are signalled through a pidfd after their environment shows the run's token. A helper that hides its environment (non-dumpable, like the OCR helper) is accepted only if it is in the resident's session while the resident is not yet reaped.
  - **The receiver** (the fixture server) is stopped through `subprocess.Popen`: terminate, then kill after 5 s.

Traps: the shim needs `ASAN_OPTIONS=verify_asan_link_order=0` (set by the harness). An editor session waits for its user for ever, so the epoch check cancels every editor first.
