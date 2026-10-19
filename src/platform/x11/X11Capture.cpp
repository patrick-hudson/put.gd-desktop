#include "X11Capture.h"
#include "X11Reply.h"
#include "X11WindowOwner.h"
#include <QGuiApplication>
#include <QScreen>
#include <QWindow>
#include <QPixmap>
#include <xcb/randr.h>
#include <cstring>
namespace putgd {
namespace {using x11::reply;Error targetError(){return {"capture_target","Focused external window is unavailable or changed",{}};}}
X11Capture::X11Capture(QObject* parent):QObject(parent) {
 if(QGuiApplication::platformName()!="xcb")return;
 connection_=xcb_connect(nullptr,nullptr);if(xcb_connection_has_error(connection_))return;
 root_=xcb_setup_roots_iterator(xcb_get_setup(connection_)).data->root;
}
X11Capture::~X11Capture(){if(connection_)xcb_disconnect(connection_);}
bool X11Capture::available()const{return connection_&&!xcb_connection_has_error(connection_)&&root_!=XCB_WINDOW_NONE;}
bool X11Capture::cinnamonCompositor(){
 if(!available())return false;
 auto owner=reply(xcb_get_selection_owner_reply(connection_,xcb_get_selection_owner(connection_,atom("_NET_WM_CM_S0")),nullptr));
 if(!owner||owner->owner==XCB_WINDOW_NONE)return false;
 // Muffin's compositor selection window has no WM_CLASS. Its EWMH leader
 // carries the native identity and points back to itself.
 const auto supporting=atom("_NET_SUPPORTING_WM_CHECK");
 auto leader=reply(xcb_get_property_reply(connection_,xcb_get_property(connection_,0,root_,supporting,XCB_ATOM_WINDOW,0,1),nullptr));
 if(!leader||leader->format!=32||xcb_get_property_value_length(leader.get())!=4)return false;
 xcb_window_t window=XCB_WINDOW_NONE;memcpy(&window,xcb_get_property_value(leader.get()),4);if(!window)return false;
 auto check=reply(xcb_get_property_reply(connection_,xcb_get_property(connection_,0,window,supporting,XCB_ATOM_WINDOW,0,1),nullptr));
 if(!check||check->format!=32||xcb_get_property_value_length(check.get())!=4||memcmp(xcb_get_property_value(check.get()),&window,4)!=0)return false;
 auto version=reply(xcb_get_property_reply(connection_,xcb_get_property(connection_,0,window,atom("_MUFFIN_VERSION"),atom("UTF8_STRING"),0,64),nullptr));
 return version&&version->format==8&&xcb_get_property_value_length(version.get())>0;
}
xcb_atom_t X11Capture::atom(const char* name){return x11::internAtom(connection_,name);}
QList<CaptureFrame> X11Capture::layout(Error& error) {
 QList<CaptureFrame> result;
 if(!available()){error={"capture_unavailable","X11 capture requires the Qt xcb session backend",{}};return result;}
 const auto* extension=xcb_get_extension_data(connection_,&xcb_randr_id);if(!extension||!extension->present){error={"capture_unavailable","RandR monitor discovery is unavailable",{}};return {};}
 auto resources=reply(xcb_randr_get_screen_resources_current_reply(connection_,xcb_randr_get_screen_resources_current(connection_,root_),nullptr));
 if(!resources){error={"capture_geometry","Cannot discover enabled X11 outputs",{}};return {};}
 auto outputs=xcb_randr_get_screen_resources_current_outputs(resources.get());
 for(int i=0;i<xcb_randr_get_screen_resources_current_outputs_length(resources.get());++i) {
  auto output=reply(xcb_randr_get_output_info_reply(connection_,xcb_randr_get_output_info(connection_,outputs[i],resources->config_timestamp),nullptr));
  if(!output||output->connection!=XCB_RANDR_CONNECTION_CONNECTED||output->crtc==XCB_NONE)continue;
  QString name=QString::fromUtf8(reinterpret_cast<const char*>(xcb_randr_get_output_info_name(output.get())),xcb_randr_get_output_info_name_length(output.get()));
  auto crtc=reply(xcb_randr_get_crtc_info_reply(connection_,xcb_randr_get_crtc_info(connection_,output->crtc,resources->config_timestamp),nullptr));if(!crtc||!crtc->width||!crtc->height)continue;
  QScreen* screen=nullptr;for(auto* s:QGuiApplication::screens())if(s->name()==name){screen=s;break;}
  if(!screen){error={"capture_geometry","Qt and RandR monitor identities disagree",{}};return {};}
  int rotation=0;if(crtc->rotation&XCB_RANDR_ROTATION_ROTATE_90)rotation=90;else if(crtc->rotation&XCB_RANDR_ROTATION_ROTATE_180)rotation=180;else if(crtc->rotation&XCB_RANDR_ROTATION_ROTATE_270)rotation=270;
  const QRect native(crtc->x,crtc->y,crtc->width,crtc->height);const QRectF logical(screen->geometry());
  const qreal sx=native.width()/logical.width(),sy=native.height()/logical.height();
  if(qAbs(sx-sy)>0.02){error={"capture_geometry","Unsupported inconsistent monitor coordinate scaling",{}};return {};}
  result.append({name,logical,native,sx,rotation,{}});
 }
 if(result.isEmpty())error={"capture_geometry","No enabled monitors",{}};
 return result;
}
QRect X11Capture::windowRect(xcb_window_t window,Error& error) {
 auto attributes=reply(xcb_get_window_attributes_reply(connection_,xcb_get_window_attributes(connection_,window),nullptr));
 auto geometry=reply(xcb_get_geometry_reply(connection_,xcb_get_geometry(connection_,window),nullptr));
 auto coordinates=reply(xcb_translate_coordinates_reply(connection_,xcb_translate_coordinates(connection_,window,root_,0,0),nullptr));
 if(!attributes||attributes->map_state!=XCB_MAP_STATE_VIEWABLE||!geometry||!coordinates){error=targetError();return {};}
 uint32_t sides[4]={};auto extents=reply(xcb_get_property_reply(connection_,xcb_get_property(connection_,0,window,atom("_NET_FRAME_EXTENTS"),XCB_ATOM_CARDINAL,0,4),nullptr));
 if(extents&&extents->format==32&&xcb_get_property_value_length(extents.get())==16)memcpy(sides,xcb_get_property_value(extents.get()),16);
 for(auto v:sides)if(v>32767){error=targetError();return {};}
 return QRect(coordinates->dst_x-int(sides[0]),coordinates->dst_y-int(sides[2]),geometry->width+int(sides[0]+sides[1]),geometry->height+int(sides[2]+sides[3]));
}
X11Target X11Capture::freeze(const QString& action,Error& error) {
 X11Target target;target.action=action;target.layout=layout(error);if(error)return target;
 for(const auto& frame:target.layout)target.logicalSelection=target.logicalSelection.united(frame.logicalRect);
 if(action=="window") {
  auto active=reply(xcb_get_property_reply(connection_,xcb_get_property(connection_,0,root_,atom("_NET_ACTIVE_WINDOW"),XCB_ATOM_WINDOW,0,1),nullptr));
  if(!active||active->format!=32||xcb_get_property_value_length(active.get())!=4){error=targetError();return target;}
  memcpy(&target.window,xcb_get_property_value(active.get()),4);if(!target.window||target.window==root_){error=targetError();return target;}
  for(auto* window:QGuiApplication::allWindows())if(window->winId()==target.window){error={"capture_target","Focus an external window before invoking window capture",{}};return target;}
  const auto pid=reply(xcb_get_property_reply(connection_,xcb_get_property(connection_,0,target.window,atom("_NET_WM_PID"),XCB_ATOM_CARDINAL,0,1),nullptr));
  uint32_t owner=0;if(pid&&pid->format==32&&xcb_get_property_value_length(pid.get())==4)memcpy(&owner,xcb_get_property_value(pid.get()),4);
  if(owner&&((uiProcess_&&qint64(owner)==uiProcess_)||x11::startedByResident(owner))){error={"capture_target","Focus an external window before invoking window capture",{}};return target;}
  target.nativeWindowRect=windowRect(target.window,error);if(error)return target;
  target.logicalSelection={};
  for(const auto& f:target.layout){const QRect intersection=f.nativeRect.intersected(target.nativeWindowRect);if(intersection.isEmpty())continue;
   const QRectF part(f.logicalRect.x()+(intersection.x()-f.nativeRect.x())/f.density,f.logicalRect.y()+(intersection.y()-f.nativeRect.y())/f.density,intersection.width()/f.density,intersection.height()/f.density);target.logicalSelection=target.logicalSelection.united(part);
  }
  if(target.logicalSelection.isEmpty())error=targetError();
 }
 return target;
}
bool X11Capture::unchanged(const X11Target& target,Error& error) {
 auto current=layout(error);if(error)return false;
 if(current.size()!=target.layout.size()){error={"display_changed","Display layout changed during source acquisition",{}};return false;}
 for(int i=0;i<current.size();++i){const auto& a=current[i];const auto& b=target.layout[i];if(a.identity!=b.identity||a.nativeRect!=b.nativeRect||a.logicalRect!=b.logicalRect||a.density!=b.density||a.rotation!=b.rotation){error={"display_changed","Display layout changed during source acquisition",{}};return false;}}
 if(target.window&&windowRect(target.window,error)!=target.nativeWindowRect){if(!error)error=targetError();return false;}
 return !error;
}
QList<CaptureFrame> X11Capture::frames(const X11Target& target,Error& error) {
 if(!unchanged(target,error))return {};
 QList<CaptureFrame> frames=target.layout;
 for(auto& frame:frames){QScreen* screen=nullptr;for(auto* s:QGuiApplication::screens())if(s->name()==frame.identity){screen=s;break;}if(!screen){error={"display_changed","Monitor disappeared before capture",{}};return {};}
  // Qt's X11 root-window capture returns visible pixels including panels and
  // occlusion and excludes the cursor. Cinnamon region callers use the native
  // compositor scene after selection; generic X11 callers remove their surfaces.
  frame.pixels=screen->grabWindow(0).toImage();frame.pixels.setDevicePixelRatio(1);
  if(frame.pixels.isNull()||frame.pixels.size()!=frame.nativeRect.size()){error={"capture_geometry","Native frame dimensions disagree with monitor geometry",{}};return {};}
 }
 if(!unchanged(target,error))return {};
 if(target.window){
  QList<CaptureFrame> cropped;
  for(auto frame:frames){
   const auto intersection=frame.nativeRect.intersected(target.nativeWindowRect);
   if(intersection.isEmpty())continue;
   frame.pixels=frame.pixels.copy(intersection.translated(-frame.nativeRect.topLeft()));
   frame.logicalRect=QRectF(frame.logicalRect.x()+(intersection.x()-frame.nativeRect.x())/frame.density,frame.logicalRect.y()+(intersection.y()-frame.nativeRect.y())/frame.density,intersection.width()/frame.density,intersection.height()/frame.density);
   frame.nativeRect=intersection;cropped.append(std::move(frame));
  }
  return cropped;
 }
 return frames;
}
}
