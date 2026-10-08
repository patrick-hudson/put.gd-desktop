#pragma once
#include <QLibrary>
#include <xcb/xcb.h>
#include <functional>
#include <utility>

// Test-executable-only interposition. The barrier lets another real X11 client
// finish a chord at the mapping refresh boundary, without timing sleeps or a
// production hook. All calls still go to the distro's libxcb.
class X11MappingBarrier {
public:
 X11MappingBarrier(){Q_ASSERT(!current_);current_=this;}
 ~X11MappingBarrier(){current_=nullptr;}
 X11MappingBarrier(const X11MappingBarrier&)=delete;
 X11MappingBarrier& operator=(const X11MappingBarrier&)=delete;
 void arm(std::function<void(xcb_connection_t*)> callback){callback_=std::move(callback);}
 static void beforeRequest(xcb_connection_t* connection){
  auto* barrier=current_;if(!barrier)return;
  if(!barrier->connection_)barrier->connection_=connection;
  if(barrier->connection_!=connection||!barrier->callback_)return;
  auto callback=std::exchange(barrier->callback_,{});callback(connection);
 }
private:
 inline static thread_local X11MappingBarrier* current_=nullptr;
 xcb_connection_t* connection_=nullptr;
 std::function<void(xcb_connection_t*)> callback_;
};

extern "C" xcb_get_modifier_mapping_cookie_t xcb_get_modifier_mapping(xcb_connection_t* connection){
 using Request=xcb_get_modifier_mapping_cookie_t(*)(xcb_connection_t*);
 static const auto request=reinterpret_cast<Request>(QLibrary::resolve("xcb",1,"xcb_get_modifier_mapping"));
 if(!request)qFatal("Cannot resolve the native XCB mapping request");
 X11MappingBarrier::beforeRequest(connection);
 return request(connection);
}
