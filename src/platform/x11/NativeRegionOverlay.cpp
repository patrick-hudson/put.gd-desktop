#include "NativeRegionOverlay.h"
#include "ui/RegionSelection.h"
#include <QAccessibleObject>
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
class NativeRegionAccessible final:public QAccessibleObject {
public:
 explicit NativeRegionAccessible(NativeRegionOverlay* overlay):QAccessibleObject(overlay){}
 QWindow* window()const override{return static_cast<NativeRegionOverlay*>(object())->cursorWindow_;}
 QAccessibleInterface* parent()const override{return QAccessible::queryAccessibleInterface(qGuiApp);}
 QAccessibleInterface* child(int)const override{return nullptr;}
 int childCount()const override{return 0;}
 int indexOfChild(const QAccessibleInterface*)const override{return -1;}
 QString text(QAccessible::Text type)const override{
  const auto* overlay=static_cast<NativeRegionOverlay*>(object());
  if(type==QAccessible::Name)return QStringLiteral("Capture region");
  if(type==QAccessible::Description)return regionDescription(overlay->selection_,false,QStringLiteral("Display %1").arg(std::max(0,overlay->keyboardScreen_)+1),overlay->frames_[std::max(0,overlay->keyboardScreen_)].logicalRect,overlay->frames_,overlay->dragging_,overlay->outcome_);
  return {};
 }
 QRect rect()const override{
  const auto* overlay=static_cast<NativeRegionOverlay*>(object());QRectF bounds;
  for(const auto& frame:overlay->frames_)bounds=bounds.united(frame.logicalRect);
  return (overlay->selection_.isEmpty()?bounds:overlay->selection_).toAlignedRect();
 }
 QAccessible::Role role()const override{return QAccessible::Pane;}
 QAccessible::State state()const override{
  const auto* overlay=static_cast<NativeRegionOverlay*>(object());QAccessible::State state;
  state.focusable=true;state.focused=overlay->keyboardGrabbed_&&!overlay->finished_;state.invisible=!overlay->started_||overlay->finished_;return state;
 }
};
namespace {
QAccessibleInterface* accessibleRegion(const QString& name,QObject* object){
 if(name==QLatin1String("putgd::NativeRegionOverlay"))return new NativeRegionAccessible(static_cast<NativeRegionOverlay*>(object));
 return nullptr;
}
Error unavailable(){return {"capture_unavailable","Cannot initialize region selection",{}};}
// The capture helper protocol carries this reason to the notification.
Error inputHeld(int waited,int attempts,bool pointer,bool keyboard){
 return {"capture_unavailable","Region selection could not start: an open menu or another app is holding the keyboard or mouse. Close it, then try again.",{{"stage","select"},{"check","input_grab"},{"waitedMs",waited},{"attempts",attempts},{"pointerHeld",pointer},{"keyboardHeld",keyboard}}};
}
bool held(uint8_t status){return status==XCB_GRAB_STATUS_ALREADY_GRABBED||status==XCB_GRAB_STATUS_FROZEN;}
bool nativeGeometry(const QRect& r){
 // Core input reports signed16 root coordinates; core drawing also uses
 // signed16 local origins. Both complete rectangles must be representable.
 return r.width()>0&&r.height()>0&&r.width()<=32768&&r.height()<=32768&&r.x()>=-32768&&r.y()>=-32768&&qint64(r.x())+r.width()-1<=32767&&qint64(r.y())+r.height()-1<=32767;
}
}
NativeRegionOverlay::NativeRegionOverlay(QList<CaptureFrame> frames,QObject* parent,xcb_cursor_t residentCursor,QString outcome):RegionSelector(parent),frames_(std::move(frames)),outcome_(std::move(outcome)),residentCursor_(residentCursor){static const bool installed=[] {QAccessible::installFactory(accessibleRegion);return true;}();Q_UNUSED(installed);}
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
 auto* pointer=xcb_query_pointer_reply(connection_,xcb_query_pointer(connection_,root_),nullptr);
 keyboardScreen_=pointer?screenAt({pointer->root_x,pointer->root_y}):0;std::free(pointer);
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
  prepareInstructions(surface);
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
 grabClock_.start();
 grabInput();
}
void NativeRegionOverlay::grabInput(){
 if(finished_)return;
 ++grabAttempts_;
 const auto input=xcb_window_t(cursorWindow_->winId());
 const auto pointerCookie=xcb_grab_pointer(connection_,false,input,XCB_EVENT_MASK_BUTTON_PRESS|XCB_EVENT_MASK_BUTTON_RELEASE|XCB_EVENT_MASK_POINTER_MOTION,XCB_GRAB_MODE_ASYNC,XCB_GRAB_MODE_ASYNC,XCB_WINDOW_NONE,residentCursor_,XCB_CURRENT_TIME);
 const auto keyboardCookie=xcb_grab_keyboard(connection_,false,input,XCB_CURRENT_TIME,XCB_GRAB_MODE_ASYNC,XCB_GRAB_MODE_ASYNC);
 xcb_flush(connection_);
 auto* pointer=xcb_grab_pointer_reply(connection_,pointerCookie,nullptr);
 const uint8_t pointerStatus=pointer?pointer->status:uint8_t(XCB_GRAB_STATUS_INVALID_TIME);std::free(pointer);
 auto* keyboard=xcb_grab_keyboard_reply(connection_,keyboardCookie,nullptr);
 const uint8_t keyboardStatus=keyboard?keyboard->status:uint8_t(XCB_GRAB_STATUS_INVALID_TIME);std::free(keyboard);
 pointerGrabbed_=pointerStatus==XCB_GRAB_STATUS_SUCCESS;keyboardGrabbed_=keyboardStatus==XCB_GRAB_STATUS_SUCCESS;
 if(!pointerGrabbed_||!keyboardGrabbed_){
  // An open menu holds both grabs. Release a partial grab so a click or Esc
  // still reaches the menu and closes it, then retry for a bounded time.
  if(pointerGrabbed_)xcb_ungrab_pointer(connection_,XCB_CURRENT_TIME);
  if(keyboardGrabbed_)xcb_ungrab_keyboard(connection_,XCB_CURRENT_TIME);
  pointerGrabbed_=keyboardGrabbed_=false;xcb_flush(connection_);
  const bool busy=(pointerStatus==XCB_GRAB_STATUS_SUCCESS||held(pointerStatus))&&(keyboardStatus==XCB_GRAB_STATUS_SUCCESS||held(keyboardStatus));
  if(!busy){cancel(unavailable());return;}
  const int waited=int(grabClock_.elapsed());
  if(waited>=grabWaitMs){cancel(inputHeld(waited,grabAttempts_,held(pointerStatus),held(keyboardStatus)));return;}
  QPointer<NativeRegionOverlay> guard(this);
  QTimer::singleShot(grabRetryMs,this,[guard]{if(guard)guard->grabInput();});
  return;
 }
 QAccessibleEvent focused(this,QAccessible::Focus);QAccessible::updateAccessibility(&focused);
 notifier_=new QSocketNotifier(xcb_get_file_descriptor(connection_),QSocketNotifier::Read,this);
 connect(notifier_,&QSocketNotifier::activated,this,[this]{events();});
 // Replies above may already have buffered events; do not depend on another
 // socket wakeup to consume an initial button/key event.
 events();
}
QPointF NativeRegionOverlay::logicalPoint(QPoint native)const {
 const auto& chosen=frames_[screenAt(native)];
 return chosen.logicalRect.topLeft()+QPointF(qreal(native.x()-chosen.nativeRect.x())*chosen.logicalRect.width()/chosen.nativeRect.width(),qreal(native.y()-chosen.nativeRect.y())*chosen.logicalRect.height()/chosen.nativeRect.height());
}

