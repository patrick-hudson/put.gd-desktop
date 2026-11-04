#pragma once
#include <QtGlobal>

namespace putgd {
struct ImagePageRelease {int segments=0,skipped=0;qint64 bytes=0,keptBytes=0;};
// One bit per whole page of every loaded object's read-only and code segments,
// set once that page was seen mapped. A release leaves those pages alone, so a
// hotkey press under memory pressure still finds its own path mapped. It is only
// a hint: the release verifies every segment itself, so a stale snapshot can keep
// too much or too little and is never unsafe.
// Give it static storage: it relies on starting zeroed, its 48 KiB sit in .bss and
// only the part in use is ever touched. That is room for 340 objects and 1 GiB.
struct ImagePageSnapshot {
 struct Segment {quintptr start;quint32 pages,firstBit;};
 static constexpr int maxSegments=1024,maxWords=4096;
 int segments=0;quint32 bitsUsed=0;bool overflowed=false;qint64 mappedBytes=0;
 Segment segment[maxSegments];
 quint64 bits[maxWords];
 qint64 bytesInUse() const;
};
// Adds the pages mapped right now, so a slow press can't shrink what a fast one
// recorded. No allocation, about one pagemap read per 2 MiB of segments. False if
// pagemap can't be read.
bool snapshotImagePages(ImagePageSnapshot&);
// Unmaps the clean pages of every loaded program and library's read-only and
// code segments from this process, except those in `keep`. They stay in the page
// cache and return on the next touch through a minor fault, as they do after the
// kernel reclaims them; startup's relocation tables and code that is not running
// stop counting toward this process until then. Writable segments are never
// touched, and a segment holding any private or swapped page (a debugger's
// breakpoint, say) is skipped whole, because dropping such a page would lose its
// contents. A segment `keep` doesn't list is released whole; if `keep` ran out of
// room it can't tell which pages matter, so nothing is released.
ImagePageRelease releaseCleanImagePages(const ImagePageSnapshot* keep=nullptr);
}
