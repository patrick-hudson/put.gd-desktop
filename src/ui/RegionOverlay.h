#pragma once
#include "RegionSelector.h"
#include "RegionSelection.h"
#include <QObject>
#include <QWidget>
namespace putgd {
class RegionOverlay final:public RegionSelector {
 Q_OBJECT
public:
 // frozen: every frame carries the pixels captured before selection (#629),
 // drawn as the backdrop; otherwise the surfaces are transparent over a live desktop.
 explicit RegionOverlay(QList<CaptureFrame>,QObject* parent=nullptr,QString outcome={},bool frozen=false);
 ~RegionOverlay() override;
 void start() override;
 void cancel(Error error={"cancelled","Region selection canceled",{}}) override;
private:
 class Surface;
 void press(QPointF);
 void move(QPointF);
 void release(QPointF);
 void finish();
 void repaint();
 int keyboardScreen_=-1,hintScreen_=0;
 bool frozen_=false;
 RegionHintFade hint_{[this]{repaint();}};
 QList<CaptureFrame> frames_;
 QString outcome_;
 QList<Surface*> surfaces_;
 RegionPaintStats stats_;
 QRectF selection_;
 QPointF anchor_;
 bool dragging_=false,finished_=false;
};
}
