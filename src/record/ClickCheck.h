#pragma once
#include "core/Error.h"
#include <QJsonObject>
#include <QList>
#include <QString>

namespace putgd::record {
// Lab diagnostic for click highlights (#1364): does the clip show a ring at each
// scripted press, at the right place and time? One decode pass. For each
// expected press `{ms, x, y}` (clip time and clip pixels) the frames from
// `windowMs` before to `windowMs` + 1.2 s after are compared, inside a box of
// `box` pixels around the place, with the first frame of that span (the
// pointer already resting there, no ring yet). A pixel counts as changed when
// its channels differ by more than 90 in sum. Result per press: {ms, x, y,
// seen, firstMs and lastMs (clip time of the first and last frame with at least
// `floor` changed pixels), offsetMs (firstMs - ms), lengthMs, peak (most
// changed pixels in one frame), dx and dy (centroid of the changed pixels at
// the peak, minus the place), rgb (their mean colour), frames (compared)}.
// Whoever draws the ring, put.gd or the compositor, it is found the same way.
struct ExpectedClick { qint64 ms = 0; int x = 0, y = 0; };
Error measureClicks(const QString& clip, const QList<ExpectedClick>& clicks, QJsonObject& result, int windowMs = 1500, int box = 64, int floor = 24);
}
