#include "CaptureConnection.h"
#include <wayland-client.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <cstring>
#include <cstddef>
namespace putgd {
CaptureConnection::CaptureConnection(){
 const auto inherited=qgetenv("WAYLAND_SOCKET");
 if(!inherited.isNull()){
  inherited_=true;
  bool valid=false;const int fd=inherited.toInt(&valid);if(!valid||fd<0)return;
  sockaddr_un address{};socklen_t size=sizeof address;ucred peer{};socklen_t peerSize=sizeof peer;
  // A named inherited connection can be reopened and authenticated. Anonymous
  // socketpairs cannot: refuse rather than judge WAYLAND_DISPLAY's session.
  if(getpeername(fd,reinterpret_cast<sockaddr*>(&address),&size)!=0||address.sun_family!=AF_UNIX||
     size<=offsetof(sockaddr_un,sun_path)||address.sun_path[0]!='/'||
     getsockopt(fd,SOL_SOCKET,SO_PEERCRED,&peer,&peerSize)!=0)return;
  const auto length=strnlen(address.sun_path,sizeof address.sun_path);if(length==sizeof address.sun_path)return;
  path_=QByteArray(address.sun_path,int(length));serverPid_=peer.pid;serverUid_=peer.uid;serverGid_=peer.gid;
 }else{
  path_=qgetenv("WAYLAND_DISPLAY");if(path_.isEmpty())path_="wayland-0";
  if(!path_.startsWith('/')){const auto runtime=qgetenv("XDG_RUNTIME_DIR");if(runtime.isEmpty()){path_.clear();return;}path_=runtime+'/'+path_;}
 }
}
void CaptureConnection::disconnected(){if(!inherited_)serverPid_=-1;}
wl_display* CaptureConnection::connect() const{
 sockaddr_un address{};address.sun_family=AF_UNIX;
 if(path_.isEmpty()||path_.contains('\0')||size_t(path_.size())>=sizeof address.sun_path)return nullptr;
 std::memcpy(address.sun_path,path_.constData(),size_t(path_.size()));
 const int fd=::socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC|SOCK_NONBLOCK,0);if(fd<0)return nullptr;
 // AF_UNIX EAGAIN means a full backlog; do not block the event loop.
 ucred peer{};socklen_t size=sizeof peer;
 if(::connect(fd,reinterpret_cast<const sockaddr*>(&address),sizeof address)<0||
    getsockopt(fd,SOL_SOCKET,SO_PEERCRED,&peer,&size)!=0||
    (serverPid_!=-1&&(peer.pid!=serverPid_||peer.uid!=serverUid_||peer.gid!=serverGid_))){::close(fd);return nullptr;}
 serverPid_=peer.pid;serverUid_=peer.uid;serverGid_=peer.gid;
 return wl_display_connect_to_fd(fd); // libwayland owns fd, including on failure.
}
}
