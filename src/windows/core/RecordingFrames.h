#pragma once
// Pixel rules shared by the recording export and its previews, so both agree
// frame for frame (docs/recording.md, "Time, geometry and the edit model"):
// masks in source time, a solid mask an opaque black fill, a blur two passes of a
// separable box filter inside the rectangle (the Linux export's avgblur, twice),
// and the retained interval aligned to the recording's frame grid. Frames are
// 32-bit BGRX rows. No Windows headers: tested on Linux.
#include "core/Recording.h"
#include <cstddef>
#include <cstdint>
#include <utility>

namespace putgd::recording {
struct Frame {
    uint8_t* bits = nullptr;
    int width = 0, height = 0;
    size_t stride = 0;
    uint8_t* row(int y) const { return bits + stride * size_t(y); }
};
// The blur radius a mask of this size gets: its strength, at least 2, and at most
// half the shorter side minus one, so a narrow mask still blurs inside itself.
int blurRadius(const RecordingMask&);
void boxBlur(const Frame&, RecordingRect, int radius);
// Every mask active at `sourceUs` (start <= t < end), clipped to the frame.
void applyMasks(const Frame&, const RecordingEdit&, long long sourceUs);
// Area-average resample (each output pixel averages the source area it covers);
// equal sizes copy. For downscales: exports to maxEdge, previews.
void resample(const Frame& source, const Frame& target);
// [start, end) in source microseconds that an export keeps: the trim aligned to
// the frame grid at `fps`, as the review's playhead and the export agree.
std::pair<long long, long long> exportWindow(const RecordingEdit&, long long durationUs, int fps);
}
