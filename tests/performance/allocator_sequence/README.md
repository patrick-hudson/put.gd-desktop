# Reproducing the allocator sequence diagnostic

This standalone Linux/glibc diagnostic locates allocator retention. It does not
measure the put.gd resident, certify a desktop, or replace any product gate.
Use the same pinned source with distro C++20, dynamically linked Qt6 Core/Gui
(minimum6.4.2), CMake and Ninja. No new dependency or allocator is introduced.

Create a short private build/output directory, record the exact command and source
hashes, then build with normal tools:

```sh
umask 077
allocator_run=$(mktemp -d /tmp/pga-XXXXXX)
cmake -S tests/performance/allocator_sequence -B "$allocator_run/build" -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build "$allocator_run/build"
```

Before launch inventory resolved Unix sockets. This executable creates none:
there is no QApplication, resident, session.sock, GUI job, fixture server, or
socket-owning harness. Record the empty socket list. If a later wrapper introduces
any socket, compute every resolved UTF-8 path with the production socket checker
and reject paths exceeding107bytes before launching. Do not borrow an existing
application runtime or weaken ownership guards.

Run modes in separate fresh processes, serially. At most three runs per mode:

```sh
timeout 15s "$allocator_run/build/putgd-allocator-sequence-probe" dynamic > "$allocator_run/dynamic-1.json"
timeout 15s "$allocator_run/build/putgd-allocator-sequence-probe" static128k > "$allocator_run/static128k-1.json"
```

Save exit codes, monotonic durations, build log/version, source hashes and each
original JSON stdout before computing summaries. Keep rejected runs and explain
corrections; do not overwrite original files or select the smallest measurement.
Do not change MALLOC_* or GLIBC_TUNABLES to make a run pass. Record only bounded
allocator-policy provenance if the environment is intentionally tuned; never dump
arbitrary environment values. The static mode explicitly sets this process's
mmap threshold131072bytes before QCore initialization, checks success and disables
dynamic threshold adaptation. It does not change the desktop/system policy.

Both modes perform four stages:640×448,1280×800,640×448,1280×800. Every stage
append-grows raw ARGB32 QByteArray in65536-byte chunks. A temporary decode QThread
deep-copies the raw image; a separate temporary encode QThread writes PNG, reads
explicit PNG and encodes a thumbnail bounded to256×256. Threads are joined and
deleted, captured arrays and full-size images released before measurement and
malloc_trim(0). Only four-color synthetic pixels are used; none are exported.

Validate result-contract.json: exact mode, policy result, four sizes, raw byte and
capacity bounds, successful PNG stages, required nonnegative numeric snapshots,
normal exit0 and duration under15seconds. For every run report final-minus-before
RSS/PSS/anonymous and allocator live/free/arena/mmap bytes. RSS and allocator live
bytes answer different questions. Fixed-threshold reduction with similar live
bytes supports an allocator-retention explanation for this sequence; it does not
prove that every product allocation is free or that all large requests use mmap.
Existing arena chunks can still satisfy allocations.

Keep actual host and guest originals separate, with Qt/glibc versions. Preserve
primary source URLs/version qualifications without exporting full glibc sources.
Inspected2.39/2.42 source supports a mechanism; the independently reported guest
version2.43 is not an inspected2.43 source. No guest result is invented here.

Any product change still requires an approved candidate scope, versioned build,
current KDE functional/UI/capture/clipboard/helper checks, serial original cold
and post-native300-second RSS/CPU runs, and current Cinnamon/GNOME regressions in
the user-approved order. Preserve original miss evidence and native/remote gate
qualifications. Standalone gains authorize no product keeper or deployment.
