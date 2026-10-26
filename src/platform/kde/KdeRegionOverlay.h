#pragma once
#include "platform/Geometry.h"
#include <QObject>
#include <QWidget>
#include <QSet>
class QWindow;
class QScreen;
struct wl_callback;
namespace putgd {
class KdeRegionOverlay final:public QObject {
 Q_OBJECT
public:
 explicit KdeRegionOverlay(QList<CaptureFrame>,QObject* parent=nullptr);
 ~KdeRegionOverlay() override;
 void start();void cancel(Error={"cancelled","Region selection cancelled",{}});
 // Completes the selection with a logical rectangle, as a drag would (agent control).
 void select(const QRectF& logical);
signals:
 void selected(const QRectF&);void cancelled(const putgd::Error&);
private:
 friend class KdeRegionOverlayTest;
 void addSurface(const CaptureFrame&,QScreen*);
 void accept();
 bool eventFilter(QObject*,QEvent*) override;
 class Surface;void press(QPointF);void move(QPointF);void release(QPointF);void finish();void dismiss();
 QList<CaptureFrame> frames_;QList<Surface*> surfaces_;QRectF selection_;QPointF anchor_;bool dragging_=false,finished_=false,clearing_=false,dismissing_=false;QSet<QWindow*> awaitingFrames_;
 wl_callback* dismissal_=nullptr;
};
}
