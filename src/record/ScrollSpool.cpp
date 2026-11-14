#include "ScrollSpool.h"
#include <algorithm>
#include <cstring>
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
    any_ = false;
    return map();
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
