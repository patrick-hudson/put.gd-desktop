#include "core/RecordingFrames.h"
#include <algorithm>
#include <vector>

namespace putgd::recording {
int blurRadius(const RecordingMask& mask) {
    const int shorter = int(std::min(mask.rect.w, mask.rect.h));
    return std::clamp(mask.strength, 2, std::max(2, shorter / 2 - 1));
}
void boxBlur(const Frame& frame, RecordingRect rect, int radius) {
    const long long x0 = std::max<long long>(0, rect.x), y0 = std::max<long long>(0, rect.y);
    const long long x1 = std::min<long long>(frame.width, rect.x + rect.w), y1 = std::min<long long>(frame.height, rect.y + rect.h);
    const int w = int(x1 - x0), h = int(y1 - y0);
    if (w < 1 || h < 1) return;
    // Prefix sums per line: each output pixel is one subtraction per channel,
    // over the window [i - radius, i + radius] clamped to the line.
    const size_t longest = size_t(std::max(w, h));
    std::vector<int> sums[3] = {std::vector<int>(longest + 1), std::vector<int>(longest + 1), std::vector<int>(longest + 1)};
    std::vector<uint8_t> line(longest * 4);
    auto filter = [&](int count, auto pixel) {
        for (int c = 0; c < 3; ++c) sums[c][0] = 0;
        for (int i = 0; i < count; ++i) { const uint8_t* p = pixel(i); for (int c = 0; c < 3; ++c) sums[c][size_t(i) + 1] = sums[c][size_t(i)] + p[c]; }
        for (int i = 0; i < count; ++i) {
            const int a = std::max(0, i - radius), b = std::min(count - 1, i + radius), n = b - a + 1;
            for (int c = 0; c < 3; ++c) line[size_t(i) * 4 + size_t(c)] = uint8_t((sums[c][size_t(b) + 1] - sums[c][size_t(a)]) / n);
        }
        for (int i = 0; i < count; ++i) { uint8_t* p = pixel(i); p[0] = line[size_t(i) * 4]; p[1] = line[size_t(i) * 4 + 1]; p[2] = line[size_t(i) * 4 + 2]; }
    };
    for (int pass = 0; pass < 2; ++pass) {
        for (int y = 0; y < h; ++y) { uint8_t* row = frame.row(int(y0) + y) + size_t(x0) * 4; filter(w, [row](int i) { return row + size_t(i) * 4; }); }
        for (int x = 0; x < w; ++x) { const auto column = size_t(x0 + x) * 4; filter(h, [&](int i) { return frame.row(int(y0) + i) + column; }); }
    }
}
void applyMasks(const Frame& frame, const RecordingEdit& edit, long long sourceUs) {
    for (const auto& mask : edit.masks) {
        if (sourceUs < mask.start || sourceUs >= mask.end) continue;
        if (mask.kind == "solid") {
            const long long x0 = std::max<long long>(0, mask.rect.x), y0 = std::max<long long>(0, mask.rect.y);
            const long long x1 = std::min<long long>(frame.width, mask.rect.x + mask.rect.w), y1 = std::min<long long>(frame.height, mask.rect.y + mask.rect.h);
            for (long long y = y0; y < y1; ++y) for (long long x = x0; x < x1; ++x) { uint8_t* p = frame.row(int(y)) + size_t(x) * 4; p[0] = p[1] = p[2] = 0; p[3] = 255; }
        } else boxBlur(frame, mask.rect, blurRadius(mask));
    }
}
void resample(const Frame& source, const Frame& target) {
    if (target.width < 1 || target.height < 1 || source.width < 1 || source.height < 1) return;
    if (source.width == target.width && source.height == target.height) {
        for (int y = 0; y < target.height; ++y) std::copy(source.row(y), source.row(y) + size_t(target.width) * 4, target.row(y));
        return;
    }
    // Source span of output pixel i: [i * s / t, (i + 1) * s / t), at least one pixel.
    auto span = [](int i, int s, int t) { const int a = int((long long)i * s / t); return std::make_pair(a, std::max(a + 1, int((long long)(i + 1) * s / t))); };
    for (int y = 0; y < target.height; ++y) {
        const auto [sy0, sy1] = span(y, source.height, target.height);
        uint8_t* out = target.row(y);
        for (int x = 0; x < target.width; ++x) {
            const auto [sx0, sx1] = span(x, source.width, target.width);
            unsigned sum[3] = {0, 0, 0};
            for (int sy = sy0; sy < sy1; ++sy) { const uint8_t* p = source.row(sy) + size_t(sx0) * 4; for (int sx = sx0; sx < sx1; ++sx, p += 4) { sum[0] += p[0]; sum[1] += p[1]; sum[2] += p[2]; } }
            const unsigned n = unsigned((sy1 - sy0) * (sx1 - sx0));
            out[size_t(x) * 4] = uint8_t(sum[0] / n); out[size_t(x) * 4 + 1] = uint8_t(sum[1] / n); out[size_t(x) * 4 + 2] = uint8_t(sum[2] / n); out[size_t(x) * 4 + 3] = 255;
        }
    }
}
std::pair<long long, long long> exportWindow(const RecordingEdit& edit, long long duration, int fps) {
    const long long start = alignToFrame(edit.trimStart, fps);
    const long long end = std::min(duration, alignToFrame(edit.exportEnd(duration), fps));
    return {start, std::max(start, end)};
}
}
