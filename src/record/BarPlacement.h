#pragma once
#include <QList>
#include <QRect>
#include <QSize>

namespace putgd::record {
// Where the recording control bar goes, in one coordinate space (Qt logical
// desktop pixels). Candidates, in order: below the recorded rectangle, then
// above it (both kept on its home screen horizontally), then the top and the
// bottom centre of every other screen. A candidate must lie within one screen
// and stay clear of the rectangle. When none does, the bar sits inside the
// rectangle's bottom-right corner on its home screen (`outside` false). The
// home screen holds most of the rectangle. With allowOutside false only the
// inside corner is used. The result always lies within `screens[screen]`
// unless the bar is larger than that screen.
struct BarSpot {
    QRect rect;
    int screen = -1;
    bool outside = false;
};
BarSpot placeBar(const QSize& bar, const QRect& recorded, const QList<QRect>& screens, bool allowOutside = true, int gap = 10);
// Moves rect the least distance that keeps it within screen; a rect larger
// than the screen keeps its top-left corner on it.
QRect clampToScreen(QRect rect, const QRect& screen);
// The screen holding most of rect, else the one containing its centre, else 0
// (-1 when there are no screens).
int homeScreen(const QRect& rect, const QList<QRect>& screens);
}
