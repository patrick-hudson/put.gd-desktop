#pragma once
// Where the recording control bar goes (the Linux record/BarPlacement rules), in
// virtual-screen pixels. Candidates, in order: below the recorded rectangle, then
// above it (both kept on its home monitor horizontally), then the top and the
// bottom centre of every other monitor. A candidate must lie within one monitor
// and stay clear of the rectangle. When none does, the bar sits inside the
// rectangle's bottom-right corner on its home monitor (`outside` false). The home
// monitor holds most of the rectangle. No Windows headers: tested on Linux.
#include "core/Recording.h"
#include <vector>

namespace putgd::recording {
struct BarSpot {
    RecordingRect rect;
    int screen = -1;
    bool outside = false;
};
BarSpot placeBar(int width, int height, const RecordingRect& recorded, const std::vector<RecordingRect>& screens, bool allowOutside = true, int gap = 10);
// Moves rect the least distance that keeps it within screen.
RecordingRect clampToScreen(RecordingRect rect, const RecordingRect& screen);
// The screen holding most of rect, else the one containing its centre, else 0 (-1 with none).
int homeScreen(const RecordingRect& rect, const std::vector<RecordingRect>& screens);
}
