#include "EventPump.h"
#include "ImageCopyCapture.h"
#include "CaptureConnection.h"
#include "WaylandTransform.h"
#include "ext-image-copy-capture-v1-client-protocol.h"
#include "ext-image-capture-source-v1-client-protocol.h"
#include "ext-foreign-toplevel-list-v1-client-protocol.h"
#include "wlr-foreign-toplevel-management-unstable-v1-client-protocol.h"
#include <wayland-client.h>
#include <QHash>
#include <QJsonArray>
#include <QSet>
#include <QMap>
#include <QPointer>
#include <QSocketNotifier>
#include <QThread>
#include <QTimer>
#include <QTransform>
#include <poll.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <algorithm>

namespace putgd {
namespace {
constexpr int maximumToplevels=512;
constexpr int maximumText=4096;
constexpr int captureTimeoutMs=5000;
constexpr int maximumConstraintRetries=2;
QString boundedText(const char* value){const auto length=strnlen(value,maximumText+1);return QString::fromUtf8(value,int(std::min<size_t>(length,maximumText)));}
}
QImage::Format shmImageFormat(uint32_t format){
#if Q_BYTE_ORDER == Q_LITTLE_ENDIAN
 switch(format){
  case WL_SHM_FORMAT_XRGB8888:return QImage::Format_RGB32;
  case WL_SHM_FORMAT_ARGB8888:return QImage::Format_ARGB32_Premultiplied;
  case WL_SHM_FORMAT_XBGR8888:return QImage::Format_RGBX8888;
  case WL_SHM_FORMAT_ABGR8888:return QImage::Format_RGBA8888_Premultiplied;
  default:return QImage::Format_Invalid;
 }
#else
 Q_UNUSED(format);return QImage::Format_Invalid;
#endif
}
struct ImageCopyCapture::Impl:QObject {
 struct ExtWindow {Impl* owner=nullptr;ext_foreign_toplevel_handle_v1* native=nullptr;Toplevel current,pending;bool done=false;};
 struct WlrWindow {Impl* owner=nullptr;zwlr_foreign_toplevel_handle_v1* native=nullptr;Toplevel current,pending;bool done=false;};
 struct Output {Impl* owner=nullptr;uint32_t global=0;wl_output* native=nullptr;QString name;};
 struct Request {
  Impl* owner=nullptr;quint64 id=0;bool window=false;QString target;ResourceLimits limits;Reply reply;
  ext_image_capture_source_v1* source=nullptr;ext_image_copy_capture_session_v1* session=nullptr;ext_image_copy_capture_frame_v1* frame=nullptr;
  wl_shm_pool* pool=nullptr;wl_buffer* buffer=nullptr;void* map=MAP_FAILED;size_t mapSize=0;int stride=0;
  // size/formats accumulate the batch being announced; a batch ends with done and is kept until a
  // frame is allocated from it, so one that arrives while a frame is in flight is not lost.
  QSize size,batchSize,allocated;QList<uint32_t> formats,batchFormats;bool batchReady=false;
  QImage::Format imageFormat=QImage::Format_Invalid;uint32_t transform=WL_OUTPUT_TRANSFORM_NORMAL;int retries=0;QTimer* timer=nullptr;
 };
 ImageCopyCapture& q;CaptureConnection connection;
 wl_display* display=nullptr;wl_registry* registry=nullptr;wl_shm* shm=nullptr;
 ext_image_copy_capture_manager_v1* copy=nullptr;ext_output_image_capture_source_manager_v1* outputSources=nullptr;
 ext_foreign_toplevel_image_capture_source_manager_v1* windowSources=nullptr;ext_foreign_toplevel_list_v1* extList=nullptr;zwlr_foreign_toplevel_manager_v1* wlrList=nullptr;
 QMap<QString,int> versions;QMap<uint32_t,QString> globals;
 QMap<uint32_t,Output*> outputs;QList<ExtWindow*> extWindows;QList<WlrWindow*> wlrWindows;bool toplevelOverflow=false;
 QHash<quint64,Request*> requests;quint64 nextId=0;
 struct Freeze {Impl* owner=nullptr;wl_callback* callback=nullptr;std::function<void(Toplevel,Error)> reply;};QList<Freeze*> freezes;
 QList<QThread*> workers;QSet<quint64> turning;QHash<quint64,Error> turnedAway;bool cancelling=false;
 QSocketNotifier* readNotifier=nullptr;QSocketNotifier* writeNotifier=nullptr;QTimer reconnect;wl_callback* initialSync=nullptr;
 QString error="Image copy capture has not connected";bool connected=false,dispatching=false,lossPending=false,closing=false;
 explicit Impl(ImageCopyCapture& q):q(q){reconnect.setInterval(1000);connect(&reconnect,&QTimer::timeout,this,[this]{if(!display)open();});open();reconnect.start();}
 ~Impl() override{closing=true;reconnect.stop();cancelAll({"cancelled","Image capture closed",{}});reset();for(auto* worker:workers){worker->wait();delete worker;}}
 void notify(){emit q.changed();}
 // Connection --------------------------------------------------------------
 void open(){
  if(closing||display)return;
  display=connection.connect();
  if(!display){error="Cannot connect to the Wayland compositor for image capture";return;}
  registry=wl_display_get_registry(display);static const wl_registry_listener listener={global,removed};wl_registry_add_listener(registry,&listener,this);
  const auto fd=wl_display_get_fd(display);readNotifier=new QSocketNotifier(fd,QSocketNotifier::Read,this);writeNotifier=new QSocketNotifier(fd,QSocketNotifier::Write,this);writeNotifier->setEnabled(false);
  connect(readNotifier,&QSocketNotifier::activated,this,[this]{events();});connect(writeNotifier,&QSocketNotifier::activated,this,[this]{flush();});
  initialSync=wl_display_sync(display);
  static const wl_callback_listener synced={[](void* data,wl_callback* callback,uint32_t){auto* self=static_cast<Impl*>(data);wl_callback_destroy(callback);self->initialSync=nullptr;self->connected=true;self->error.clear();self->notify();}};
  wl_callback_add_listener(initialSync,&synced,this);flush();
 }
 void flush(){if(!display)return;bool pending=false;if(!wayland::flush(display,pending)){lost();return;}if(writeNotifier)writeNotifier->setEnabled(pending);}
 void events(){
  if(!display||dispatching)return;
  dispatching=true;
  const bool ok=wayland::pump(display);
  dispatching=false;
  if(!ok){lost();return;}
  flush();
 }

