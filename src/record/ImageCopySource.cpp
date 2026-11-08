#include "ImageCopySource.h"
#include "DebugLog.h"
#include "platform/wayland/ToplevelMatch.h"
#include "platform/wayland/WaylandTransform.h"
#include "ext-image-copy-capture-v1-client-protocol.h"
#include "ext-image-capture-source-v1-client-protocol.h"
#include "ext-foreign-toplevel-list-v1-client-protocol.h"
#include "wlr-foreign-toplevel-management-unstable-v1-client-protocol.h"
#include <gst/app/gstappsrc.h>
#include <wayland-client.h>
#include <QJsonArray>
#include <QElapsedTimer>
#include <QMap>
#include <fcntl.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <mutex>
#include <vector>

namespace putgd::record {
namespace {
constexpr int ringSize=4;
constexpr qint64 keepaliveNs=1'000'000'000;
constexpr int openTimeoutMs=3000;
qint64 monotonicNs(){timespec t{};clock_gettime(CLOCK_MONOTONIC,&t);return qint64(t.tv_sec)*1'000'000'000+t.tv_nsec;}
QString text(const char* value){return QString::fromUtf8(value,int(strnlen(value,4096)));}
const char* gstFormat(uint32_t format){
 switch(format){case WL_SHM_FORMAT_XRGB8888:return "BGRx";case WL_SHM_FORMAT_ARGB8888:return "BGRA";case WL_SHM_FORMAT_XBGR8888:return "RGBx";case WL_SHM_FORMAT_ABGR8888:return "RGBA";default:return nullptr;}
}
}
struct ImageCopySource::Impl {
 struct Output {wl_output* native=nullptr;QString name;uint32_t transform=WL_OUTPUT_TRANSFORM_NORMAL;};
 struct Window {Impl* owner=nullptr;void* native=nullptr;WaylandToplevel current,pending;bool done=false;};
 // One shared-memory frame buffer. GStreamer holds a slot from the push until
 // the pipeline's copy releases it; a slot from an older size is retired and
 // unmapped once released.
 // Shared with every slot: a buffer GStreamer releases after this source is
 // gone still finds its lock, and no wake descriptor.
 struct Shared {std::mutex lock;int wake=-1;};
 struct Slot {std::shared_ptr<Shared> shared;wl_buffer* buffer=nullptr;void* data=MAP_FAILED;size_t size=0;bool busy=false,retired=false;};
 std::shared_ptr<Shared> shared=std::make_shared<Shared>();
 wl_display* display=nullptr;wl_registry* registry=nullptr;wl_shm* shm=nullptr;
 ext_image_copy_capture_manager_v1* copy=nullptr;ext_output_image_capture_source_manager_v1* outputSources=nullptr;
 ext_foreign_toplevel_image_capture_source_manager_v1* windowSources=nullptr;ext_foreign_toplevel_list_v1* extList=nullptr;zwlr_foreign_toplevel_manager_v1* wlrList=nullptr;
 QMap<QString,int> versions;QList<Output> outputs;QList<Window*> extWindows,wlrWindows;
 ext_image_capture_source_v1* source=nullptr;ext_image_copy_capture_session_v1* session=nullptr;ext_image_copy_capture_frame_v1* frame=nullptr;
 QSize size,pendingSize;QList<uint32_t> formats,pendingFormats;uint32_t format=0;bool constraints=false,reallocate=false;
 // How the compositor turned the buffer (the output's transform, then each frame's own event), and the
 // upright size the stream's caps announce (review #1275: a rotated output recorded sideways).
 uint32_t transform=WL_OUTPUT_TRANSFORM_NORMAL,frameTransform=WL_OUTPUT_TRANSFORM_NORMAL;QSize declared;
 QString capsFor(QSize upright)const{return QStringLiteral("video/x-raw,format=%1,width=%2,height=%3,framerate=0/1").arg(QString::fromLatin1(gstFormat(format))).arg(upright.width()).arg(upright.height());}
 void setCaps(QSize upright){declared=upright;if(GstCaps* caps=gst_caps_from_string(capsFor(upright).toUtf8().constData())){gst_app_src_set_caps(GST_APP_SRC(appsrc),caps);gst_caps_unref(caps);}}
 std::vector<Slot*> ring;Slot* inFlight=nullptr;
 GstElement* appsrc=nullptr;GstBuffer* last=nullptr;qint64 lastPush=0,presentation=-1;
 std::function<void(Error)> lost;std::thread thread;std::atomic_bool stopping{false};
 std::atomic<qint64> frames{0},keepalives{0},constraintChanges{0};bool ended=false;
 Error failure;
 ~Impl(){close();}
 void close(){
  stopping=true;wakeUp();
  if(thread.joinable())thread.join();
  if(appsrc){gst_object_unref(appsrc);appsrc=nullptr;}
  if(last){gst_buffer_unref(last);last=nullptr;}
  if(frame){ext_image_copy_capture_frame_v1_destroy(frame);frame=nullptr;}
  if(session){ext_image_copy_capture_session_v1_destroy(session);session=nullptr;}
  if(source){ext_image_capture_source_v1_destroy(source);source=nullptr;}
  retireSlots();
  for(auto* w:extWindows){ext_foreign_toplevel_handle_v1_destroy(static_cast<ext_foreign_toplevel_handle_v1*>(w->native));delete w;}extWindows.clear();
  for(auto* w:wlrWindows){zwlr_foreign_toplevel_handle_v1_destroy(static_cast<zwlr_foreign_toplevel_handle_v1*>(w->native));delete w;}wlrWindows.clear();
  for(auto& o:outputs)wl_output_release(o.native);outputs.clear();
  if(extList){ext_foreign_toplevel_list_v1_destroy(extList);extList=nullptr;}
  if(wlrList){zwlr_foreign_toplevel_manager_v1_destroy(wlrList);wlrList=nullptr;}
  if(windowSources){ext_foreign_toplevel_image_capture_source_manager_v1_destroy(windowSources);windowSources=nullptr;}
  if(outputSources){ext_output_image_capture_source_manager_v1_destroy(outputSources);outputSources=nullptr;}
  if(copy){ext_image_copy_capture_manager_v1_destroy(copy);copy=nullptr;}
  if(shm){wl_shm_destroy(shm);shm=nullptr;}
  if(registry){wl_registry_destroy(registry);registry=nullptr;}
  if(display){wl_display_disconnect(display);display=nullptr;}
  {std::lock_guard guard(shared->lock);if(shared->wake>=0){::close(shared->wake);shared->wake=-1;}}
 }
 void wakeUp(){std::lock_guard guard(shared->lock);if(shared->wake>=0){const uint64_t one=1;(void)!::write(shared->wake,&one,sizeof one);}}
 void retireSlots(){
  std::lock_guard guard(shared->lock);
  for(auto* slot:ring){if(slot->buffer){wl_buffer_destroy(slot->buffer);slot->buffer=nullptr;}if(slot->busy)slot->retired=true;else{if(slot->data!=MAP_FAILED)munmap(slot->data,slot->size);delete slot;}}
  ring.clear();
 }
 static void global(void* data,wl_registry* registry,uint32_t id,const char* interface,uint32_t version){
  auto* self=static_cast<Impl*>(data);const QByteArray name(interface);
  auto bind=[&](const wl_interface* i,uint32_t wanted){const auto v=std::min(version,wanted);self->versions[QString::fromLatin1(i->name)]=int(v);return wl_registry_bind(registry,id,i,v);};
  if(name==wl_shm_interface.name&&!self->shm)self->shm=static_cast<wl_shm*>(bind(&wl_shm_interface,1));
  else if(name==ext_image_copy_capture_manager_v1_interface.name&&!self->copy)self->copy=static_cast<ext_image_copy_capture_manager_v1*>(bind(&ext_image_copy_capture_manager_v1_interface,1));
  else if(name==ext_output_image_capture_source_manager_v1_interface.name&&!self->outputSources)self->outputSources=static_cast<ext_output_image_capture_source_manager_v1*>(bind(&ext_output_image_capture_source_manager_v1_interface,1));
  else if(name==ext_foreign_toplevel_image_capture_source_manager_v1_interface.name&&!self->windowSources)self->windowSources=static_cast<ext_foreign_toplevel_image_capture_source_manager_v1*>(bind(&ext_foreign_toplevel_image_capture_source_manager_v1_interface,1));
  else if(name==ext_foreign_toplevel_list_v1_interface.name&&!self->extList){
   self->extList=static_cast<ext_foreign_toplevel_list_v1*>(bind(&ext_foreign_toplevel_list_v1_interface,1));
   static const ext_foreign_toplevel_list_v1_listener listener={[](void* data,ext_foreign_toplevel_list_v1*,ext_foreign_toplevel_handle_v1* handle){
    auto* self=static_cast<Impl*>(data);auto* w=new Window;w->owner=self;w->native=handle;self->extWindows.append(w);
    static const ext_foreign_toplevel_handle_v1_listener handleListener={
     [](void* d,ext_foreign_toplevel_handle_v1*){static_cast<Window*>(d)->done=false;static_cast<Window*>(d)->current={};},
     [](void* d,ext_foreign_toplevel_handle_v1*){auto* w=static_cast<Window*>(d);w->current=w->pending;w->done=true;},
     [](void* d,ext_foreign_toplevel_handle_v1*,const char* v){static_cast<Window*>(d)->pending.title=text(v);},
     [](void* d,ext_foreign_toplevel_handle_v1*,const char* v){static_cast<Window*>(d)->pending.appId=text(v);},
     [](void* d,ext_foreign_toplevel_handle_v1*,const char* v){static_cast<Window*>(d)->pending.identifier=text(v);}};
    ext_foreign_toplevel_handle_v1_add_listener(handle,&handleListener,w);
   },[](void*,ext_foreign_toplevel_list_v1*){}};
   ext_foreign_toplevel_list_v1_add_listener(self->extList,&listener,self);
  }else if(name==zwlr_foreign_toplevel_manager_v1_interface.name&&!self->wlrList){
   self->wlrList=static_cast<zwlr_foreign_toplevel_manager_v1*>(bind(&zwlr_foreign_toplevel_manager_v1_interface,3));
   static const zwlr_foreign_toplevel_manager_v1_listener listener={[](void* data,zwlr_foreign_toplevel_manager_v1*,zwlr_foreign_toplevel_handle_v1* handle){
    auto* self=static_cast<Impl*>(data);auto* w=new Window;w->owner=self;w->native=handle;self->wlrWindows.append(w);
    static const zwlr_foreign_toplevel_handle_v1_listener handleListener={
     [](void* d,zwlr_foreign_toplevel_handle_v1*,const char* v){static_cast<Window*>(d)->pending.title=text(v);},
     [](void* d,zwlr_foreign_toplevel_handle_v1*,const char* v){static_cast<Window*>(d)->pending.appId=text(v);},
     [](void*,zwlr_foreign_toplevel_handle_v1*,wl_output*){},[](void*,zwlr_foreign_toplevel_handle_v1*,wl_output*){},
     [](void* d,zwlr_foreign_toplevel_handle_v1*,wl_array* state){auto* w=static_cast<Window*>(d);w->pending.activated=false;const auto* v=static_cast<const uint32_t*>(state->data);for(size_t i=0;i<state->size/sizeof(uint32_t);++i)if(v[i]==ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_ACTIVATED)w->pending.activated=true;},
     [](void* d,zwlr_foreign_toplevel_handle_v1*){auto* w=static_cast<Window*>(d);w->current=w->pending;w->done=true;},
     [](void* d,zwlr_foreign_toplevel_handle_v1*){auto* w=static_cast<Window*>(d);w->done=false;w->current={};},
     [](void*,zwlr_foreign_toplevel_handle_v1*,zwlr_foreign_toplevel_handle_v1*){}};
    zwlr_foreign_toplevel_handle_v1_add_listener(handle,&handleListener,w);
   },[](void*,zwlr_foreign_toplevel_manager_v1*){}};
   zwlr_foreign_toplevel_manager_v1_add_listener(self->wlrList,&listener,self);
  }else if(name==wl_output_interface.name&&version>=4&&self->outputs.size()<16){
   Output output;output.native=static_cast<wl_output*>(wl_registry_bind(registry,id,&wl_output_interface,4));self->outputs.append(output);
   static const wl_output_listener listener={[](void* data,wl_output* native,int32_t,int32_t,int32_t,int32_t,int32_t,const char*,const char*,int32_t transform){auto* self=static_cast<Impl*>(data);for(auto& o:self->outputs)if(o.native==native)o.transform=uint32_t(transform);},
    [](void*,wl_output*,uint32_t,int32_t,int32_t,int32_t){},[](void*,wl_output*){},[](void*,wl_output*,int32_t){},
    [](void* data,wl_output* native,const char* value){auto* self=static_cast<Impl*>(data);for(auto& o:self->outputs)if(o.native==native)o.name=text(value);},[](void*,wl_output*,const char*){}};
   wl_output_add_listener(output.native,&listener,self);
  }
 }
 bool connect(QString* reason){
  display=wl_display_connect(nullptr);
  if(!display){*reason="Cannot connect to the Wayland compositor";return false;}
  registry=wl_display_get_registry(display);static const wl_registry_listener listener={global,[](void*,wl_registry*,uint32_t){}};wl_registry_add_listener(registry,&listener,this);
  // Globals, then the objects they announce (output names, toplevel handles), then their done events.
  for(int i=0;i<3;++i)if(wl_display_roundtrip(display)<0){*reason="The Wayland connection failed";return false;}
  return true;
 }
 QString outputReason()const{
  if(!copy)return QStringLiteral("The compositor does not offer ext-image-copy-capture-v1");
  if(!outputSources)return QStringLiteral("The compositor does not offer ext-output-image-capture-source-manager-v1");
  if(!shm)return QStringLiteral("The compositor does not offer wl_shm");
  if(outputs.isEmpty())return QStringLiteral("Output capture needs wl_output version 4 for output names");
  return {};
 }
 QString windowReason()const{
  if(!outputReason().isEmpty())return outputReason();
  if(!windowSources)return QStringLiteral("Window recording needs ext-foreign-toplevel-image-capture-source-manager-v1, which this compositor does not offer (labwc 0.20 or later and Mir 2.29 or later have it)");
  if(!extList)return QStringLiteral("Window recording needs ext-foreign-toplevel-list-v1");
  if(!wlrList)return QStringLiteral("Recording the active window needs zwlr-foreign-toplevel-management-v1");
  return {};
 }
 // Session --------------------------------------------------------------------
 static void sessionSize(void* d,ext_image_copy_capture_session_v1*,uint32_t w,uint32_t h){auto* s=static_cast<Impl*>(d);s->pendingSize=QSize(int(std::min<uint32_t>(w,16384)),int(std::min<uint32_t>(h,16384)));}
 static void sessionFormat(void* d,ext_image_copy_capture_session_v1*,uint32_t f){auto* s=static_cast<Impl*>(d);if(s->pendingFormats.size()<64)s->pendingFormats.append(f);}
 static void sessionDone(void* d,ext_image_copy_capture_session_v1*){
  auto* s=static_cast<Impl*>(d);
  // WL_SHM_FORMAT_ARGB8888 is 0, so "found" is its own flag.
  uint32_t chosen=0;bool found=false;for(const auto f:{uint32_t(WL_SHM_FORMAT_XRGB8888),uint32_t(WL_SHM_FORMAT_ARGB8888),uint32_t(WL_SHM_FORMAT_XBGR8888),uint32_t(WL_SHM_FORMAT_ABGR8888)})if(s->pendingFormats.contains(f)){chosen=f;found=true;break;}
  // Constraints come as a batch ended by done, in any order within it.
  const auto offered=s->pendingFormats;s->pendingFormats.clear();
  if(!found||s->pendingSize.isEmpty()){s->fail({"recording_unsupported","The compositor offers no 32-bit shared-memory capture format",{{"formats",int(offered.size())},{"width",s->pendingSize.width()},{"height",s->pendingSize.height()}}});return;}
  const bool changed=s->constraints&&(s->pendingSize!=s->size||chosen!=s->format);
  s->size=s->pendingSize;s->format=chosen;s->formats=offered;
  if(changed){s->reallocate=true;s->constraintChanges.fetch_add(1);}
  s->constraints=true;
 }
 static void sessionStopped(void* d,ext_image_copy_capture_session_v1*){auto* s=static_cast<Impl*>(d);s->ended=true;s->fail({"recording_target","The recorded window or output is gone; the recording up to here was kept",{}});}
 void fail(Error error){if(!failure)failure=std::move(error);}
 bool allocate(){
  retireSlots();
  const int stride=size.width()*4;const size_t bytes=size_t(stride)*size_t(size.height());
  for(int i=0;i<ringSize;++i){
   const int fd=int(::syscall(SYS_memfd_create,"putgd-record-frame",MFD_CLOEXEC));
   if(fd<0||ftruncate(fd,off_t(bytes))<0){if(fd>=0)::close(fd);return false;}
   auto* slot=new Slot;slot->shared=shared;slot->size=bytes;slot->data=mmap(nullptr,bytes,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
   if(slot->data==MAP_FAILED){::close(fd);delete slot;return false;}
   auto* pool=wl_shm_create_pool(shm,fd,int32_t(bytes));::close(fd);
   slot->buffer=wl_shm_pool_create_buffer(pool,0,size.width(),size.height(),stride,format);wl_shm_pool_destroy(pool);
   std::lock_guard guard(shared->lock);ring.push_back(slot);
  }
  reallocate=false;return true;
 }
 Slot* freeSlot(){std::lock_guard guard(shared->lock);for(auto* slot:ring)if(!slot->busy&&slot!=inFlight)return slot;return nullptr;}
 void capture(Slot* slot){
  inFlight=slot;presentation=-1;frameTransform=transform;
  frame=ext_image_copy_capture_session_v1_create_frame(session);
  static const ext_image_copy_capture_frame_v1_listener listener={
   [](void* d,ext_image_copy_capture_frame_v1*,uint32_t value){static_cast<Impl*>(d)->frameTransform=value<=WL_OUTPUT_TRANSFORM_FLIPPED_270?value:WL_OUTPUT_TRANSFORM_NORMAL;},
   [](void*,ext_image_copy_capture_frame_v1*,int32_t,int32_t,int32_t,int32_t){},
   [](void* d,ext_image_copy_capture_frame_v1*,uint32_t hi,uint32_t lo,uint32_t nsec){static_cast<Impl*>(d)->presentation=qint64((quint64(hi)<<32)|lo)*1'000'000'000+nsec;},
   [](void* d,ext_image_copy_capture_frame_v1*){static_cast<Impl*>(d)->ready();},
   [](void* d,ext_image_copy_capture_frame_v1*,uint32_t reason){static_cast<Impl*>(d)->frameFailed(reason);}};
  ext_image_copy_capture_frame_v1_add_listener(frame,&listener,this);
  ext_image_copy_capture_frame_v1_attach_buffer(frame,slot->buffer);
  // A reused slot is stale everywhere since its last use; the compositor copies it whole.
  ext_image_copy_capture_frame_v1_damage_buffer(frame,0,0,size.width(),size.height());
  ext_image_copy_capture_frame_v1_capture(frame);
 }
 void frameFailed(uint32_t reason){
  ext_image_copy_capture_frame_v1_destroy(frame);frame=nullptr;inFlight=nullptr;
  // Constraints: fresh ones and done follow; the loop reallocates then.
  if(reason==EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_STOPPED){ended=true;fail({"recording_target","The recorded window or output is gone; the recording up to here was kept",{}});}
 }
 static void release(gpointer data){
  auto* slot=static_cast<Slot*>(data);const auto shared=slot->shared;bool drop=false;
  {
   std::lock_guard guard(shared->lock);slot->busy=false;drop=slot->retired;
   if(!drop&&shared->wake>=0){const uint64_t one=1;(void)!::write(shared->wake,&one,sizeof one);}
  }
  if(drop){if(slot->data!=MAP_FAILED)munmap(slot->data,slot->size);delete slot;}
 }
 void push(GstBuffer* buffer){
  // gst_app_src_push_buffer takes this reference.
  if(gst_app_src_push_buffer(GST_APP_SRC(appsrc),buffer)!=GST_FLOW_OK)stopping=true;
  lastPush=monotonicNs();
 }
 void ready(){
  ext_image_copy_capture_frame_v1_destroy(frame);frame=nullptr;auto* slot=inFlight;inFlight=nullptr;
  if(!slot)return;
  transform=frameTransform;
  GstBuffer* buffer=nullptr;
  if(transform==WL_OUTPUT_TRANSFORM_NORMAL){
   {std::lock_guard guard(shared->lock);slot->busy=true;}
   buffer=gst_buffer_new_wrapped_full(GST_MEMORY_FLAG_READONLY,slot->data,slot->size,0,slot->size,slot,&Impl::release);
  }else{
   // A turned buffer is copied upright (as the screenshot path does) and its slot is free again at once.
   const QImage view(static_cast<const uchar*>(slot->data),size.width(),size.height(),size.width()*4,QImage::Format_RGB32);
   auto* image=new QImage(waylandUpright(view,transform));
   buffer=gst_buffer_new_wrapped_full(GST_MEMORY_FLAG_READONLY,image->bits(),size_t(image->sizeInBytes()),0,size_t(image->sizeInBytes()),image,[](gpointer data){delete static_cast<QImage*>(data);});
  }
  if(const auto upright=waylandUprightSize(size,transform);upright!=declared)setCaps(upright);
  const qint64 now=monotonicNs();
  GST_BUFFER_PTS(buffer)=GstClockTime(presentation>0&&presentation<=now?presentation:now);
  frames.fetch_add(1);
  if(last)gst_buffer_unref(last);
  last=gst_buffer_ref(buffer);
  push(buffer);
 }
 void keepalive(){
  if(!last)return;
  GstBuffer* again=gst_buffer_copy(last);if(!again)return;
  GST_BUFFER_PTS(again)=GstClockTime(monotonicNs());keepalives.fetch_add(1);push(again);
 }
 void run(){
  while(!stopping&&!ended){
   if(reallocate&&!frame){
    if(!allocate()){fail({"recording_io","Cannot allocate capture buffers",{}});break;}
    // The stream's caps follow the source's new size; the pipeline scales into the canvas.
    setCaps(waylandUprightSize(size,transform));
    if(last){gst_buffer_unref(last);last=nullptr;}
   }
   if(!frame&&constraints&&!reallocate)if(auto* slot=freeSlot())capture(slot);
   if(last&&monotonicNs()-lastPush>=keepaliveNs)keepalive();
   if(wl_display_flush(display)<0&&errno!=EAGAIN){fail({"recording_target","The Wayland connection for recording was lost",{}});break;}
   while(wl_display_prepare_read(display)!=0)if(wl_display_dispatch_pending(display)<0){fail({"recording_target","The Wayland connection for recording was lost",{}});ended=true;break;}
   if(ended)break;
   int wake=-1;{std::lock_guard guard(shared->lock);wake=shared->wake;}
   pollfd fds[2]={{wl_display_get_fd(display),POLLIN,0},{wake,POLLIN,0}};
   const int count=poll(fds,2,100);
   if(count>0&&(fds[0].revents&POLLIN)){if(wl_display_read_events(display)<0){fail({"recording_target","The Wayland connection for recording was lost",{}});break;}}
   else wl_display_cancel_read(display);
   if(fds[1].revents&POLLIN){uint64_t value=0;(void)!::read(wake,&value,sizeof value);}
   if(wl_display_dispatch_pending(display)<0){fail({"recording_target","The Wayland connection for recording was lost",{}});break;}
  }
  if(failure&&lost&&!stopping)lost(failure);
 }
};
ImageCopySource::Facts ImageCopySource::probe(){
 Facts facts;Impl impl;QString reason;
 if(!impl.connect(&reason)){facts.reason=reason;facts.windowReason=reason;return facts;}
 for(auto it=impl.versions.cbegin();it!=impl.versions.cend();++it)facts.protocols[it.key()]=it.value();
 facts.reason=impl.outputReason();facts.available=facts.reason.isEmpty();
 facts.windowReason=impl.windowReason();facts.windows=facts.windowReason.isEmpty();
 return facts;
}
ImageCopySource::ImageCopySource():d_(std::make_unique<Impl>()){}
ImageCopySource::~ImageCopySource()=default;
Error ImageCopySource::open(const Target& target,QSize* size,QString* identity,QString* appId){
 QString reason;
 if(!d_->connect(&reason))return {"recording_unavailable",reason,{{"protocol","ext-image-copy-capture-v1"}}};
 reason=target.window?d_->windowReason():d_->outputReason();
 if(!reason.isEmpty())return {target.window?"recording_unsupported":"recording_unavailable",reason,{{"protocol","ext-image-copy-capture-v1"}}};
 if(target.window){
  QList<WaylandToplevel> ext,wlr;for(auto* w:d_->extWindows)if(w->done)ext.append(w->current);for(auto* w:d_->wlrWindows)if(w->done)wlr.append(w->current);
  WaylandToplevel chosen;
  if(target.identity.isEmpty()){Error error;const auto active=matchActiveToplevel(ext,wlr,error);if(!active)return {"recording_target",error.message,error.details};chosen=*active;}
  else{bool found=false;for(const auto& w:ext)if(w.identifier==target.identity){chosen=w;found=true;}if(!found)return {"recording_target","No window has this identity; list them with capture.windows",{}};}
  if(putgdAppId(chosen.appId))return {"recording_target","put.gd does not record its own windows",{}};
  for(auto* w:d_->extWindows)if(w->done&&w->current.identifier==chosen.identifier)d_->source=ext_foreign_toplevel_image_capture_source_manager_v1_create_source(d_->windowSources,static_cast<ext_foreign_toplevel_handle_v1*>(w->native));
  if(identity)*identity=chosen.identifier;if(appId)*appId=chosen.appId;
 }else{
  wl_output* output=nullptr;QString name=target.identity;
  if(name.isEmpty()&&d_->outputs.size()==1)name=d_->outputs.first().name;
  for(const auto& o:d_->outputs)if(o.name==name)output=o.native;
  if(!output)return {"recording_target","No output has this name",{{"identity",target.identity}}};
  d_->source=ext_output_image_capture_source_manager_v1_create_source(d_->outputSources,output);
  for(const auto& o:d_->outputs)if(o.native==output)d_->transform=o.transform;
  if(identity)*identity=name;
 }
 if(!d_->source)return {"recording_target","The recording source could not be created",{}};
 d_->session=ext_image_copy_capture_manager_v1_create_session(d_->copy,d_->source,EXT_IMAGE_COPY_CAPTURE_MANAGER_V1_OPTIONS_PAINT_CURSORS);
 static const ext_image_copy_capture_session_v1_listener listener={&Impl::sessionSize,&Impl::sessionFormat,[](void*,ext_image_copy_capture_session_v1*,wl_array*){},[](void*,ext_image_copy_capture_session_v1*,uint32_t,wl_array*){},&Impl::sessionDone,&Impl::sessionStopped};
 ext_image_copy_capture_session_v1_add_listener(d_->session,&listener,d_.get());
 QElapsedTimer clock;clock.start();
 while(!d_->constraints&&!d_->failure&&clock.elapsed()<openTimeoutMs)if(wl_display_roundtrip(d_->display)<0)return {"recording_unavailable","The Wayland connection failed",{}};
 if(d_->failure)return d_->failure;
 if(!d_->constraints)return {"recording_unavailable","The compositor did not describe the capture buffers",{{"protocol","ext-image-copy-capture-v1"}}};
 if(!d_->allocate())return {"recording_io","Cannot allocate capture buffers",{}};
 {std::lock_guard guard(d_->shared->lock);d_->shared->wake=eventfd(0,EFD_CLOEXEC|EFD_NONBLOCK);}
 d_->declared=waylandUprightSize(d_->size,d_->transform);
 if(size)*size=d_->declared;
 return {};
}
QString ImageCopySource::caps()const{return d_->capsFor(d_->declared);}
void ImageCopySource::attach(GstElement* appsrc,std::function<void(Error)> lost){
 if(d_->thread.joinable()||!appsrc)return;
 d_->appsrc=GST_ELEMENT(gst_object_ref(appsrc));d_->lost=std::move(lost);
 d_->thread=std::thread([impl=d_.get()]{impl->run();});
}
void ImageCopySource::stop(){d_->stopping=true;d_->wakeUp();if(d_->thread.joinable())d_->thread.join();if(d_->last){gst_buffer_unref(d_->last);d_->last=nullptr;}}
QJsonObject ImageCopySource::status()const{
 return {{"acquisition","wlr-capture"},{"frames",double(d_->frames.load())},{"keepalives",double(d_->keepalives.load())},{"sizeChanges",double(d_->constraintChanges.load())},{"width",d_->size.width()},{"height",d_->size.height()},{"transform",int(d_->transform)}};
}
}
