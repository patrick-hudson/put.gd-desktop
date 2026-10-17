#pragma once
#include <QList>
#include <QObject>
#include <QPointer>
#include <QRect>
#include <QTimer>
#include <xcb/xcb.h>
class QSocketNotifier;
namespace putgd {
// A compositing manager presents an unmapped selector some frames after the X
// server has processed the unmap, and KWin/X11 may animate the close. Neither
// it nor the server reports when that frame is on screen, so this follows root
// Damage, which each composited repaint produces, and settles once repaints over
// the selected native areas have been quiet for a few frames, within a hard
// bound. Without a compositor, the server's unmap already restored the pixels.
class X11CompositorSettle final:public QObject {
 Q_OBJECT
public:
 struct Timing {int quietMs;int limitMs;};
 explicit X11CompositorSettle(QList<QRect> nativeAreas,QObject* parent=nullptr,Timing timing={100,1000});
 ~X11CompositorSettle() override;
 void start();
 // Ends the wait now. settled() is still emitted once, as the sole completion.
 void cancel(){finish();}
 bool composited()const{return composited_;}
 int repaints()const{return repaints_;}
signals:
 void settled();
private:
 void events();
 void finish();
 void release();
 QList<QRect> areas_;
 Timing timing_;
 xcb_connection_t* connection_=nullptr;
 uint32_t damage_=0;
 uint8_t damageEvent_=0;
 QPointer<QSocketNotifier> notifier_;
 QTimer quiet_,limit_;
 int repaints_=0;
 bool started_=false,finished_=false,composited_=false;
};
}
