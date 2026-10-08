#pragma once
#include "platform/Geometry.h"
#include <QObject>
#include <QWidget>
namespace putgd {
class RegionOverlay final:public QObject {
 Q_OBJECT
public:
 explicit RegionOverlay(QList<CaptureFrame>,QObject* parent=nullptr);
 ~RegionOverlay() override;
 void start();
 void cancel(Error error={"cancelled","Region selection cancelled",{}});
signals:
 void selected(const QRectF&);
 void cancelled(const putgd::Error&);
private:
 class Surface;
 void press(QPointF);
 void move(QPointF);
 void release(QPointF);
 void finish();
 QList<CaptureFrame> frames_;
 QList<Surface*> surfaces_;
 QRectF selection_;
 QPointF anchor_;
 bool dragging_=false,finished_=false;
};
}
