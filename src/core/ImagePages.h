#pragma once
#include <QtGlobal>

namespace putgd {
struct ImagePageRelease {int segments=0,skipped=0;qint64 bytes=0;};
// Unmaps the clean pages of every loaded program and library's read-only and
// code segments from this process. They stay in the page cache and return on
// the next touch through a minor fault, as they do after the kernel reclaims
// them; startup's relocation tables and code that is not running stop counting
// toward this process until then. Writable segments are never touched, and a
// segment holding any private or swapped page (a debugger's breakpoint, say)
// is skipped whole, because dropping such a page would lose its contents.
ImagePageRelease releaseCleanImagePages();
}
