#include "platform/wayland/EventPump.h"
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
#include <algorithm>
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
constexpr qint64 cursorIntervalNs=ImageCopySource::cursorIntervalNs;
constexpr int cursorSettleMs=400;
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
 struct Slot {std::shared_ptr<Shared> shared;wl_buffer* buffer=nullptr;void* data=MAP_FAILED;size_t size=0;QSize dims;bool busy=false,retired=false;};
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
 std::atomic<qint64> frames{0},keepalives{0},constraintChanges{0},restarts{0};bool ended=false;
 // refresh(): the session the loop wants and the one whose frames are being pushed (each buffer's offset).
 std::atomic<quint64> refreshWanted{0};quint64 generation=0;
 Error failure;
 // Own cursor (#1364): the session has no painted cursor; this draws it from the cursor session.
 bool ownCursor=false,cursorHidden=false,seatPointer=false;wl_seat* seat=nullptr;wl_pointer* pointerDevice=nullptr;
 ext_image_copy_capture_cursor_session_v1* cursorSession=nullptr;ext_image_copy_capture_session_v1* cursorCapture=nullptr;ext_image_copy_capture_frame_v1* cursorFrame=nullptr;
 QSize cursorSize,cursorPendingSize;QList<uint32_t> cursorPendingFormats;uint32_t cursorFormat=0;bool cursorConstraints=false,cursorReallocate=false,cursorEnded=false;
 wl_buffer* cursorBuffer=nullptr;void* cursorData=MAP_FAILED;size_t cursorBytes=0;QSize cursorBufferSize;
 // The cursor's picture, premultiplied, already in the frame's channel order.
 std::vector<uint32_t> cursorPixels;QSize cursorImage;
 bool cursorIn=false,cursorDirty=false;QPoint cursorAt,cursorHot;qint64 cursorRetry=0,lastPts=0;
 // The newest captured frame, kept out of the ring so a cursor move can be drawn onto a fresh copy of it.
 Slot* clean=nullptr;
 mutable std::mutex pointerLock;bool pointerIn=false,pointerEver=false;QPoint pointerAt;
 std::atomic<qint64> cursorImages{0},cursorPushes{0};
 ~Impl(){close();}
 void close(){
  stopping=true;wakeUp();
  if(thread.joinable())thread.join();
  if(appsrc){gst_object_unref(appsrc);appsrc=nullptr;}
  if(last){gst_buffer_unref(last);last=nullptr;}
  if(frame){ext_image_copy_capture_frame_v1_destroy(frame);frame=nullptr;}
  if(cursorFrame){ext_image_copy_capture_frame_v1_destroy(cursorFrame);cursorFrame=nullptr;}
  if(cursorCapture){ext_image_copy_capture_session_v1_destroy(cursorCapture);cursorCapture=nullptr;}
  if(cursorSession){ext_image_copy_capture_cursor_session_v1_destroy(cursorSession);cursorSession=nullptr;}
  freeCursorBuffer();
  if(pointerDevice){wl_pointer_destroy(pointerDevice);pointerDevice=nullptr;}
  if(seat){wl_seat_destroy(seat);seat=nullptr;}
  if(session){ext_image_copy_capture_session_v1_destroy(session);session=nullptr;}
  if(source){ext_image_capture_source_v1_destroy(source);source=nullptr;}
  dropClean();
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
  else if(name==wl_seat_interface.name&&!self->seat){
   self->seat=static_cast<wl_seat*>(wl_registry_bind(registry,id,&wl_seat_interface,1));
   static const wl_seat_listener listener={[](void* d,wl_seat*,uint32_t capabilities){static_cast<Impl*>(d)->seatPointer=capabilities&WL_SEAT_CAPABILITY_POINTER;},[](void*,wl_seat*,const char*){}};
   wl_seat_add_listener(self->seat,&listener,self);
  }
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
  if(!found||s->pendingSize.isEmpty()){s->ended=true;s->fail({"recording_unsupported","The compositor offers no 32-bit shared-memory capture format",{{"formats",int(offered.size())},{"width",s->pendingSize.width()},{"height",s->pendingSize.height()}}});return;}
  const bool changed=(s->constraints||!s->ring.empty())&&(s->pendingSize!=s->size||chosen!=s->format);
  s->size=s->pendingSize;s->format=chosen;s->formats=offered;
  if(changed){s->reallocate=true;s->constraintChanges.fetch_add(1);}
  s->constraints=true;
 }
 static void sessionStopped(void* d,ext_image_copy_capture_session_v1*){auto* s=static_cast<Impl*>(d);s->ended=true;s->fail({"recording_target","The recorded window or output is gone; the recording up to here was kept",{}});}
 void fail(Error error){if(!failure)failure=std::move(error);}
 bool allocate(){
  dropClean();retireSlots();
  const int stride=size.width()*4;const size_t bytes=size_t(stride)*size_t(size.height());
  for(int i=0;i<ringSize;++i){
   const int fd=int(::syscall(SYS_memfd_create,"putgd-record-frame",MFD_CLOEXEC));
   if(fd<0||ftruncate(fd,off_t(bytes))<0){if(fd>=0)::close(fd);return false;}
   auto* slot=new Slot;slot->shared=shared;slot->size=bytes;slot->dims=size;slot->data=mmap(nullptr,bytes,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
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
 // The capture session, with the compositor painting the cursor unless this source draws it; waits for its constraints.
 void createSession(){
  if(session){ext_image_copy_capture_session_v1_destroy(session);session=nullptr;}
  constraints=false;pendingFormats.clear();pendingSize={};
  session=ext_image_copy_capture_manager_v1_create_session(copy,source,ownCursor||cursorHidden?0u:uint32_t(EXT_IMAGE_COPY_CAPTURE_MANAGER_V1_OPTIONS_PAINT_CURSORS));
  static const ext_image_copy_capture_session_v1_listener listener={&Impl::sessionSize,&Impl::sessionFormat,[](void*,ext_image_copy_capture_session_v1*,wl_array*){},[](void*,ext_image_copy_capture_session_v1*,uint32_t,wl_array*){},&Impl::sessionDone,&Impl::sessionStopped};
  ext_image_copy_capture_session_v1_add_listener(session,&listener,this);
 }
 // A frame is copied only when the source changed since the session's last one, so a still screen sends
 // nothing; the first frame of a new session is the source as it is. On the capture thread.
 void restartSession(quint64 wanted){
  if(frame){ext_image_copy_capture_frame_v1_destroy(frame);frame=nullptr;}
  inFlight=nullptr;
  createSession();
  if(last){gst_buffer_unref(last);last=nullptr;}
  generation=wanted;restarts.fetch_add(1);
 }
 Error startSession(){
  createSession();
  QElapsedTimer clock;clock.start();
  while(!constraints&&!failure&&clock.elapsed()<openTimeoutMs)if(wl_display_roundtrip(display)<0)return {"recording_unavailable","The Wayland connection failed",{}};
  if(failure)return failure;
  if(!constraints)return {"recording_unavailable","The compositor did not describe the capture buffers",{{"protocol","ext-image-copy-capture-v1"}}};
  return {};
 }
 // Own cursor ------------------------------------------------------------------
 void closeCursor(){
  if(cursorFrame){ext_image_copy_capture_frame_v1_destroy(cursorFrame);cursorFrame=nullptr;}
  if(cursorCapture){ext_image_copy_capture_session_v1_destroy(cursorCapture);cursorCapture=nullptr;}
  if(cursorSession){ext_image_copy_capture_cursor_session_v1_destroy(cursorSession);cursorSession=nullptr;}
  freeCursorBuffer();
  cursorIn=false;cursorPixels.clear();setPointer(false,{});
 }
 // Whether any output but `recorded` reports the pointer (two round trips; nothing is kept).
 bool pointerOnAnotherOutput(wl_output* recorded){
  struct Probe {ext_image_capture_source_v1* source=nullptr;ext_image_copy_capture_cursor_session_v1* cursor=nullptr;bool in=false;};
  std::vector<std::unique_ptr<Probe>> probes;
  static const ext_image_copy_capture_cursor_session_v1_listener listener={
   [](void* d,ext_image_copy_capture_cursor_session_v1*){static_cast<Probe*>(d)->in=true;},[](void* d,ext_image_copy_capture_cursor_session_v1*){static_cast<Probe*>(d)->in=false;},
   [](void*,ext_image_copy_capture_cursor_session_v1*,int32_t,int32_t){},[](void*,ext_image_copy_capture_cursor_session_v1*,int32_t,int32_t){}};
  for(const auto& o:outputs){
   if(o.native==recorded)continue;
   auto probe=std::make_unique<Probe>();
   probe->source=ext_output_image_capture_source_manager_v1_create_source(outputSources,o.native);
   probe->cursor=ext_image_copy_capture_manager_v1_create_pointer_cursor_session(copy,probe->source,pointerDevice);
   ext_image_copy_capture_cursor_session_v1_add_listener(probe->cursor,&listener,probe.get());
   probes.push_back(std::move(probe));
  }
  if(!probes.empty())for(int i=0;i<2;++i)if(wl_display_roundtrip(display)<0)break;
  bool found=false;
  for(auto& probe:probes){found|=probe->in;ext_image_copy_capture_cursor_session_v1_destroy(probe->cursor);ext_image_capture_source_v1_destroy(probe->source);}
  return found;
 }
 // A plain arrow, hotspot at its tip, for a pointer whose picture the compositor has not delivered
 // (yet): a recording never shows clicks around nothing.
 static const std::vector<uint32_t>& arrow(QSize* size){
  static const char* const rows[]={
   "X           ","XX          ","X.X         ","X..X        ","X...X       ","X....X      ","X.....X     ","X......X    ","X.......X   ",
   "X........X  ","X.........X ","X......XXXXX","X...X..X    ","X..X X..X   ","X.X  X..X   ","XX    X..X  ","X     X..X  ","       XX   "};
  static const std::vector<uint32_t> pixels=[]{
   std::vector<uint32_t> out;
   for(const char* row:rows)for(const char* c=row;*c;++c)out.push_back(*c=='X'?0xff000000u:*c=='.'?0xffffffffu:0u);
   return out;
  }();
  *size=QSize(12,int(std::size(rows)));
  return pixels;
 }
 void dropClean(){if(clean){release(clean);clean=nullptr;}}
 void freeCursorBuffer(){
  if(cursorBuffer){wl_buffer_destroy(cursorBuffer);cursorBuffer=nullptr;}
  if(cursorData!=MAP_FAILED){munmap(cursorData,cursorBytes);cursorData=MAP_FAILED;}
 }
 void setPointer(bool in,QPoint at){std::lock_guard guard(pointerLock);pointerIn=in;pointerEver|=in;pointerAt=at;}
 static void cursorEnter(void* d,ext_image_copy_capture_cursor_session_v1*){auto* s=static_cast<Impl*>(d);s->cursorIn=true;s->cursorDirty=true;s->setPointer(true,s->cursorAt);}
 static void cursorLeave(void* d,ext_image_copy_capture_cursor_session_v1*){auto* s=static_cast<Impl*>(d);s->cursorIn=false;s->cursorDirty=true;s->setPointer(false,s->cursorAt);}
 static void cursorPosition(void* d,ext_image_copy_capture_cursor_session_v1*,int32_t x,int32_t y){auto* s=static_cast<Impl*>(d);s->cursorAt=QPoint(x,y);s->cursorDirty=true;s->setPointer(s->cursorIn,s->cursorAt);}
 static void cursorHotspot(void* d,ext_image_copy_capture_cursor_session_v1*,int32_t x,int32_t y){auto* s=static_cast<Impl*>(d);s->cursorHot=QPoint(x,y);s->cursorDirty=true;}
 static void cursorExtent(void* d,ext_image_copy_capture_session_v1*,uint32_t w,uint32_t h){static_cast<Impl*>(d)->cursorPendingSize=QSize(int(std::min<uint32_t>(w,1024)),int(std::min<uint32_t>(h,1024)));}
 static void cursorShmFormat(void* d,ext_image_copy_capture_session_v1*,uint32_t f){auto* s=static_cast<Impl*>(d);if(s->cursorPendingFormats.size()<64)s->cursorPendingFormats.append(f);}
 static void cursorDone(void* d,ext_image_copy_capture_session_v1*){
  auto* s=static_cast<Impl*>(d);
  // A cursor needs its alpha: ARGB or ABGR, nothing else.
  const auto offered=s->cursorPendingFormats;s->cursorPendingFormats.clear();
  const bool argb=offered.contains(uint32_t(WL_SHM_FORMAT_ARGB8888)),abgr=offered.contains(uint32_t(WL_SHM_FORMAT_ABGR8888));
  if((!argb&&!abgr)||s->cursorPendingSize.isEmpty()){s->cursorConstraints=false;return;}
  s->cursorSize=s->cursorPendingSize;s->cursorFormat=uint32_t(argb?WL_SHM_FORMAT_ARGB8888:WL_SHM_FORMAT_ABGR8888);
  s->cursorConstraints=true;s->cursorReallocate=true;
 }
 static void cursorStopped(void* d,ext_image_copy_capture_session_v1*){static_cast<Impl*>(d)->cursorEnded=true;}
 bool allocateCursor(){
  freeCursorBuffer();
  const int stride=cursorSize.width()*4;cursorBytes=size_t(stride)*size_t(cursorSize.height());
  const int fd=int(::syscall(SYS_memfd_create,"putgd-record-cursor",MFD_CLOEXEC));
  if(fd<0||ftruncate(fd,off_t(cursorBytes))<0){if(fd>=0)::close(fd);return false;}
  cursorData=mmap(nullptr,cursorBytes,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
  if(cursorData==MAP_FAILED){::close(fd);return false;}
  auto* pool=wl_shm_create_pool(shm,fd,int32_t(cursorBytes));::close(fd);
  cursorBuffer=wl_shm_pool_create_buffer(pool,0,cursorSize.width(),cursorSize.height(),stride,cursorFormat);wl_shm_pool_destroy(pool);
  cursorBufferSize=cursorSize;cursorReallocate=false;return true;
 }
 void cursorReady(){
  ext_image_copy_capture_frame_v1_destroy(cursorFrame);cursorFrame=nullptr;
  if(cursorData==MAP_FAILED)return;
  // Into the frame's channel order once, so drawing is one blend per pixel.
  const bool frameBgr=format==WL_SHM_FORMAT_XRGB8888||format==WL_SHM_FORMAT_ARGB8888,cursorBgr=cursorFormat==WL_SHM_FORMAT_ARGB8888;
  const size_t count=size_t(cursorBufferSize.width())*size_t(cursorBufferSize.height());
  cursorPixels.resize(count);std::memcpy(cursorPixels.data(),cursorData,count*4);
  if(frameBgr!=cursorBgr)for(auto& pixel:cursorPixels)pixel=(pixel&0xff00ff00u)|((pixel&0x00ff0000u)>>16)|((pixel&0x000000ffu)<<16);
  cursorImage=cursorBufferSize;cursorDirty=true;cursorImages.fetch_add(1);
 }
 void cursorFailed(uint32_t reason){
  ext_image_copy_capture_frame_v1_destroy(cursorFrame);cursorFrame=nullptr;
  if(reason==EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_STOPPED)cursorEnded=true;
  // New constraints follow a size change; anything else is tried again shortly, never in a tight loop.
  else cursorRetry=monotonicNs()+200'000'000;
 }
 // Keeps one cursor frame asked for: the compositor answers when the cursor's picture changes.
 void pumpCursor(){
  if(!cursorCapture||cursorEnded||cursorFrame||!cursorConstraints||monotonicNs()<cursorRetry)return;
  if(cursorReallocate&&!allocateCursor()){cursorEnded=true;return;}
  cursorFrame=ext_image_copy_capture_session_v1_create_frame(cursorCapture);
  static const ext_image_copy_capture_frame_v1_listener listener={
   [](void*,ext_image_copy_capture_frame_v1*,uint32_t){},[](void*,ext_image_copy_capture_frame_v1*,int32_t,int32_t,int32_t,int32_t){},
   [](void*,ext_image_copy_capture_frame_v1*,uint32_t,uint32_t,uint32_t){},
   [](void* d,ext_image_copy_capture_frame_v1*){static_cast<Impl*>(d)->cursorReady();},
   [](void* d,ext_image_copy_capture_frame_v1*,uint32_t reason){static_cast<Impl*>(d)->cursorFailed(reason);}};
  ext_image_copy_capture_frame_v1_add_listener(cursorFrame,&listener,this);
  ext_image_copy_capture_frame_v1_attach_buffer(cursorFrame,cursorBuffer);
  ext_image_copy_capture_frame_v1_damage_buffer(cursorFrame,0,0,cursorBufferSize.width(),cursorBufferSize.height());
  ext_image_copy_capture_frame_v1_capture(cursorFrame);
 }
 void openCursor(){
  pointerDevice=wl_seat_get_pointer(seat);
  cursorSession=ext_image_copy_capture_manager_v1_create_pointer_cursor_session(copy,source,pointerDevice);
  static const ext_image_copy_capture_cursor_session_v1_listener listener={&Impl::cursorEnter,&Impl::cursorLeave,&Impl::cursorPosition,&Impl::cursorHotspot};
  ext_image_copy_capture_cursor_session_v1_add_listener(cursorSession,&listener,this);
  cursorCapture=ext_image_copy_capture_cursor_session_v1_get_capture_session(cursorSession);
  static const ext_image_copy_capture_session_v1_listener capture={&Impl::cursorExtent,&Impl::cursorShmFormat,[](void*,ext_image_copy_capture_session_v1*,wl_array*){},[](void*,ext_image_copy_capture_session_v1*,uint32_t,wl_array*){},&Impl::cursorDone,&Impl::cursorStopped};
  ext_image_copy_capture_session_v1_add_listener(cursorCapture,&capture,this);
 }
 // A copy of the newest frame with the cursor drawn where the cursor session says it is.
 void pushComposed(qint64 pts,bool cursorOnly=false){
  if(!clean)return;
  const int w=clean->dims.width(),h=clean->dims.height();const size_t bytes=size_t(w)*4*size_t(h);
  auto* pixels=static_cast<uint32_t*>(g_try_malloc(bytes));if(!pixels)return;
  std::memcpy(pixels,clean->data,bytes);
  if(cursorIn){
   // The compositor's own cursor picture; until it has come, a plain arrow at the same place.
   QSize shape=cursorImage;const bool own=!cursorPixels.empty();
   const std::vector<uint32_t>& picture=own?cursorPixels:arrow(&shape);
   const QPoint hot=own?cursorHot:QPoint();
   const int left=cursorAt.x()-hot.x(),top=cursorAt.y()-hot.y(),cw=shape.width(),ch=shape.height();
   for(int y=std::max(0,-top);y<ch&&top+y<h;++y){
    const uint32_t* from=picture.data()+size_t(y)*size_t(cw);uint32_t* row=pixels+size_t(top+y)*size_t(w);
    for(int x=std::max(0,-left);x<cw&&left+x<w;++x){
     const uint32_t s=from[x],a=s>>24;
     if(!a)continue;
     if(a==255){row[left+x]=s;continue;}
     // Premultiplied source over the frame, one channel at a time.
     const uint32_t d=row[left+x],k=255-a;
     auto over=[s,d,k](int shift){return std::min<uint32_t>(((s>>shift)&0xff)+(((d>>shift)&0xff)*k+127)/255,255)<<shift;};
     row[left+x]=0xff000000u|over(16)|over(8)|over(0);
    }
   }
  }
  GstBuffer* buffer=nullptr;
  if(transform==WL_OUTPUT_TRANSFORM_NORMAL)buffer=gst_buffer_new_wrapped_full(GstMemoryFlags(0),pixels,bytes,0,bytes,pixels,g_free);
  else{
   const QImage view(reinterpret_cast<const uchar*>(pixels),w,h,w*4,QImage::Format_RGB32);
   auto* image=new QImage(waylandUpright(view,transform));g_free(pixels);
   buffer=gst_buffer_new_wrapped_full(GST_MEMORY_FLAG_READONLY,image->bits(),size_t(image->sizeInBytes()),0,size_t(image->sizeInBytes()),image,[](gpointer data){delete static_cast<QImage*>(data);});
  }
  if(const auto upright=waylandUprightSize(clean->dims,transform);upright!=declared)setCaps(upright);
  // A cursor frame is stamped now, a captured one when it was shown: never backwards.
  lastPts=std::max(pts,lastPts+1);GST_BUFFER_PTS(buffer)=GstClockTime(lastPts);
  if(last)gst_buffer_unref(last);
  last=gst_buffer_ref(buffer);
  // The recorder's own frame, for the cadence lock. The reference kept for a keepalive stays unflagged.
  if(cursorOnly){buffer=gst_buffer_make_writable(buffer);GST_BUFFER_FLAG_SET(buffer,GST_BUFFER_FLAG_DROPPABLE);}
  push(buffer);
 }
 void ready(){
  ext_image_copy_capture_frame_v1_destroy(frame);frame=nullptr;auto* slot=inFlight;inFlight=nullptr;
  if(!slot)return;
  transform=frameTransform;
  if(ownCursor){
   dropClean();
   {std::lock_guard guard(shared->lock);slot->busy=true;}
   clean=slot;cursorDirty=false;frames.fetch_add(1);
   const qint64 now=monotonicNs();
   pushComposed(presentation>0&&presentation<=now?presentation:now);
   return;
  }
  GstBuffer* buffer=nullptr;
  if(transform==WL_OUTPUT_TRANSFORM_NORMAL){
   {std::lock_guard guard(shared->lock);slot->busy=true;}
   buffer=gst_buffer_new_wrapped_full(GST_MEMORY_FLAG_READONLY,slot->data,slot->size,0,slot->size,slot,&Impl::release);
  }else{
   // A turned buffer is copied upright (as the screenshot path does) and its slot is free again at once.
   // The slot's own size: a newer constraint batch may already have changed `size` (review #1275).
   const QImage view(static_cast<const uchar*>(slot->data),slot->dims.width(),slot->dims.height(),slot->dims.width()*4,QImage::Format_RGB32);
   auto* image=new QImage(waylandUpright(view,transform));
   buffer=gst_buffer_new_wrapped_full(GST_MEMORY_FLAG_READONLY,image->bits(),size_t(image->sizeInBytes()),0,size_t(image->sizeInBytes()),image,[](gpointer data){delete static_cast<QImage*>(data);});
  }
  if(const auto upright=waylandUprightSize(slot->dims,transform);upright!=declared)setCaps(upright);
  const qint64 now=monotonicNs();
  GST_BUFFER_PTS(buffer)=GstClockTime(presentation>0&&presentation<=now?presentation:now);
  GST_BUFFER_OFFSET(buffer)=generation;
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
   if(const quint64 wanted=refreshWanted.load();wanted!=generation)restartSession(wanted);
   if(reallocate&&!frame){
    if(!allocate()){fail({"recording_io","Cannot allocate capture buffers",{}});break;}
    // The stream's caps follow the source's new size; the pipeline scales into the canvas.
    setCaps(waylandUprightSize(size,transform));
    if(last){gst_buffer_unref(last);last=nullptr;}
   }
   if(ownCursor){
    pumpCursor();
    if(cursorDirty&&clean&&monotonicNs()-lastPush>=cursorIntervalNs){cursorDirty=false;cursorPushes.fetch_add(1);pushComposed(monotonicNs(),true);}
   }
   if(!frame&&constraints&&!reallocate)if(auto* slot=freeSlot())capture(slot);
   if(last&&monotonicNs()-lastPush>=keepaliveNs)keepalive();
   bool pending=false;
   if(!wayland::flush(display,pending)){fail({"recording_target","The Wayland connection for recording was lost",{}});break;}
   int wake=-1;{std::lock_guard guard(shared->lock);wake=shared->wake;}
   pollfd fds[2]={{wl_display_get_fd(display),short(POLLIN|(pending?POLLOUT:0)),0},{wake,POLLIN,0}};
   // A cursor move waiting for its turn is drawn when the interval is over, not at the next event.
   if(!wayland::pump(display,fds,2,ownCursor&&cursorDirty&&clean?int(std::clamp<qint64>((cursorIntervalNs-(monotonicNs()-lastPush))/1'000'000,1,16)):100)){
    fail({"recording_target","The Wayland connection for recording was lost",{}});break;
   }
   if(fds[1].revents&POLLIN){uint64_t value=0;(void)!::read(wake,&value,sizeof value);}
  }
  if(failure&&lost&&!stopping)lost(failure);
 }
};
ImageCopySource::Facts ImageCopySource::probe(){
 Facts facts;Impl impl;QString reason;
 if(!impl.connect(&reason)){facts.reason=reason;facts.windowReason=reason;return facts;}
 for(auto it=impl.versions.cbegin();it!=impl.versions.cend();++it)facts.protocols[it.key()]=it.value();
 facts.outputs=int(impl.outputs.size());facts.pointer=impl.copy&&impl.seat&&impl.seatPointer;
 facts.reason=impl.outputReason();facts.available=facts.reason.isEmpty();
 facts.windowReason=impl.windowReason();facts.windows=facts.windowReason.isEmpty();
 return facts;
}
ImageCopySource::ImageCopySource():d_(std::make_unique<Impl>()){}
ImageCopySource::~ImageCopySource()=default;
Error ImageCopySource::open(const Target& target,QSize* size,QString* identity,QString* appId,bool pointer,bool* pointerOpened,bool cursorHidden){
 if(pointerOpened)*pointerOpened=false;
 d_->cursorHidden=cursorHidden;if(cursorHidden)pointer=false;
 QString reason;
 if(!d_->connect(&reason))return {"recording_unavailable",reason,{{"protocol","ext-image-copy-capture-v1"}}};
 reason=target.window?d_->windowReason():d_->outputReason();
 if(!reason.isEmpty())return {target.window?"recording_unsupported":"recording_unavailable",reason,{{"protocol","ext-image-copy-capture-v1"}}};
 wl_output* recorded=nullptr;
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
  recorded=output;
  if(identity)*identity=name;
 }
 if(!d_->source)return {"recording_target","The recording source could not be created",{}};
 // The cursor is this source's to draw only for an upright output on a seat with a pointer:
 // a window source has no cursor session in wlroots, and a turned output's is not handled here.
 d_->ownCursor=pointer&&!target.window&&d_->seat&&d_->seatPointer&&d_->transform==WL_OUTPUT_TRANSFORM_NORMAL;
 if(auto e=d_->startSession())return e;
 if(d_->ownCursor){
  // The first frames should already carry the cursor: wait, briefly, until the
  // cursor session has said where it is and its picture has arrived, or that it is elsewhere.
  d_->openCursor();
  QElapsedTimer settle;settle.start();
  for(int trips=0;settle.elapsed()<cursorSettleMs;++trips){
   if(wl_display_roundtrip(d_->display)<0)return {"recording_unavailable","The Wayland connection failed",{}};
   d_->pumpCursor();
   if(!d_->cursorPixels.empty()||(trips>=2&&!d_->cursorIn))break;
  }
  // The recording never loses its cursor to this. Two cases hand it back to the compositor, which
  // then paints it as in any other recording (and the stream has no pointer position):
  // the pointer is on this output and its picture did not come; or no output reports the pointer at
  // all, which is what a compositor without a hardware cursor looks like (its cursor source reads
  // only that). A pointer that is on another output proves nothing: it may come to this one later.
  const bool noPicture=d_->cursorIn&&d_->cursorPixels.empty();
  const bool nowhere=!d_->cursorIn&&!d_->pointerOnAnotherOutput(recorded);
  if(noPicture||nowhere){
   debugLog(QStringLiteral("image copy: the compositor paints the cursor (%1)").arg(noPicture?"the pointer's picture did not arrive":"no output reports the pointer"));
   d_->closeCursor();d_->ownCursor=false;
   if(auto e=d_->startSession())return e;
  }
 }
 if(!d_->allocate())return {"recording_io","Cannot allocate capture buffers",{}};
 {std::lock_guard guard(d_->shared->lock);d_->shared->wake=eventfd(0,EFD_CLOEXEC|EFD_NONBLOCK);}
 d_->declared=waylandUprightSize(d_->size,d_->transform);
 if(size)*size=d_->declared;
 if(pointerOpened)*pointerOpened=d_->ownCursor;
 return {};
}
bool ImageCopySource::pointer(QPoint* at,bool* ever)const{
 if(ever)*ever=false;
 if(!d_->ownCursor)return false;
 std::lock_guard guard(d_->pointerLock);
 if(ever)*ever=d_->pointerEver;
 if(!d_->pointerIn)return false;
 if(at)*at=d_->pointerAt;
 return true;
}
QString ImageCopySource::caps()const{return d_->capsFor(d_->declared);}
void ImageCopySource::attach(GstElement* appsrc,std::function<void(Error)> lost){
 if(d_->thread.joinable()||!appsrc)return;
 d_->appsrc=GST_ELEMENT(gst_object_ref(appsrc));d_->lost=std::move(lost);
 d_->thread=std::thread([impl=d_.get()]{impl->run();});
}
quint64 ImageCopySource::refresh(){const quint64 wanted=d_->refreshWanted.fetch_add(1)+1;d_->wakeUp();return wanted;}
void ImageCopySource::stop(){d_->stopping=true;d_->wakeUp();if(d_->thread.joinable())d_->thread.join();if(d_->last){gst_buffer_unref(d_->last);d_->last=nullptr;}d_->dropClean();}
QJsonObject ImageCopySource::status()const{
 QJsonObject status{{"acquisition","wlr-capture"},{"frames",double(d_->frames.load())},{"keepalives",double(d_->keepalives.load())},{"sizeChanges",double(d_->constraintChanges.load())},{"width",d_->size.width()},{"height",d_->size.height()},{"transform",int(d_->transform)}};
 if(d_->ownCursor){
  // moves: frames pushed for a cursor move alone; images: cursor pictures received.
  std::lock_guard guard(d_->pointerLock);
  status["cursor"]=QJsonObject{{"drawnBy","recorder"},{"onOutput",d_->pointerIn},{"seen",d_->pointerEver},{"images",double(d_->cursorImages.load())},{"moves",double(d_->cursorPushes.load())}};
 }
 return status;
}
}
