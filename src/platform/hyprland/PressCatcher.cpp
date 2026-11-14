#include "platform/wayland/EventPump.h"
#include "PressCatcher.h"
// The generated header names a parameter "namespace".
#define namespace namespace_
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#undef namespace
#include <QHash>
#include <QSocketNotifier>
#include <QTimer>
#include <wayland-client.h>
#include <linux/input-event-codes.h>
#include <poll.h>
#include <sys/mman.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <cstring>

namespace putgd {
namespace {
struct OutputEntry {wl_output* native=nullptr;QString name;};
int qtKeyOf(uint32_t code){
 switch(code){
 case KEY_LEFT:return Qt::Key_Left;case KEY_RIGHT:return Qt::Key_Right;case KEY_UP:return Qt::Key_Up;case KEY_DOWN:return Qt::Key_Down;
 case KEY_ENTER:return Qt::Key_Return;case KEY_KPENTER:return Qt::Key_Enter;case KEY_SPACE:return Qt::Key_Space;case KEY_TAB:return Qt::Key_Tab;
 default:return 0;
 }
}
// A hole in a memfd: all zeros, so fully transparent, and no memory until read.
wl_buffer* transparentBuffer(wl_shm* shm,int width,int height){
 const int stride=width*4;const qint64 size=qint64(stride)*height;
 if(width<=0||height<=0||size>INT32_MAX)return nullptr;
 const int fd=memfd_create("putgd-press-catcher",MFD_CLOEXEC);if(fd<0)return nullptr;
 wl_buffer* buffer=nullptr;
 if(ftruncate(fd,size)==0){
  auto* pool=wl_shm_create_pool(shm,fd,int32_t(size));
  buffer=wl_shm_pool_create_buffer(pool,0,width,height,stride,WL_SHM_FORMAT_ARGB8888);
  wl_shm_pool_destroy(pool);
 }
 ::close(fd);return buffer;
}
}
struct PressCatcher::Impl {
 struct Layer {Impl* owner=nullptr;wl_surface* surface=nullptr;zwlr_layer_surface_v1* layer=nullptr;wl_buffer* buffer=nullptr;QRectF logical;};
 PressCatcher& q;
 wl_display* display=nullptr;wl_registry* registry=nullptr;wl_compositor* compositor=nullptr;wl_shm* shm=nullptr;zwlr_layer_shell_v1* shell=nullptr;
 wl_seat* seat=nullptr;wl_pointer* pointer=nullptr;wl_keyboard* keyboard=nullptr;
 QHash<uint32_t,OutputEntry*> outputs;QList<Layer*> layers;QSocketNotifier* notifier=nullptr;QSocketNotifier* writeNotifier=nullptr;QTimer retry;
 Layer* over=nullptr;QPointF at;bool down=false,dispatching=false;uint32_t modifiers=0;
 explicit Impl(PressCatcher& owner):q(owner){retry.setSingleShot(true);QObject::connect(&retry,&QTimer::timeout,&q,[this]{open();});open();}
 ~Impl(){reset();}
 void open(){
  if(display)return;
  display=wl_display_connect(nullptr);
  if(!display){retry.start(5000);return;}
  registry=wl_display_get_registry(display);
  static const wl_registry_listener listener{global,removed};
  wl_registry_add_listener(registry,&listener,this);
  notifier=new QSocketNotifier(wl_display_get_fd(display),QSocketNotifier::Read,&q);
  writeNotifier=new QSocketNotifier(wl_display_get_fd(display),QSocketNotifier::Write,&q);writeNotifier->setEnabled(false);
  QObject::connect(writeNotifier,&QSocketNotifier::activated,&q,[this]{flush();});
  QObject::connect(notifier,&QSocketNotifier::activated,&q,[this]{events();});
  flush();
 }
 void reset(){
  if(writeNotifier){writeNotifier->setEnabled(false);writeNotifier->deleteLater();writeNotifier=nullptr;}
  clearLayers();
  if(notifier){notifier->setEnabled(false);notifier->deleteLater();notifier=nullptr;}
  if(pointer)wl_pointer_destroy(pointer);if(keyboard)wl_keyboard_destroy(keyboard);if(seat)wl_seat_destroy(seat);
  pointer=nullptr;keyboard=nullptr;seat=nullptr;
  for(auto* output:outputs){wl_output_destroy(output->native);delete output;}outputs.clear();
  if(shell)zwlr_layer_shell_v1_destroy(shell);if(shm)wl_shm_destroy(shm);if(compositor)wl_compositor_destroy(compositor);if(registry)wl_registry_destroy(registry);
  shell=nullptr;shm=nullptr;compositor=nullptr;registry=nullptr;
  if(display){wl_display_flush(display);wl_display_disconnect(display);display=nullptr;}
 }
 void lost(){reset();retry.start(5000);}
 void flush(){if(!display)return;bool pending=false;if(!wayland::flush(display,pending)){lost();return;}if(writeNotifier)writeNotifier->setEnabled(pending);}
 void events(){
  if(!display||dispatching)return;dispatching=true;
  const bool ok=wayland::pump(display);
  dispatching=false;
  if(!ok){lost();return;}
  flush();
 }

