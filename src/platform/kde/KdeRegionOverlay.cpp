#include <QCursor>
#include "ui/SelectionOutline.h"
#include "ui/RegionSelection.h"
#include "KdeRegionOverlay.h"
#include "KdeRegionProtocol.h"
#include <QGuiApplication>
#include <QScreen>
#include <QPainter>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QCloseEvent>
#include <QtMath>
#include <QWindow>
#include <QTimer>
#include <QCursor>
#if defined(PUTGD_HAVE_LAYER_SHELL) && QT_VERSION >= QT_VERSION_CHECK(6,5,0)
#include <LayerShellQt/Window>
#include <wayland-client.h>
#define PUTGD_NATIVE_REGION 1
#endif
namespace putgd {
class KdeRegionOverlay::Surface final:public QWidget {
public:
 Surface(KdeRegionOverlay& owner,const CaptureFrame& frame,QScreen* screen):QWidget(nullptr,Qt::Tool|Qt::FramelessWindowHint),owner_(owner),logicalRect_(frame.logicalRect),identity_(frame.identity){
  setFont(nativeRegionFont());setObjectName("putgd-kde-region-overlay");setAccessibleName(owner_.recording_?"Recording region":"Capture region");describeRegion(this,{},owner_.recording_,QStringLiteral("Display %1").arg(activeScreen()+1),logicalRect_,owner_.frames_,false,owner_.outcome_);setAttribute(Qt::WA_TranslucentBackground);setScreen(screen);resize(frame.logicalRect.size().toSize());setCursor(Qt::CrossCursor);setMouseTracking(true);setFocusPolicy(Qt::StrongFocus);
#ifdef PUTGD_NATIVE_REGION
  // An individual layer role is applied before show; ordinary app windows keep
  // their normal xdg-shell integration. Never mutate QT_WAYLAND_SHELL_INTEGRATION.
  winId();auto* layer=LayerShellQt::Window::get(windowHandle());layer->setScope("putgd-region-selector");layer->setLayer(LayerShellQt::Window::LayerOverlay);
  const auto anchors=LayerShellQt::Window::Anchors(LayerShellQt::Window::AnchorTop)
   |LayerShellQt::Window::AnchorBottom|LayerShellQt::Window::AnchorLeft
   |LayerShellQt::Window::AnchorRight;
  layer->setAnchors(anchors);
  layer->setExclusiveZone(-1);layer->setKeyboardInteractivity(LayerShellQt::Window::KeyboardInteractivityExclusive);
#endif
 }
 void updateSelection(){describeRegion(this,owner_.selection_,owner_.recording_,QStringLiteral("Display %1").arg(activeScreen()+1),owner_.frames_[activeScreen()].logicalRect,owner_.frames_,owner_.dragging_,owner_.outcome_);update();}
protected:
 void paintEvent(QPaintEvent*) override{
  QPainter p(this);p.setCompositionMode(QPainter::CompositionMode_Source);p.fillRect(rect(),Qt::transparent);p.setCompositionMode(QPainter::CompositionMode_SourceOver);
  if(owner_.clearing_)return;
  const int own=ownScreen();
  if(owner_.frozen_&&!owner_.frames_[own].pixels.isNull())p.drawImage(rect(),owner_.frames_[own].pixels);
  // No scrim on any display (#629): the outline, a fading hint and the size.
  const auto clipped=(owner_.selection_.isEmpty()&&owner_.frames_[activeScreen()].identity==identity_?logicalRect_:owner_.selection_.intersected(logicalRect_));
  if(!clipped.isEmpty())paintSelectionOutline(p,clipped.translated(-logicalRect_.topLeft()));
  QList<QRectF> displays;for(const auto& frame:owner_.frames_)displays.append(frame.logicalRect);
  paintRegionChrome(p,rect(),owner_.selection_.translated(-logicalRect_.topLeft()),regionSizeLabel(owner_.selection_,owner_.frames_),own==owner_.hintScreen_,owner_.hint_.opacity(),regionLabelDisplay(displays,owner_.selection_)==own);
 }
 int ownScreen() const {for(int i=0;i<owner_.frames_.size();++i)if(owner_.frames_[i].identity==identity_)return i;return 0;}
 void mousePressEvent(QMouseEvent* e) override{if(e->button()==Qt::RightButton)owner_.cancel();else if(e->button()==Qt::LeftButton)owner_.press(logicalRect_.topLeft()+e->position());}
 void mouseMoveEvent(QMouseEvent* e) override{owner_.move(logicalRect_.topLeft()+e->position());}
 void mouseReleaseEvent(QMouseEvent* e) override{if(e->button()==Qt::LeftButton)owner_.release(logicalRect_.topLeft()+e->position());}
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
  case RegionKeyAction::Accept:owner_.accept();break;
  case RegionKeyAction::Change:owner_.keyboardScreen_=screen;owner_.dragging_=false;for(auto* surface:owner_.surfaces_)surface->updateSelection();break;
  case RegionKeyAction::Ignore:e->ignore();break;
  }
 }
 void closeEvent(QCloseEvent* e) override{owner_.cancel({"display_changed","The compositor dismissed a selection surface",{}});e->accept();}
