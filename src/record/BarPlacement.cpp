#include "BarPlacement.h"
#include <algorithm>

namespace putgd::record {
QRect clampToScreen(QRect rect, const QRect& screen) {
    if (screen.isEmpty()) return rect;
    rect.moveLeft(std::max(screen.left(), std::min(rect.left(), screen.left() + screen.width() - rect.width())));
    rect.moveTop(std::max(screen.top(), std::min(rect.top(), screen.top() + screen.height() - rect.height())));
    return rect;
}
int homeScreen(const QRect& rect, const QList<QRect>& screens) {
    int best = -1;
    qint64 bestArea = 0;
    for (int i = 0; i < screens.size(); ++i) {
        const QRect piece = screens[i].intersected(rect);
        const qint64 area = qint64(piece.width()) * piece.height();
        if (area > bestArea) { best = i; bestArea = area; }
    }
    if (best >= 0) return best;
    for (int i = 0; i < screens.size(); ++i) if (screens[i].contains(rect.center())) return i;
    return screens.isEmpty() ? -1 : 0;
}
BarSpot placeBar(const QSize& bar, const QRect& recorded, const QList<QRect>& screens, bool allowOutside, int gap) {
    const int home = homeScreen(recorded, screens);
    if (home < 0) return {QRect(recorded.topLeft(), bar), -1, false};
    const QRect area = screens[home];
    auto clear = [&](const QRect& candidate) -> int {
        if (candidate.intersects(recorded)) return -1;
        for (int i = 0; i < screens.size(); ++i) if (screens[i].contains(candidate)) return i;
        return -1;
    };
    const int left = std::max(area.left(), std::min(recorded.left(), area.left() + area.width() - bar.width()));
    QList<QRect> candidates{QRect(QPoint(left, recorded.bottom() + 1 + gap), bar), QRect(QPoint(left, recorded.top() - gap - bar.height()), bar)};
    for (int i = 0; i < screens.size(); ++i) {
        if (i == home) continue;
        const int x = screens[i].center().x() - bar.width() / 2;
        candidates.append(QRect(QPoint(x, screens[i].top() + gap), bar));
        candidates.append(QRect(QPoint(x, screens[i].bottom() + 1 - gap - bar.height()), bar));
    }
    for (const auto& candidate : candidates)
        if (const int screen = allowOutside ? clear(candidate) : -1; screen >= 0) return {candidate, screen, true};
    QRect inside = recorded.intersected(area);
    if (inside.isEmpty()) inside = area;
    const QRect corner(QPoint(inside.right() + 1 - gap - bar.width(), inside.bottom() + 1 - gap - bar.height()), bar);
    return {clampToScreen(corner, area), home, false};
}
}
