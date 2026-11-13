#include "EventPump.h"
#include "CursorOutputProbe.h"
#include "ext-image-capture-source-v1-client-protocol.h"
#include "ext-image-copy-capture-v1-client-protocol.h"
#include <QSocketNotifier>
#include <QTimer>
#include <wayland-client.h>
#include <cerrno>
#include <utility>
namespace putgd {
namespace {
Error unavailable(const QString& reason){return {"capture_target","The compositor did not report which monitor the pointer is on",{{"reason",reason}}};}
}
struct CursorOutputProbe::Run:QObject {
 struct Output {Run* run=nullptr;wl_output* native=nullptr;QString name;ext_image_capture_source_v1* source=nullptr;ext_image_copy_capture_cursor_session_v1* session=nullptr;bool entered=false,positioned=false;QPoint position;};
 wl_display* display=nullptr;wl_registry* registry=nullptr;wl_seat* seat=nullptr;wl_pointer* pointer=nullptr;
 ext_image_copy_capture_manager_v1* copy=nullptr;ext_output_image_capture_source_manager_v1* sources=nullptr;
 QList<Output*> outputs;uint32_t capabilities=0;int stage=0;wl_callback* fence=nullptr;QTimer deadline;Reply reply;bool finished=false;
 ~Run() override{
  for(auto* output:outputs){if(output->session)ext_image_copy_capture_cursor_session_v1_destroy(output->session);if(output->source)ext_image_capture_source_v1_destroy(output->source);if(output->native)wl_output_destroy(output->native);delete output;}
  if(fence)wl_callback_destroy(fence);
  if(pointer)wl_pointer_destroy(pointer);if(seat)wl_seat_destroy(seat);
  if(copy)ext_image_copy_capture_manager_v1_destroy(copy);if(sources)ext_output_image_capture_source_manager_v1_destroy(sources);
  if(registry)wl_registry_destroy(registry);
  if(display)wl_display_disconnect(display);
 }
 void finish(QList<Position> positions,Error error){
  if(finished)return;
  finished=true;deadline.stop();if(writeNotifier)writeNotifier->setEnabled(false);
  auto answer=std::exchange(reply,{});
  // Outside the Wayland dispatch that may have called this.
  QTimer::singleShot(0,this,[this]{delete this;});
  if(answer)answer(std::move(positions),std::move(error));
 }
 bool start(Reply callback){
  reply=std::move(callback);
  display=wl_display_connect(nullptr);
  if(!display)return false;
  registry=wl_display_get_registry(display);
  static const wl_registry_listener listener={[](void* data,wl_registry* registry,uint32_t id,const char* interface,uint32_t version){
   auto* run=static_cast<Run*>(data);const QByteArray name(interface);
   if(name==wl_seat_interface.name&&!run->seat){
    run->seat=static_cast<wl_seat*>(wl_registry_bind(registry,id,&wl_seat_interface,1));
    static const wl_seat_listener seatListener={[](void* data,wl_seat*,uint32_t capabilities){static_cast<Run*>(data)->capabilities=capabilities;},[](void*,wl_seat*,const char*){}};
    wl_seat_add_listener(run->seat,&seatListener,run);
   }else if(name==ext_image_copy_capture_manager_v1_interface.name&&!run->copy)run->copy=static_cast<ext_image_copy_capture_manager_v1*>(wl_registry_bind(registry,id,&ext_image_copy_capture_manager_v1_interface,1));
   else if(name==ext_output_image_capture_source_manager_v1_interface.name&&!run->sources)run->sources=static_cast<ext_output_image_capture_source_manager_v1*>(wl_registry_bind(registry,id,&ext_output_image_capture_source_manager_v1_interface,1));
   else if(name==wl_output_interface.name&&version>=4&&run->outputs.size()<16){
    auto* output=new Output;output->run=run;output->native=static_cast<wl_output*>(wl_registry_bind(registry,id,&wl_output_interface,4));run->outputs.append(output);
    static const wl_output_listener outputListener={[](void*,wl_output*,int32_t,int32_t,int32_t,int32_t,int32_t,const char*,const char*,int32_t){},
     [](void*,wl_output*,uint32_t,int32_t,int32_t,int32_t){},[](void*,wl_output*){},[](void*,wl_output*,int32_t){},
     [](void* data,wl_output*,const char* value){static_cast<Output*>(data)->name=QString::fromUtf8(value).left(256);},[](void*,wl_output*,const char*){}};
    wl_output_add_listener(output->native,&outputListener,output);
   }
  },[](void*,wl_registry*,uint32_t){}};
  wl_registry_add_listener(registry,&listener,this);
  auto* notifier=new QSocketNotifier(wl_display_get_fd(display),QSocketNotifier::Read,this);
  writeNotifier=new QSocketNotifier(wl_display_get_fd(display),QSocketNotifier::Write,this);writeNotifier->setEnabled(false);
  connect(writeNotifier,&QSocketNotifier::activated,this,[this]{flush();});
  connect(notifier,&QSocketNotifier::activated,this,[this]{read();});
  deadline.setSingleShot(true);connect(&deadline,&QTimer::timeout,this,[this]{finish({},unavailable("timeout"));});deadline.start(timeoutMs);
  advance();
  return true;
 }
 // Each stage ends at a display sync: everything requested before it has been answered.
 void advance(){
  if(fence){wl_callback_destroy(fence);fence=nullptr;}
  fence=wl_display_sync(display);
  static const wl_callback_listener listener={[](void* data,wl_callback*,uint32_t){static_cast<Run*>(data)->synced();}};
  wl_callback_add_listener(fence,&listener,this);
  flush();
 }
 void synced(){
  if(finished)return;
  ++stage;
  // 1: the globals are bound; their names and the seat's capabilities follow.
  if(stage==1){
   if(!copy||!sources){finish({},unavailable("ext-image-copy-capture-v1 is not offered"));return;}
   if(!seat){finish({},unavailable("no seat"));return;}
   advance();return;
  }
  // 2: one cursor session per output.
  if(stage==2){
   if(!(capabilities&WL_SEAT_CAPABILITY_POINTER)){finish({},unavailable("the seat has no pointer"));return;}
   pointer=wl_seat_get_pointer(seat);
   static const ext_image_copy_capture_cursor_session_v1_listener listener={
    [](void* data,ext_image_copy_capture_cursor_session_v1*){static_cast<Output*>(data)->entered=true;},
    [](void* data,ext_image_copy_capture_cursor_session_v1*){auto* output=static_cast<Output*>(data);output->entered=false;output->positioned=false;},
    [](void* data,ext_image_copy_capture_cursor_session_v1*,int32_t x,int32_t y){auto* output=static_cast<Output*>(data);output->positioned=true;output->position=QPoint(x,y);},
    [](void*,ext_image_copy_capture_cursor_session_v1*,int32_t,int32_t){}};
   for(auto* output:outputs){
    output->source=ext_output_image_capture_source_manager_v1_create_source(sources,output->native);
    output->session=ext_image_copy_capture_manager_v1_create_pointer_cursor_session(copy,output->source,pointer);
    ext_image_copy_capture_cursor_session_v1_add_listener(output->session,&listener,output);
   }
   advance();return;
  }
  // 3 and 4: the sessions' first events. A compositor may send them a beat
  // after the session exists, so one more sync is allowed before giving up.
  QList<Position> positions;
  for(const auto* output:std::as_const(outputs))if(output->entered&&output->positioned&&!output->name.isEmpty())positions.append({output->name,output->position});
  if(!positions.isEmpty()){finish(std::move(positions),{});return;}
  if(stage<6){advance();return;}
  finish({},unavailable("no output reported the cursor"));
 }
 QSocketNotifier* writeNotifier=nullptr;
 void flush(){if(finished)return;bool pending=false;if(!wayland::flush(display,pending)){finish({},unavailable("connection lost"));return;}if(writeNotifier)writeNotifier->setEnabled(pending);}
 void read(){
  if(finished)return;
  if(!wayland::pump(display)){finish({},unavailable("connection lost"));return;}
  if(!finished)flush();
 }

};
CursorOutputProbe::CursorOutputProbe(QObject* parent):QObject(parent){}
CursorOutputProbe::~CursorOutputProbe(){if(run_){run_->reply={};delete run_;}}
void CursorOutputProbe::cancel(){
 if(!run_)return;
 auto* run=std::exchange(run_,nullptr);run->finish({},{"cancelled","Monitor capture cancelled",{}});
}
void CursorOutputProbe::probe(Reply reply){
 cancel();
 auto* run=new Run;run_=run;
 if(!run->start([this,run,reply=std::move(reply)](QList<Position> positions,Error error){if(run_==run)run_=nullptr;reply(std::move(positions),std::move(error));})){
  auto answer=std::exchange(run->reply,{});run_=nullptr;delete run;
  if(answer)answer({},unavailable("cannot connect to the compositor"));
 }
}
}
