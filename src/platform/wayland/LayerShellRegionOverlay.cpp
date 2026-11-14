#include <QCursor>
#include "ui/SelectionOutline.h"
#include "ui/RegionSelection.h"
#include "ui/CountdownPill.h"
#include "LayerShellRegionOverlay.h"
#include "RegionProtocol.h"
#include <QGuiApplication>
#include <QScreen>
#include <QPainter>
#include <QPaintEvent>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QCloseEvent>
#include <QtMath>
#include <QElapsedTimer>
#include <QWindow>
#include <QTimer>
#include <algorithm>
#include <QCursor>
#include <QFontMetricsF>
#include <QPointer>
#include <atomic>
#include <utility>
#if defined(PUTGD_HAVE_LAYER_SHELL) && QT_VERSION >= QT_VERSION_CHECK(6,5,0)
#include <LayerShellQt/Window>
#include <wayland-client.h>
#define PUTGD_NATIVE_REGION 1
#endif
namespace putgd {
namespace {
// Pill fonts in this process: 0 load on first use, 1 loading on a worker, 2 loaded.
std::atomic_int pillFonts{0};
// Overlays that drew frames without pills meanwhile; GUI thread only.
QList<QPointer<LayerShellRegionOverlay>> fontWaiters;
bool pillsReady(){return pillFonts.load()!=1;}
}
class LayerShellRegionOverlay::Surface final:public QWidget {
public:
 Surface(LayerShellRegionOverlay& owner,const CaptureFrame& frame,QScreen* screen):QWidget(nullptr,Qt::Tool|Qt::FramelessWindowHint),owner_(owner),logicalRect_(frame.logicalRect),identity_(frame.identity){
  setFont(nativeRegionFont());setObjectName("putgd-wayland-region-overlay");setAccessibleName(owner_.recording_?"Recording region":"Capture region");describeRegion(this,{},owner_.recording_,QStringLiteral("Display %1").arg(activeScreen()+1),logicalRect_,owner_.frames_,false,owner_.outcome_);setAttribute(Qt::WA_TranslucentBackground);setScreen(screen);
  // Place the window on its output before the platform window exists. Qt moves a
  // top-level whose geometry centre lies on another screen to that screen, and
  // the layer surface binds the QWindow's output: a surface left at (0,0) lands
  // on the head at the origin and the other heads get no selector (#1048).
  setGeometry(frame.logicalRect.toAlignedRect());setCursor(Qt::CrossCursor);setMouseTracking(true);setFocusPolicy(Qt::StrongFocus);
#ifdef PUTGD_NATIVE_REGION
  // An individual layer role is applied before show; ordinary app windows keep
  // their normal xdg-shell integration. Never mutate QT_WAYLAND_SHELL_INTEGRATION.
  winId();auto* layer=LayerShellQt::Window::get(windowHandle());layer->setScope("putgd-region-selector");layer->setLayer(LayerShellQt::Window::LayerOverlay);
  const auto anchors=LayerShellQt::Window::Anchors(LayerShellQt::Window::AnchorTop)
   |LayerShellQt::Window::AnchorBottom|LayerShellQt::Window::AnchorLeft
   |LayerShellQt::Window::AnchorRight;
  layer->setAnchors(anchors);
  layer->setExclusiveZone(-1);layer->setKeyboardInteractivity(owner_.deferKeyboard_?LayerShellQt::Window::KeyboardInteractivityNone:LayerShellQt::Window::KeyboardInteractivityExclusive);
#endif
 }
 // Damage only the outline rings and pills of the previous and next state;
 // the frozen backdrop is blitted for those pixels alone and Qt Wayland
 // commits that region, not a monitor-sized buffer, per pointer event (#6).
 void updateSelection(){describeRegion(this,owner_.selection_,owner_.recording_,QStringLiteral("Display %1").arg(activeScreen()+1),owner_.frames_[activeScreen()].logicalRect,owner_.frames_,owner_.dragging_,owner_.outcome_);const auto next=chromeArea();update(painted_|next);painted_=next;}
 // The outline never waits for the fonts, only the pills do.
 QRegion chromeArea()const{if(!pillsReady())return selectionOutlineArea(owner_.selection_.translated(-logicalRect_.topLeft())).intersected(rect());const int own=ownScreen();QList<QRectF> displays;for(const auto& frame:owner_.frames_)displays.append(frame.logicalRect);return regionChromeArea(rect(),QFontMetrics(font()),owner_.selection_.translated(-logicalRect_.topLeft()),regionSizeLabel(owner_.selection_,owner_.frames_),own==owner_.hintScreen_&&owner_.hint_.visible(),regionLabelDisplay(displays,owner_.selection_)==own);}
protected:
 void paintEvent(QPaintEvent* e) override{
  QElapsedTimer work;work.start();
  QPainter p(this);p.setClipRegion(e->region());p.setCompositionMode(QPainter::CompositionMode_Source);p.fillRect(rect(),Qt::transparent);p.setCompositionMode(QPainter::CompositionMode_SourceOver);
  if(owner_.clearing_)return;
  const int own=ownScreen();
  if(owner_.frozen_&&!owner_.frames_[own].pixels.isNull())p.drawImage(rect(),owner_.frames_[own].pixels);
  // No scrim on any display (#629): the outline, a fading hint and the size.
  const auto clipped=owner_.selection_.intersected(logicalRect_);
  if(!clipped.isEmpty())paintSelectionOutline(p,clipped.translated(-logicalRect_.topLeft()));
  QList<QRectF> displays;for(const auto& frame:owner_.frames_)displays.append(frame.logicalRect);
  if(!pillsReady()){if(!fontWaiters.contains(&owner_))fontWaiters.append(&owner_);}
  else paintRegionChrome(p,rect(),owner_.selection_.translated(-logicalRect_.topLeft()),regionSizeLabel(owner_.selection_,owner_.frames_),own==owner_.hintScreen_,owner_.hint_.opacity(),regionLabelDisplay(displays,owner_.selection_)==own);
  owner_.stats_.painted(work.nsecsElapsed()/1e6);
  owner_.surfacePainted(this);
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
private:LayerShellRegionOverlay& owner_;QRectF logicalRect_;QString identity_;QRegion painted_;
};
LayerShellRegionOverlay::LayerShellRegionOverlay(QList<CaptureFrame> frames,QObject* parent,bool recording,QString outcome,bool frozen):QObject(parent),frames_(std::move(frames)),outcome_(std::move(outcome)),recording_(recording),frozen_(frozen){}
// The hint fade and the clearing commit before dismissal: clearing repaints everything.
void LayerShellRegionOverlay::repaint(){for(auto* surface:surfaces_){if(clearing_)surface->update();else surface->updateSelection();}}
std::thread LayerShellRegionOverlay::preloadFonts(){
 pillFonts=1;
 return std::thread([]{
  // Qt shares the font database under its own lock; engines stay per thread.
  QFontMetricsF(nativeRegionFont()).horizontalAdvance(regionHint());
  pillFonts=2;
  QMetaObject::invokeMethod(qApp,[]{for(const auto& overlay:std::exchange(fontWaiters,{}))if(overlay)overlay->fontsLoaded();},Qt::QueuedConnection);
 });
}
void LayerShellRegionOverlay::fontsLoaded(){if(!finished_)repaint();}
LayerShellRegionOverlay::~LayerShellRegionOverlay(){
#ifdef PUTGD_NATIVE_REGION
 if(dismissal_)wl_callback_destroy(dismissal_);
#endif
 finish();for(auto* surface:surfaces_)delete surface;
}
void LayerShellRegionOverlay::start(){
#ifndef PUTGD_NATIVE_REGION
 cancel({"region_unavailable","Native region selection requires current distro Qt and per-window LayerShellQt",{}});return;
#else
 connect(qGuiApp,&QGuiApplication::screenAdded,this,[this]{cancel({"display_changed","Display changed during region selection",{}});});connect(qGuiApp,&QGuiApplication::screenRemoved,this,[this]{cancel({"display_changed","Display changed during region selection",{}});});
 keyboardScreen_=0;for(int i=0;i<frames_.size();++i)if(frames_[i].logicalRect.contains(QCursor::pos()))keyboardScreen_=i;
 hintScreen_=keyboardScreen_;
 for(const auto& frame:frames_){QScreen* match=nullptr;for(auto* screen:QGuiApplication::screens())if(screen->name()==frame.identity){match=screen;break;}if(!match){cancel({"display_changed","Capture output disappeared before selection",{}});return;}
  connect(match,&QScreen::geometryChanged,this,[this]{cancel({"display_changed","Display geometry changed during region selection",{}});});connect(match,&QScreen::logicalDotsPerInchChanged,this,[this]{cancel({"display_changed","Display scale changed during region selection",{}});});connect(match,&QScreen::physicalDotsPerInchChanged,this,[this]{cancel({"display_changed","Display density changed during region selection",{}});});connect(match,&QScreen::orientationChanged,this,[this]{cancel({"display_changed","Display rotation changed during region selection",{}});});
  if(!addSurface(frame,match))return;
 }
 hint_.start();
#endif
}
bool LayerShellRegionOverlay::addSurface(const CaptureFrame& frame,QScreen* screen){
 auto* surface=new Surface(*this,frame,screen);surfaces_.append(surface);surface->winId();
 // Every output needs its own selector surface; one that Qt moved elsewhere
 // would leave its head without input and map drags to the wrong output.
 if(surface->windowHandle()->screen()!=screen){cancel({"display_changed","A selection surface is not on its output",{}});return false;}
 surface->show();return true;
}
void LayerShellRegionOverlay::surfacePainted(const void* surface){
 if(shown_||!deferKeyboard_)return;drawn_.insert(surface);if(drawn_.size()!=surfaces_.size())return;shown_=true;
 // Not yet: this first paint is still to be committed, and a commit that maps the
 // surface with the keyboard is what ends a held button. Qt Wayland holds
 // requestUpdate() until the compositor has rendered the committed buffer.
 for(auto* each:surfaces_)if(auto* window=each->windowHandle()){awaitingMapped_.insert(window);window->installEventFilter(this);window->requestUpdate();}
}
void LayerShellRegionOverlay::acceptKeyboardOnDemand(){
#ifdef PUTGD_NATIVE_REGION
 if(finished_||keyboardTaken_)return;
 for(auto* each:surfaces_){LayerShellQt::Window::get(each->windowHandle())->setKeyboardInteractivity(LayerShellQt::Window::KeyboardInteractivityOnDemand);each->update();}
#endif
}
void LayerShellRegionOverlay::takeKeyboard(int qtKey,int qtModifiers){
 if(finished_||dragging_||surfaces_.isEmpty())return;
 keyboardTaken_=true;
#ifdef PUTGD_NATIVE_REGION
 for(auto* surface:surfaces_){LayerShellQt::Window::get(surface->windowHandle())->setKeyboardInteractivity(LayerShellQt::Window::KeyboardInteractivityExclusive);surface->update();}
#endif
 QKeyEvent press(QEvent::KeyPress,qtKey,Qt::KeyboardModifiers(qtModifiers));QCoreApplication::sendEvent(surfaces_.first(),&press);
}
void LayerShellRegionOverlay::relayPointer(const QString& kind,QPointF logical){
 // A relayed press off every output (#1487) starts no drag; the selector stays open.
 if(kind=="press"&&std::none_of(frames_.begin(),frames_.end(),[&](const CaptureFrame& frame){return frame.logicalRect.contains(logical);}))return;
 if(kind=="press")press(logical);else if(kind=="move")move(logical);else if(kind=="release")release(logical);}
void LayerShellRegionOverlay::select(const QRectF& logical){if(finished_||logical.isEmpty())return;press(logical.topLeft());release(logical.bottomRight());}
void LayerShellRegionOverlay::press(QPointF p){if(finished_)return;keyboardScreen_=-1;anchor_=waylandRegionEdge(p);dragging_=true;selection_=QRectF(anchor_,QSizeF{});for(auto* surface:surfaces_)surface->updateSelection();}
void LayerShellRegionOverlay::move(QPointF p){if(!dragging_||finished_)return;const auto endpoint=waylandRegionEdge(p);selection_=QRectF(anchor_,endpoint).normalized();++stats_.moves;for(auto* surface:surfaces_)surface->updateSelection();}
void LayerShellRegionOverlay::release(QPointF p){if(!dragging_||finished_)return;move(p);if(selection_.width()<1||selection_.height()<1){cancel();return;}accept();
}
void LayerShellRegionOverlay::accept(){
 if(finished_)return;
 dragging_=false;
 // A software cursor is a separate compositor surface: clearing the widget
 // does not clear it. Hide it before the transparent frame and its fence.
 for(auto* surface:surfaces_)surface->setCursor(Qt::BlankCursor);
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
bool LayerShellRegionOverlay::eventFilter(QObject* watched,QEvent* event){
 if(event->type()==QEvent::UpdateRequest&&awaitingMapped_.remove(static_cast<QWindow*>(watched))&&awaitingMapped_.isEmpty())QTimer::singleShot(0,this,[this]{acceptKeyboardOnDemand();});
 if(event->type()==QEvent::UpdateRequest&&awaitingFrames_.remove(static_cast<QWindow*>(watched))&&awaitingFrames_.isEmpty())QTimer::singleShot(0,this,[this]{dismiss();});
 return QObject::eventFilter(watched,event);
}
void LayerShellRegionOverlay::dismiss(){
#ifdef PUTGD_NATIVE_REGION
 if(dismissing_)return;
 dismissing_=true;
 finish();
 // Order the capture after KWin has processed the unmaps. D-Bus and Wayland are
 // separate transports, so merely hiding the widgets is not enough.
 auto* native=qGuiApp->nativeInterface<QNativeInterface::QWaylandApplication>();
 if(!native){emit cancelled({"region_unavailable","Qt has no native Wayland connection",{}});return;}
 static const wl_callback_listener listener{[](void* data,wl_callback* callback,uint32_t){
  auto* owner=static_cast<LayerShellRegionOverlay*>(data);wl_callback_destroy(callback);owner->dismissal_=nullptr;
  emit owner->selected(owner->selection_);
 }};
 dismissal_=wl_display_sync(native->display());
 wl_callback_add_listener(dismissal_,&listener,this);wl_display_flush(native->display());
#endif
}
void LayerShellRegionOverlay::finish(){if(stats_.paints&&!logged_){logged_=true;regionDebugLog(stats_.summary(QStringLiteral("wayland region")));}finished_=true;hint_.stop();for(auto* surface:surfaces_)surface->hide();}
void LayerShellRegionOverlay::cancel(Error error){
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
#ifdef PUTGD_NATIVE_REGION
namespace {
// "The output the compositor picks": wantsToBeOnActiveScreen where LayerShellQt
// has it (6.6), the screen configuration it replaced before that.
template<class Layer> void placeOnActiveScreen(Layer* layer){
 if constexpr(requires{layer->setWantsToBeOnActiveScreen(true);})layer->setWantsToBeOnActiveScreen(true);
 else layer->setScreenConfiguration(Layer::ScreenFromCompositor);
}
}
#endif
bool showLayerShellCountdown(QWidget& pill,QScreen* screen){
#ifdef PUTGD_NATIVE_REGION
 pill.winId();auto* layer=pill.windowHandle()?LayerShellQt::Window::get(pill.windowHandle()):nullptr;if(!layer)return false;
 // Hyprland's no_anim rule names this scope, so the pill leaves in one step.
 layer->setScope("putgd-capture-countdown");layer->setLayer(LayerShellQt::Window::LayerOverlay);
 layer->setAnchors(LayerShellQt::Window::AnchorTop);layer->setMargins(QMargins(0,Ui::space(Ui::Space::l),0,0));
 layer->setKeyboardInteractivity(LayerShellQt::Window::KeyboardInteractivityNone);
 if(screen)pill.windowHandle()->setScreen(screen);
 else placeOnActiveScreen(layer);
 pill.show();return true;
#else
 Q_UNUSED(pill);Q_UNUSED(screen);return false;
#endif
}
#ifdef PUTGD_NATIVE_REGION
namespace {
// One pill's removal: blank frame rendered, unmap, display sync.
class CountdownDismissal final:public QObject {
public:
 CountdownDismissal(CountdownPill& pill,std::function<void(bool,bool)> done):QObject(&pill),pill_(pill),done_(std::move(done)){}
 ~CountdownDismissal() override{if(fence_){wl_callback_destroy(fence_);fence_=nullptr;}}
 void start(){
  pill_.blank();
  auto* window=pill_.windowHandle();
  if(!window){unmap();return;}
  // Qt Wayland withholds requestUpdate() until the frame callback of the buffer
  // just committed has fired: the compositor has rendered the empty frame.
  window->installEventFilter(this);window->requestUpdate();
  // A frame callback comes within a frame or two on a screen that is on and
  // unlocked, and late on a compositor that is busy: a software-rendered guest
  // starting a browser took over a second once. None for three seconds means
  // this surface is not being shown.
  QTimer::singleShot(3000,this,[this]{unmap();});
 }
protected:
 bool eventFilter(QObject* watched,QEvent* event)override{
  if(event->type()==QEvent::UpdateRequest&&!unmapped_){presented_=true;QTimer::singleShot(0,this,[this]{unmap();});}
  return QObject::eventFilter(watched,event);
 }
private:
 void unmap(){
  if(unmapped_)return;
  unmapped_=true;pill_.hide();
  auto* native=qGuiApp->nativeInterface<QNativeInterface::QWaylandApplication>();
  fence_=native?wl_display_sync(native->display()):nullptr;
  if(!fence_){finish(false);return;}
  static const wl_callback_listener listener{[](void* data,wl_callback*,uint32_t){static_cast<CountdownDismissal*>(data)->finish(true);}};
  wl_callback_add_listener(fence_,&listener,this);wl_display_flush(native->display());
  QTimer::singleShot(1000,this,[this]{finish(false);});
 }
 void finish(bool acknowledged){
  if(!done_)return;
  auto done=std::exchange(done_,{});
  if(fence_){wl_callback_destroy(fence_);fence_=nullptr;}
  deleteLater();done(acknowledged,presented_);
 }
 CountdownPill& pill_;std::function<void(bool,bool)> done_;wl_callback* fence_=nullptr;bool unmapped_=false,presented_=false;
};
}
#endif
void hideLayerShellCountdown(CountdownPill& pill,std::function<void(bool,bool)> done){
#ifdef PUTGD_NATIVE_REGION
 (new CountdownDismissal(pill,std::move(done)))->start();
#else
 pill.hide();done(false,false);
#endif
}
}
