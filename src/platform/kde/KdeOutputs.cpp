#include "KdeOutputs.h"
#include "platform/NativeSession.h"
#include "xdg-output-unstable-v1-client-protocol.h"
#include <wayland-client.h>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QSocketNotifier>
#include <QTimer>
#include <QMap>
#include <QSet>
#include <poll.h>
#include <cstring>
#include <cerrno>
#include <cmath>
#include <algorithm>

namespace putgd {
namespace {
constexpr int maximumOutputs=16;
constexpr int maximumDimension=65536;
constexpr int maximumCoordinate=1000000;
bool sameFrames(const QList<CaptureFrame>& a,const QList<CaptureFrame>& b){
 if(a.size()!=b.size())return false;
 for(qsizetype i=0;i<a.size();++i){const auto& x=a[i];const auto& y=b[i];
  if(x.identity!=y.identity||x.logicalRect!=y.logicalRect||x.nativeRect!=y.nativeRect||x.density!=y.density||x.rotation!=y.rotation)return false;
 }
 return true;
}
bool validName(const QString& name){
 if(name.isEmpty()||name.toUtf8().size()>256)return false;
 for(const auto c:name)if(c.isNull()||c.isSpace()||c.category()==QChar::Other_Control)return false;
 return true;
}
}
QList<CaptureFrame> KdeOutputs::framesFromMetadata(const QList<Metadata>& metadata,QString* error){
 if(error)error->clear();
 const auto fail=[error](const char* reason){if(error)*error=QString::fromLatin1(reason);return QList<CaptureFrame>{};};
 if(metadata.isEmpty()||metadata.size()>maximumOutputs)return fail("No usable bounded output layout is available");
 QList<CaptureFrame> frames;QSet<QString> names;
 for(const auto& output:metadata){
  if(!validName(output.name)||names.contains(output.name))return fail("Output names are missing, invalid, or duplicated");
  const auto rect=output.logical;
  if(rect.width()<1||rect.height()<1||rect.width()>maximumDimension||rect.height()>maximumDimension||
     std::abs(qint64(rect.x()))>maximumCoordinate||std::abs(qint64(rect.y()))>maximumCoordinate||
     std::abs(qint64(rect.x())+rect.width())>maximumCoordinate||std::abs(qint64(rect.y())+rect.height())>maximumCoordinate||
     output.mode.width()<1||output.mode.height()<1||output.mode.width()>maximumDimension||output.mode.height()>maximumDimension||
     output.scale<1||output.scale>16||output.transform<WL_OUTPUT_TRANSFORM_NORMAL||output.transform>WL_OUTPUT_TRANSFORM_FLIPPED_270)
   return fail("Output geometry, scale, or transform is invalid");
  QSize native=output.mode;
  if(output.transform%2)native.transpose();
  const double density=double(native.width())/rect.width();
  if(!std::isfinite(density)||density<=0||density>16)return fail("Output density is outside supported bounds");
  // Fractional scaling rounds logical dimensions independently. Never replace
  // the compositor logical rectangle with mode / integer wl_output.scale.
  CaptureFrame frame;frame.identity=output.name;frame.logicalRect=rect;frame.density=density;
  frame.nativeRect=QRect(QPoint(qRound(rect.x()*density),qRound(rect.y()*density)),native);
  frame.rotation=(output.transform%4)*90;frames.append(frame);names.insert(output.name);
 }
 std::sort(frames.begin(),frames.end(),[](const auto& a,const auto& b){return a.identity<b.identity;});
 return frames;
}
struct KdeOutputs::Impl:QObject {
 struct Output {
  Impl* owner=nullptr;uint32_t global=0;wl_output* native=nullptr;zxdg_output_v1* logical=nullptr;
  Metadata pending;bool position=false,size=false,mode=false,wlDone=false,xdgDone=false,coherent=false,invalid=false;
 };
 KdeOutputs& q;wl_display* display=nullptr;wl_registry* registry=nullptr;zxdg_output_manager_v1* manager=nullptr;
 uint32_t managerName=0,managerVersion=0;wl_callback* initialSync=nullptr;
 QSocketNotifier* readNotifier=nullptr;QSocketNotifier* writeNotifier=nullptr;QTimer reconnect;
 QMap<uint32_t,Output*> outputs;QSet<uint32_t> overflowOutputs,unsupportedOutputs;bool overflowUntracked=false;
 QList<CaptureFrame> current;quint64 epoch=0;QString error="Native output discovery has not connected";
 bool dispatching=false,lossPending=false,closing=false;
 explicit Impl(KdeOutputs& q):q(q){
  reconnect.setInterval(1000);connect(&reconnect,&QTimer::timeout,this,[this]{if(!display)open();});
  if(!qobject_cast<QGuiApplication*>(QCoreApplication::instance())&&kdeCoreSessionEnvironment()){open();reconnect.start();}
  else error="Native output discovery is reserved for the KDE Core session";
 }
 ~Impl() override{closing=true;reconnect.stop();reset();}
 void publish(const QString& connectionError={}){
  QList<Metadata> complete;QString nextError;
  if(!display)nextError=connectionError.isEmpty()?QStringLiteral("Native output connection is unavailable"):connectionError;
  else if(!manager)nextError="Compositor lacks named xdg-output support";
  else if(!unsupportedOutputs.isEmpty())nextError="Compositor output protocol lacks wl_output v2 batch completion";
  else if((!overflowOutputs.isEmpty()||overflowUntracked))nextError="Compositor output count exceeds supported bounds";
  else if(outputs.isEmpty())nextError="Waiting for compositor outputs";
  else for(const auto* output:outputs){
   if(output->invalid){nextError="Compositor output metadata is invalid";break;}
   if(!output->coherent){nextError=managerVersion<3?"Waiting for fresh native and logical output completion (xdg-output v2)":"Waiting for a completed compositor output batch";break;}
   complete.append(output->pending);
  }
  auto next=nextError.isEmpty()?framesFromMetadata(complete,&nextError):QList<CaptureFrame>{};
  const bool changed=!sameFrames(current,next)||error!=nextError;
  current=std::move(next);error=std::move(nextError);
  if(changed){++epoch;emit q.changed();}
 }
 void dirty(Output* output){
  // Invalidate at the first changed property, before done. An output that
  // changes away and back must still invalidate an invocation-time snapshot.
  if(output->coherent){
   // v2 has no atomic event shared by the two protocols. Discard BOTH old
   // completion flags on the first actual change. Fresh done events from both
   // channels are required in either order; a compositor that updates only one
   // channel remains unavailable rather than publishing guessed geometry.
   // v3 uses the protocol-defined atomic wl_output.done instead.
   if(managerVersion<3)output->wlDone=output->xdgDone=false;
   output->coherent=false;++epoch;publish();
  }
 }
 void commit(Output* output){
  output->coherent=output->logical&&output->position&&output->size&&output->mode&&output->wlDone&&output->xdgDone&&!output->invalid;
  publish();
 }
 static void geometry(void* data,wl_output*,int32_t,int32_t,int32_t,int32_t,int32_t,const char*,const char*,int32_t transform){
  auto* output=static_cast<Output*>(data);output->wlDone=false;if(output->pending.transform!=transform)output->owner->dirty(output);output->pending.transform=transform;
 }
 static void mode(void* data,wl_output*,uint32_t flags,int32_t width,int32_t height,int32_t){
  if(!(flags&WL_OUTPUT_MODE_CURRENT))return;
  auto* output=static_cast<Output*>(data);output->wlDone=false;
  if(!output->mode||output->pending.mode!=QSize(width,height))output->owner->dirty(output);
  output->mode=true;output->pending.mode=QSize(width,height);
 }
 static void done(void* data,wl_output*){
  auto* output=static_cast<Output*>(data);output->wlDone=true;
  // Version 3 makes wl_output.done the atomic completion event for both
  // protocols. The deprecated xdg done event is deliberately ignored there.
  if(output->logical&&zxdg_output_v1_get_version(output->logical)>=3)output->xdgDone=true;
  output->owner->commit(output);
 }
 static void scale(void* data,wl_output*,int32_t factor){auto* output=static_cast<Output*>(data);output->wlDone=false;if(output->pending.scale!=factor)output->owner->dirty(output);output->pending.scale=factor;}
 static void name(Output* output,const char* value){
  const auto length=strnlen(value,257);const auto bytes=QByteArray(value,int(std::min(size_t(256),length)));const auto text=QString::fromUtf8(bytes);
  if(length>256||text.toUtf8()!=bytes||!validName(text)||(!output->pending.name.isEmpty()&&output->pending.name!=text)){
   output->owner->dirty(output);output->invalid=true;output->owner->publish();return;
  }
  if(output->pending.name!=text)output->owner->dirty(output);
  output->pending.name=text;
 }
 static void nativeName(void* data,wl_output*,const char* value){auto* output=static_cast<Output*>(data);output->wlDone=false;name(output,value);}
 static void description(void*,wl_output*,const char*){}
 static void logicalPosition(void* data,zxdg_output_v1*,int32_t x,int32_t y){
  auto* output=static_cast<Output*>(data);output->xdgDone=false;if(zxdg_output_v1_get_version(output->logical)>=3)output->wlDone=false;if(!output->position||output->pending.logical.topLeft()!=QPoint(x,y))output->owner->dirty(output);
  output->position=true;output->pending.logical.moveTopLeft(QPoint(x,y));
 }
 static void logicalSize(void* data,zxdg_output_v1*,int32_t width,int32_t height){
  auto* output=static_cast<Output*>(data);output->xdgDone=false;if(zxdg_output_v1_get_version(output->logical)>=3)output->wlDone=false;if(!output->size||output->pending.logical.size()!=QSize(width,height))output->owner->dirty(output);
  output->size=true;output->pending.logical.setSize(QSize(width,height));
 }
 static void logicalDone(void* data,zxdg_output_v1* logical){
  auto* output=static_cast<Output*>(data);if(zxdg_output_v1_get_version(logical)>=3)return;
  output->xdgDone=true;output->owner->commit(output);
 }
 static void logicalName(void* data,zxdg_output_v1*,const char* value){auto* output=static_cast<Output*>(data);output->xdgDone=false;if(zxdg_output_v1_get_version(output->logical)>=3)output->wlDone=false;name(output,value);}
 static void logicalDescription(void*,zxdg_output_v1*,const char*){}
 void makeLogical(Output* output){
  if(!manager||output->logical)return;
  dirty(output);output->position=output->size=output->xdgDone=false;if(managerVersion>=3)output->wlDone=false;
  output->logical=zxdg_output_manager_v1_get_xdg_output(manager,output->native);
  static const zxdg_output_v1_listener listener={logicalPosition,logicalSize,logicalDone,logicalName,logicalDescription};
  zxdg_output_v1_add_listener(output->logical,&listener,output);
 }
 static void global(void* data,wl_registry* registry,uint32_t id,const char* interface,uint32_t version){
  auto* self=static_cast<Impl*>(data);
  if(QByteArray(interface)==zxdg_output_manager_v1_interface.name&&!self->manager&&version>=2){
   self->managerVersion=std::min(version,uint32_t(3));self->managerName=id;
   self->manager=static_cast<zxdg_output_manager_v1*>(wl_registry_bind(registry,id,&zxdg_output_manager_v1_interface,self->managerVersion));
   for(auto* output:self->outputs)self->makeLogical(output);
  }else if(QByteArray(interface)==wl_output_interface.name){
   // wl_output v2 is needed for atomic mode/transform/scale completion.
   if(version<2||self->outputs.size()>=maximumOutputs){
    auto& rejected=version<2?self->unsupportedOutputs:self->overflowOutputs;
    if(self->overflowOutputs.size()+self->unsupportedOutputs.size()<maximumOutputs)rejected.insert(id);else self->overflowUntracked=true;
    self->publish();return;
   }
   auto* output=new Output;output->owner=self;output->global=id;
   output->native=static_cast<wl_output*>(wl_registry_bind(registry,id,&wl_output_interface,std::min(version,uint32_t(4))));self->outputs.insert(id,output);
   static const wl_output_listener listener={geometry,mode,done,scale,nativeName,description};
   wl_output_add_listener(output->native,&listener,output);self->makeLogical(output);
  }
  self->publish();
 }
 void destroyOutput(Output* output){
  if(output->logical)zxdg_output_v1_destroy(output->logical);
  if(wl_output_get_version(output->native)>=3)wl_output_release(output->native);else wl_output_destroy(output->native);
  delete output;
 }
 static void removed(void* data,wl_registry*,uint32_t id){
  auto* self=static_cast<Impl*>(data);
  if(auto* output=self->outputs.take(id)){self->destroyOutput(output);++self->epoch;self->publish();}
  const bool removedUnsupported=self->unsupportedOutputs.remove(id);
  if(self->overflowOutputs.remove(id)||removedUnsupported)self->publish();
  if(id==self->managerName){
   for(auto* output:self->outputs){self->dirty(output);if(output->logical)zxdg_output_v1_destroy(output->logical);output->logical=nullptr;output->xdgDone=false;output->coherent=false;}
   zxdg_output_manager_v1_destroy(self->manager);self->manager=nullptr;self->managerName=self->managerVersion=0;self->publish();
  }
 }
 void open(){
  if(closing||display)return;
  display=wl_display_connect(nullptr);
  if(!display){publish(QStringLiteral("Cannot connect to the Wayland compositor for output discovery"));return;}
  registry=wl_display_get_registry(display);static const wl_registry_listener listener={global,removed};wl_registry_add_listener(registry,&listener,this);
  const auto fd=wl_display_get_fd(display);readNotifier=new QSocketNotifier(fd,QSocketNotifier::Read,this);writeNotifier=new QSocketNotifier(fd,QSocketNotifier::Write,this);writeNotifier->setEnabled(false);
  connect(readNotifier,&QSocketNotifier::activated,this,[this]{events();});connect(writeNotifier,&QSocketNotifier::activated,this,[this]{flush();});
  initialSync=wl_display_sync(display);static const wl_callback_listener synced={[](void* data,wl_callback* callback,uint32_t){auto* self=static_cast<Impl*>(data);wl_callback_destroy(callback);self->initialSync=nullptr;self->publish();}};
  wl_callback_add_listener(initialSync,&synced,this);flush();
 }
 void flush(){if(!display)return;if(wl_display_flush(display)<0){if(errno==EAGAIN){if(writeNotifier)writeNotifier->setEnabled(true);}else lost();}else if(writeNotifier)writeNotifier->setEnabled(false);}
 void events(){
  if(!display||dispatching)return;
  dispatching=true;
  if(wl_display_dispatch_pending(display)<0){dispatching=false;lost();return;}
  pollfd readiness{wl_display_get_fd(display),POLLIN,0};const int ready=poll(&readiness,1,0);
  if(ready<0&&errno!=EINTR){dispatching=false;lost();return;}
  if(ready>0){
   while(wl_display_prepare_read(display)!=0)if(wl_display_dispatch_pending(display)<0){dispatching=false;lost();return;}
   // Only this connection is read; no Qt/QPA event queue is dispatched.
   if(wl_display_read_events(display)<0||wl_display_dispatch_pending(display)<0){dispatching=false;lost();return;}
  }
  dispatching=false;flush();
 }
 void reset(){
  // Loss can occur inside activated(). Disable and detach immediately, but
  // retire QObject senders after their current event dispatch returns.
  if(readNotifier){auto* retired=readNotifier;readNotifier=nullptr;retired->setEnabled(false);retired->disconnect(this);retired->deleteLater();}
  if(writeNotifier){auto* retired=writeNotifier;writeNotifier=nullptr;retired->setEnabled(false);retired->disconnect(this);retired->deleteLater();}
  if(initialSync){wl_callback_destroy(initialSync);initialSync=nullptr;}
  for(auto* output:outputs)destroyOutput(output);
  outputs.clear();overflowOutputs.clear();unsupportedOutputs.clear();overflowUntracked=false;
  if(manager){zxdg_output_manager_v1_destroy(manager);manager=nullptr;}
  if(registry){wl_registry_destroy(registry);registry=nullptr;}if(display){wl_display_disconnect(display);display=nullptr;}
  managerName=managerVersion=0;
 }
 void lost(){
  if(dispatching){if(!lossPending){lossPending=true;QTimer::singleShot(0,this,[this]{lossPending=false;lost();});}return;}
  reset();++epoch;publish();
 }
};
KdeOutputs::KdeOutputs(QObject* parent):QObject(parent),d_(std::make_unique<Impl>(*this)){}
KdeOutputs::~KdeOutputs()=default;
QList<CaptureFrame> KdeOutputs::layout()const{d_->events();return d_->current;}
quint64 KdeOutputs::revision()const{d_->events();return d_->epoch;}
QJsonObject KdeOutputs::status()const{d_->events();return {{"available",!d_->current.isEmpty()},{"protocol","wl-output+xdg-output"},{"protocol_version",int(d_->managerVersion)},{"outputs",d_->current.size()},{"revision",qint64(d_->epoch)},{"error",d_->error}};}
}
