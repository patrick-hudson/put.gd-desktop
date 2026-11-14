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
// which the PNG writer reads from the mapping. Only rows written take disk.
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
    // A row outside the room, or the file could not be resized or mapped.
    bool failed() const { return failed_; }
    // Rows [top(), bottom()) back to back, width * 4 bytes each; valid until close.
    const uint8_t* image() const;
    size_t rowBytes() const { return rowBytes_; }
    bool sync();
    void close();
private:
    bool map();
    int fd_ = -1;
    int width_ = 0;
    size_t rowBytes_ = 0;
    long long above_ = 0, below_ = 0;
    uint8_t* map_ = nullptr;
    size_t mapBytes_ = 0;
    long long top_ = 0, bottom_ = 0;
    bool any_ = false, failed_ = false;
};
}
