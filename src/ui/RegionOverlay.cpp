#include "ui/SelectionOutline.h"
#include "RegionOverlay.h"
#include <QPainter>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QGuiApplication>
#include <QScreen>
#include <QCursor>
#include <QtGui/qguiapplication_platform.h>
#include <cstdlib>
#include <cstring>
#if QT_CONFIG(xcb)
#include <xcb/xcb.h>
#endif
namespace putgd {
namespace {
// KWin/X11 otherwise fades a closed selector out over several hundred
// milliseconds, and region capture waits for the compositor to stop repainting.
void skipCloseAnimation(QWidget* surface){
#if QT_CONFIG(xcb)
 auto* native=qGuiApp->nativeInterface<QNativeInterface::QX11Application>();
 auto* connection=native?native->connection():nullptr;
 if(!connection)return;
 const char name[]="_KDE_NET_WM_SKIP_CLOSE_ANIMATION";
 auto* atom=xcb_intern_atom_reply(connection,xcb_intern_atom(connection,0,std::strlen(name),name),nullptr);
 if(!atom)return;
 const uint32_t skip=1;
 xcb_change_property(connection,XCB_PROP_MODE_REPLACE,surface->winId(),atom->atom,XCB_ATOM_CARDINAL,32,1,&skip);
 std::free(atom);
#else
 Q_UNUSED(surface);
#endif
}
}
class RegionOverlay::Surface final:public QWidget {
public:
 Surface(RegionOverlay& owner,const CaptureFrame& frame):QWidget(nullptr,Qt::Tool|Qt::FramelessWindowHint|Qt::WindowStaysOnTopHint|Qt::X11BypassWindowManagerHint|Qt::WindowDoesNotAcceptFocus),owner_(owner),logicalRect_(frame.logicalRect){setObjectName("putgd-region-overlay");setAttribute(Qt::WA_TranslucentBackground);for(auto* screen:QGuiApplication::screens())if(screen->name()==frame.identity){setScreen(screen);break;}setGeometry(frame.logicalRect.toAlignedRect());setCursor(Qt::CrossCursor);setMouseTracking(true);setFocusPolicy(Qt::StrongFocus);}
 // X11 compositors can sample newer pixels using an earlier damage clip.
 // Full-surface damage keeps an outward-moving edge inside that clip.
 void updateSelection(const QRectF& previous){if(!previous.intersected(logicalRect_).isEmpty()||!owner_.selection_.intersected(logicalRect_).isEmpty())update();}
protected:
 void paintEvent(QPaintEvent*) override {QPainter painter(this);painter.setCompositionMode(QPainter::CompositionMode_Source);painter.fillRect(rect(),Qt::transparent);painter.setCompositionMode(QPainter::CompositionMode_SourceOver);const auto clipped=owner_.selection_.intersected(logicalRect_);if(!clipped.isEmpty()){const QRectF local=clipped.translated(-logicalRect_.topLeft());paintSelectionOutline(painter,local);}}
 void mousePressEvent(QMouseEvent* e) override {if(e->button()==Qt::RightButton){owner_.cancel();return;}if(e->button()==Qt::LeftButton){owner_.press(e->globalPosition());grabMouse();}}
 void mouseMoveEvent(QMouseEvent* e) override {owner_.move(e->globalPosition());}
 void mouseReleaseEvent(QMouseEvent* e) override {if(e->button()==Qt::LeftButton){releaseMouse();owner_.release(e->globalPosition());}}
 void keyPressEvent(QKeyEvent* e) override {if(e->key()==Qt::Key_Escape)owner_.cancel();}
private:
 RegionOverlay& owner_;
 QRectF logicalRect_;
};
RegionOverlay::RegionOverlay(QList<CaptureFrame> frames,QObject* parent):RegionSelector(parent),frames_(std::move(frames)){}
RegionOverlay::~RegionOverlay(){finish();for(auto* s:surfaces_)delete s;}
void RegionOverlay::start(){
 for(const auto& frame:frames_){auto* surface=new Surface(*this,frame);surfaces_.append(surface);skipCloseAnimation(surface);surface->show();surface->raise();}
 Surface* focus=surfaces_.isEmpty()?nullptr:surfaces_.first();for(auto* s:surfaces_)if(s->geometry().contains(QCursor::pos()))focus=s;
 if(focus)focus->grabKeyboard();
 connect(qGuiApp,&QGuiApplication::screenAdded,this,[this]{cancel({"display_changed","Display changed during region selection",{}});});connect(qGuiApp,&QGuiApplication::screenRemoved,this,[this]{cancel({"display_changed","Display changed during region selection",{}});});
 for(auto* screen:QGuiApplication::screens()) {connect(screen,&QScreen::geometryChanged,this,[this]{cancel({"display_changed","Display geometry changed during region selection",{}});});connect(screen,&QScreen::logicalDotsPerInchChanged,this,[this]{cancel({"display_changed","Display scale changed during region selection",{}});});connect(screen,&QScreen::orientationChanged,this,[this]{cancel({"display_changed","Display rotation changed during region selection",{}});});}
}
void RegionOverlay::press(QPointF point){if(finished_)return;anchor_=point;dragging_=true;selection_=QRectF(point,QSizeF{});}
void RegionOverlay::move(QPointF point){if(!dragging_||finished_)return;const auto previous=selection_;selection_=QRectF(anchor_,point).normalized();for(auto* s:surfaces_)s->updateSelection(previous);}
void RegionOverlay::release(QPointF point){if(!dragging_||finished_)return;move(point);if(selection_.width()<1||selection_.height()<1){cancel();return;}finish();emit selected(selection_);}
void RegionOverlay::finish(){finished_=true;for(auto* s:surfaces_){s->releaseKeyboard();s->releaseMouse();s->hide();}}
void RegionOverlay::cancel(Error error){if(finished_)return;finish();emit cancelled(error);}
}
