#pragma once
// The review window's timeline arithmetic (docs/recording.md, "Review window"):
// time and pixel position along the track, mask lanes (overlapping masks take
// separate lanes, up to four) and the clock text. No Windows headers: tested on Linux.
#include "core/Recording.h"
#include <string>
#include <vector>

namespace putgd::recording {
inline constexpr int maximumMaskLanes = 4;
// Lane of each mask, in order: the first lane whose last bar ends at or before
// the mask's start; masks beyond the lanes share the last one.
std::vector<int> maskLanes(const std::vector<RecordingMask>&, int lanes = maximumMaskLanes);
// Clip time at x along a track of `width` pixels starting at `left`, clamped to [0, duration].
long long timeAt(int x, int left, int width, long long durationUs);
int xAt(long long us, int left, int width, long long durationUs);
// "m:ss.cc" (hundredths), or "m:ss.d" with tenths, as the review and the bar show it.
std::string clockText(long long us, bool tenths = false);
}
