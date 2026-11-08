#pragma once
#include "platform/Geometry.h"
#include "ui/RegionSelection.h"
#include <QObject>
#include <QWidget>
#include <QSet>
#include <thread>
class QWindow;
class QScreen;
struct wl_callback;
namespace putgd {
class LayerShellRegionOverlay final:public QObject {
 Q_OBJECT
public:
 // frozen: frames carry the pixels captured before selection (#629), shown as
 // the backdrop; otherwise the layer surfaces are transparent over the live desktop.
 explicit LayerShellRegionOverlay(QList<CaptureFrame>,QObject* parent=nullptr,bool recording=false,QString outcome={},bool frozen=false);
 ~LayerShellRegionOverlay() override;
 void start();void cancel(Error={"cancelled","Region selection cancelled",{}});
 // The first font metrics populate Qt's whole font database. This does it on a
 // worker; until it ends, surfaces map and draw the outline without the hint and
 // size pills, which follow. Call once after QApplication; join before it ends.
 [[nodiscard]] static std::thread preloadFonts();
 // Completes the selection with a logical rectangle, as a drag would (agent control).
 void select(const QRectF& logical);
signals:
 void selected(const QRectF&);void cancelled(const putgd::Error&);
private:
 friend class WaylandRegionOverlayTest;
 bool addSurface(const CaptureFrame&,QScreen*);
 void accept();
 bool eventFilter(QObject*,QEvent*) override;
 class Surface;void press(QPointF);void move(QPointF);void release(QPointF);void finish();void dismiss();
 int keyboardScreen_=-1;
 QList<CaptureFrame> frames_;QString outcome_;QList<Surface*> surfaces_;QRectF selection_;QPointF anchor_;bool dragging_=false,finished_=false,clearing_=false,dismissing_=false;QSet<QWindow*> awaitingFrames_;
 bool recording_=false,frozen_=false;
 int hintScreen_=0;
 RegionPaintStats stats_;bool logged_=false;
 void repaint();
 void fontsLoaded();
 RegionHintFade hint_{[this]{repaint();}};
 wl_callback* dismissal_=nullptr;
};
}