 bool start(const QList<Area>& areas){
  if(!layers.isEmpty())return true;
  if(!display||!compositor||!shm||!shell)return false;
  over=nullptr;down=false;
  for(const auto& area:areas)for(auto* output:outputs)if(output->name==area.output){
   auto* entry=new Layer{this,wl_compositor_create_surface(compositor),nullptr,nullptr,area.logical};
   entry->layer=zwlr_layer_shell_v1_get_layer_surface(shell,entry->surface,output->native,ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY,"putgd-region-catcher");
   zwlr_layer_surface_v1_set_anchor(entry->layer,ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP|ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM|ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT|ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
   zwlr_layer_surface_v1_set_exclusive_zone(entry->layer,-1);
   zwlr_layer_surface_v1_set_keyboard_interactivity(entry->layer,ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_ON_DEMAND);
   static const zwlr_layer_surface_v1_listener listener{configured,closed};
   zwlr_layer_surface_v1_add_listener(entry->layer,&listener,entry);
   wl_surface_commit(entry->surface);layers.append(entry);
  }
  flush();
  return !layers.isEmpty();
 }
 void clearLayers(){
  for(auto* layer:layers){if(layer->layer)zwlr_layer_surface_v1_destroy(layer->layer);if(layer->surface)wl_surface_destroy(layer->surface);if(layer->buffer)wl_buffer_destroy(layer->buffer);delete layer;}
  layers.clear();over=nullptr;down=false;
 }
 // From inside a listener the objects being dispatched must outlive it.
 void finishLater(){QTimer::singleShot(0,&q,[this]{clearLayers();flush();});}
 static void configured(void* data,zwlr_layer_surface_v1* layer,uint32_t serial,uint32_t width,uint32_t height){
  auto* entry=static_cast<Layer*>(data);zwlr_layer_surface_v1_ack_configure(layer,serial);
  if(entry->buffer)return;
  const auto rect=entry->logical.toAlignedRect();const int w=width?int(width):rect.width(),h=height?int(height):rect.height();
  entry->buffer=transparentBuffer(entry->owner->shm,w,h);
  if(!entry->buffer){entry->owner->finishLater();return;}
  wl_surface_attach(entry->surface,entry->buffer,0,0);wl_surface_damage(entry->surface,0,0,w,h);wl_surface_commit(entry->surface);
 }
 static void closed(void* data,zwlr_layer_surface_v1*){static_cast<Layer*>(data)->owner->finishLater();}
 void place(wl_fixed_t x,wl_fixed_t y){if(over)at=over->logical.topLeft()+QPointF(wl_fixed_to_double(x),wl_fixed_to_double(y));}
 static void entered(void* data,wl_pointer*,uint32_t,wl_surface* surface,wl_fixed_t x,wl_fixed_t y){
  auto& self=*static_cast<Impl*>(data);if(!surface)return;
  for(auto* layer:self.layers)if(layer->surface==surface)self.over=layer;
  self.place(x,y);
 }
 static void left(void* data,wl_pointer*,uint32_t,wl_surface*){auto& self=*static_cast<Impl*>(data);if(!self.down)self.over=nullptr;}
 static void moved(void* data,wl_pointer*,uint32_t,wl_fixed_t x,wl_fixed_t y){auto& self=*static_cast<Impl*>(data);self.place(x,y);if(self.down)emit self.q.pointer(PressCatcher::Move,self.at);}
 static void clicked(void* data,wl_pointer*,uint32_t,uint32_t,uint32_t button,uint32_t state){
  auto& self=*static_cast<Impl*>(data);if(!self.over)return;
  if(button==BTN_LEFT&&state==WL_POINTER_BUTTON_STATE_PRESSED&&!self.down){self.down=true;emit self.q.pointer(PressCatcher::Press,self.at);}
  else if(button==BTN_LEFT&&state==WL_POINTER_BUTTON_STATE_RELEASED&&self.down){self.down=false;emit self.q.pointer(PressCatcher::Release,self.at);self.finishLater();}
  else if(button==BTN_RIGHT&&state==WL_POINTER_BUTTON_STATE_PRESSED&&!self.down){emit self.q.pointer(PressCatcher::Cancel,self.at);self.finishLater();}
 }
 static void axis(void*,wl_pointer*,uint32_t,uint32_t,wl_fixed_t){}
 static void keymap(void*,wl_keyboard*,uint32_t,int fd,uint32_t){::close(fd);}
 static void keyEnter(void*,wl_keyboard*,uint32_t,wl_surface*,wl_array*){}
 static void keyLeave(void*,wl_keyboard*,uint32_t,wl_surface*){}
 static void keyModifiers(void* data,wl_keyboard*,uint32_t,uint32_t depressed,uint32_t,uint32_t,uint32_t){static_cast<Impl*>(data)->modifiers=depressed;}
 static void keyEvent(void* data,wl_keyboard*,uint32_t,uint32_t,uint32_t code,uint32_t state){
  auto& self=*static_cast<Impl*>(data);
  if(state!=WL_KEYBOARD_KEY_STATE_PRESSED||self.layers.isEmpty()||self.down)return;
  if(code==KEY_ESC){emit self.q.pointer(PressCatcher::Cancel,self.at);self.finishLater();return;}
  const int qtKey=qtKeyOf(code);if(!qtKey)return;
  // The default xkb modifier layout: Shift, Control, Alt.
  int mods=Qt::NoModifier;if(self.modifiers&0x1)mods|=Qt::ShiftModifier;if(self.modifiers&0x4)mods|=Qt::ControlModifier;if(self.modifiers&0x8)mods|=Qt::AltModifier;
  emit self.q.key(qtKey,mods);
 }
 static void capabilities(void* data,wl_seat* seat,uint32_t caps){
  auto& self=*static_cast<Impl*>(data);
  if(!(caps&WL_SEAT_CAPABILITY_POINTER)&&self.pointer){
   // No release can come now: a drag in progress ends here.
   if(self.down){emit self.q.pointer(PressCatcher::Cancel,self.at);self.finishLater();}
   wl_pointer_destroy(self.pointer);self.pointer=nullptr;self.over=nullptr;self.down=false;
  }
  if(!(caps&WL_SEAT_CAPABILITY_KEYBOARD)&&self.keyboard){wl_keyboard_destroy(self.keyboard);self.keyboard=nullptr;}
  if((caps&WL_SEAT_CAPABILITY_POINTER)&&!self.pointer){
   self.pointer=wl_seat_get_pointer(seat);
   // Only events of the version bound below can arrive, so the rest stay unset.
   static const wl_pointer_listener listener=[]{wl_pointer_listener l{};l.enter=entered;l.leave=left;l.motion=moved;l.button=clicked;l.axis=axis;return l;}();
   wl_pointer_add_listener(self.pointer,&listener,&self);
  }
  if((caps&WL_SEAT_CAPABILITY_KEYBOARD)&&!self.keyboard){
   self.keyboard=wl_seat_get_keyboard(seat);
   static const wl_keyboard_listener listener=[]{wl_keyboard_listener l{};l.keymap=keymap;l.enter=keyEnter;l.leave=keyLeave;l.key=keyEvent;l.modifiers=keyModifiers;return l;}();
   wl_keyboard_add_listener(self.keyboard,&listener,&self);
  }
 }
 static void seatName(void*,wl_seat*,const char*){}
 static void outputGeometry(void*,wl_output*,int32_t,int32_t,int32_t,int32_t,int32_t,const char*,const char*,int32_t){}
 static void outputMode(void*,wl_output*,uint32_t,int32_t,int32_t,int32_t){}
 static void outputDone(void*,wl_output*){}
 static void outputScale(void*,wl_output*,int32_t){}
 static void outputNamed(void* data,wl_output*,const char* name){static_cast<OutputEntry*>(data)->name=QString::fromUtf8(name);}
 static void outputDescription(void*,wl_output*,const char*){}
 static void global(void* data,wl_registry* registry,uint32_t name,const char* interface,uint32_t version){
  auto& self=*static_cast<Impl*>(data);
  if(std::strcmp(interface,wl_compositor_interface.name)==0&&!self.compositor)self.compositor=static_cast<wl_compositor*>(wl_registry_bind(registry,name,&wl_compositor_interface,1));
  else if(std::strcmp(interface,wl_shm_interface.name)==0&&!self.shm)self.shm=static_cast<wl_shm*>(wl_registry_bind(registry,name,&wl_shm_interface,1));
  // on_demand needs version 4; an older compositor gets no catcher and the selector works alone.
  else if(std::strcmp(interface,zwlr_layer_shell_v1_interface.name)==0&&!self.shell&&version>=4)self.shell=static_cast<zwlr_layer_shell_v1*>(wl_registry_bind(registry,name,&zwlr_layer_shell_v1_interface,4));
  else if(std::strcmp(interface,wl_seat_interface.name)==0&&!self.seat){
   self.seat=static_cast<wl_seat*>(wl_registry_bind(registry,name,&wl_seat_interface,std::min(version,3u)));
   static const wl_seat_listener listener=[]{wl_seat_listener l{};l.capabilities=capabilities;l.name=seatName;return l;}();
   wl_seat_add_listener(self.seat,&listener,&self);
  }else if(std::strcmp(interface,wl_output_interface.name)==0&&version>=4){
   auto* entry=new OutputEntry{static_cast<wl_output*>(wl_registry_bind(registry,name,&wl_output_interface,4)),{}};
   static const wl_output_listener listener=[]{wl_output_listener l{};l.geometry=outputGeometry;l.mode=outputMode;l.done=outputDone;l.scale=outputScale;l.name=outputNamed;l.description=outputDescription;return l;}();
   wl_output_add_listener(entry->native,&listener,entry);self.outputs.insert(name,entry);
  }
 }
 static void removed(void* data,wl_registry*,uint32_t name){
  auto& self=*static_cast<Impl*>(data);
  if(auto* entry=self.outputs.take(name)){wl_output_destroy(entry->native);delete entry;}
 }
};
PressCatcher::PressCatcher(QObject* parent):QObject(parent),d_(std::make_unique<Impl>(*this)){}
PressCatcher::~PressCatcher()=default;
bool PressCatcher::start(const QList<Area>& areas){return d_->start(areas);}
void PressCatcher::stop(){d_->clearLayers();d_->flush();}
bool PressCatcher::running()const{return !d_->layers.isEmpty();}
}
