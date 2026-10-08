#pragma once
#include "core/Error.h"
#include <QImage>
#include <QRectF>
#include <QList>
namespace putgd {
// Pixels are already oriented as displayed. Rotation records the native output
// orientation; it must not cause a second rotation of the desktop snapshot.
struct CaptureFrame {
 QString identity;
 QRectF logicalRect;
 QRect nativeRect;
 qreal density=1;
 int rotation=0;
 QImage pixels;
};
QImage composeFrames(const QList<CaptureFrame>&,const QRectF& selection,qint64 maxPixels,Error&);
}
