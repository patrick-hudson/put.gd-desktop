#pragma once
#include <wayland-client.h>
#include <cerrno>
#include <poll.h>

namespace putgd::wayland {
// The caller owns the connection, dispatch lifetime and loss policy. A pending
// flush must arm POLLOUT (or a Qt write notifier) until a later flush drains it.
inline bool flush(wl_display* display,bool& pending){
 pending=false;
 if(wl_display_flush(display)>=0)return true;
 if(errno==EAGAIN){pending=true;return true;}
 return false;
}
// fds[0] is the display; additional descriptors and the timeout belong to the
// caller (the recorder has a wake fd). Always consume or cancel a prepared read,
// including timeout, EINTR, wake-only and socket-error paths.
inline bool pump(wl_display* display,pollfd* fds,nfds_t count,int timeout){
 if(wl_display_dispatch_pending(display)<0)return false;
 while(wl_display_prepare_read(display)!=0)
  if(wl_display_dispatch_pending(display)<0)return false;
 const int ready=::poll(fds,count,timeout);
 if(ready<0){const int error=errno;wl_display_cancel_read(display);return error==EINTR;}
 const bool lost=fds[0].revents&(POLLHUP|POLLERR|POLLNVAL);
 // A readable final reply may contain wl_display.error. Dispatch it before
 // reporting loss so owners can retain their protocol-specific diagnostics.
 if(fds[0].revents&POLLIN){if(wl_display_read_events(display)<0)return false;}
 else {wl_display_cancel_read(display);if(lost)return false;}
 return wl_display_dispatch_pending(display)>=0&&!lost;
}
inline bool pump(wl_display* display){
 pollfd fd{wl_display_get_fd(display),POLLIN,0};
 return pump(display,&fd,1,0);
}
}
