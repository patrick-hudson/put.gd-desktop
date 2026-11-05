#include "X11NativeSnapshot.h"
#include "X11Reply.h"
#include "X11WindowOwner.h"
#include "platform/NativeSession.h"
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QStringList>
#include <QRect>
#include <QCoreApplication>
#include <xcb/randr.h>
#include <cstring>
#include <algorithm>
namespace putgd {
namespace {
Error targetError(){return {"capture_target","Focused external window is unavailable or changed",{}};}
Error changedError(){return {"display_changed","Display layout or scale changed during source acquisition",{}};}
QJsonArray rectangle(const QRect& r){return {r.x(),r.y(),r.width(),r.height()};}
QRect rectangle(const QJsonArray& a){return {a[0].toInt(),a[1].toInt(),a[2].toInt(),a[3].toInt()};}
bool integers(const QJsonArray& a){if(a.size()!=4)return false;for(const auto& v:a)if(!v.isDouble()||v.toDouble()!=v.toInteger()||v.toInteger()<-32768||v.toInteger()>131070)return false;return a[2].toInteger()>0&&a[3].toInteger()>0;}
}
X11NativeSnapshot::X11NativeSnapshot(){
 if(!nativeX11SessionEnvironment())return;
 connection_=xcb_connect(nullptr,nullptr);if(!connection_||xcb_connection_has_error(connection_))return;
 root_=xcb_setup_roots_iterator(xcb_get_setup(connection_)).data->root;
}
X11NativeSnapshot::~X11NativeSnapshot(){if(connection_)xcb_disconnect(connection_);}
bool X11NativeSnapshot::available()const{return connection_&&!xcb_connection_has_error(connection_)&&root_;}
xcb_atom_t X11NativeSnapshot::atom(const char* name){return x11::internAtom(connection_,name);}
QByteArray X11NativeSnapshot::property(xcb_window_t window,const char* name,Error& error){
 auto value=x11::reply(xcb_get_property_reply(connection_,xcb_get_property(connection_,0,window,atom(name),XCB_GET_PROPERTY_TYPE_ANY,0,262145),nullptr));
 if(!value||value->bytes_after||xcb_get_property_value_length(value.get())>1024*1024){error={"capture_geometry","Cannot freeze bounded X11 scaling metadata",{}};return {};}
 QByteArray bytes;bytes.append(QByteArray::number(value->type));bytes+=':';bytes+=QByteArray::number(value->format);bytes+=':';bytes.append(static_cast<const char*>(xcb_get_property_value(value.get())),xcb_get_property_value_length(value.get()));return bytes;
}
QString X11NativeSnapshot::scaleFingerprint(Error& error){
 QCryptographicHash hash(QCryptographicHash::Sha256);hash.addData(property(root_,"RESOURCE_MANAGER",error));if(error)return {};
 const auto selection=atom("_XSETTINGS_S0");auto owner=x11::reply(xcb_get_selection_owner_reply(connection_,xcb_get_selection_owner(connection_,selection),nullptr));if(!owner){error=changedError();return {};}
 hash.addData(QByteArray::number(owner->owner));if(owner->owner)hash.addData(property(owner->owner,"_XSETTINGS_SETTINGS",error));if(error)return {};
 for(const auto* name:{"DISPLAY","QT_SCALE_FACTOR","QT_SCREEN_SCALE_FACTORS","QT_AUTO_SCREEN_SCALE_FACTOR","QT_ENABLE_HIGHDPI_SCALING","QT_FONT_DPI","QT_USE_PHYSICAL_DPI","QT_SCALE_FACTOR_ROUNDING_POLICY","XCURSOR_THEME","XCURSOR_SIZE","XCURSOR_PATH","XCURSOR_RESIZED","XCURSOR_DISCOVER","XCURSOR_CORE"}){hash.addData(name);hash.addData(QByteArray(1,'\0'));if(std::strncmp(name,"XCURSOR_",8)==0)hash.addData(QByteArray(1,qEnvironmentVariableIsSet(name)?'1':'0'));hash.addData(qgetenv(name));hash.addData(QByteArray(1,'\0'));}
 return QString::fromLatin1(hash.result().toHex());
}
QJsonObject X11NativeSnapshot::layout(Error& error){
 if(!available()){error={"capture_unavailable","A native X11 session is unavailable",{}};return {};}
 const auto* extension=xcb_get_extension_data(connection_,&xcb_randr_id);if(!extension||!extension->present){error={"capture_unavailable","RandR monitor discovery is unavailable",{}};return {};}
 auto resources=x11::reply(xcb_randr_get_screen_resources_current_reply(connection_,xcb_randr_get_screen_resources_current(connection_,root_),nullptr));if(!resources){error=changedError();return {};}
 QList<QJsonObject> outputs;const auto* ids=xcb_randr_get_screen_resources_current_outputs(resources.get());
 for(int i=0;i<xcb_randr_get_screen_resources_current_outputs_length(resources.get());++i){
  auto output=x11::reply(xcb_randr_get_output_info_reply(connection_,xcb_randr_get_output_info(connection_,ids[i],resources->config_timestamp),nullptr));if(!output){error=changedError();return {};}
  if(output->connection!=XCB_RANDR_CONNECTION_CONNECTED||!output->crtc)continue;
  auto crtc=x11::reply(xcb_randr_get_crtc_info_reply(connection_,xcb_randr_get_crtc_info(connection_,output->crtc,resources->config_timestamp),nullptr));if(!crtc||!crtc->width||!crtc->height){error=changedError();return {};}
  const QString name=QString::fromUtf8(reinterpret_cast<const char*>(xcb_randr_get_output_info_name(output.get())),xcb_randr_get_output_info_name_length(output.get()));
  outputs.append({{"identity",name},{"native",rectangle(QRect(crtc->x,crtc->y,crtc->width,crtc->height))},{"rotation",int(crtc->rotation)}});
 }
 if(outputs.isEmpty()||outputs.size()>64){error={"capture_geometry","Enabled monitor count is unsupported",{}};return {};}
 std::sort(outputs.begin(),outputs.end(),[](const auto& a,const auto& b){return a["identity"].toString()<b["identity"].toString();});QJsonArray frames;for(const auto& o:outputs)frames.append(o);
 auto rootGeometry=x11::reply(xcb_get_geometry_reply(connection_,xcb_get_geometry(connection_,root_),nullptr));if(!rootGeometry){error=changedError();return {};}
 const auto scale=scaleFingerprint(error);if(error)return {};
 return {{"outputs",frames},{"configuration",QString::number(resources->config_timestamp)},{"root",rectangle(QRect(0,0,rootGeometry->width,rootGeometry->height))},{"scale",scale}};
}
QRect X11NativeSnapshot::windowRect(xcb_window_t window,Error& error){
 auto attributes=x11::reply(xcb_get_window_attributes_reply(connection_,xcb_get_window_attributes(connection_,window),nullptr));auto geometry=x11::reply(xcb_get_geometry_reply(connection_,xcb_get_geometry(connection_,window),nullptr));auto coordinates=x11::reply(xcb_translate_coordinates_reply(connection_,xcb_translate_coordinates(connection_,window,root_,0,0),nullptr));
 if(!attributes||attributes->map_state!=XCB_MAP_STATE_VIEWABLE||!geometry||!coordinates){error=targetError();return {};}
 uint32_t sides[4]={};auto extents=x11::reply(xcb_get_property_reply(connection_,xcb_get_property(connection_,0,window,atom("_NET_FRAME_EXTENTS"),XCB_ATOM_CARDINAL,0,4),nullptr));if(extents&&extents->format==32&&xcb_get_property_value_length(extents.get())==16)std::memcpy(sides,xcb_get_property_value(extents.get()),16);for(auto v:sides)if(v>32767){error=targetError();return {};}
 return QRect(coordinates->dst_x-int(sides[0]),coordinates->dst_y-int(sides[2]),geometry->width+int(sides[0]+sides[1]),geometry->height+int(sides[2]+sides[3]));
}
QJsonObject X11NativeSnapshot::freeze(const QString& action,const QSet<qint64>& excluded,Error& error){
 if(action!="region"&&action!="window"&&action!="desktop"){error={"invalid_action","Unsupported native capture action",{}};return {};}
 const auto before=layout(error);if(error)return {};
 xcb_window_t window=0;QRect geometry;
 if(action=="window"){
  auto active=x11::reply(xcb_get_property_reply(connection_,xcb_get_property(connection_,0,root_,atom("_NET_ACTIVE_WINDOW"),XCB_ATOM_WINDOW,0,1),nullptr));if(!active||active->format!=32||xcb_get_property_value_length(active.get())!=4){error=targetError();return {};}
  std::memcpy(&window,xcb_get_property_value(active.get()),4);if(!window||window==root_){error=targetError();return {};}
  auto owner=x11::reply(xcb_get_property_reply(connection_,xcb_get_property(connection_,0,window,atom("_NET_WM_PID"),XCB_ATOM_CARDINAL,0,1),nullptr));uint32_t pid=0;if(owner&&owner->format==32&&xcb_get_property_value_length(owner.get())==4)std::memcpy(&pid,xcb_get_property_value(owner.get()),4);
  if(pid&&(excluded.contains(pid)||x11::startedByResident(pid))){error={"capture_target","Focus an external window before invoking window capture",{}};return {};}
  geometry=windowRect(window,error);if(error)return {};
 }
 if(layout(error)!=before){if(!error)error=changedError();return {};}
 return {{"schemaVersion",1},{"action",action},{"window",QString::number(window)},{"windowRect",window?rectangle(geometry):QJsonArray{}},{"layout",before}};
}
Error X11NativeSnapshot::validate(const QJsonObject& snapshot){
 if(snapshot.size()!=5||snapshot["schemaVersion"]!=1||!QStringList{"region","window","desktop"}.contains(snapshot["action"].toString())||!snapshot["window"].isString()||!snapshot["windowRect"].isArray()||!snapshot["layout"].isObject())return {"capture_protocol","Invalid frozen capture descriptor",{}};
 bool ok=false;const auto value=snapshot["window"].toString();const auto window=value.toULongLong(&ok);if(!ok||window>UINT32_MAX||QString::number(window)!=value||(snapshot["action"]=="window")!=(window!=0)||(window?!integers(snapshot["windowRect"].toArray()):!snapshot["windowRect"].toArray().isEmpty()))return {"capture_protocol","Invalid frozen window identity",{}};
 const auto l=snapshot["layout"].toObject();const auto frames=l["outputs"].toArray();if(l.size()!=4||!l["outputs"].isArray()||frames.isEmpty()||frames.size()>64||!integers(l["root"].toArray())||l["scale"].toString().size()!=64||!l["configuration"].isString())return {"capture_protocol","Invalid frozen display descriptor",{}};
 bool numeric=false;const auto config=l["configuration"].toString();const auto timestamp=config.toULongLong(&numeric);if(!numeric||timestamp>UINT32_MAX||QString::number(timestamp)!=config)return {"capture_protocol","Invalid display configuration identity",{}};
 for(const auto c:l["scale"].toString())if(!((c>='0'&&c<='9')||(c>='a'&&c<='f')))return {"capture_protocol","Invalid scaling fingerprint",{}};
 QSet<QString> names;for(const auto& v:frames){const auto f=v.toObject();const auto n=f["identity"].toString();if(f.size()!=3||n.isEmpty()||n.size()>1024||names.contains(n)||!integers(f["native"].toArray())||!f["rotation"].isDouble()||f["rotation"].toDouble()!=f["rotation"].toInteger()||f["rotation"].toInteger()<1||f["rotation"].toInteger()>63)return {"capture_protocol","Invalid frozen monitor identity",{}};names.insert(n);}
 return {};
}
bool X11NativeSnapshot::unchanged(const QJsonObject& snapshot,Error& error){
 error=validate(snapshot);if(error)return false;
 if(layout(error)!=snapshot["layout"].toObject()){if(!error)error=changedError();return false;}
 const auto window=xcb_window_t(snapshot["window"].toString().toUInt());if(window&&windowRect(window,error)!=rectangle(snapshot["windowRect"].toArray())){if(!error)error=targetError();return false;}return !error;
}
}
