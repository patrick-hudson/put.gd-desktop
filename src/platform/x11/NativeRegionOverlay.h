#pragma once
#include "ui/RegionSelector.h"
#include "ui/RegionSelection.h"
#include <QElapsedTimer>
#include <algorithm>
#include <QRegion>
#include <QPointer>
#include <QTimer>
#include <thread>
#include <xcb/xcb.h>
#include <xcb/xcb_keysyms.h>
class QSocketNotifier;
class QWindow;
namespace putgd {
// Capture-helper selector: native transparent drawables have no full-monitor
// Qt backing stores. The resident's themed cursor is optional; Qt remains the fallback.
class NativeRegionOverlay final:public RegionSelector {
 Q_OBJECT
public:
 // frozen (#629): every frame carries the pixels captured before selection.
 // They become each surface's server-side background once input is grabbed.
 explicit NativeRegionOverlay(QList<CaptureFrame>,QObject* parent=nullptr,xcb_cursor_t residentCursor=XCB_CURSOR_NONE,QString outcome={},bool frozen=false);
 ~NativeRegionOverlay() override;
 static bool supportsLayout(const QList<CaptureFrame>&);
 // The first font metrics populate Qt's whole font database: about 30 ms on
 // a 4-core i5 (#1063). This does it on a worker; until it ends, frames draw
 // the outline without the hint and size pills, which follow when it does.
 // Call once after QGuiApplication; join the thread before it is destroyed.
 [[nodiscard]] static std::thread preloadFonts();
 // How long selection waits for another client's grab, such as an open
 // menu's, to end before it explains why it cannot start (#341).
 static constexpr int grabWaitMs=3000,grabRetryMs=50;
 // A frozen selection already holds the menu's pixels, so it waits for the
 // menu to close instead of failing; the helper deadline still bounds it.
 static constexpr int frozenGrabWaitMs=60000;
 // Qt starts its AT-SPI bridge with the first accessibility event: 20-30 ms
 // of synchronous D-Bus in the capture helper (#1063). It waits for the first
 // outline frame, or this long after the grab if nothing is drawn.
 static constexpr int accessibilityDelayMs=100;
 void start() override;
 void cancel(Error error={"cancelled","Region selection cancelled",{}}) override;
 // Presentation statistics for lab logs (PUTGD_REGION_DEBUG_LOG) and tests.
 // Pointer motions while dragging are coalesced into presents: one flush of
 // every changed surface, at most one per display refresh. wait: from the
 // first unpresented motion's arrival to the flush that showed it; present:
 // how long preparing and queueing one frame took on this side.
 struct Stats {
  qint64 motions=0,presents=0,fullRedraws=0,waits=0;
  double waitSumMs=0,waitMaxMs=0,presentSumMs=0,presentMaxMs=0;
  int frameIntervalMs=0;
 };
 const Stats& stats()const{return stats_;}
private:
 friend class NativeRegionAccessible;
 bool eventFilter(QObject*,QEvent*) override;
 void describe();
 // Raises the focus events held back until the first outline frame.
 void startAccessibility();
 void initializeSurfaces();
 void grabInput();
 // Only the compact hint and size pills are client raster images; a frozen
 // frame is uploaded once into a server pixmap and repainted by the server.
 struct Badge {QImage image;QString key;QPoint at;};
 // painted: the native pixels this surface currently shows (outline strips
 // and pill images); the next present clears only what it no longer covers.
 struct Surface {CaptureFrame frame;int index=0;xcb_window_t window=XCB_WINDOW_NONE;QRect outline;int pen=1;QImage frozen;xcb_pixmap_t background=XCB_NONE;Badge hint,size;QRegion painted;};
 // Reads every queued event, then presents their final state once.
 void events();
 void drain();
 QPointF logicalPoint(QPoint)const;
 void move(QPointF);
 void release(QPointF);
 void draw(Surface&,bool full);
 // One frame: describe, prepare and draw every surface, flush once.
 void present();
 // Present now when a frame interval has passed since the last one, else at
 // the end of the current interval; more presents could not be shown.
 void schedulePresent();
 int frameIntervalMs()const;
 void key(int,Qt::KeyboardModifiers,QPoint);
 int screenAt(QPoint)const;
 void prepareBadges(Surface&);
 // The worker from preloadFonts() finished: draw the pills it held back.
 void fontsLoaded();
 void prepareBadge(Surface&,Badge&,const QRect&,const QString&,qreal);
 void showFrozen();
 bool presenting()const{return !frozen_||frozenShown_;}
 bool outlined()const{return std::any_of(surfaces_.begin(),surfaces_.end(),[](const Surface& surface){return !surface.outline.isEmpty();});}
 void finish();
 bool checked(xcb_void_cookie_t);
 QList<CaptureFrame> frames_;
 QString outcome_;
 QList<Surface> surfaces_;
 xcb_connection_t* connection_=nullptr;
 xcb_window_t root_=XCB_WINDOW_NONE;
 xcb_cursor_t residentCursor_=XCB_CURSOR_NONE;
 xcb_colormap_t colormap_=XCB_COLORMAP_NONE;
 xcb_gcontext_t gc_=XCB_NONE;
 xcb_key_symbols_t* keys_=nullptr;
 QPointer<QSocketNotifier> notifier_;
 QWindow* cursorWindow_=nullptr;
 QWindow* foreignParent_=nullptr;
 xcb_connection_t* qtConnection_=nullptr;
 QRectF selection_;
 QPointF anchor_, pointer_;
 bool dragging_=false,started_=false,finished_=false;
 bool pointerGrabbed_=false,keyboardGrabbed_=false;
 bool initialized_=false,accessible_=false;
 QElapsedTimer grabClock_;
 int grabAttempts_=0,keyboardScreen_=-1,hintScreen_=0;
 bool frozen_=false,frozenShown_=false;
 QTimer presentTimer_;
 QElapsedTimer presentClock_,pendingSince_;
 // draining_: events() is reading a queued batch; its presents wait for the end.
 bool pending_=false,draining_=false;
 Stats stats_;
 RegionHintFade hint_{[this]{schedulePresent();}};
};
}
