#include "EventPump.h"
#include "SurfaceVisibility.h"
#include "CaptureConnection.h"
#include <QObject>
#include <QSocketNotifier>
#include <QTimer>
#include <wayland-client.h>
#include <poll.h>
#include <sys/mman.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <utility>
#define namespace namespace_
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#undef namespace
namespace putgd {
namespace {
class Visibility final:public QObject {
 wl_display* display_=nullptr;wl_registry* registry_=nullptr;wl_compositor* compositor_=nullptr;
 wl_shm* shm_=nullptr;zwlr_layer_shell_v1* shell_=nullptr;zwlr_layer_surface_v1* layer_=nullptr;
 wl_surface* surface_=nullptr;wl_buffer* buffer_=nullptr;wl_callback* callback_=nullptr;
 QSocketNotifier* notifier_=nullptr;QSocketNotifier* writeNotifier_=nullptr;QTimer timeout_;std::function<void(bool)> reply_;bool done_=false;
 void finish(bool visible){
  if(std::exchange(done_,true))return;
  timeout_.stop();if(notifier_)notifier_->setEnabled(false);if(writeNotifier_)writeNotifier_->setEnabled(false);
  // A listener's storage and display must outlive the current dispatch.
  QTimer::singleShot(0,this,[this,visible]{auto reply=std::move(reply_);deleteLater();reply(visible);});
 }
 void flush(){if(done_)return;bool pending=false;if(!wayland::flush(display_,pending)){finish(false);return;}if(writeNotifier_)writeNotifier_->setEnabled(pending);}
 void events(){
  if(done_)return;
  if(!wayland::pump(display_))finish(false);else if(!done_)flush();
 }

 void configured(uint32_t serial){
  zwlr_layer_surface_v1_ack_configure(layer_,serial);if(buffer_)return;
  const int fd=memfd_create("putgd-visibility",MFD_CLOEXEC);if(fd<0){finish(false);return;}
  if(ftruncate(fd,4)!=0){::close(fd);finish(false);return;}
  auto* pool=wl_shm_create_pool(shm_,fd,4);::close(fd);
  buffer_=wl_shm_pool_create_buffer(pool,0,1,1,4,WL_SHM_FORMAT_ARGB8888);wl_shm_pool_destroy(pool);
  // A zero-filled buffer changes no pixels and an empty input region never
  // steals a click, focus, or a key. The compositor still schedules its frame.
  auto* empty=wl_compositor_create_region(compositor_);wl_surface_set_input_region(surface_,empty);wl_region_destroy(empty);
  wl_surface_attach(surface_,buffer_,0,0);wl_surface_damage(surface_,0,0,1,1);
  callback_=wl_surface_frame(surface_);
  static const wl_callback_listener frame{[](void* data,wl_callback*,uint32_t){static_cast<Visibility*>(data)->finish(true);}};
  wl_callback_add_listener(callback_,&frame,this);
  // Registry/configure can wait behind the first software-rendered window.
  // Give busy software renderers three seconds for the actual frame witness
  // after setup (five seconds maximum), for an eight-second total bound.
  timeout_.start(3000);wl_surface_commit(surface_);flush();
 }
 void ready(){
  if(!compositor_||!shm_||!shell_){finish(false);return;}
  surface_=wl_compositor_create_surface(compositor_);
  layer_=zwlr_layer_shell_v1_get_layer_surface(shell_,surface_,nullptr,ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY,"putgd-visibility");
  zwlr_layer_surface_v1_set_size(layer_,1,1);
  zwlr_layer_surface_v1_set_anchor(layer_,ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP|ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT);
  zwlr_layer_surface_v1_set_exclusive_zone(layer_,-1);
  static const zwlr_layer_surface_v1_listener listener{
   [](void* data,zwlr_layer_surface_v1*,uint32_t serial,uint32_t,uint32_t){static_cast<Visibility*>(data)->configured(serial);},
   [](void* data,zwlr_layer_surface_v1*){static_cast<Visibility*>(data)->finish(false);}};
  zwlr_layer_surface_v1_add_listener(layer_,&listener,this);wl_surface_commit(surface_);flush();
 }
public:
 Visibility(QObject* context,const CaptureConnection& connection,std::function<void(bool)> reply):QObject(context),reply_(std::move(reply)){
  timeout_.setSingleShot(true);connect(&timeout_,&QTimer::timeout,this,[this]{finish(false);});timeout_.start(5000);
  display_=connection.connect();if(!display_){finish(false);return;}
  registry_=wl_display_get_registry(display_);
  static const wl_registry_listener globals{
   [](void* data,wl_registry* registry,uint32_t id,const char* name,uint32_t){auto* self=static_cast<Visibility*>(data);
    if(!strcmp(name,"wl_compositor"))self->compositor_=static_cast<wl_compositor*>(wl_registry_bind(registry,id,&wl_compositor_interface,1));
    else if(!strcmp(name,"wl_shm"))self->shm_=static_cast<wl_shm*>(wl_registry_bind(registry,id,&wl_shm_interface,1));
    else if(!strcmp(name,"zwlr_layer_shell_v1"))self->shell_=static_cast<zwlr_layer_shell_v1*>(wl_registry_bind(registry,id,&zwlr_layer_shell_v1_interface,1));},
   [](void*,wl_registry*,uint32_t){}};
  wl_registry_add_listener(registry_,&globals,this);
  callback_=wl_display_sync(display_);
  static const wl_callback_listener sync{[](void* data,wl_callback* callback,uint32_t){auto* self=static_cast<Visibility*>(data);wl_callback_destroy(callback);self->callback_=nullptr;self->ready();}};
  wl_callback_add_listener(callback_,&sync,this);
  notifier_=new QSocketNotifier(wl_display_get_fd(display_),QSocketNotifier::Read,this);
  writeNotifier_=new QSocketNotifier(wl_display_get_fd(display_),QSocketNotifier::Write,this);writeNotifier_->setEnabled(false);
  connect(writeNotifier_,&QSocketNotifier::activated,this,[this]{flush();});
  connect(notifier_,&QSocketNotifier::activated,this,[this]{events();});flush();
 }
 ~Visibility() override{
  if(notifier_)notifier_->setEnabled(false);if(writeNotifier_)writeNotifier_->setEnabled(false);
  if(callback_)wl_callback_destroy(callback_);
  if(layer_)zwlr_layer_surface_v1_destroy(layer_);if(surface_)wl_surface_destroy(surface_);
  if(buffer_)wl_buffer_destroy(buffer_);
  if(shell_)zwlr_layer_shell_v1_destroy(shell_);if(shm_)wl_shm_destroy(shm_);
  if(compositor_)wl_compositor_destroy(compositor_);
  if(registry_)wl_registry_destroy(registry_);
  if(display_){wl_display_flush(display_);wl_display_disconnect(display_);}
 }
};
}
void querySurfaceVisible(QObject* context,const CaptureConnection& connection,std::function<void(bool)> reply){new Visibility(context,connection,std::move(reply));}
}
