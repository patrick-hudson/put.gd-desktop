#include "core/ReviewTimeline.h"
#include <algorithm>
#include <cstdio>

namespace putgd::recording {
std::vector<int> maskLanes(const std::vector<RecordingMask>& masks, int lanes) {
    std::vector<int> result;
    std::vector<long long> ends(size_t(std::max(1, lanes)), -1);
    for (const auto& mask : masks) {
        int lane = int(ends.size()) - 1;
        for (size_t i = 0; i < ends.size(); ++i) if (ends[i] <= mask.start) { lane = int(i); break; }
        ends[size_t(lane)] = std::max(ends[size_t(lane)], mask.end);
        result.push_back(lane);
    }
    return result;
}
long long timeAt(int x, int left, int width, long long duration) {
    if (width <= 1 || duration <= 0) return 0;
    const double fraction = std::clamp(double(x - left) / double(width - 1), 0.0, 1.0);
    return (long long)(fraction * double(duration) + 0.5);
}
int xAt(long long us, int left, int width, long long duration) {
    if (width <= 1 || duration <= 0) return left;
    const double fraction = std::clamp(double(us) / double(duration), 0.0, 1.0);
    return left + int(fraction * double(width - 1) + 0.5);
}
std::string clockText(long long us, bool tenths) {
    if (us < 0) us = 0;
    const long long minutes = us / 60000000, seconds = (us / 1000000) % 60;
    char text[32];
    if (tenths) std::snprintf(text, sizeof text, "%lld:%02lld.%lld", minutes, seconds, (us / 100000) % 10);
    else std::snprintf(text, sizeof text, "%lld:%02lld.%02lld", minutes, seconds, (us / 10000) % 100);
    return text;
}
}
