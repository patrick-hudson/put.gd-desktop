#pragma once
// Any libc header defines __GLIBC__; test it only after one, whatever the includer did first.
#include <cstdlib>
#if defined(__linux__) && defined(__GLIBC__)
#include <malloc.h>
#endif

namespace putgd {
// Resident glibc policy: a fixed 128 KiB mmap threshold. Large transient
// buffers (captures, editor rasters, PNG handoffs, bridge transfers) are then
// mmapped and unmapped on free. Without it glibc raises the threshold after the
// first large free, later rasters land in per-thread arenas, and their freed
// tops stay resident because malloc_trim only shrinks the main arena's top.
// The same limitation applies to small chunks: upload, journal and query
// workers each took their own arena, and after a burst those arenas kept
// megabytes of entirely free top chunk that the idle malloc_trim could not
// return (#560). One arena keeps every free top trimmable; the workers are
// I/O bound and per-thread caches still serve their small allocations.
// The editor helper uses the same policy for its previews and exports.
// Call before application construction, before worker threads start.
// Returns false only if glibc refused.
inline bool applyResidentAllocatorPolicy() {
#if defined(__linux__) && defined(__GLIBC__)
 const bool threshold=mallopt(M_MMAP_THRESHOLD,128*1024)==1;
 const bool arenas=mallopt(M_ARENA_MAX,1)==1;
 return threshold&&arenas;
#else
 return true;
#endif
}
}
