#pragma once
#include "ui/RegionSelector.h"
#include "ui/RegionSelection.h"
#include <QElapsedTimer>
#include <QRegion>
#include <QPointer>
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
 // How long selection waits for another client's grab, such as an open
 // menu's, to end before it explains why it cannot start (#341).
 static constexpr int grabWaitMs=3000,grabRetryMs=50;
 // A frozen selection already holds the menu's pixels, so it waits for the
 // menu to close instead of failing; the helper deadline still bounds it.
 static constexpr int frozenGrabWaitMs=60000;
 void start() override;
 void cancel(Error error={"cancelled","Region selection cancelled",{}}) override;
private:
 friend class NativeRegionAccessible;
 void describe();
 void initializeSurfaces();
 void grabInput();
 // Only the compact hint and size pills are client raster images; a frozen
 // frame is uploaded once into a server pixmap and repainted by the server.
 struct Badge {QImage image;QString key;QPoint at;};
 struct Surface {CaptureFrame frame;int index=0;xcb_window_t window=XCB_WINDOW_NONE;QRect outline;int pen=1;QImage frozen;xcb_pixmap_t background=XCB_NONE;Badge hint,size;};
 void events();
 QPointF logicalPoint(QPoint)const;
 void move(QPointF);
 void release(QPointF);
 void draw(Surface&);
 void key(int,Qt::KeyboardModifiers,QPoint);
 int screenAt(QPoint)const;
 void prepareBadges(Surface&);
 void prepareBadge(Surface&,Badge&,const QRect&,const QString&,qreal);
 void showFrozen();
 bool presenting()const{return !frozen_||frozenShown_;}
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
 bool initialized_=false;
 QElapsedTimer grabClock_;
 int grabAttempts_=0,keyboardScreen_=-1,hintScreen_=0;
 bool frozen_=false,frozenShown_=false;
 RegionHintFade hint_{[this]{if(finished_||!connection_||hintScreen_>=surfaces_.size())return;auto& surface=surfaces_[hintScreen_];prepareBadges(surface);draw(surface);xcb_flush(connection_);}};
};
}
