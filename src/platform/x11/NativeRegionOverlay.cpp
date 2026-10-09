#include "NativeRegionOverlay.h"
#include <QCursor>
#include <QGuiApplication>
#include <QScreen>
#include <QSocketNotifier>
#include <QWindow>
#include <QTimer>
#include <QSurfaceFormat>
#include <QtGui/qguiapplication_platform.h>
#include <X11/keysym.h>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <unistd.h>
namespace putgd {
namespace {
Error unavailable(){return {"capture_unavailable","Cannot initialize region selection",{}};}
bool nativeGeometry(const QRect& r){
 // Core input reports signed16 root coordinates; core drawing also uses
 // signed16 local origins. Both complete rectangles must be representable.
 return r.width()>0&&r.height()>0&&r.width()<=32768&&r.height()<=32768&&r.x()>=-32768&&r.y()>=-32768&&qint64(r.x())+r.width()-1<=32767&&qint64(r.y())+r.height()-1<=32767;
}
}
NativeRegionOverlay::NativeRegionOverlay(QList<CaptureFrame> frames,QObject* parent,xcb_cursor_t residentCursor):RegionSelector(parent),frames_(std::move(frames)),residentCursor_(residentCursor){}
NativeRegionOverlay::~NativeRegionOverlay(){finish();}
bool NativeRegionOverlay::supportsLayout(const QList<CaptureFrame>& frames){
 if(frames.isEmpty())return false;
 for(const auto& frame:frames){
  const auto& logical=frame.logicalRect;
  if(!nativeGeometry(frame.nativeRect)||!std::isfinite(logical.x())||!std::isfinite(logical.y())||!std::isfinite(logical.width())||!std::isfinite(logical.height())||!std::isfinite(logical.right())||!std::isfinite(logical.bottom())||logical.width()<=0||logical.height()<=0||!std::isfinite(frame.density)||frame.density<=0||frame.density>32768)return false;
  if(logical.x()<std::numeric_limits<int>::min()||logical.x()>std::numeric_limits<int>::max()||logical.y()<std::numeric_limits<int>::min()||logical.y()>std::numeric_limits<int>::max())return false;
  if(!std::isfinite(frame.nativeRect.width()/logical.width())||!std::isfinite(frame.nativeRect.height()/logical.height()))return false;
 }
 return true;
}
bool NativeRegionOverlay::checked(xcb_void_cookie_t cookie){auto* error=xcb_request_check(connection_,cookie);const bool ok=!error;std::free(error);return ok;}
void NativeRegionOverlay::start(){
 if(started_||finished_)return;
 started_=true;
 if(!supportsLayout(frames_)){cancel(unavailable());return;}
 connection_=xcb_connect(nullptr,nullptr);
 if(!connection_||xcb_connection_has_error(connection_)){cancel(unavailable());return;}
 root_=xcb_setup_roots_iterator(xcb_get_setup(connection_)).data->root;
 // A native Qt raster window supplies the themed cursor without constructing
 // a QWidget, style or backing store. It remains unmapped until embedded.
#if QT_CONFIG(xcb)
 auto* native=qGuiApp->nativeInterface<QNativeInterface::QX11Application>();
 qtConnection_=native?native->connection():nullptr;
#endif
 if(!qtConnection_){cancel(unavailable());return;}
 cursorWindow_=new QWindow;
 cursorWindow_->setSurfaceType(QSurface::RasterSurface);
 QSurfaceFormat format;format.setAlphaBufferSize(8);cursorWindow_->setFormat(format);
 cursorWindow_->setFlags(Qt::FramelessWindowHint|Qt::WindowDoesNotAcceptFocus|Qt::WindowTransparentForInput);
 for(auto* screen:QGuiApplication::screens())if(screen->name()==frames_.first().identity){cursorWindow_->setScreen(screen);break;}
 cursorWindow_->setGeometry(QRect(frames_.first().logicalRect.topLeft().toPoint(),QSize(1,1)));
 if(!residentCursor_)cursorWindow_->setCursor(Qt::CrossCursor);
 cursorWindow_->create();
 // Monitor changes must cancel while the cursor child and native surfaces initialize.
 connect(qGuiApp,&QGuiApplication::screenAdded,this,[this]{cancel({"display_changed","Display changed during region selection",{}});});
 connect(qGuiApp,&QGuiApplication::screenRemoved,this,[this]{cancel({"display_changed","Display changed during region selection",{}});});
 for(auto* screen:QGuiApplication::screens()){
  connect(screen,&QScreen::geometryChanged,this,[this]{cancel({"display_changed","Display geometry changed during region selection",{}});});
  connect(screen,&QScreen::logicalDotsPerInchChanged,this,[this]{cancel({"display_changed","Display scale changed during region selection",{}});});
  connect(screen,&QScreen::orientationChanged,this,[this]{cancel({"display_changed","Display rotation changed during region selection",{}});});
 }
 // Defer once without pumping events, so cancellation or display changes
 // queued by the caller can release the unmapped Qt resource first.
 QTimer::singleShot(0,this,[this]{if(!finished_)initializeSurfaces();});
}
void NativeRegionOverlay::initializeSurfaces(){
 if(finished_||initialized_)return;
 initialized_=true;
 const auto input=xcb_window_t(cursorWindow_->winId());
 // Flush Qt's cursor/window creation on its own public native connection.
 auto* qtFence=xcb_get_input_focus_reply(qtConnection_,xcb_get_input_focus(qtConnection_),nullptr);std::free(qtFence);
 const auto geometryCookie=xcb_get_geometry(connection_,input);
 const auto attributesCookie=xcb_get_window_attributes(connection_,input);
 auto* geometry=xcb_get_geometry_reply(connection_,geometryCookie,nullptr);
 auto* attributes=xcb_get_window_attributes_reply(connection_,attributesCookie,nullptr);
 const bool visualOk=geometry&&attributes&&geometry->depth==32&&!attributes->override_redirect&&attributes->map_state==XCB_MAP_STATE_UNMAPPED;
 const auto visual=attributes?attributes->visual:xcb_visualid_t{};
 std::free(geometry);std::free(attributes);
 if(!visualOk){cancel(unavailable());return;}
 colormap_=xcb_generate_id(connection_);
 if(!checked(xcb_create_colormap_checked(connection_,XCB_COLORMAP_ALLOC_NONE,colormap_,root_,visual))){cancel(unavailable());return;}
 const auto pidCookie=xcb_intern_atom(connection_,false,11,"_NET_WM_PID");
 auto* pidAtom=xcb_intern_atom_reply(connection_,pidCookie,nullptr);
 if(!pidAtom){cancel(unavailable());return;}
 const auto pidProperty=pidAtom->atom;std::free(pidAtom);
 QList<xcb_void_cookie_t> creates;
 for(const auto& frame:frames_){
  Surface surface;surface.frame=frame;surface.frame.pixels={};surface.window=xcb_generate_id(connection_);surface.pen=std::max(1,qRound(frame.density));
  const uint32_t values[]={0,0,1,XCB_EVENT_MASK_EXPOSURE|XCB_EVENT_MASK_BUTTON_PRESS|XCB_EVENT_MASK_BUTTON_RELEASE|XCB_EVENT_MASK_POINTER_MOTION|XCB_EVENT_MASK_KEY_PRESS,colormap_};
  creates.append(xcb_create_window_checked(connection_,32,surface.window,root_,int16_t(frame.nativeRect.x()),int16_t(frame.nativeRect.y()),uint16_t(frame.nativeRect.width()),uint16_t(frame.nativeRect.height()),0,XCB_WINDOW_CLASS_INPUT_OUTPUT,visual,XCB_CW_BACK_PIXEL|XCB_CW_BORDER_PIXEL|XCB_CW_OVERRIDE_REDIRECT|XCB_CW_EVENT_MASK|XCB_CW_COLORMAP,values));
  const uint32_t pid=uint32_t(getpid());xcb_change_property(connection_,XCB_PROP_MODE_REPLACE,surface.window,pidProperty,XCB_ATOM_CARDINAL,32,1,&pid);
  surfaces_.append(std::move(surface));
 }
 for(const auto cookie:creates)if(!checked(cookie)){cancel(unavailable());return;}
 gc_=xcb_generate_id(connection_);const uint32_t gcValues[]={0xffffffff,0};
 if(!checked(xcb_create_gc_checked(connection_,gc_,surfaces_.first().window,XCB_GC_FOREGROUND|XCB_GC_GRAPHICS_EXPOSURES,gcValues))){cancel(unavailable());return;}
 keys_=xcb_key_symbols_alloc(connection_);
 if(!keys_){cancel(unavailable());return;}
 // fromWinId is used solely for its documented embedding/parenting purpose;
 // the wrapper never maps, modifies or owns the native monitor parent.
 foreignParent_=QWindow::fromWinId(surfaces_.first().window);
 if(!foreignParent_){cancel(unavailable());return;}
 cursorWindow_->setParent(foreignParent_);
 cursorWindow_->setGeometry(0,0,1,1);
 const uint32_t transparent=0;
 if(!checked(xcb_change_window_attributes_checked(connection_,input,XCB_CW_BACK_PIXEL,&transparent))){cancel(unavailable());return;}
 // The child maps immediately, but stays unviewable while its parent is
 // unmapped. Qt's empty input shape lets early clicks reach the native parent.
 cursorWindow_->show();
 qtFence=xcb_get_input_focus_reply(qtConnection_,xcb_get_input_focus(qtConnection_),nullptr);std::free(qtFence);
 auto* parent=xcb_query_tree_reply(connection_,xcb_query_tree(connection_,input),nullptr);
 const bool embedded=parent&&parent->parent==surfaces_.first().window;std::free(parent);
 if(!embedded){cancel(unavailable());return;}
 // Map every monitor, then request both grabs in one ordered XCB batch. Any
 // pre-grab parent input and subsequent child-grab input are delivered to this
 // same connection, preserving event order without a Qt logical-input fallback.
 for(const auto& surface:surfaces_)xcb_map_window(connection_,surface.window);
 const auto pointerCookie=xcb_grab_pointer(connection_,false,input,XCB_EVENT_MASK_BUTTON_PRESS|XCB_EVENT_MASK_BUTTON_RELEASE|XCB_EVENT_MASK_POINTER_MOTION,XCB_GRAB_MODE_ASYNC,XCB_GRAB_MODE_ASYNC,XCB_WINDOW_NONE,residentCursor_,XCB_CURRENT_TIME);
 const auto keyboardCookie=xcb_grab_keyboard(connection_,false,input,XCB_CURRENT_TIME,XCB_GRAB_MODE_ASYNC,XCB_GRAB_MODE_ASYNC);
 xcb_flush(connection_);
 auto* pointer=xcb_grab_pointer_reply(connection_,pointerCookie,nullptr);
 pointerGrabbed_=pointer&&pointer->status==XCB_GRAB_STATUS_SUCCESS;std::free(pointer);
 auto* keyboard=xcb_grab_keyboard_reply(connection_,keyboardCookie,nullptr);
 keyboardGrabbed_=keyboard&&keyboard->status==XCB_GRAB_STATUS_SUCCESS;std::free(keyboard);
 if(!pointerGrabbed_||!keyboardGrabbed_){cancel(unavailable());return;}
 notifier_=new QSocketNotifier(xcb_get_file_descriptor(connection_),QSocketNotifier::Read,this);
 connect(notifier_,&QSocketNotifier::activated,this,[this]{events();});
 // Replies above may already have buffered events; do not depend on another
 // socket wakeup to consume an initial button/key event.
 events();
}
QPointF NativeRegionOverlay::logicalPoint(QPoint native)const {
 const CaptureFrame* chosen=&frames_.first();qreal nearest=std::numeric_limits<qreal>::max();
 for(const auto& frame:frames_){
  if(frame.nativeRect.contains(native)){chosen=&frame;break;}
  const auto r=frame.nativeRect;const qreal dx=std::max({qreal(r.left()-native.x()),qreal(native.x()-r.right()),qreal(0)}),dy=std::max({qreal(r.top()-native.y()),qreal(native.y()-r.bottom()),qreal(0)});
  const qreal distance=dx*dx+dy*dy;if(distance<nearest){nearest=distance;chosen=&frame;}
 }
 return chosen->logicalRect.topLeft()+QPointF(qreal(native.x()-chosen->nativeRect.x())*chosen->logicalRect.width()/chosen->nativeRect.width(),qreal(native.y()-chosen->nativeRect.y())*chosen->logicalRect.height()/chosen->nativeRect.height());
}
void NativeRegionOverlay::events(){
 QPointer<NativeRegionOverlay> guard(this);
 while(connection_&&!finished_){
  auto* raw=xcb_poll_for_event(connection_);if(!raw)break;
  const auto type=raw->response_type&127;
  if(type==XCB_BUTTON_PRESS){const auto event=*reinterpret_cast<xcb_button_press_event_t*>(raw);std::free(raw);if(event.detail==3){cancel();return;}if(event.detail==1){anchor_=logicalPoint({event.root_x,event.root_y});selection_=QRectF(anchor_,QSizeF{});dragging_=true;}}
  else if(type==XCB_MOTION_NOTIFY){const auto event=*reinterpret_cast<xcb_motion_notify_event_t*>(raw);std::free(raw);move(logicalPoint({event.root_x,event.root_y}));}
  else if(type==XCB_BUTTON_RELEASE){const auto event=*reinterpret_cast<xcb_button_release_event_t*>(raw);std::free(raw);if(event.detail==1){release(logicalPoint({event.root_x,event.root_y}));if(!guard||finished_)return;}}
  else if(type==XCB_KEY_PRESS){const auto event=*reinterpret_cast<xcb_key_press_event_t*>(raw);std::free(raw);if(xcb_key_symbols_get_keysym(keys_,event.detail,0)==XK_Escape){cancel();return;}}
  else if(type==XCB_EXPOSE){const auto event=*reinterpret_cast<xcb_expose_event_t*>(raw);std::free(raw);for(auto& surface:surfaces_)if(surface.window==event.window){draw(surface);break;}}
  else {std::free(raw);if(type==0){cancel(unavailable());return;}}
  if(!guard)return;
 }
 if(connection_&&!finished_){if(xcb_connection_has_error(connection_)){cancel(unavailable());return;}xcb_flush(connection_);}
}
void NativeRegionOverlay::draw(Surface& surface){
 if(!surface.outline.isEmpty()){
  const auto outer=surface.outline.adjusted(-surface.pen,-surface.pen,surface.pen,surface.pen);
  const auto inner=surface.outline.adjusted(surface.pen+1,surface.pen+1,-surface.pen-1,-surface.pen-1);
  const QRegion oldOutline=QRegion(outer).subtracted(QRegion(inner));
  for(const auto& strip:oldOutline){const auto bounded=strip.intersected(QRect(QPoint{},surface.frame.nativeRect.size()));if(!bounded.isEmpty())xcb_clear_area(connection_,false,surface.window,int16_t(bounded.x()),int16_t(bounded.y()),uint16_t(bounded.width()),uint16_t(bounded.height()));}
 }
 const auto clipped=selection_.intersected(surface.frame.logicalRect);
 if(clipped.isEmpty()){surface.outline={};return;}
 const auto local=clipped.translated(-surface.frame.logicalRect.topLeft());const qreal sx=qreal(surface.frame.nativeRect.width())/surface.frame.logicalRect.width(),sy=qreal(surface.frame.nativeRect.height())/surface.frame.logicalRect.height();
 surface.outline=QRectF(local.x()*sx,local.y()*sy,local.width()*sx,local.height()*sy).toAlignedRect().intersected(QRect(QPoint{},surface.frame.nativeRect.size()));
 const uint32_t pen=uint32_t(surface.pen);xcb_change_gc(connection_,gc_,XCB_GC_LINE_WIDTH,&pen);
 // Keep the stroke inside the requested region, including HiDPI pen rounding.
 const int inset=surface.pen/2;const auto box=surface.outline.adjusted(inset,inset,-(surface.pen-inset),-(surface.pen-inset));
 if(box.width()<=0||box.height()<=0)return;
 const xcb_rectangle_t rectangle{int16_t(box.x()),int16_t(box.y()),uint16_t(box.width()),uint16_t(box.height())};xcb_poly_rectangle(connection_,surface.window,gc_,1,&rectangle);
}
void NativeRegionOverlay::move(QPointF point){if(!dragging_||finished_)return;selection_=QRectF(anchor_,point).normalized();for(auto& surface:surfaces_)draw(surface);xcb_flush(connection_);}
void NativeRegionOverlay::release(QPointF point){if(!dragging_||finished_)return;move(point);if(selection_.width()<1||selection_.height()<1){cancel();return;}const auto result=selection_;QPointer<NativeRegionOverlay> guard(this);finish();if(guard)emit selected(result);}
void NativeRegionOverlay::finish(){
 if(finished_)return;
 finished_=true;
 if(notifier_){notifier_->setEnabled(false);delete notifier_;notifier_=nullptr;}
 // Destroy the Qt-owned child on Qt's connection before its native parent.
 // A native fence flushes destruction without running application callbacks.
 if(cursorWindow_){cursorWindow_->hide();delete cursorWindow_;cursorWindow_=nullptr;}
 delete foreignParent_;foreignParent_=nullptr;
 if(qtConnection_){auto* fence=xcb_get_input_focus_reply(qtConnection_,xcb_get_input_focus(qtConnection_),nullptr);std::free(fence);qtConnection_=nullptr;}
 if(connection_){
  if(pointerGrabbed_)xcb_ungrab_pointer(connection_,XCB_CURRENT_TIME);
  if(keyboardGrabbed_)xcb_ungrab_keyboard(connection_,XCB_CURRENT_TIME);
  for(const auto& surface:surfaces_){xcb_unmap_window(connection_,surface.window);xcb_destroy_window(connection_,surface.window);}
  surfaces_.clear();
  if(gc_)xcb_free_gc(connection_,gc_);
  if(colormap_)xcb_free_colormap(connection_,colormap_);
  // Wait until all native surfaces/grabs are gone before reporting a result.
  auto* fence=xcb_get_input_focus_reply(connection_,xcb_get_input_focus(connection_),nullptr);std::free(fence);
  if(keys_)xcb_key_symbols_free(keys_);
  xcb_disconnect(connection_);connection_=nullptr;keys_=nullptr;
 }
}
void NativeRegionOverlay::cancel(Error error){if(finished_)return;QPointer<NativeRegionOverlay> guard(this);finish();if(guard)emit cancelled(error);}
}
