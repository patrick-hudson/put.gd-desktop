#pragma once
#include "ScrollSession.h"
#include <cstddef>
#include <cstdint>

namespace putgd::record {
// The scroll session's image on disk (#1515): the tracker's canvas backed by
// the session's private spool file, with random access and room at both ends,
// because the page can grow upwards too (rows above the first frame's are
// negative). The file is sparse: page row r lives at file row rowsAbove + r,
// so the rows are in order and the image is one contiguous block at the end,
// which the PNG writer reads from the mapping. Only rows written take disk:
// they are given their blocks (posix_fallocate) before the mapping writes
// them, so a full disk fails the write with a reason (failed(), error())
// instead of raising SIGBUS in the copy (CodeRabbit on #1536).
class SpoolCanvas final : public SessionCanvas {
public:
    SpoolCanvas() = default;
    ~SpoolCanvas() override;
    SpoolCanvas(const SpoolCanvas&) = delete;
    SpoolCanvas& operator=(const SpoolCanvas&) = delete;
    // fd: the session's own empty spool file, kept open by the caller. Page
    // rows [-rowsAbove, rowsBelow) of `width` pixels fit.
    bool open(int fd, int width, long long rowsAbove, long long rowsBelow);
    void writeSpan(long long row, int x0, int x1, const uint8_t* bgrx) override;
    bool readRow(long long row, uint8_t* bgrx) const override;
    long long top() const override { return any_ ? top_ : 0; }
    long long bottom() const override { return any_ ? bottom_ : 0; }
    void clear() override;
    // A row outside the room, the file could not be resized or mapped, or
    // its rows could not be given blocks (error() is the errno, ENOSPC for a
    // full disk).
    bool failed() const override { return failed_; }
    int error() const { return error_; }
    // Tests: blocks beyond this many bytes are refused as on a full disk.
    void setSpaceLimit(size_t bytes) { spaceLimit_ = bytes; }
    // Rows [top(), bottom()) back to back, width * 4 bytes each; valid until close.
    const uint8_t* image() const;
    size_t rowBytes() const { return rowBytes_; }
    bool sync();
    void close();
private:
    bool map();
    bool reserve(long long fileRow);
    int fd_ = -1;
    int width_ = 0;
    size_t rowBytes_ = 0;
    long long above_ = 0, below_ = 0;
    uint8_t* map_ = nullptr;
    size_t mapBytes_ = 0;
    long long top_ = 0, bottom_ = 0;
    bool any_ = false, failed_ = false;
    // File rows [heldTop_, heldBottom_) have their blocks.
    long long heldTop_ = 0, heldBottom_ = 0;
    bool held_ = false;
    int error_ = 0;
    size_t spaceLimit_ = SIZE_MAX;
};
}