void NativeRegionOverlay::events(){
 QPointer<NativeRegionOverlay> guard(this);
 while(connection_&&!finished_){
  auto* raw=xcb_poll_for_event(connection_);if(!raw)break;
  const auto type=raw->response_type&127;
  if(type==XCB_BUTTON_PRESS){const auto event=*reinterpret_cast<xcb_button_press_event_t*>(raw);std::free(raw);if(event.detail==3){cancel();return;}if(event.detail==1){keyboardScreen_=-1;anchor_=logicalPoint({event.root_x,event.root_y});selection_=QRectF(anchor_,QSizeF{});dragging_=true;move(anchor_);}}
  else if(type==XCB_MOTION_NOTIFY){const auto event=*reinterpret_cast<xcb_motion_notify_event_t*>(raw);std::free(raw);move(logicalPoint({event.root_x,event.root_y}));}
  else if(type==XCB_BUTTON_RELEASE){const auto event=*reinterpret_cast<xcb_button_release_event_t*>(raw);std::free(raw);if(event.detail==1){release(logicalPoint({event.root_x,event.root_y}));if(!guard||finished_)return;}}
  else if(type==XCB_KEY_PRESS){const auto event=*reinterpret_cast<xcb_key_press_event_t*>(raw);std::free(raw);const auto symbol=xcb_key_symbols_get_keysym(keys_,event.detail,0);int qtKey=0;
   switch(symbol){case XK_Tab:case XK_ISO_Left_Tab:qtKey=Qt::Key_Tab;break;case XK_Escape:qtKey=Qt::Key_Escape;break;case XK_Return:case XK_KP_Enter:qtKey=Qt::Key_Return;break;
   case XK_Left:case XK_KP_Left:qtKey=Qt::Key_Left;break;case XK_Right:case XK_KP_Right:qtKey=Qt::Key_Right;break;
   case XK_Up:case XK_KP_Up:qtKey=Qt::Key_Up;break;case XK_Down:case XK_KP_Down:qtKey=Qt::Key_Down;break;default:break;}
   Qt::KeyboardModifiers modifiers;if(event.state&XCB_MOD_MASK_CONTROL)modifiers|=Qt::ControlModifier;if(event.state&XCB_MOD_MASK_SHIFT)modifiers|=Qt::ShiftModifier;
   key(qtKey,modifiers,{event.root_x,event.root_y});if(!guard||finished_)return;}
  else if(type==XCB_EXPOSE){const auto event=*reinterpret_cast<xcb_expose_event_t*>(raw);std::free(raw);for(auto& surface:surfaces_)if(surface.window==event.window){draw(surface);break;}}
  else {std::free(raw);if(type==0){cancel(unavailable());return;}}
  if(!guard)return;
 }
 if(connection_&&!finished_){if(xcb_connection_has_error(connection_)){cancel(unavailable());return;}xcb_flush(connection_);}
}
int NativeRegionOverlay::screenAt(QPoint native)const{
 int chosen=0;qreal nearest=std::numeric_limits<qreal>::max();
 for(int i=0;i<frames_.size();++i){
  const auto r=frames_[i].nativeRect;if(r.contains(native))return i;
  const qreal dx=std::max({qreal(r.left()-native.x()),qreal(native.x()-r.right()),qreal(0)}),dy=std::max({qreal(r.top()-native.y()),qreal(native.y()-r.bottom()),qreal(0)});
  const qreal distance=dx*dx+dy*dy;if(distance<nearest){nearest=distance;chosen=i;}
 }
 return chosen;
}

