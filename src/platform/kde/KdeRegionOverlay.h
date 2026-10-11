#pragma once
#include "platform/Geometry.h"
#include <QObject>
#include <QWidget>
struct wl_callback;
namespace putgd {
class KdeRegionOverlay final:public QObject {
 Q_OBJECT
public:
 explicit KdeRegionOverlay(QList<CaptureFrame>,QObject* parent=nullptr);
 ~KdeRegionOverlay() override;
 void start();void cancel(Error={"cancelled","Region selection cancelled",{}});
signals:
 void selected(const QRectF&);void cancelled(const putgd::Error&);
private:
 class Surface;void press(QPointF);void move(QPointF);void release(QPointF);void finish();
 QList<CaptureFrame> frames_;QList<Surface*> surfaces_;QRectF selection_;QPointF anchor_;bool dragging_=false,finished_=false;
 wl_callback* dismissal_=nullptr;
};
}
