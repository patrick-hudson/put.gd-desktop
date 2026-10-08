# Standalone SQLite locating replay

Use existing distro Qt6.4.2+ Core/Sql, C++20, CMake and Ninja. Coordinate a quiet
slot before running. No WAL case is authorized here. No existing user history,
configuration, credential or payload input is read.

[Source](source/), [build receipt](build/build-identity.json), [exact recorded
parameters/PIDs/library hashes](raw/manifest.json), [portable order](parameters.json).

From the repository root, regenerate private synthetic databases with fresh
processes, six cases forward and then reversed. Stop/preserve any failure;
do not silently repeat it.

~~~bash
umask 077
sqlite_probe_run=$(mktemp -d /tmp/putgd-sqlite-replay.XXXXXXXX)
cmake -S docs/evidence/2026-10-08-sqlite-locating-raw/source -B "$sqlite_probe_run/build" -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build "$sqlite_probe_run/build" --parallel 1
sqlite_probe_bin="$sqlite_probe_run/build/sqlite-latency-probe"
sqlite_probe_ordinal=0
for sqlite_probe_case in "reopen 2000" "connection 2000" "statements 2000" "reopen 512" "connection 512" "statements 512" "statements 512" "connection 512" "reopen 512" "statements 2000" "connection 2000" "reopen 2000"; do
  sqlite_probe_ordinal=$((sqlite_probe_ordinal+1))
  printf -v sqlite_probe_label '%02d' "$sqlite_probe_ordinal"
  read -r sqlite_probe_mode sqlite_probe_cache <<< "$sqlite_probe_case"
  "$sqlite_probe_bin" "$sqlite_probe_run/$sqlite_probe_label" "$sqlite_probe_mode" "$sqlite_probe_cache" --lock >"$sqlite_probe_run/$sqlite_probe_label.stdout.log" 2>"$sqlite_probe_run/$sqlite_probe_label.stderr.log"
  sqlite_probe_exit=$?
  printf '%s\n' "$sqlite_probe_exit" >"$sqlite_probe_run/$sqlite_probe_label.exit"
  if [ "$sqlite_probe_exit" -ne 0 ]; then break; fi
done
sha256sum docs/evidence/2026-10-08-sqlite-locating-raw/source/probe.cpp docs/evidence/2026-10-08-sqlite-locating-raw/source/CMakeLists.txt "$sqlite_probe_bin"
~~~

Record actual compiler/Qt/SQLite and source/binary/library hashes, results and
errors. The original controller has 45-second case/600-second total deadlines
and an owned-child /proc library sampler; its absolute paths remain verbatim
provenance. This portable shell recipe has no deadlines/library sampler and does
not supersede the original run receipt.

Each case has five warmups and thirty scored iterations, each with three
FULL-sync durable synthetic states. A separate 10000-row scan compares cache
caps/shrink/re-read RSS/time. The injected 100 ms writer lock is diagnostic,
not the actual physical fsync delay. Preserve the overwritten-reader-beginNs
limitation: absolute reader start is unavailable; phase durations and writer
timestamps remain usable.

Generated databases/journals and all payload bytes are excluded. Retain private
results before removing only the specifically created temporary tree. These
standalone results certify neither native 40 MiB/150 ms gates nor production
history/recovery/concurrency/durability. [Report](report.md),
[summary](summary.json), [official SQLite sources](official-sources.json).