private:KdeRegionOverlay& owner_;QRectF logicalRect_;QString identity_;
};
KdeRegionOverlay::KdeRegionOverlay(QList<CaptureFrame> frames,QObject* parent,bool recording,QString outcome,bool frozen):QObject(parent),frames_(std::move(frames)),outcome_(std::move(outcome)),recording_(recording),frozen_(frozen){}
void KdeRegionOverlay::repaint(){for(auto* surface:surfaces_)surface->update();}
KdeRegionOverlay::~KdeRegionOverlay(){
#ifdef PUTGD_NATIVE_REGION
 if(dismissal_)wl_callback_destroy(dismissal_);
#endif
 finish();for(auto* surface:surfaces_)delete surface;
}
void KdeRegionOverlay::start(){
#ifndef PUTGD_NATIVE_REGION
 cancel({"region_unavailable","Native region selection requires current distro Qt and per-window LayerShellQt",{}});return;
#else
 connect(qGuiApp,&QGuiApplication::screenAdded,this,[this]{cancel({"display_changed","Display changed during region selection",{}});});connect(qGuiApp,&QGuiApplication::screenRemoved,this,[this]{cancel({"display_changed","Display changed during region selection",{}});});
 keyboardScreen_=0;for(int i=0;i<frames_.size();++i)if(frames_[i].logicalRect.contains(QCursor::pos()))keyboardScreen_=i;
 hintScreen_=keyboardScreen_;
 for(const auto& frame:frames_){QScreen* match=nullptr;for(auto* screen:QGuiApplication::screens())if(screen->name()==frame.identity){match=screen;break;}if(!match){cancel({"display_changed","Capture output disappeared before selection",{}});return;}
  connect(match,&QScreen::geometryChanged,this,[this]{cancel({"display_changed","Display geometry changed during region selection",{}});});connect(match,&QScreen::logicalDotsPerInchChanged,this,[this]{cancel({"display_changed","Display scale changed during region selection",{}});});connect(match,&QScreen::physicalDotsPerInchChanged,this,[this]{cancel({"display_changed","Display density changed during region selection",{}});});connect(match,&QScreen::orientationChanged,this,[this]{cancel({"display_changed","Display rotation changed during region selection",{}});});
  addSurface(frame,match);
 }
 hint_.start();
#endif
}
void KdeRegionOverlay::addSurface(const CaptureFrame& frame,QScreen* screen){auto* surface=new Surface(*this,frame,screen);surfaces_.append(surface);surface->show();}
void KdeRegionOverlay::select(const QRectF& logical){if(finished_||logical.isEmpty())return;press(logical.topLeft());release(logical.bottomRight());}
void KdeRegionOverlay::press(QPointF p){if(finished_)return;keyboardScreen_=-1;anchor_=kdeRegionEdge(p);dragging_=true;selection_=QRectF(anchor_,QSizeF{});for(auto* surface:surfaces_)surface->updateSelection();}
void KdeRegionOverlay::move(QPointF p){if(!dragging_||finished_)return;const auto endpoint=kdeRegionEdge(p);selection_=QRectF(anchor_,endpoint).normalized();for(auto* surface:surfaces_)surface->updateSelection();}
void KdeRegionOverlay::release(QPointF p){if(!dragging_||finished_)return;move(p);if(selection_.width()<1||selection_.height()<1){cancel();return;}accept();
}
void KdeRegionOverlay::accept(){
 if(finished_)return;
 dragging_=false;
#ifdef PUTGD_NATIVE_REGION
 // KWin animates closing surfaces on real GPUs, starting from the last frame it
 // rendered for them, and ScreenShot2 renders that animation. So first commit
 // fully transparent content and wait until KWin has rendered it: Qt Wayland
 // requests a frame callback with each committed buffer and withholds
 // requestUpdate() until it fires. Only then unmap, so the close animation has
 // nothing visible to fade. The bound covers a compositor that never renders.
 finished_=true;clearing_=true;
 for(auto* surface:surfaces_){surface->repaint();if(auto* window=surface->windowHandle()){awaitingFrames_.insert(window);window->installEventFilter(this);window->requestUpdate();}}
 QTimer::singleShot(250,this,[this]{if(!awaitingFrames_.isEmpty()){awaitingFrames_.clear();dismiss();}});
 if(awaitingFrames_.isEmpty())dismiss();
#else
 finish();emit selected(selection_);
#endif
}
bool KdeRegionOverlay::eventFilter(QObject* watched,QEvent* event){
 if(event->type()==QEvent::UpdateRequest&&awaitingFrames_.remove(static_cast<QWindow*>(watched))&&awaitingFrames_.isEmpty())QTimer::singleShot(0,this,[this]{dismiss();});
 return QObject::eventFilter(watched,event);
}
void KdeRegionOverlay::dismiss(){
#ifdef PUTGD_NATIVE_REGION
 if(dismissing_)return;
 dismissing_=true;
 finish();
 // Order the capture after KWin has processed the unmaps. D-Bus and Wayland are
 // separate transports, so merely hiding the widgets is not enough.
 auto* native=qGuiApp->nativeInterface<QNativeInterface::QWaylandApplication>();
 if(!native){emit cancelled({"region_unavailable","Qt has no native Wayland connection",{}});return;}
 static const wl_callback_listener listener{[](void* data,wl_callback* callback,uint32_t){
  auto* owner=static_cast<KdeRegionOverlay*>(data);wl_callback_destroy(callback);owner->dismissal_=nullptr;
  emit owner->selected(owner->selection_);
 }};
 dismissal_=wl_display_sync(native->display());
 wl_callback_add_listener(dismissal_,&listener,this);wl_display_flush(native->display());
#endif
}
void KdeRegionOverlay::finish(){finished_=true;hint_.stop();for(auto* surface:surfaces_)surface->hide();}
void KdeRegionOverlay::cancel(Error error){
 if(finished_){
#ifdef PUTGD_NATIVE_REGION
  if(!dismissal_&&(dismissing_||!clearing_))return;
  awaitingFrames_.clear();dismissing_=true;finish();
  if(dismissal_){wl_callback_destroy(dismissal_);dismissal_=nullptr;}
#else
  return;
#endif
 }else finish();
 emit cancelled(error);
}
}
