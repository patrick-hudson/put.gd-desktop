#pragma once
#include "platform/Geometry.h"
#include <QObject>
namespace putgd {
// Common selection lifecycle; implementations own and release their surfaces
// before reporting a selection or cancellation.
class RegionSelector:public QObject {
 Q_OBJECT
public:
 using QObject::QObject;
 virtual void start()=0;
 virtual void cancel(Error error={"cancelled","Region selection canceled",{}})=0;
signals:
 void selected(const QRectF&);
 void cancelled(const putgd::Error&);
};
}
