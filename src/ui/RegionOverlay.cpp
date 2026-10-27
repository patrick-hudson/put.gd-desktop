#include <QCursor>
#include "ui/SelectionOutline.h"
#include "ui/RegionSelection.h"
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
 Surface(RegionOverlay& owner,const CaptureFrame& frame):QWidget(nullptr,Qt::Tool|Qt::FramelessWindowHint|Qt::WindowStaysOnTopHint|Qt::X11BypassWindowManagerHint|Qt::WindowDoesNotAcceptFocus),owner_(owner),logicalRect_(frame.logicalRect),identity_(frame.identity){setObjectName("putgd-region-overlay");setAccessibleName("Capture region");describeRegion(this,{},false,QStringLiteral("Display %1").arg(activeScreen()+1),logicalRect_,owner_.frames_);setAttribute(Qt::WA_TranslucentBackground);for(auto* screen:QGuiApplication::screens())if(screen->name()==frame.identity){setScreen(screen);break;}setGeometry(frame.logicalRect.toAlignedRect());setCursor(Qt::CrossCursor);setMouseTracking(true);setFocusPolicy(Qt::StrongFocus);}
 // X11 compositors can sample newer pixels using an earlier damage clip.
 // Full-surface damage keeps an outward-moving edge inside that clip.
 void updateSelection(const QRectF& previous){describeRegion(this,owner_.selection_,false,QStringLiteral("Display %1").arg(activeScreen()+1),owner_.frames_[activeScreen()].logicalRect,owner_.frames_);Q_UNUSED(previous);update();}
protected:
 void paintEvent(QPaintEvent*) override {QPainter painter(this);const auto clipped=(owner_.selection_.isEmpty()&&owner_.frames_[activeScreen()].identity==identity_?logicalRect_:owner_.selection_.intersected(logicalRect_));const QRectF local=clipped.translated(-logicalRect_.topLeft());paintRegionScrim(painter,rect(),local);if(!clipped.isEmpty()){paintSelectionOutline(painter,local);}if(owner_.keyboardScreen_<0||owner_.frames_[activeScreen()].identity==identity_)paintRegionInstructions(painter,rect(),palette(),regionInstructions(owner_.selection_,false,QStringLiteral("Display %1").arg(activeScreen()+1),owner_.frames_[activeScreen()].logicalRect,owner_.frames_), owner_.selection_.translated(-logicalRect_.topLeft()), owner_.dragging_ ? std::optional<QPointF>(mapFromGlobal(QCursor::pos())) : std::nullopt);}
 void mousePressEvent(QMouseEvent* e) override {if(e->button()==Qt::RightButton){owner_.cancel();return;}if(e->button()==Qt::LeftButton){owner_.press(e->globalPosition());grabMouse();}}
 void mouseMoveEvent(QMouseEvent* e) override {owner_.move(e->globalPosition());}
 void mouseReleaseEvent(QMouseEvent* e) override {if(e->button()==Qt::LeftButton){releaseMouse();owner_.release(e->globalPosition());}}
 bool event(QEvent* e) override {if(e->type()==QEvent::KeyPress){auto* key=static_cast<QKeyEvent*>(e);if(key->key()==Qt::Key_Tab||key->key()==Qt::Key_Backtab){keyPressEvent(key);return true;}}return QWidget::event(e);}
 int activeScreen() const {if(owner_.keyboardScreen_>=0)return owner_.keyboardScreen_;for(int i=0;i<owner_.frames_.size();++i)if(owner_.frames_[i].identity==identity_)return i;return 0;}
 void keyPressEvent(QKeyEvent* e) override {
  if(owner_.finished_)return;
  const auto previous=owner_.selection_;Q_UNUSED(previous);
  int screen=activeScreen();QList<QRectF> screens;for(const auto& frame:owner_.frames_)screens.append(frame.logicalRect);
  const bool switched=switchRegionDisplay(owner_.selection_,screens,screen,e->key(),e->modifiers());
  const auto action=switched?RegionKeyAction::Change:regionKey(owner_.selection_,screens[screen],e->key(),e->modifiers());
  switch(action){
  case RegionKeyAction::Cancel:owner_.cancel();break;
  case RegionKeyAction::Accept:{const auto selected=owner_.selection_;owner_.finish();emit owner_.selected(selected);break;}
  case RegionKeyAction::Change:owner_.keyboardScreen_=screen;owner_.dragging_=false;for(auto* surface:owner_.surfaces_)surface->updateSelection(previous);break;
  case RegionKeyAction::Ignore:e->ignore();break;
  }
 }

private:
 RegionOverlay& owner_;
 QRectF logicalRect_;QString identity_;
};
RegionOverlay::RegionOverlay(QList<CaptureFrame> frames,QObject* parent):RegionSelector(parent),frames_(std::move(frames)){}
RegionOverlay::~RegionOverlay(){finish();for(auto* s:surfaces_)delete s;}
void RegionOverlay::start(){
 for(const auto& frame:frames_){auto* surface=new Surface(*this,frame);surfaces_.append(surface);skipCloseAnimation(surface);surface->show();surface->raise();}
 Surface* focus=surfaces_.isEmpty()?nullptr:surfaces_.first();for(auto* s:surfaces_)if(s->geometry().contains(QCursor::pos()))focus=s;
 if(focus){keyboardScreen_=surfaces_.indexOf(focus);focus->grabKeyboard();for(auto* surface:surfaces_)surface->updateSelection({});}
 connect(qGuiApp,&QGuiApplication::screenAdded,this,[this]{cancel({"display_changed","Display changed during region selection",{}});});connect(qGuiApp,&QGuiApplication::screenRemoved,this,[this]{cancel({"display_changed","Display changed during region selection",{}});});
 for(auto* screen:QGuiApplication::screens()) {connect(screen,&QScreen::geometryChanged,this,[this]{cancel({"display_changed","Display geometry changed during region selection",{}});});connect(screen,&QScreen::logicalDotsPerInchChanged,this,[this]{cancel({"display_changed","Display scale changed during region selection",{}});});connect(screen,&QScreen::orientationChanged,this,[this]{cancel({"display_changed","Display rotation changed during region selection",{}});});}
}
void RegionOverlay::press(QPointF point){if(finished_)return;keyboardScreen_=-1;anchor_=point;dragging_=true;selection_=QRectF(point,QSizeF{});}
void RegionOverlay::move(QPointF point){if(!dragging_||finished_)return;const auto previous=selection_;selection_=QRectF(anchor_,point).normalized();for(auto* s:surfaces_)s->updateSelection(previous);}
void RegionOverlay::release(QPointF point){if(!dragging_||finished_)return;move(point);if(selection_.width()<1||selection_.height()<1){cancel();return;}finish();emit selected(selection_);}
void RegionOverlay::finish(){finished_=true;for(auto* s:surfaces_){s->releaseKeyboard();s->releaseMouse();s->hide();}}
void RegionOverlay::cancel(Error error){if(finished_)return;finish();emit cancelled(error);}
}
