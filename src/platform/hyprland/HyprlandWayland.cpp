#include "HyprlandWayland.h"
#include <wayland-client.h>
#include <poll.h>
#include <cerrno>

namespace putgd {
WaylandRoundTrips::WaylandRoundTrips(int timeoutMs):display_(wl_display_connect(nullptr)),deadline_(timeoutMs){}
WaylandRoundTrips::~WaylandRoundTrips(){if(display_)wl_display_disconnect(display_);}
bool WaylandRoundTrips::roundTrip(){
 if(!display_)return false;
 bool done=false;
 auto* callback=wl_display_sync(display_);
 static const wl_callback_listener listener{[](void* data,wl_callback*,uint32_t){*static_cast<bool*>(data)=true;}};
 wl_callback_add_listener(callback,&listener,&done);
 const auto fail=[&]{wl_callback_destroy(callback);return false;};
 while(!done){
  if(wl_display_dispatch_pending(display_)<0)return fail();
  if(done)break;
  while(wl_display_prepare_read(display_)!=0)if(wl_display_dispatch_pending(display_)<0)return fail();
  if(wl_display_flush(display_)<0&&errno!=EAGAIN){wl_display_cancel_read(display_);return fail();}
  const auto remaining=deadline_.remainingTime();
  if(remaining==0){wl_display_cancel_read(display_);return fail();}
  pollfd ready{wl_display_get_fd(display_),POLLIN,0};
  const int count=::poll(&ready,1,remaining<0?-1:int(remaining));
  if(count<0&&errno==EINTR){wl_display_cancel_read(display_);continue;}
  if(count<=0){wl_display_cancel_read(display_);return fail();}
  if(wl_display_read_events(display_)<0)return fail();
 }
 // The listener already ran; the callback object is spent.
 wl_callback_destroy(callback);
 return true;
}
}
