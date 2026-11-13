#pragma once
#include <QObject>
#include <QSocketNotifier>
#include <QString>
#include <QTemporaryDir>
#include <wayland-server.h>
#include <sys/stat.h>
#include <unistd.h>
namespace putgd::test {
// The base of a private in-process Wayland compositor: a socket in a new private
// runtime directory that XDG_RUNTIME_DIR and WAYLAND_DISPLAY then point at, a seat
// with no capabilities, and the event loop served through a socket notifier.
// A subclass adds the globals it is testing and calls stop() first in its own
// destructor, because resources it owns still point back at it. It proves a
// protocol path, not any real desktop. `error` is set when the base could not start.
class WaylandTestServer:public QObject {
public:
 QString error;
 void flush(){if(display_)wl_display_flush_clients(display_);}
protected:
 // `directoryTemplate` is a QTemporaryDir template such as "/tmp/pgk-XXXXXX".
 explicit WaylandTestServer(const QString& directoryTemplate):directory_(directoryTemplate){
  struct stat metadata{};
  if(!directory_.isValid()||lstat(directory_.path().toUtf8().constData(),&metadata)<0||!S_ISDIR(metadata.st_mode)||metadata.st_uid!=geteuid()||(metadata.st_mode&077)!=0){error="Fixture runtime directory is not private and owned";return;}
  if(directory_.filePath("wayland-test").toUtf8().size()>107){error="Fixture Unix socket exceeds Linux byte limit";return;}
  qputenv("XDG_RUNTIME_DIR",directory_.path().toUtf8());qputenv("WAYLAND_DISPLAY","wayland-test");
  display_=wl_display_create();if(!display_||wl_display_add_socket(display_,"wayland-test")<0){error="Cannot bind private fixture socket";return;}
  wl_global_create(display_,&wl_seat_interface,5,this,bindSeat);
  auto* loop=wl_display_get_event_loop(display_);notifier_=new QSocketNotifier(wl_event_loop_get_fd(loop),QSocketNotifier::Read,this);
  connect(notifier_,&QSocketNotifier::activated,this,[this,loop]{wl_event_loop_dispatch(loop,0);flush();});
 }
 ~WaylandTestServer() override{stop();}
 void stop(){
  if(notifier_){notifier_->setEnabled(false);delete notifier_;notifier_=nullptr;}
  if(display_){wl_display_destroy_clients(display_);wl_display_destroy(display_);display_=nullptr;}
 }
 static void destroyRequest(wl_client*,wl_resource* resource){wl_resource_destroy(resource);}
 QTemporaryDir directory_;wl_display* display_=nullptr;QSocketNotifier* notifier_=nullptr;
private:
 static void bindSeat(wl_client* client,void*,uint32_t version,uint32_t id){
  auto* seat=wl_resource_create(client,&wl_seat_interface,int(version),id);
  static const struct wl_seat_interface implementation={nullptr,nullptr,nullptr,destroyRequest};
  wl_resource_set_implementation(seat,&implementation,nullptr,nullptr);wl_seat_send_capabilities(seat,0);
 }
};
}
