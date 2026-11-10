#pragma once
#include "core/Error.h"
#include <QJsonObject>
#include <QList>
#include <QString>

namespace putgd::record {
// Lab diagnostic for click highlights (#1364): what changes near a place in a
// clip, and when. One decode pass. For each watch `{fromMs, toMs, x, y}` (clip
// time and clip pixels) the frames from `fromMs` to `toMs` are compared, inside
// a box of `box` pixels around the place, with the first frame of that span. A
// pixel counts as changed when its channels differ by more than 90 in sum.
// Result per watch: {fromMs, toMs, x, y, seen, firstMs and lastMs (clip time of
// the first and last frame with at least `floor` changed pixels), lengthMs,
// peak (most changed pixels in one frame), peakMs, dx and dy (centroid of the
// changed pixels at the peak, minus the place), rgb (their mean colour),
// frames (compared)}.
// A run asks twice per scripted press: when the pointer arrived at the place
// (a span that starts before it did), then when a ring appeared (a span that
// starts with the pointer resting there). The time between the two is the time
// between the pointer move and the press, whoever draws the ring, put.gd or
// the compositor, and wherever the clip's own zero lies.
struct ClickWatch { qint64 fromMs = 0, toMs = 0; int x = 0, y = 0; };
Error measureClicks(const QString& clip, const QList<ClickWatch>& watches, QJsonObject& result, int box = 64, int floor = 24);
}
