#pragma once
#include "platform/Geometry.h"
#include <QJsonArray>
#include <QJsonObject>
#include <cmath>

namespace putgd {
// Geometry-only payloads on the existing bounded private CaptureControl pipes.
QJsonArray kdeRegionLayout(const QList<CaptureFrame>&, Error&);
QList<CaptureFrame> kdeRegionLayout(const QJsonArray&, Error&);
// The resident tracks native transform/scale revisions. Qt's orientation angle
// is not the Wayland transform enum; helper equality covers name/logical rect.
bool kdeRegionLayoutsMatch(const QList<CaptureFrame>&, const QList<CaptureFrame>&);
QJsonObject kdeRegionRectangle(const QRectF&);
QRectF kdeRegionRectangle(const QJsonObject&, const QList<CaptureFrame>&, Error&);
// Wayland pointer positions are fractional logical coordinates. A selection edge
// is the logical pixel that contains the hotspot, as X11's integer root positions
// are, so a drag from pixel A to pixel B selects B-A (half-open). Rounding to the
// nearest edge instead adds a pixel whenever the end fraction is .5 or more (#333).
inline QPointF kdeRegionEdge(QPointF p) { return {std::floor(p.x()), std::floor(p.y())}; }
// Screenshot first (#629): the resident's frozen frames travel to the helper in
// one sealed, close-on-exec memfd, in layout order as tightly packed
// premultiplied ARGB32 rows. Only the helper's own launch clears close-on-exec.
struct KdeFrozenFrames { int fd=-1; QJsonArray frames; };
KdeFrozenFrames kdeFrozenFrames(const QList<CaptureFrame>& layout, const QList<CaptureFrame>& pixels, Error&);
// Helper side: checks the seals and exact size, attaches pixels/density/native
// geometry to the bootstrap layout, then closes the descriptor.
bool kdeFrozenPixels(int fd, const QJsonArray& frames, QList<CaptureFrame>& layout, Error&);
}
