#pragma once
#include "ui/RegionSelector.h"
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
 explicit NativeRegionOverlay(QList<CaptureFrame>,QObject* parent=nullptr,xcb_cursor_t residentCursor=XCB_CURSOR_NONE,QString outcome={});
 ~NativeRegionOverlay() override;
 static bool supportsLayout(const QList<CaptureFrame>&);
 // How long selection waits for another client's grab, such as an open
 // menu's, to end before it explains why it cannot start (#341).
 static constexpr int grabWaitMs=3000,grabRetryMs=50;
 void start() override;
 void cancel(Error error={"cancelled","Region selection cancelled",{}}) override;
private:
 friend class NativeRegionAccessible;
 void describe();
 void initializeSurfaces();
 void grabInput();
 struct Surface {CaptureFrame frame;xcb_window_t window=XCB_WINDOW_NONE;QRect outline;int pen=1;QImage instructions;QString instructionsKey;QPoint instructionsAt;};
 void events();
 QPointF logicalPoint(QPoint)const;
 void move(QPointF);
 void release(QPointF);
 void draw(Surface&);
 void key(int,Qt::KeyboardModifiers,QPoint);
 int screenAt(QPoint)const;
 void prepareInstructions(Surface&);
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
 int grabAttempts_=0,keyboardScreen_=-1;
};
}
