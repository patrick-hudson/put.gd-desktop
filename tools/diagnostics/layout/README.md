# Inspect inline type layout

This standalone C++20 probe reads the current public application headers at compile
time and prints JSON with compiler/Qt versions, pointer width, `sizeof` and
`alignof`. It constructs no application, GUI, model or service; it accesses no
configuration, history, clipboard, network or payload and dumps no object memory.
It uses the existing dynamically linked distribution Qt 6.4.2+ and normal
CMake/Ninja. It does not build application sources or use the root CMake project.

From the repository root, these commands create a fresh build directory each time:

```sh
(
set -eu
mkdir -p work/layout-inspection
layout_run=$(mktemp -d work/layout-inspection/run-XXXXXX)
cmake -S tools/diagnostics/layout -B "$layout_run/build" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  > "$layout_run/configure.log" 2>&1
cmake --build "$layout_run/build" > "$layout_run/build.log" 2>&1
"$layout_run/build/putgd-layout-inspect" > "$layout_run/layout.json"
ninja -C "$layout_run/build" -t deps > "$layout_run/compiler-dependencies.txt"
sha256sum tools/diagnostics/layout/CMakeLists.txt \
  tools/diagnostics/layout/main.cpp tools/diagnostics/layout/README.md \
  "$layout_run/build/putgd-layout-inspect" "$layout_run/layout.json" \
  > "$layout_run/sha256.txt"
printf 'Saved layout inspection in %s\n' "$layout_run"
)
```

Stop if configure, build or run fails. Preserve the logs and compiler flags in
`build/CMakeCache.txt` and `build/compile_commands.json`. Record the exact commands,
platform, compiler/Qt versions, output, source hashes and binary hash together.
The application headers are inputs too: preserve/hash the repository headers in
the saved `compiler-dependencies.txt` list,
not unrelated files or private inputs. Re-run after header, compiler, ABI or Qt
changes; an earlier output does not describe a later build.

`ResourceLimits` member offsets use `offsetof` guarded by a standard-layout
`static_assert`. Other production types get only size/alignment metadata;
the probe applies no `offsetof` to non-standard-layout Qt types or models.
The synthetic structs show two separate cases: narrowing a count can leave the
same stride because alignment padding grows; grouping larger-alignment members
can reduce padding without narrowing. Their sizes follow the measured ABI.
They are examples, not recommendations for changing the application.

`<cstdint>` names describe integer widths and bounds; they do not automatically
pack members or reduce allocations. `std::int32_t`, `qint32` and `int` may have
the same representation. `int_least32_t` guarantees at least 32 bits
including sign; `int_fast32_t` can be wider. The JSON includes their actual sizes
and numeric limits. Limits are strings to preserve 64-bit values exactly.
Primary definitions are the [C++ integer-width synopsis](https://eel.is/c++draft/cstdint.syn)
and [Qt integer type aliases](https://doc.qt.io/qt-6/qttypes.html).
The probe uses the established `qint32`/`qint64` aliases available in Qt 6.4.2.

`sizeof` is the inline object stride, including padding. It excludes allocations
behind `QString`, JSON, containers, QObject and image/other buffers. Implicit
sharing, container capacity, allocator rounding and pages further separate a
stride change from resident memory. The probe observes no live object counts.
For a proposed change, estimate `live count × (old stride − new stride)` first,
and state which workload and population supplied that count. Zero live records
imply zero record-stride saving. Then measure the actual workload's RSS/CPU and
first-action behavior with the same frozen observer/reference; this output alone
cannot establish any memory target or acceptance gate.

Before narrowing, validate defaults, every allowed value, intermediate arithmetic,
signedness, casts, overflow checks, serialized/database fields and compatibility.
For example, the current `ResourceLimits` defaults for `maxFileBytes`,
`maxBatchBytes` and `pendingArtifactBytes` are 2,147,483,648, exceeding signed
32-bit maximum. Reordering can also change ABI, aggregate initialization or
layout-dependent interfaces; check all consumers and the private module's build
identity, and rebuild matching binaries. Preserve semantics and run the relevant
contracts before remeasuring. Keep or reject a production change using its actual
saving and regression evidence; this diagnostic changes no production type.
