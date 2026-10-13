#include "ui/SelectionOutline.h"
#include "KdeRegionOverlay.h"
#include <QGuiApplication>
#include <QScreen>
#include <QPainter>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QCloseEvent>
#include <QtMath>
#if defined(PUTGD_HAVE_LAYER_SHELL) && QT_VERSION >= QT_VERSION_CHECK(6,5,0)
#include <LayerShellQt/Window>
#include <wayland-client.h>
#define PUTGD_NATIVE_REGION 1
#endif
namespace putgd {
class KdeRegionOverlay::Surface final:public QWidget {
public:
 Surface(KdeRegionOverlay& owner,const CaptureFrame& frame,QScreen* screen):QWidget(nullptr,Qt::Tool|Qt::FramelessWindowHint),owner_(owner),logicalRect_(frame.logicalRect){
  setObjectName("putgd-kde-region-overlay");setAttribute(Qt::WA_TranslucentBackground);setScreen(screen);resize(frame.logicalRect.size().toSize());setCursor(Qt::CrossCursor);setMouseTracking(true);setFocusPolicy(Qt::StrongFocus);
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
protected:
 void paintEvent(QPaintEvent*) override{QPainter p(this);p.setCompositionMode(QPainter::CompositionMode_Source);p.fillRect(rect(),Qt::transparent);p.setCompositionMode(QPainter::CompositionMode_SourceOver);const auto clipped=owner_.selection_.intersected(logicalRect_);if(!clipped.isEmpty()){const auto local=clipped.translated(-logicalRect_.topLeft());paintSelectionOutline(p,local);}}
 void mousePressEvent(QMouseEvent* e) override{if(e->button()==Qt::RightButton)owner_.cancel();else if(e->button()==Qt::LeftButton)owner_.press(logicalRect_.topLeft()+e->position());}
 void mouseMoveEvent(QMouseEvent* e) override{owner_.move(logicalRect_.topLeft()+e->position());}
 void mouseReleaseEvent(QMouseEvent* e) override{if(e->button()==Qt::LeftButton)owner_.release(logicalRect_.topLeft()+e->position());}
 void keyPressEvent(QKeyEvent* e) override{if(e->key()==Qt::Key_Escape)owner_.cancel();}
 void closeEvent(QCloseEvent* e) override{owner_.cancel({"display_changed","The compositor dismissed a selection surface",{}});e->accept();}
private:KdeRegionOverlay& owner_;QRectF logicalRect_;
};
KdeRegionOverlay::KdeRegionOverlay(QList<CaptureFrame> frames,QObject* parent):QObject(parent),frames_(std::move(frames)){}
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
 for(const auto& frame:frames_){QScreen* match=nullptr;for(auto* screen:QGuiApplication::screens())if(screen->name()==frame.identity){match=screen;break;}if(!match){cancel({"display_changed","Capture output disappeared before selection",{}});return;}
  connect(match,&QScreen::geometryChanged,this,[this]{cancel({"display_changed","Display geometry changed during region selection",{}});});connect(match,&QScreen::logicalDotsPerInchChanged,this,[this]{cancel({"display_changed","Display scale changed during region selection",{}});});connect(match,&QScreen::physicalDotsPerInchChanged,this,[this]{cancel({"display_changed","Display density changed during region selection",{}});});connect(match,&QScreen::orientationChanged,this,[this]{cancel({"display_changed","Display rotation changed during region selection",{}});});
  auto* surface=new Surface(*this,frame,match);surfaces_.append(surface);surface->show();
 }
#endif
}
void KdeRegionOverlay::press(QPointF p){if(finished_)return;anchor_=QPointF(qRound(p.x()),qRound(p.y()));dragging_=true;selection_=QRectF(anchor_,QSizeF{});}
void KdeRegionOverlay::move(QPointF p){if(!dragging_||finished_)return;const QPointF endpoint(qRound(p.x()),qRound(p.y()));selection_=QRectF(anchor_,endpoint).normalized();for(auto* surface:surfaces_)surface->update();}
void KdeRegionOverlay::release(QPointF p){if(!dragging_||finished_)return;move(p);if(selection_.width()<1||selection_.height()<1){cancel();return;}finish();
#ifdef PUTGD_NATIVE_REGION
 // Order the capture after KWin has processed Qt's surface unmaps. D-Bus and
 // Wayland are separate transports, so merely hiding the widgets is not enough.
 auto* native=qGuiApp->nativeInterface<QNativeInterface::QWaylandApplication>();
 if(!native){emit cancelled({"region_unavailable","Qt has no native Wayland connection",{}});return;}
 static const wl_callback_listener listener{[](void* data,wl_callback* callback,uint32_t){
  auto* owner=static_cast<KdeRegionOverlay*>(data);wl_callback_destroy(callback);owner->dismissal_=nullptr;
  emit owner->selected(owner->selection_);
 }};
 dismissal_=wl_display_sync(native->display());
 wl_callback_add_listener(dismissal_,&listener,this);wl_display_flush(native->display());
#else
 emit selected(selection_);
#endif
}
void KdeRegionOverlay::finish(){finished_=true;for(auto* surface:surfaces_)surface->hide();}
void KdeRegionOverlay::cancel(Error error){
 if(finished_){
#ifdef PUTGD_NATIVE_REGION
  if(!dismissal_)return;
  wl_callback_destroy(dismissal_);dismissal_=nullptr;
#else
  return;
#endif
 }else finish();
 emit cancelled(error);
}
}
