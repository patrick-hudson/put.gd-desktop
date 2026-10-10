#pragma once
#include "platform/Geometry.h"
#include <QJsonArray>
#include <QJsonObject>

namespace putgd {
// Geometry-only payloads on the existing bounded private CaptureControl pipes.
QJsonArray kdeRegionLayout(const QList<CaptureFrame>&, Error&);
QList<CaptureFrame> kdeRegionLayout(const QJsonArray&, Error&);
// The resident tracks native transform/scale revisions. Qt's orientation angle
// is not the Wayland transform enum; helper equality covers name/logical rect.
bool kdeRegionLayoutsMatch(const QList<CaptureFrame>&, const QList<CaptureFrame>&);
QJsonObject kdeRegionRectangle(const QRectF&);
QRectF kdeRegionRectangle(const QJsonObject&, const QList<CaptureFrame>&, Error&);
}
