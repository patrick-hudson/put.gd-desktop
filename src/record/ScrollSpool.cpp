#include "ScrollSpool.h"
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace putgd::record {
SpoolCanvas::~SpoolCanvas() { close(); }

bool SpoolCanvas::open(int fd, int width, long long rowsAbove, long long rowsBelow) {
    close();
    if (fd < 0 || width <= 0 || rowsAbove < 0 || rowsBelow <= 0) { failed_ = true; return false; }
    fd_ = fd;
    width_ = width;
    rowBytes_ = size_t(width) * 4;
    above_ = rowsAbove;
    below_ = rowsBelow;
    failed_ = false;
    error_ = 0;
    any_ = false;
    held_ = false;
    return map();
}

// Blocks for the file row about to be written, with the rows between it and
// those that have them, and some to spare in the direction the image grows
// (one call per 64 rows, not one per row). The image is one block of rows,
// so the file stays sparse outside it.
bool SpoolCanvas::reserve(long long fileRow) {
    if (held_ && fileRow >= heldTop_ && fileRow < heldBottom_) return true;
    constexpr long long kSpare = 64;
    const long long rows = above_ + below_;
    long long lo = fileRow, hi = fileRow + 1;
    if (held_ && fileRow >= heldBottom_) lo = heldBottom_, hi = std::min(rows, fileRow + 1 + kSpare);
    else if (held_) lo = std::max(0LL, fileRow - kSpare), hi = heldTop_;
    else hi = std::min(rows, fileRow + 1 + kSpare);
    const long long nextTop = held_ ? std::min(heldTop_, lo) : lo, nextBottom = held_ ? std::max(heldBottom_, hi) : hi;
    int err = 0;
    if (size_t(nextBottom - nextTop) * rowBytes_ > spaceLimit_) err = ENOSPC;
    else err = ::posix_fallocate(fd_, off_t(lo) * off_t(rowBytes_), off_t(hi - lo) * off_t(rowBytes_));
    if (err != 0) {
        error_ = err;
        failed_ = true;
        return false;
    }
    heldTop_ = nextTop;
    heldBottom_ = nextBottom;
    held_ = true;
    return true;
}

bool SpoolCanvas::map() {
    mapBytes_ = size_t(above_ + below_) * rowBytes_;
    // Sparse: nothing is allocated until a row is written.
    if (::ftruncate(fd_, off_t(mapBytes_)) != 0) { failed_ = true; return false; }
    void* p = ::mmap(nullptr, mapBytes_, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
    if (p == MAP_FAILED) { map_ = nullptr; failed_ = true; return false; }
    map_ = static_cast<uint8_t*>(p);
    return true;
}

void SpoolCanvas::writeSpan(long long row, int x0, int x1, const uint8_t* bgrx) {
    if (!map_) { failed_ = true; return; }
    if (row < -above_ || row >= below_) { failed_ = true; return; }
    x0 = std::max(0, x0);
    x1 = std::min(width_, x1);
    if (x1 <= x0) return;
    if (!reserve(row + above_)) return;
    std::memcpy(map_ + size_t(row + above_) * rowBytes_ + size_t(x0) * 4, bgrx, size_t(x1 - x0) * 4);
    if (!any_) { top_ = row; bottom_ = row + 1; any_ = true; }
    else { top_ = std::min(top_, row); bottom_ = std::max(bottom_, row + 1); }
}

bool SpoolCanvas::readRow(long long row, uint8_t* bgrx) const {
    if (!map_ || !any_ || row < top_ || row >= bottom_) return false;
    std::memcpy(bgrx, map_ + size_t(row + above_) * rowBytes_, rowBytes_);
    return true;
}

void SpoolCanvas::clear() {
    if (fd_ < 0) return;
    if (map_) ::munmap(map_, mapBytes_);
    map_ = nullptr;
    any_ = false;
    held_ = false;
    // Every row back to zero, and the disk they took given back.
    if (::ftruncate(fd_, 0) != 0) { failed_ = true; return; }
    map();
}

const uint8_t* SpoolCanvas::image() const {
    if (!map_ || !any_) return nullptr;
    return map_ + size_t(top_ + above_) * rowBytes_;
}

bool SpoolCanvas::sync() {
    return map_ && ::msync(map_, mapBytes_, MS_SYNC) == 0;
}

void SpoolCanvas::close() {
    if (map_) ::munmap(map_, mapBytes_);
    map_ = nullptr;
    mapBytes_ = 0;
    fd_ = -1;
    any_ = false;
}
}
