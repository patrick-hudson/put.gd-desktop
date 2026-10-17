#pragma once
#if defined(__linux__) && defined(__GLIBC__)
#include <malloc.h>
#endif

namespace putgd {
// Resident glibc policy: a fixed 128 KiB mmap threshold. Large transient
// buffers (captures, editor rasters, PNG handoffs, bridge transfers) are then
// mmapped and unmapped on free. Without it glibc raises the threshold after the
// first large free, later rasters land in per-thread arenas, and their freed
// tops stay resident because malloc_trim only shrinks the main arena's top.
// Call before application construction. Returns false only if glibc refused.
inline bool applyResidentAllocatorPolicy() {
#if defined(__linux__) && defined(__GLIBC__)
 return mallopt(M_MMAP_THRESHOLD,128*1024)==1;
#else
 return true;
#endif
}
}
