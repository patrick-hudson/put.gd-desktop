#include "ToplevelMapping.h"
#include "HyprlandWayland.h"
#include "ext-foreign-toplevel-list-v1-client-protocol.h"
#include "hyprland-toplevel-mapping-v1-client-protocol.h"
#include <QPointer>
#include <QThread>
#include <wayland-client.h>
#include <cstring>
#include <memory>

namespace putgd {
namespace {
constexpr int maximumToplevels=512;
struct Handle {ext_foreign_toplevel_handle_v1* native=nullptr;QString identifier;};
struct State {
 ext_foreign_toplevel_list_v1* list=nullptr;hyprland_toplevel_mapping_manager_v1* mapping=nullptr;
 QList<Handle*> handles;QList<ext_foreign_toplevel_handle_v1*> unlisted;bool overflow=false;
 bool mapped=false,failed=false;quint64 address=0;
};
void global(void* data,wl_registry* registry,uint32_t name,const char* interface,uint32_t version){
 auto* state=static_cast<State*>(data);
 if(std::strcmp(interface,ext_foreign_toplevel_list_v1_interface.name)==0&&!state->list)
  state->list=static_cast<ext_foreign_toplevel_list_v1*>(wl_registry_bind(registry,name,&ext_foreign_toplevel_list_v1_interface,1));
 else if(std::strcmp(interface,hyprland_toplevel_mapping_manager_v1_interface.name)==0&&!state->mapping)
  state->mapping=static_cast<hyprland_toplevel_mapping_manager_v1*>(wl_registry_bind(registry,name,&hyprland_toplevel_mapping_manager_v1_interface,1));
 Q_UNUSED(version);
}
void removed(void*,wl_registry*,uint32_t){}
void handleClosed(void*,ext_foreign_toplevel_handle_v1*){}
void handleDone(void*,ext_foreign_toplevel_handle_v1*){}
void handleTitle(void*,ext_foreign_toplevel_handle_v1*,const char*){}
void handleAppId(void*,ext_foreign_toplevel_handle_v1*,const char*){}
void handleIdentifier(void* data,ext_foreign_toplevel_handle_v1*,const char* identifier){static_cast<Handle*>(data)->identifier=QString::fromUtf8(identifier,int(strnlen(identifier,256)));}
const ext_foreign_toplevel_handle_v1_listener handleListener{handleClosed,handleDone,handleTitle,handleAppId,handleIdentifier};
void ignoredIdentifier(void*,ext_foreign_toplevel_handle_v1*,const char*){}
const ext_foreign_toplevel_handle_v1_listener ignoredListener{handleClosed,handleDone,handleTitle,handleAppId,ignoredIdentifier};
void toplevel(void* data,ext_foreign_toplevel_list_v1*,ext_foreign_toplevel_handle_v1* native){
 auto* state=static_cast<State*>(data);
 // Past the cap a handle is kept, its events ignored, and destroyed with the rest.
 if(state->handles.size()>=maximumToplevels){state->overflow=true;state->unlisted.append(native);ext_foreign_toplevel_handle_v1_add_listener(native,&ignoredListener,nullptr);return;}
 auto* handle=new Handle;handle->native=native;state->handles.append(handle);
 ext_foreign_toplevel_handle_v1_add_listener(native,&handleListener,handle);
}
void finished(void*,ext_foreign_toplevel_list_v1*){}
const ext_foreign_toplevel_list_v1_listener listListener{toplevel,finished};
void windowAddress(void* data,hyprland_toplevel_window_mapping_handle_v1*,uint32_t high,uint32_t low){auto* state=static_cast<State*>(data);state->address=(quint64(high)<<32)|low;state->mapped=true;}
void mappingFailed(void* data,hyprland_toplevel_window_mapping_handle_v1*){static_cast<State*>(data)->failed=true;}
const hyprland_toplevel_window_mapping_handle_v1_listener mappingListener{windowAddress,mappingFailed};
}
ToplevelMapping::ToplevelMapping(QObject* parent):QObject(parent){}
ToplevelMapping::~ToplevelMapping(){for(auto* worker:workers_){worker->wait();delete worker;}}
bool ToplevelMapping::sameAddress(const QString& a,const QString& b){
 bool okA=false,okB=false;
 const auto left=QString(a).remove(QStringLiteral("0x"),Qt::CaseInsensitive).toULongLong(&okA,16);
 const auto right=QString(b).remove(QStringLiteral("0x"),Qt::CaseInsensitive).toULongLong(&okB,16);
 return okA&&okB&&left==right&&left!=0;
}
QString ToplevelMapping::resolve(const QString& identifier,Error& error,int timeoutMs){
 error={};
 WaylandRoundTrips wayland(timeoutMs);
 if(!wayland.display()){error={"capture_failed","Cannot connect to the Wayland compositor to confirm the window",{}};return {};}
 State state;
 auto* registry=wl_display_get_registry(wayland.display());
 static const wl_registry_listener registryListener{global,removed};
 wl_registry_add_listener(registry,&registryListener,&state);
 const auto cleanup=[&]{
  for(auto* handle:state.handles){ext_foreign_toplevel_handle_v1_destroy(handle->native);delete handle;}state.handles.clear();
  for(auto* native:std::as_const(state.unlisted))ext_foreign_toplevel_handle_v1_destroy(native);state.unlisted.clear();
  if(state.list)ext_foreign_toplevel_list_v1_destroy(state.list);
  if(state.mapping)hyprland_toplevel_mapping_manager_v1_destroy(state.mapping);
  wl_registry_destroy(registry);
 };
 // Globals, then every toplevel, then each toplevel's identifier. The list
 // sends its toplevels only after the bind, so its listener is in time here.
 const bool globals=wayland.roundTrip();
 if(globals&&state.list)ext_foreign_toplevel_list_v1_add_listener(state.list,&listListener,&state);
 if(!globals||(state.list&&(!wayland.roundTrip()||!wayland.roundTrip()))){cleanup();error={"capture_failed","The compositor did not answer while confirming the window",{}};return {};}
 if(!state.list||!state.mapping){
  cleanup();
  error={"capture_target",QStringLiteral("The compositor does not offer %1").arg(!state.list?"ext-foreign-toplevel-list-v1":"hyprland-toplevel-mapping-v1"),{}};return {};
 }
 Handle* match=nullptr;
 for(auto* handle:std::as_const(state.handles))if(handle->identifier==identifier){match=handle;break;}
 if(!match){
  cleanup();
  error={"capture_target","The active window is not in the compositor's window list",{{"identity",identifier},{"listed",int(state.handles.size())},{"truncated",state.overflow}}};return {};
 }
 auto* request=hyprland_toplevel_mapping_manager_v1_get_window_for_toplevel(state.mapping,match->native);
 hyprland_toplevel_window_mapping_handle_v1_add_listener(request,&mappingListener,&state);
 const bool answered=wayland.roundTrip();
 hyprland_toplevel_window_mapping_handle_v1_destroy(request);
 cleanup();
 if(!answered){error={"capture_failed","The compositor did not answer while confirming the window",{}};return {};}
 if(!state.mapped||state.failed){error={"capture_target","Hyprland could not map the active window's capture handle",{{"identity",identifier}}};return {};}
 return QStringLiteral("0x%1").arg(state.address,0,16);
}
void ToplevelMapping::addressFor(const QString& identifier,Reply reply){
 struct Result {QString address;Error error;};
 auto result=std::make_shared<Result>();
 auto* worker=QThread::create([identifier,result]{result->address=resolve(identifier,result->error);});
 workers_.append(worker);
 connect(worker,&QThread::finished,this,[this,worker,result,reply=std::move(reply)]{workers_.removeOne(worker);worker->deleteLater();reply(result->address,result->error);});
 worker->start();
}
}