void NativeRegionOverlay::key(int key,Qt::KeyboardModifiers modifiers,QPoint native){
 int screen=keyboardScreen_<0?screenAt(native):keyboardScreen_;
 QList<QRectF> screens;for(const auto& frame:frames_)screens.append(frame.logicalRect);
 const bool switched=switchRegionDisplay(selection_,screens,screen,key,modifiers);
 switch(switched?RegionKeyAction::Change:regionKey(selection_,frames_[screen].logicalRect,key,modifiers)){
 case RegionKeyAction::Cancel:cancel();break;
 case RegionKeyAction::Accept:{const auto result=selection_;QPointer<NativeRegionOverlay> guard(this);finish();if(guard)emit selected(result);break;}
 case RegionKeyAction::Change:
  keyboardScreen_=screen;dragging_=false;describe();
  for(auto& surface:surfaces_){prepareInstructions(surface);draw(surface);}
  break;
 case RegionKeyAction::Ignore:break;
 }
}
void NativeRegionOverlay::describe(){QAccessibleEvent changed(this,QAccessible::DescriptionChanged);QAccessible::updateAccessibility(&changed);}
void NativeRegionOverlay::prepareInstructions(Surface& surface){
 const QRect bounds(QPoint{},surface.frame.logicalRect.size().toSize());
 const auto font=nativeRegionFont();
 const auto display=keyboardScreen_<0?surface.frame.identity:frames_[keyboardScreen_].identity;
 if(surface.frame.identity!=display){surface.instructions={};surface.instructionsKey.clear();return;}
 const auto it=std::find_if(frames_.cbegin(),frames_.cend(),[&](const auto& frame){return frame.identity==display;});
 const int index=it==frames_.cend()?0:int(std::distance(frames_.cbegin(),it));
 const auto instructions=regionInstructions(selection_,false,QStringLiteral("Display %1").arg(index+1),frames_[index].logicalRect,frames_,dragging_,outcome_);
 const auto local=selection_.translated(-surface.frame.logicalRect.topLeft());
 const auto pointer=dragging_?std::optional<QPointF>(pointer_-surface.frame.logicalRect.topLeft()):std::nullopt;
 const auto box=regionInstructionBox(bounds,QFontMetrics(font),instructions,local,pointer);
 const auto cacheKey=instructions+font.toString()+QString("/%1/%2").arg(box.x()).arg(box.y());
 if(surface.instructionsKey==cacheKey&&!surface.instructions.isNull())return;
 surface.instructionsKey=cacheKey;
 if(box.isEmpty())return;
 const qreal sx=qreal(surface.frame.nativeRect.width())/surface.frame.logicalRect.width(),sy=qreal(surface.frame.nativeRect.height())/surface.frame.logicalRect.height();
 const auto native=QRectF(box.x()*sx,box.y()*sy,box.width()*sx,box.height()*sy).toAlignedRect();
 // Only the compact instruction badge has a raster image, never the desktop.
 surface.instructions=QImage(native.size(),QImage::Format_ARGB32_Premultiplied);surface.instructions.fill(Qt::transparent);surface.instructionsAt=native.topLeft();
 QPainter painter(&surface.instructions);painter.scale(sx,sy);painter.translate(-box.topLeft());painter.setFont(font);
 QPalette palette;palette.setColor(QPalette::Window,Qt::black);palette.setColor(QPalette::WindowText,Qt::white);
 paintRegionInstructions(painter,bounds,palette,instructions,local,pointer);
}
void NativeRegionOverlay::draw(Surface& surface){
 // The compositor can consume damage bounds before sampling this drawable's
 // newer pixels. Tight old/new stroke damage then clips an advancing edge.
 // Damage the complete transient surface so every sampled outline fits the
 // repaint area, even when several pointer events arrive before presentation.
 // Core X clears allocate no client-side monitor-sized backing image.
 xcb_clear_area(connection_,false,surface.window,0,0,0,0);
 const auto clipped=selection_.isEmpty()&&surface.frame.identity==frames_[std::max(0,keyboardScreen_)].identity?surface.frame.logicalRect:selection_.intersected(surface.frame.logicalRect);
 const auto local=clipped.translated(-surface.frame.logicalRect.topLeft());const qreal sx=qreal(surface.frame.nativeRect.width())/surface.frame.logicalRect.width(),sy=qreal(surface.frame.nativeRect.height())/surface.frame.logicalRect.height();
 const QRect bounds(QPoint{},surface.frame.nativeRect.size());
 surface.outline=clipped.isEmpty()?QRect{}:QRectF(local.x()*sx,local.y()*sy,local.width()*sx,local.height()*sy).toAlignedRect().intersected(bounds);
 // Native ARGB strips dim only the outside; no desktop-sized backing image.
 const uint32_t scrim=uint32_t(regionScrimAlpha)<<24;
 xcb_change_gc(connection_,gc_,XCB_GC_FOREGROUND,&scrim);
 const auto dim=[&](QRect r){if(r.isEmpty())return;const xcb_rectangle_t strip{int16_t(r.x()),int16_t(r.y()),uint16_t(r.width()),uint16_t(r.height())};xcb_poly_fill_rectangle(connection_,surface.window,gc_,1,&strip);};
 if(surface.outline.isEmpty())dim(bounds);
 else {const auto r=surface.outline;dim(QRect(0,0,bounds.width(),r.top()));dim(QRect(0,r.bottom()+1,bounds.width(),bounds.height()-r.bottom()-1));dim(QRect(0,r.top(),r.left(),r.height()));dim(QRect(r.right()+1,r.top(),bounds.width()-r.right()-1,r.height()));}
 const auto& badge=surface.instructions;
 if(!badge.isNull()){
  // Bound each request even on servers without a large BIG-REQUESTS limit.
  const int rows=std::max(1,int((xcb_get_maximum_request_length(connection_)*4-64)/badge.bytesPerLine()));
  for(int y=0;y<badge.height();y+=rows){const int count=std::min(rows,badge.height()-y);
   xcb_put_image(connection_,XCB_IMAGE_FORMAT_Z_PIXMAP,surface.window,gc_,uint16_t(badge.width()),uint16_t(count),int16_t(surface.instructionsAt.x()),int16_t(surface.instructionsAt.y()+y),0,32,uint32_t(count*badge.bytesPerLine()),badge.constScanLine(y));
  }
 }
 if(surface.outline.isEmpty())return;
 // Integer filled strips stay within the selection even for one-pixel drags.
 auto ring=[&](QRect r,int width,uint32_t color){
  if(r.isEmpty())return;
  const int x=std::min(width,r.width()),y=std::min(width,r.height());
  const xcb_rectangle_t strips[]={
   {int16_t(r.x()),int16_t(r.y()),uint16_t(r.width()),uint16_t(y)},
   {int16_t(r.x()),int16_t(r.y()+r.height()-y),uint16_t(r.width()),uint16_t(y)},
   {int16_t(r.x()),int16_t(r.y()),uint16_t(x),uint16_t(r.height())},
   {int16_t(r.x()+r.width()-x),int16_t(r.y()),uint16_t(x),uint16_t(r.height())}};
  xcb_change_gc(connection_,gc_,XCB_GC_FOREGROUND,&color);
  xcb_poly_fill_rectangle(connection_,surface.window,gc_,4,strips);
 };
 ring(surface.outline,3*surface.pen,0xff000000);
 const int inset=surface.pen;
 ring(surface.outline.adjusted(inset,inset,-inset,-inset),surface.pen,0xffffffff);
}
void NativeRegionOverlay::move(QPointF point){
 pointer_=point;if(!dragging_||finished_)return;selection_=QRectF(anchor_,point).normalized();describe();for(auto& surface:surfaces_){prepareInstructions(surface);draw(surface);}xcb_flush(connection_);}
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
