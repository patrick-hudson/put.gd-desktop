#include "core/BarPlacement.h"
#include <algorithm>

namespace putgd::recording {
namespace {
RecordingRect intersected(const RecordingRect& a, const RecordingRect& b) {
    const long long x0 = std::max(a.x, b.x), y0 = std::max(a.y, b.y), x1 = std::min(a.x + a.w, b.x + b.w), y1 = std::min(a.y + a.h, b.y + b.h);
    return x1 > x0 && y1 > y0 ? RecordingRect{x0, y0, x1 - x0, y1 - y0} : RecordingRect{};
}
bool contains(const RecordingRect& outer, const RecordingRect& inner) { return inner.x >= outer.x && inner.y >= outer.y && inner.x + inner.w <= outer.x + outer.w && inner.y + inner.h <= outer.y + outer.h; }
bool containsPoint(const RecordingRect& r, long long x, long long y) { return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h; }
}
RecordingRect clampToScreen(RecordingRect rect, const RecordingRect& screen) {
    if (screen.empty()) return rect;
    rect.x = std::max(screen.x, std::min(rect.x, screen.x + screen.w - rect.w));
    rect.y = std::max(screen.y, std::min(rect.y, screen.y + screen.h - rect.h));
    return rect;
}
int homeScreen(const RecordingRect& rect, const std::vector<RecordingRect>& screens) {
    int best = -1;
    long long bestArea = 0;
    for (size_t i = 0; i < screens.size(); ++i) {
        const auto piece = intersected(screens[i], rect);
        if (piece.w * piece.h > bestArea) { best = int(i); bestArea = piece.w * piece.h; }
    }
    if (best >= 0) return best;
    for (size_t i = 0; i < screens.size(); ++i) if (containsPoint(screens[i], rect.x + rect.w / 2, rect.y + rect.h / 2)) return int(i);
    return screens.empty() ? -1 : 0;
}
BarSpot placeBar(int width, int height, const RecordingRect& recorded, const std::vector<RecordingRect>& screens, bool allowOutside, int gap) {
    const int home = homeScreen(recorded, screens);
    if (home < 0) return {{recorded.x, recorded.y, width, height}, -1, false};
    const auto& area = screens[size_t(home)];
    auto clear = [&](const RecordingRect& candidate) {
        if (!intersected(candidate, recorded).empty()) return -1;
        for (size_t i = 0; i < screens.size(); ++i) if (contains(screens[i], candidate)) return int(i);
        return -1;
    };
    const long long left = std::max(area.x, std::min(recorded.x, area.x + area.w - width));
    std::vector<RecordingRect> candidates{{left, recorded.y + recorded.h + gap, width, height}, {left, recorded.y - gap - height, width, height}};
    for (size_t i = 0; i < screens.size(); ++i) {
        if (int(i) == home) continue;
        const long long x = screens[i].x + screens[i].w / 2 - width / 2;
        candidates.push_back({x, screens[i].y + gap, width, height});
        candidates.push_back({x, screens[i].y + screens[i].h - gap - height, width, height});
    }
    if (allowOutside) for (const auto& candidate : candidates) if (const int screen = clear(candidate); screen >= 0) return {candidate, screen, true};
    auto inside = intersected(recorded, area);
    if (inside.empty()) inside = area;
    const RecordingRect corner{inside.x + inside.w - gap - width, inside.y + inside.h - gap - height, width, height};
    return {clampToScreen(corner, area), home, false};
}
}