 void lost(){
  // Never inside dispatch or cancelAll(): both walk objects that a nested cancel would delete.
  if(dispatching||cancelling){if(!lossPending){lossPending=true;QTimer::singleShot(0,this,[this]{lossPending=false;lost();});}return;}
  cancelAll({"capture_failed","The Wayland compositor connection for image capture was lost",{}});reset();connection.disconnected();error="The Wayland compositor connection for image capture was lost";notify();
 }
 void reset(){
  if(readNotifier){auto* retired=readNotifier;readNotifier=nullptr;retired->setEnabled(false);retired->disconnect(this);retired->deleteLater();}
  if(writeNotifier){auto* retired=writeNotifier;writeNotifier=nullptr;retired->setEnabled(false);retired->disconnect(this);retired->deleteLater();}
  if(initialSync){wl_callback_destroy(initialSync);initialSync=nullptr;}
  for(auto* freeze:freezes){if(freeze->callback)wl_callback_destroy(freeze->callback);delete freeze;}freezes.clear();
  for(auto* window:extWindows){ext_foreign_toplevel_handle_v1_destroy(window->native);delete window;}extWindows.clear();
  for(auto* window:wlrWindows){zwlr_foreign_toplevel_handle_v1_destroy(window->native);delete window;}wlrWindows.clear();
  for(auto* output:outputs){if(wl_output_get_version(output->native)>=3)wl_output_release(output->native);else wl_output_destroy(output->native);delete output;}outputs.clear();
  if(extList){ext_foreign_toplevel_list_v1_destroy(extList);extList=nullptr;}
  if(wlrList){zwlr_foreign_toplevel_manager_v1_destroy(wlrList);wlrList=nullptr;}
  if(windowSources){ext_foreign_toplevel_image_capture_source_manager_v1_destroy(windowSources);windowSources=nullptr;}
  if(outputSources){ext_output_image_capture_source_manager_v1_destroy(outputSources);outputSources=nullptr;}
  if(copy){ext_image_copy_capture_manager_v1_destroy(copy);copy=nullptr;}
  if(shm){wl_shm_destroy(shm);shm=nullptr;}
  if(registry){wl_registry_destroy(registry);registry=nullptr;}
  if(display){wl_display_disconnect(display);display=nullptr;}
  versions.clear();globals.clear();toplevelOverflow=false;connected=false;
 }
 template<typename T> T* bind(uint32_t id,const wl_interface* interface,uint32_t offered,uint32_t wanted){
  const auto version=std::min(offered,wanted);versions[QString::fromLatin1(interface->name)]=int(version);globals[id]=QString::fromLatin1(interface->name);
  return static_cast<T*>(wl_registry_bind(registry,id,interface,version));
 }
 static void global(void* data,wl_registry*,uint32_t id,const char* interface,uint32_t version){
  auto* self=static_cast<Impl*>(data);const QByteArray name(interface);
  if(name==wl_shm_interface.name&&!self->shm)self->shm=self->bind<wl_shm>(id,&wl_shm_interface,version,1);
  else if(name==ext_image_copy_capture_manager_v1_interface.name&&!self->copy)self->copy=self->bind<ext_image_copy_capture_manager_v1>(id,&ext_image_copy_capture_manager_v1_interface,version,1);
  else if(name==ext_output_image_capture_source_manager_v1_interface.name&&!self->outputSources)self->outputSources=self->bind<ext_output_image_capture_source_manager_v1>(id,&ext_output_image_capture_source_manager_v1_interface,version,1);
  else if(name==ext_foreign_toplevel_image_capture_source_manager_v1_interface.name&&!self->windowSources)self->windowSources=self->bind<ext_foreign_toplevel_image_capture_source_manager_v1>(id,&ext_foreign_toplevel_image_capture_source_manager_v1_interface,version,1);
  else if(name==ext_foreign_toplevel_list_v1_interface.name&&!self->extList){
   self->extList=self->bind<ext_foreign_toplevel_list_v1>(id,&ext_foreign_toplevel_list_v1_interface,version,1);
   static const ext_foreign_toplevel_list_v1_listener listener={extToplevel,extFinished};ext_foreign_toplevel_list_v1_add_listener(self->extList,&listener,self);
  }else if(name==zwlr_foreign_toplevel_manager_v1_interface.name&&!self->wlrList){
   self->wlrList=self->bind<zwlr_foreign_toplevel_manager_v1>(id,&zwlr_foreign_toplevel_manager_v1_interface,version,3);
   static const zwlr_foreign_toplevel_manager_v1_listener listener={wlrToplevel,wlrFinished};zwlr_foreign_toplevel_manager_v1_add_listener(self->wlrList,&listener,self);
  }else if(name==wl_output_interface.name&&version>=4&&self->outputs.size()<16){
   auto* output=new Output;output->owner=self;output->global=id;
   output->native=static_cast<wl_output*>(wl_registry_bind(self->registry,id,&wl_output_interface,4));self->outputs.insert(id,output);
   static const wl_output_listener listener={[](void*,wl_output*,int32_t,int32_t,int32_t,int32_t,int32_t,const char*,const char*,int32_t){},
    [](void*,wl_output*,uint32_t,int32_t,int32_t,int32_t){},[](void*,wl_output*){},[](void*,wl_output*,int32_t){},
    [](void* data,wl_output*,const char* value){auto* output=static_cast<Output*>(data);output->name=boundedText(value);output->owner->notify();},[](void*,wl_output*,const char*){}};
   wl_output_add_listener(output->native,&listener,output);
  }else if(name==wl_output_interface.name&&version<4)self->versions["wl_output"]=int(version);
  self->notify();
 }
 static void removed(void* data,wl_registry*,uint32_t id){
  auto* self=static_cast<Impl*>(data);
  if(auto* output=self->outputs.take(id)){wl_output_release(output->native);delete output;self->notify();return;}
  const auto name=self->globals.take(id);if(name.isEmpty())return;
  // A capture manager that leaves ends the requests that depend on it.
  self->cancelAll({"capture_failed",name+" was withdrawn by the compositor",{}});
  if(name==QString::fromLatin1(ext_image_copy_capture_manager_v1_interface.name)){ext_image_copy_capture_manager_v1_destroy(self->copy);self->copy=nullptr;}
  else if(name==QString::fromLatin1(ext_output_image_capture_source_manager_v1_interface.name)){ext_output_image_capture_source_manager_v1_destroy(self->outputSources);self->outputSources=nullptr;}
  else if(name==QString::fromLatin1(ext_foreign_toplevel_image_capture_source_manager_v1_interface.name)){ext_foreign_toplevel_image_capture_source_manager_v1_destroy(self->windowSources);self->windowSources=nullptr;}
  self->versions.remove(name);self->notify();
 }
 // Toplevels ---------------------------------------------------------------
 bool roomForWindow(){if(extWindows.size()+wlrWindows.size()<maximumToplevels)return true;toplevelOverflow=true;return false;}
 static void extToplevel(void* data,ext_foreign_toplevel_list_v1*,ext_foreign_toplevel_handle_v1* handle){
  auto* self=static_cast<Impl*>(data);if(!self->roomForWindow()){ext_foreign_toplevel_handle_v1_destroy(handle);return;}
  auto* window=new ExtWindow;window->owner=self;window->native=handle;self->extWindows.append(window);
  static const ext_foreign_toplevel_handle_v1_listener listener={
   [](void* data,ext_foreign_toplevel_handle_v1*){auto* w=static_cast<ExtWindow*>(data);auto* owner=w->owner;owner->extWindows.removeOne(w);ext_foreign_toplevel_handle_v1_destroy(w->native);delete w;owner->notify();},
   [](void* data,ext_foreign_toplevel_handle_v1*){auto* w=static_cast<ExtWindow*>(data);w->current=w->pending;w->done=true;w->owner->notify();},
   [](void* data,ext_foreign_toplevel_handle_v1*,const char* value){static_cast<ExtWindow*>(data)->pending.title=boundedText(value);},
   [](void* data,ext_foreign_toplevel_handle_v1*,const char* value){static_cast<ExtWindow*>(data)->pending.appId=boundedText(value);},
   [](void* data,ext_foreign_toplevel_handle_v1*,const char* value){static_cast<ExtWindow*>(data)->pending.identifier=boundedText(value);}};
  ext_foreign_toplevel_handle_v1_add_listener(handle,&listener,window);
 }
 static void extFinished(void* data,ext_foreign_toplevel_list_v1*){auto* self=static_cast<Impl*>(data);if(self->extList){ext_foreign_toplevel_list_v1_destroy(self->extList);self->extList=nullptr;}self->versions.remove(ext_foreign_toplevel_list_v1_interface.name);self->notify();}
 static void wlrToplevel(void* data,zwlr_foreign_toplevel_manager_v1*,zwlr_foreign_toplevel_handle_v1* handle){
  auto* self=static_cast<Impl*>(data);if(!self->roomForWindow()){zwlr_foreign_toplevel_handle_v1_destroy(handle);return;}
  auto* window=new WlrWindow;window->owner=self;window->native=handle;self->wlrWindows.append(window);
  static const zwlr_foreign_toplevel_handle_v1_listener listener={
   [](void* data,zwlr_foreign_toplevel_handle_v1*,const char* value){static_cast<WlrWindow*>(data)->pending.title=boundedText(value);},
   [](void* data,zwlr_foreign_toplevel_handle_v1*,const char* value){static_cast<WlrWindow*>(data)->pending.appId=boundedText(value);},
   [](void*,zwlr_foreign_toplevel_handle_v1*,wl_output*){},[](void*,zwlr_foreign_toplevel_handle_v1*,wl_output*){},
   [](void* data,zwlr_foreign_toplevel_handle_v1*,wl_array* state){
    auto* w=static_cast<WlrWindow*>(data);w->pending.activated=false;
    const auto* values=static_cast<const uint32_t*>(state->data);
    for(size_t i=0;i<state->size/sizeof(uint32_t);++i)if(values[i]==ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_ACTIVATED)w->pending.activated=true;
   },
   [](void* data,zwlr_foreign_toplevel_handle_v1*){auto* w=static_cast<WlrWindow*>(data);w->current=w->pending;w->done=true;w->owner->notify();},
   [](void* data,zwlr_foreign_toplevel_handle_v1*){auto* w=static_cast<WlrWindow*>(data);auto* owner=w->owner;owner->wlrWindows.removeOne(w);zwlr_foreign_toplevel_handle_v1_destroy(w->native);delete w;owner->notify();},
   [](void*,zwlr_foreign_toplevel_handle_v1*,zwlr_foreign_toplevel_handle_v1*){}};
  zwlr_foreign_toplevel_handle_v1_add_listener(handle,&listener,window);
 }
 static void wlrFinished(void* data,zwlr_foreign_toplevel_manager_v1*){auto* self=static_cast<Impl*>(data);if(self->wlrList){zwlr_foreign_toplevel_manager_v1_destroy(self->wlrList);self->wlrList=nullptr;}self->versions.remove(zwlr_foreign_toplevel_manager_v1_interface.name);self->notify();}
 QList<Toplevel> extToplevels()const{QList<Toplevel> out;for(const auto* w:extWindows)if(w->done)out.append(w->current);return out;}
 QList<Toplevel> wlrToplevels()const{QList<Toplevel> out;for(const auto* w:wlrWindows)if(w->done)out.append(w->current);return out;}
 // Availability ------------------------------------------------------------
 QString outputReason()const{
  if(!display)return error.isEmpty()?QStringLiteral("Not connected to the Wayland compositor"):error;
  if(!connected)return QStringLiteral("Waiting for the compositor's globals");
  if(!copy)return QStringLiteral("The compositor does not offer ext-image-copy-capture-v1");
  if(!outputSources)return QStringLiteral("The compositor does not offer ext-output-image-capture-source-manager-v1");
  if(!shm)return QStringLiteral("The compositor does not offer wl_shm");
  if(outputs.isEmpty())return versions.contains("wl_output")?QStringLiteral("Output capture needs wl_output version 4 for output names"):QStringLiteral("Waiting for compositor outputs");
  // wl_output.name follows the bind, a round trip after the initial sync; until then captureOutput can't find the output.
  for(const auto* output:outputs)if(output->name.isEmpty())return QStringLiteral("Waiting for compositor outputs");
  return {};
 }
 QString windowReason()const{
  if(!display||!connected||!copy||!shm)return outputReason().isEmpty()?QStringLiteral("The compositor does not offer ext-image-copy-capture-v1"):outputReason();
  if(!windowSources)return QStringLiteral("Window capture needs ext-foreign-toplevel-image-capture-source-manager-v1, which this compositor does not offer (labwc 0.20 or later and Mir 2.29 or later have it)");
  if(!extList)return QStringLiteral("Window capture needs ext-foreign-toplevel-list-v1");
  if(!wlrList)return QStringLiteral("Finding the active window needs zwlr-foreign-toplevel-management-v1");
  return {};
 }
 // Capture -----------------------------------------------------------------
 void cancelAll(const Error& reason){
  cancelling=true;
  const auto pending=requests.values();for(auto* request:pending)finish(request,{},reason);
  // Frames being turned upright answer with this reason (cancelled, or a lost connection) when their worker ends.
  for(const auto id:std::as_const(turning))turnedAway.insert(id,reason);turning.clear();
  cancelling=false;
  for(auto* freeze:freezes){if(freeze->callback)wl_callback_destroy(freeze->callback);auto reply=std::move(freeze->reply);delete freeze;if(reply)reply({},reason);}freezes.clear();
 }
 void release(Request* request){
  if(request->frame){ext_image_copy_capture_frame_v1_destroy(request->frame);request->frame=nullptr;}
  if(request->buffer){wl_buffer_destroy(request->buffer);request->buffer=nullptr;}
  if(request->pool){wl_shm_pool_destroy(request->pool);request->pool=nullptr;}
  if(request->session){ext_image_copy_capture_session_v1_destroy(request->session);request->session=nullptr;}
  if(request->source){ext_image_capture_source_v1_destroy(request->source);request->source=nullptr;}
  if(request->map!=MAP_FAILED){munmap(request->map,request->mapSize);request->map=MAP_FAILED;}
  if(request->timer){request->timer->stop();request->timer->deleteLater();request->timer=nullptr;}
  flush();
 }
 void finish(Request* request,QImage image,Error failure){
  requests.remove(request->id);release(request);auto reply=std::move(request->reply);delete request;
  if(reply)reply(std::move(image),std::move(failure));
 }
 void start(bool window,const QString& target,const ResourceLimits& limits,Reply reply){
  events();
  const auto reason=window?windowReason():outputReason();
  if(!reason.isEmpty()){reply({},{window?"window_unavailable":"capture_unavailable",reason,{}});return;}
  ext_image_capture_source_v1* source=nullptr;
  if(window){
   ExtWindow* found=nullptr;for(auto* w:std::as_const(extWindows))if(w->done&&w->current.identifier==target)found=w;
   if(!found){reply({},{"target_unavailable","The window closed or is no longer listed",{}});return;}
   source=ext_foreign_toplevel_image_capture_source_manager_v1_create_source(windowSources,found->native);
  }else{
   Output* found=nullptr;for(auto* o:std::as_const(outputs))if(o->name==target)found=o;
   if(!found){reply({},{"display_changed","The output is no longer present",{}});return;}
   source=ext_output_image_capture_source_manager_v1_create_source(outputSources,found->native);
  }
  auto* request=new Request;request->owner=this;request->id=++nextId;request->window=window;request->target=target;request->limits=limits;request->reply=std::move(reply);request->source=source;
  requests.insert(request->id,request);
  request->timer=new QTimer(this);request->timer->setSingleShot(true);
  connect(request->timer,&QTimer::timeout,this,[this,id=request->id]{if(auto* r=requests.value(id))finish(r,{},{"capture_timeout","The compositor did not deliver the frame in time",{}});});
  request->timer->start(captureTimeoutMs);
  // Screenshots never paint the cursor.
  request->session=ext_image_copy_capture_manager_v1_create_session(copy,source,0);
  static const ext_image_copy_capture_session_v1_listener listener={
   [](void* data,ext_image_copy_capture_session_v1*,uint32_t width,uint32_t height){auto* r=static_cast<Request*>(data);r->size=QSize(int(std::min<uint32_t>(width,65536)),int(std::min<uint32_t>(height,65536)));},
   [](void* data,ext_image_copy_capture_session_v1*,uint32_t format){auto* r=static_cast<Request*>(data);if(r->formats.size()<64)r->formats.append(format);},
   [](void*,ext_image_copy_capture_session_v1*,wl_array*){},[](void*,ext_image_copy_capture_session_v1*,uint32_t,wl_array*){},
   [](void* data,ext_image_copy_capture_session_v1*){auto* r=static_cast<Request*>(data);r->batchFormats=r->formats;r->batchSize=r->size;r->batchReady=true;r->formats.clear();r->owner->constraints(r);},
   [](void* data,ext_image_copy_capture_session_v1*){auto* r=static_cast<Request*>(data);r->owner->finish(r,{},{r->window?"target_unavailable":"capture_failed",r->window?"The window closed during capture":"The compositor stopped the capture",{}});}};
  ext_image_copy_capture_session_v1_add_listener(request->session,&listener,request);
  flush();
 }
 void constraints(Request* request){
  // A batch that ends while a frame is in flight is kept; that frame's buffer_constraints failure uses it.
  if(request->frame||!request->batchReady)return;
  request->batchReady=false;
  uint32_t chosen=0;QImage::Format format=QImage::Format_Invalid;
  for(const auto preferred:{uint32_t(WL_SHM_FORMAT_XRGB8888),uint32_t(WL_SHM_FORMAT_ARGB8888),uint32_t(WL_SHM_FORMAT_XBGR8888),uint32_t(WL_SHM_FORMAT_ABGR8888)})
   if(request->batchFormats.contains(preferred)){chosen=preferred;format=shmImageFormat(preferred);break;}
  if(format==QImage::Format_Invalid){finish(request,{},{"capture_unavailable","The compositor offers no 32-bit shared-memory capture format",{}});return;}
  const auto width=request->batchSize.width(),height=request->batchSize.height();
  if(width<1||height<1){finish(request,{},{"capture_failed","The compositor reported an empty capture buffer",{}});return;}
  if(qint64(width)*height>request->limits.imagePixels){finish(request,{},{"input_excessive","Capture exceeds accepted operation resource limits",{}});return;}
  const int stride=width*4;const auto bytes=size_t(stride)*size_t(height);
  if(qint64(bytes)>request->limits.imageBytes){finish(request,{},{"input_excessive","Capture exceeds accepted operation resource limits",{}});return;}
  if(request->map!=MAP_FAILED){munmap(request->map,request->mapSize);request->map=MAP_FAILED;}
  if(request->buffer){wl_buffer_destroy(request->buffer);request->buffer=nullptr;}
  if(request->pool){wl_shm_pool_destroy(request->pool);request->pool=nullptr;}
  const int fd=int(::syscall(SYS_memfd_create,"putgd-capture",MFD_CLOEXEC));
  if(fd<0||ftruncate(fd,off_t(bytes))<0){if(fd>=0)close(fd);finish(request,{},{"capture_failed","Cannot allocate the capture buffer",{}});return;}
  request->map=mmap(nullptr,bytes,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);request->mapSize=bytes;request->stride=stride;request->imageFormat=format;request->allocated=QSize(width,height);
  if(request->map==MAP_FAILED){close(fd);finish(request,{},{"capture_failed","Cannot map the capture buffer",{}});return;}
  request->pool=wl_shm_create_pool(shm,fd,int32_t(bytes));close(fd);
  request->buffer=wl_shm_pool_create_buffer(request->pool,0,width,height,stride,chosen);
  request->frame=ext_image_copy_capture_session_v1_create_frame(request->session);
  static const ext_image_copy_capture_frame_v1_listener listener={
   [](void* data,ext_image_copy_capture_frame_v1*,uint32_t transform){static_cast<Request*>(data)->transform=transform<=WL_OUTPUT_TRANSFORM_FLIPPED_270?transform:WL_OUTPUT_TRANSFORM_NORMAL;},
   [](void*,ext_image_copy_capture_frame_v1*,int32_t,int32_t,int32_t,int32_t){},
   [](void*,ext_image_copy_capture_frame_v1*,uint32_t,uint32_t,uint32_t){},
   [](void* data,ext_image_copy_capture_frame_v1*){auto* r=static_cast<Request*>(data);r->owner->ready(r,r->imageFormat);},
   [](void* data,ext_image_copy_capture_frame_v1*,uint32_t reason){auto* r=static_cast<Request*>(data);r->owner->failed(r,reason);}};
  ext_image_copy_capture_frame_v1_add_listener(request->frame,&listener,request);
  ext_image_copy_capture_frame_v1_attach_buffer(request->frame,request->buffer);
  ext_image_copy_capture_frame_v1_damage_buffer(request->frame,0,0,width,height);
  ext_image_copy_capture_frame_v1_capture(request->frame);
  flush();
 }
 void failed(Request* request,uint32_t reason){
  if(reason==EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_BUFFER_CONSTRAINTS&&request->retries<maximumConstraintRetries){
   // Fresh constraints and done follow; if they already arrived during the frame, allocate from them now.
   ++request->retries;ext_image_copy_capture_frame_v1_destroy(request->frame);request->frame=nullptr;constraints(request);return;
  }
  if(reason==EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_STOPPED){finish(request,{},{request->window?"target_unavailable":"capture_failed",request->window?"The window closed during capture":"The compositor stopped the capture",{}});return;}
  finish(request,{},{"capture_failed","The compositor could not copy the frame",{{"reason",int(reason)}}});
 }
 void ready(Request* request,QImage::Format format){
  // The image owns the mapping from here; every protocol object goes now.
  void* map=request->map;const size_t size=request->mapSize;request->map=MAP_FAILED;
  struct Mapping {void* address;size_t size;};auto* mapping=new Mapping{map,size};
  // The allocated size, not a newer announcement: the mapping is that big (review #1275).
  QImage view(static_cast<uchar*>(map),request->allocated.width(),request->allocated.height(),request->stride,format,[](void* info){auto* m=static_cast<Mapping*>(info);munmap(m->address,m->size);delete m;},mapping);
  const auto transform=request->transform;const auto id=request->id;
  requests.remove(id);auto reply=std::move(request->reply);release(request);delete request;
  if(view.isNull()){reply({},{"capture_failed","Cannot read the capture buffer",{}});return;}
  if(transform==WL_OUTPUT_TRANSFORM_NORMAL){reply(std::move(view),{});return;}
  // Turning a frame upright copies it: do that off the session thread.
  auto result=std::make_shared<QImage>();turning.insert(id);
  auto* worker=QThread::create([view=std::move(view),transform,result]{*result=waylandUpright(view,transform);});workers.append(worker);
  connect(worker,&QThread::finished,this,[this,worker,result,id,reply=std::move(reply)]{
   workers.removeOne(worker);worker->deleteLater();
   if(!turning.remove(id)){reply({},turnedAway.take(id));return;}
   if(result->isNull())reply({},{"capture_failed","Cannot turn the capture upright",{}});else reply(std::move(*result),{});});
  worker->start();
 }
 void freeze(std::function<void(Toplevel,Error)> reply){
  events();
  const auto reason=windowReason();if(!reason.isEmpty()){reply({},{"window_unavailable",reason,{}});return;}
  auto* item=new Freeze;item->owner=this;item->reply=std::move(reply);item->callback=wl_display_sync(display);freezes.append(item);
  static const wl_callback_listener listener={[](void* data,wl_callback* callback,uint32_t){
   auto* item=static_cast<Freeze*>(data);auto* self=item->owner;wl_callback_destroy(callback);item->callback=nullptr;self->freezes.removeOne(item);
   auto reply=std::move(item->reply);delete item;
   Error error;const auto match=matchActiveToplevel(self->extToplevels(),self->wlrToplevels(),error);
   if(reply)reply(match?*match:Toplevel{},error);
  }};
  wl_callback_add_listener(item->callback,&listener,item);flush();
 }
};
ImageCopyCapture::ImageCopyCapture(QObject* parent):QObject(parent),d_(std::make_unique<Impl>(*this)){}
ImageCopyCapture::~ImageCopyCapture()=default;
bool ImageCopyCapture::outputsAvailable(QString* reason)const{d_->events();const auto why=d_->outputReason();if(reason)*reason=why;return why.isEmpty();}
bool ImageCopyCapture::windowsAvailable(QString* reason)const{d_->events();const auto why=d_->windowReason();if(reason)*reason=why;return why.isEmpty();}
QList<ImageCopyCapture::Toplevel> ImageCopyCapture::toplevels()const{
 d_->events();auto list=d_->extToplevels();
 Error error;const auto active=matchActiveToplevel(list,d_->wlrToplevels(),error);
 for(auto& window:list)window.activated=active&&window.identifier==active->identifier;
 return list;
}
QJsonObject ImageCopyCapture::status()const{
 d_->events();QJsonObject protocols;for(auto it=d_->versions.cbegin();it!=d_->versions.cend();++it)protocols[it.key()]=it.value();
 const auto output=d_->outputReason(),window=d_->windowReason();
 QJsonObject result{{"available",output.isEmpty()},{"protocol","ext-image-copy-capture-v1"},{"protocols",protocols},
  {"outputs",QJsonObject{{"available",output.isEmpty()},{"error",output}}},{"windows",QJsonObject{{"available",window.isEmpty()},{"error",window},{"listed",int(d_->extToplevels().size())}}}};
 if(d_->toplevelOverflow)result["toplevelOverflow"]=true;
 if(!output.isEmpty())result["error"]=output;
 return result;
}
void ImageCopyCapture::freezeActive(std::function<void(Toplevel,Error)> reply){d_->freeze(std::move(reply));}
void ImageCopyCapture::captureOutput(const QString& name,const ResourceLimits& limits,Reply reply){d_->start(false,name,limits,std::move(reply));}
void ImageCopyCapture::captureToplevel(const QString& identifier,const ResourceLimits& limits,Reply reply){d_->start(true,identifier,limits,std::move(reply));}
void ImageCopyCapture::cancel(){d_->cancelAll({"cancelled","Image capture cancelled",{}});}
const CaptureConnection& ImageCopyCapture::connection() const{return d_->connection;}
}
