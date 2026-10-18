#include "DataControlClipboard.h"
#include "KdeKeyboardMap.h"
#include <xkbcommon/xkbcommon.h>
#include "KWinTransfer.h"
#include "KdeClipboardFormats.h"
#include "core/ImageFormat.h"
#include "core/ImageJob.h"
#include "platform/NativeSession.h"
#include <QCoreApplication>
#include "ext-data-control-v1-client-protocol.h"
#include "wlr-data-control-unstable-v1-client-protocol.h"
#include <wayland-client.h>
#include <QGuiApplication>
#include <QSocketNotifier>
#include <QImageReader>
#include <QBuffer>
#include <QUrl>
#include <QQueue>
#include <QPointer>
#include <QThread>
#include <QTimer>
#include <QJsonArray>
#include <QUuid>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <pthread.h>
#include <cerrno>
namespace putgd {
namespace {
PayloadResult failure(const char* code,const char* message){return {{},{QString::fromLatin1(code),QString::fromLatin1(message),{}}};}
// Clipboard receiver FDs are pipes. Mask SIGPIPE only around our own write and
// consume only a signal we generated, preserving the caller's prior signal state.
ssize_t safeWrite(int fd,const char* data,size_t size){sigset_t block,prior,pending;sigemptyset(&block);sigaddset(&block,SIGPIPE);pthread_sigmask(SIG_BLOCK,&block,&prior);sigpending(&pending);const bool already=sigismember(&pending,SIGPIPE);const auto n=write(fd,data,size);const int saved=errno;if(n<0&&saved==EPIPE&&!already){timespec zero{};sigtimedwait(&block,nullptr,&zero);}pthread_sigmask(SIG_SETMASK,&prior,nullptr);errno=saved;return n;}
class FdWriter final:public QObject {
 int fd_;QByteArray bytes_;qsizetype offset_=0;QSocketNotifier* notifier_;QTimer timer_;
public:
 FdWriter(int fd,QByteArray bytes,QObject* parent):QObject(parent),fd_(fd),bytes_(std::move(bytes)),notifier_(new QSocketNotifier(fd,QSocketNotifier::Write,this)){
  const int flags=fcntl(fd_,F_GETFL);if(flags<0||fcntl(fd_,F_SETFL,flags|O_NONBLOCK)<0){close(fd_);fd_=-1;notifier_->setEnabled(false);deleteLater();return;}
  connect(notifier_,&QSocketNotifier::activated,this,[this]{pump();});timer_.setSingleShot(true);connect(&timer_,&QTimer::timeout,this,[this]{finish();});timer_.start(5000);QTimer::singleShot(0,this,[this]{pump();});
 }
 ~FdWriter() override{if(fd_>=0)close(fd_);}
 void pump(){if(fd_<0)return;for(int i=0;i<16&&offset_<bytes_.size();++i){const auto n=safeWrite(fd_,bytes_.constData()+offset_,std::min<qsizetype>(65536,bytes_.size()-offset_));if(n>0)offset_+=n;else if(n<0&&(errno==EAGAIN||errno==EWOULDBLOCK))return;else if(n<0&&errno==EINTR){--i;continue;}else{finish();return;}}if(offset_==bytes_.size())finish();}
 void finish(){timer_.stop();notifier_->setEnabled(false);if(fd_>=0){close(fd_);fd_=-1;}deleteLater();}
};
}
struct DataControlClipboard::Impl:QObject {
 DataControlClipboard& q;wl_display* display=nullptr;wl_callback* initialSync=nullptr;wl_registry* registry=nullptr;wl_seat* seat=nullptr;wl_keyboard* keyboard=nullptr;xkb_keymap* keyboardMap=nullptr;ext_data_control_manager_v1* manager=nullptr;ext_data_control_device_v1* device=nullptr;
 // ext-data-control-v1 is wlr-data-control-unstable-v1 version 2 standardized
 // with identical requests, events and argument layouts; only the global's
 // interface name differs. Compositors without ext (KWin before 6.4) still
 // offer wlr, so it is bound under its own name and driven through the ext API.
 uint32_t managerName=0,seatName=0,wlrName=0,wlrVersion=0;const char* protocol=nullptr;QSocketNotifier* readNotifier=nullptr;QSocketNotifier* writeNotifier=nullptr;QTimer reconnect,copyTimer;bool closing=false,dispatching=false,lossPending=false,selectionReady=false;quint64 revision=0,generation=0;
 QString error="Native Wayland data control has not connected";QHash<ext_data_control_offer_v1*,QStringList> offers;ext_data_control_offer_v1* selection=nullptr;
 struct Source {Impl* owner;ext_data_control_source_v1* native;QHash<QString,QByteArray> data;QString token;};QHash<ext_data_control_source_v1*,Source*> sources;
 std::optional<quint64> lastCopyRevision;
 std::function<void(Error)> copyReply;QString copyToken;wl_callback* copyFence=nullptr;QHash<QString,QByteArray> pendingCopyData;bool copyCommitted=false;quint64 copyEpoch=0,copyRevision=0,pendingCopyEpoch=0;
 struct Import {ResourceLimits limits;std::function<void(PayloadResult)> reply;quint64 revision;QStringList formats;QString mime;ImageJob::Cancel cancelled=std::make_shared<std::atomic_bool>(false);};QQueue<std::shared_ptr<Import>> pending;std::shared_ptr<Import> active;QPointer<AsyncFdReader> reader;QList<QThread*> workers;
 explicit Impl(DataControlClipboard& q):q(q){reconnect.setInterval(1000);connect(&reconnect,&QTimer::timeout,this,[this]{if(!display)open();});copyTimer.setSingleShot(true);connect(&copyTimer,&QTimer::timeout,this,[this]{finishCopy({"clipboard_copy_failed","Data-control ownership acknowledgement timed out",{}});});const bool graphical=qobject_cast<QGuiApplication*>(QCoreApplication::instance())!=nullptr;const bool allowed=graphical?QGuiApplication::platformName().startsWith("wayland"):kdeCoreSessionEnvironment();if(allowed){open();reconnect.start();}else error="A native KDE Wayland Core session or Qt Wayland platform is required";}
 ~Impl() override{closing=true;reconnect.stop();cancel();finishCopy({"cancelled","Clipboard adapter closed",{}});for(auto* worker:workers){worker->wait();delete worker;}reset();}
 bool available()const{return display&&manager&&seat&&device&&selectionReady;}
 void notify(){emit q.changed();}
 void flush(){if(!display)return;if(wl_display_flush(display)<0){if(errno==EAGAIN){if(writeNotifier)writeNotifier->setEnabled(true);}else lost();}else if(writeNotifier)writeNotifier->setEnabled(false);}
 void open(){if(closing||display)return;display=wl_display_connect(nullptr);if(!display){error="Cannot connect to the Wayland compositor";return;}registry=wl_display_get_registry(display);
  static const wl_registry_listener listener={global,removed};wl_registry_add_listener(registry,&listener,this);
  const auto fd=wl_display_get_fd(display);readNotifier=new QSocketNotifier(fd,QSocketNotifier::Read,this);writeNotifier=new QSocketNotifier(fd,QSocketNotifier::Write,this);writeNotifier->setEnabled(false);connect(readNotifier,&QSocketNotifier::activated,this,[this]{events();});connect(writeNotifier,&QSocketNotifier::activated,this,[this]{flush();});
  initialSync=wl_display_sync(display);static const wl_callback_listener synced={[](void* data,wl_callback* callback,uint32_t){auto* self=static_cast<Impl*>(data);wl_callback_destroy(callback);self->initialSync=nullptr;self->bindWlr();if(!self->manager||!self->seat)self->error="Compositor lacks ext-data-control-v1, wlr-data-control or a usable seat";else if(!self->selectionReady)self->error="Waiting for compositor clipboard ownership discovery";self->notify();}};wl_callback_add_listener(initialSync,&synced,this);flush();
 }
 void events(){if(!display||dispatching)return;dispatching=true;
  // This is a private connection. No Qt-owned Wayland queue is ever dispatched.
  if(wl_display_dispatch_pending(display)<0){dispatching=false;lost();return;}
  pollfd p{wl_display_get_fd(display),POLLIN,0};if(poll(&p,1,0)>0){
   while(wl_display_prepare_read(display)!=0)if(wl_display_dispatch_pending(display)<0){dispatching=false;lost();return;}
   if(wl_display_read_events(display)<0||wl_display_dispatch_pending(display)<0){dispatching=false;lost();return;}
  }
  dispatching=false;flush();
 }
 void reset(){
  if(readNotifier){readNotifier->setEnabled(false);delete readNotifier;readNotifier=nullptr;}if(writeNotifier){writeNotifier->setEnabled(false);delete writeNotifier;writeNotifier=nullptr;}
  selectionReady=false;selection=nullptr;for(auto* offer:offers.keys())ext_data_control_offer_v1_destroy(offer);offers.clear();for(auto* source:sources){ext_data_control_source_v1_destroy(source->native);delete source;}sources.clear();
  if(initialSync){wl_callback_destroy(initialSync);initialSync=nullptr;}if(device){ext_data_control_device_v1_destroy(device);device=nullptr;}if(manager){ext_data_control_manager_v1_destroy(manager);manager=nullptr;}clearKeyboard();if(seat){if(wl_seat_get_version(seat)>=WL_SEAT_RELEASE_SINCE_VERSION)wl_seat_release(seat);else wl_seat_destroy(seat);seat=nullptr;}if(registry){wl_registry_destroy(registry);registry=nullptr;}if(display){wl_display_disconnect(display);display=nullptr;}managerName=seatName=wlrName=wlrVersion=0;protocol=nullptr;
 }
 void lost(){if(dispatching){if(!lossPending){lossPending=true;QTimer::singleShot(0,this,[this]{lossPending=false;lost();});}return;}selectionReady=false;error="Wayland data-control connection closed; restart the app if the Qt session also disconnected";++revision;cancel();finishCopy({"clipboard_unavailable",error,{}});reset();notify();}
 void bindWlr(){if(manager||!wlrName||!registry)return;
  manager=static_cast<ext_data_control_manager_v1*>(wl_registry_bind(registry,wlrName,&zwlr_data_control_manager_v1_interface,std::min(uint32_t(2),wlrVersion)));managerName=wlrName;protocol="wlr-data-control-unstable-v1";makeDevice();
 }
 void makeDevice(){if(device||!manager||!seat)return;device=ext_data_control_manager_v1_get_data_device(manager,seat);
  static const ext_data_control_device_v1_listener listener={offer,selected,finished,primary};ext_data_control_device_v1_add_listener(device,&listener,this);error.clear();notify();flush();
 }
 void clearKeyboard(){
  if(keyboard){if(wl_keyboard_get_version(keyboard)>=WL_KEYBOARD_RELEASE_SINCE_VERSION)wl_keyboard_release(keyboard);else wl_keyboard_destroy(keyboard);keyboard=nullptr;}
  if(keyboardMap){xkb_keymap_unref(keyboardMap);keyboardMap=nullptr;}
  if(!closing)emit q.keyboardChanged();
 }
 static void seatCapabilities(void* data,wl_seat* seat,uint32_t capabilities){
  auto* self=static_cast<Impl*>(data);
  if(!(capabilities&WL_SEAT_CAPABILITY_KEYBOARD)){self->clearKeyboard();return;}
  if(self->keyboard)return;
  self->keyboard=wl_seat_get_keyboard(seat);
  static const wl_keyboard_listener listener={
   [](void* data,wl_keyboard*,uint32_t format,int fd,uint32_t size){
    auto* self=static_cast<Impl*>(data);
    // Each map owns its context. Reusing a context would retain interned
    // names from every prior map even after those maps were released.
    auto* context=xkb_context_new(xkb_context_flags(XKB_CONTEXT_NO_ENVIRONMENT_NAMES|XKB_CONTEXT_NO_DEFAULT_INCLUDES));
    if(context)xkb_context_set_log_fn(context,[](xkb_context*,xkb_log_level,const char*,va_list){});
    auto* next=readKdeKeyboardMap(context,format,fd,size);
    if(context)xkb_context_unref(context);
    if(self->keyboardMap)xkb_keymap_unref(self->keyboardMap);
    self->keyboardMap=next;emit self->q.keyboardChanged();
   },
   [](void*,wl_keyboard*,uint32_t,wl_surface*,wl_array*){},
   [](void*,wl_keyboard*,uint32_t,wl_surface*){},
   [](void*,wl_keyboard*,uint32_t,uint32_t,uint32_t,uint32_t){},
   [](void*,wl_keyboard*,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t){},
   [](void*,wl_keyboard*,int32_t,int32_t){}
  };
  wl_keyboard_add_listener(self->keyboard,&listener,self);self->flush();
 }
 static void global(void* data,wl_registry* r,uint32_t name,const char* iface,uint32_t version){auto* s=static_cast<Impl*>(data);if(QByteArray(iface)==ext_data_control_manager_v1_interface.name&&!s->manager){s->manager=static_cast<ext_data_control_manager_v1*>(wl_registry_bind(r,name,&ext_data_control_manager_v1_interface,1));s->managerName=name;s->protocol="ext-data-control-v1";}
  // Prefer ext: wlr is bound only after the initial registry burst lacks ext.
  else if(QByteArray(iface)==zwlr_data_control_manager_v1_interface.name&&!s->wlrName){s->wlrName=name;s->wlrVersion=version;if(!s->initialSync)s->bindWlr();}
  else if(QByteArray(iface)==wl_seat_interface.name&&!s->seat){s->seat=static_cast<wl_seat*>(wl_registry_bind(r,name,&wl_seat_interface,std::min(uint32_t(5),version)));s->seatName=name;static const wl_seat_listener seatListener={seatCapabilities,[](void*,wl_seat*,const char*){}};wl_seat_add_listener(s->seat,&seatListener,s);}
  s->makeDevice();
 }
 static void removed(void* data,wl_registry*,uint32_t name){auto* s=static_cast<Impl*>(data);if(name==s->wlrName&&name!=s->managerName){s->wlrName=s->wlrVersion=0;return;}if(name!=s->managerName&&name!=s->seatName)return;s->selectionReady=false;s->error="Data-control manager or seat was removed";++s->revision;s->cancel();s->finishCopy({"clipboard_unavailable",s->error,{}});
  if(s->device){ext_data_control_device_v1_destroy(s->device);s->device=nullptr;}s->selection=nullptr;for(auto* offer:s->offers.keys())ext_data_control_offer_v1_destroy(offer);s->offers.clear();
  if(name==s->managerName){ext_data_control_manager_v1_destroy(s->manager);s->manager=nullptr;if(name==s->wlrName)s->wlrName=s->wlrVersion=0;s->managerName=0;s->protocol=nullptr;}if(name==s->seatName){s->clearKeyboard();if(wl_seat_get_version(s->seat)>=WL_SEAT_RELEASE_SINCE_VERSION)wl_seat_release(s->seat);else wl_seat_destroy(s->seat);s->seat=nullptr;s->seatName=0;}s->notify();
 }
 static void offer(void* data,ext_data_control_device_v1*,ext_data_control_offer_v1* o){auto* s=static_cast<Impl*>(data);s->offers.insert(o,{});static const ext_data_control_offer_v1_listener listener={[](void* data,ext_data_control_offer_v1* offer,const char* mime){auto* s=static_cast<Impl*>(data);auto& types=s->offers[offer];if(types.size()<1024&&qstrlen(mime)<4096)types.append(QString::fromUtf8(mime));}};ext_data_control_offer_v1_add_listener(o,&listener,s);}
 static void selected(void* data,ext_data_control_device_v1*,ext_data_control_offer_v1* o){auto* s=static_cast<Impl*>(data);auto* prior=s->selection;const bool initial=!s->selectionReady;s->selectionReady=true;s->selection=o;s->error.clear();++s->revision;if(initial)s->notify();if(prior&&prior!=o){s->offers.remove(prior);ext_data_control_offer_v1_destroy(prior);}
  if(s->active&&s->active->revision!=s->revision)s->complete(failure("clipboard_changed","Clipboard owner changed during acquisition"));
  if(s->copyReply){if(!s->copyCommitted)s->finishCopy({"copy_suppressed","A newer clipboard owner superseded the unsent result copy",{}});else if(o&&s->offers.value(o).contains(s->copyToken))s->finishCopy({});else s->finishCopy({"clipboard_copy_failed","Clipboard ownership changed before the result could be confirmed",{}});}
 }
 static void primary(void* data,ext_data_control_device_v1*,ext_data_control_offer_v1* o){auto* s=static_cast<Impl*>(data);if(o&&o!=s->selection){s->offers.remove(o);ext_data_control_offer_v1_destroy(o);}}
 static void finished(void* data,ext_data_control_device_v1* device){auto* s=static_cast<Impl*>(data);ext_data_control_device_v1_destroy(device);s->device=nullptr;s->selectionReady=false;s->error="The compositor finished the data-control session";++s->revision;s->cancel();s->finishCopy({"clipboard_unavailable",s->error,{}});s->notify();QTimer::singleShot(0,s,[s]{s->makeDevice();});}
 void acquire(const ResourceLimits& limits,std::function<void(PayloadResult)> cb,quint64 expected){events();if(!available()){cb(failure("clipboard_unavailable","Native Wayland data-control clipboard import is unavailable"));return;}
  if(pending.size()+(active||!workers.isEmpty()?1:0)>=qint64(limits.pendingOperations)+limits.concurrentUploads){cb(failure("queue_full","Clipboard acquisition queue reached resource limits"));return;}
  auto request=std::make_shared<Import>();request->limits=limits;request->reply=std::move(cb);request->revision=expected;pending.enqueue(request);pump();
 }
 void pump(){if(closing||active||!workers.isEmpty()||pending.isEmpty())return;active=pending.dequeue();
  const auto dispatchedRequest=active;const auto dispatchedGeneration=generation;QPointer<Impl> self(this);
  events();if(!self||active!=dispatchedRequest||generation!=dispatchedGeneration)return;
  if(!available()){complete(failure("clipboard_unavailable","Data-control clipboard is unavailable"));return;}if(active->revision!=revision){complete(failure("clipboard_changed","Clipboard changed after invocation; invoke sharing again"));return;}
  if(!selection){complete(failure("source_unsupported","Clipboard has no owner"));return;}
  const auto offered=offers.value(selection);bool needsDiscovery=false;
  active->formats=kdeClipboardImportFormats(offered,[&]{needsDiscovery=true;return QList<QByteArray>{};});
  if(!needsDiscovery){nextFormat();return;}
  // Unknown plugin aliases keep their original priority without instantiating
  // image codecs in the Core resident or blocking its Wayland event loop.
  const auto currentGeneration=generation;const auto request=active;const auto cancelled=request->cancelled;
  auto types=std::make_shared<QList<QByteArray>>();auto error=std::make_shared<Error>();
  auto* worker=QThread::create([types,error,cancelled]{*types=ImageJob().discover(*error,cancelled);});
  workers.append(worker);connect(worker,&QThread::finished,this,[this,worker,types,error,offered,currentGeneration,request]{
   workers.removeOne(worker);worker->deleteLater();
   if(currentGeneration!=generation||active!=request){pump();return;}
   QPointer<Impl> self(this);events();if(!self)return;
   // Dispatch may finish the old request and synchronously enqueue a new one.
   if(currentGeneration!=generation||active!=request)return;
   if(active->revision!=revision){complete(failure("clipboard_changed","Clipboard changed during image format discovery"));return;}
   if(*error){complete({{},*error});return;}
   active->formats=kdeClipboardImportFormats(offered,[&]{return *types;});nextFormat();
  });worker->start();
 }
 void nextFormat(){if(!active)return;if(active->formats.isEmpty()){complete(failure("source_unsupported","Clipboard offers no supported local files, image, or text"));return;}if(!selection||active->revision!=revision){complete(failure("clipboard_changed","Clipboard changed before import"));return;}
  active->mime=active->formats.takeFirst();int pipes[2];if(pipe2(pipes,O_NONBLOCK|O_CLOEXEC)<0){complete(failure("transfer_fd","Cannot create a clipboard transfer pipe"));return;}auto* transfer=new AsyncFdReader(this);reader=transfer;const auto mime=active->mime;const auto limits=active->limits;const auto currentGeneration=generation;
  transfer->start(pipes[0],mime.startsWith("image/")?limits.imageBytes:limits.clipboardTextBytes,5000,[this,transfer,mime,limits,currentGeneration](QByteArray bytes,Error error){transfer->deleteLater();if(reader==transfer)reader=nullptr;if(!active||currentGeneration!=generation)return;if(error){complete({{},error});return;}decode(std::move(bytes),mime,limits,currentGeneration);});
  ext_data_control_offer_v1_receive(selection,mime.toUtf8().constData(),pipes[1]);close(pipes[1]);flush();
 }
 void decode(QByteArray bytes,QString mime,ResourceLimits limits,quint64 currentGeneration){
  auto result=std::make_shared<PayloadResult>();auto noLocal=std::make_shared<bool>(false);const auto request=active;const auto cancelled=request->cancelled;auto* worker=QThread::create([bytes=std::move(bytes),mime,limits,result,noLocal,cancelled]{QMimeData data;
   if(mime=="text/uri-list"){QList<QUrl> urls;for(auto line:bytes.split('\n')){line=line.trimmed();if(!line.isEmpty()&&!line.startsWith('#'))urls.append(QUrl::fromEncoded(line));}*noLocal=true;for(const auto& url:urls)if(url.isLocalFile())*noLocal=false;data.setUrls(urls);}
   else if(mime.startsWith("image/")){
    QImage image;
    if(imageFormatHint(bytes)=="png"){
     // Qt 6.4.2 and 6.8.2 instantiate every installed image plugin for a
     // named format unless detection is off; the signature was checked above.
     QBuffer buffer;buffer.setData(bytes);buffer.open(QIODevice::ReadOnly);QImageReader reader(&buffer,"png");reader.setAutoDetectImageFormat(false);
     const auto size=reader.size();if(!size.isValid()||qint64(size.width())*size.height()>limits.imagePixels||qint64(size.width())*size.height()>limits.imageBytes/4){*result=failure("input_excessive","Clipboard image dimensions exceed resource limits");return;}
     image=reader.read();if(image.isNull()){*result=failure("source_unsupported","Clipboard image could not be decoded");return;}
    }else{
     Error error;image=ImageJob().decode(bytes,limits,error,cancelled);if(error){*result={{},error};return;}
    }
    data.setImageData(image);
   }
   else data.setText(QString::fromUtf8(bytes));
   *result=ClipboardPayload::fromMime(data,limits);
  });workers.append(worker);connect(worker,&QThread::finished,this,[this,worker,result,noLocal,currentGeneration,request]{workers.removeOne(worker);worker->deleteLater();if(currentGeneration!=generation||active!=request){pump();return;}QPointer<Impl> self(this);events();if(!self)return;if(currentGeneration!=generation||active!=request)return;if(active->revision!=revision){complete(failure("clipboard_changed","Clipboard changed during decoding"));return;}if(*noLocal&&!active->formats.isEmpty()){nextFormat();return;}complete(std::move(*result));});worker->start();
 }
 void complete(PayloadResult result){++generation;auto request=std::move(active);active.reset();if(request)request->cancelled->store(true);if(reader){auto* transfer=reader.data();reader=nullptr;transfer->cancel();transfer->deleteLater();}QPointer<Impl> self(this);if(request&&request->reply){auto cb=std::move(request->reply);cb(std::move(result));}if(self)QTimer::singleShot(0,this,[this]{pump();});}
 void cancel(){++generation;auto queued=std::move(pending);pending.clear();if(active)complete(failure("cancelled","Clipboard acquisition cancelled"));for(auto& request:queued)request->reply(failure("cancelled","Clipboard acquisition cancelled"));}
 void finishCopy(Error e){if(!e)lastCopyRevision=revision;copyTimer.stop();if(copyFence){wl_callback_destroy(copyFence);copyFence=nullptr;}pendingCopyData.clear();copyCommitted=false;auto cb=std::move(copyReply);copyReply={};copyToken.clear();if(cb)cb(std::move(e));}
 void invalidateCopy(){++copyEpoch;if(copyReply&&!copyCommitted)finishCopy({"copy_suppressed","A newer sharing invocation superseded this unsent native result copy",{}});}
 void commitCopy(){
  if(!copyReply)return;
  if(pendingCopyEpoch!=copyEpoch||revision!=copyRevision){finishCopy({"copy_suppressed","A newer invocation or clipboard owner superseded the result copy",{}});return;}
  if(!available()){finishCopy({"clipboard_unavailable","Native clipboard disappeared before result copying",{}});return;}
  auto* native=ext_data_control_manager_v1_create_data_source(manager);auto* source=new Source{this,native,std::move(pendingCopyData),copyToken};sources[native]=source;
  static const ext_data_control_source_v1_listener listener={send,sourceCancelled};ext_data_control_source_v1_add_listener(native,&listener,source);for(auto it=source->data.cbegin();it!=source->data.cend();++it)ext_data_control_source_v1_offer(native,it.key().toUtf8().constData());
  copyCommitted=true;ext_data_control_device_v1_set_selection(device,native);flush();
  // Never issue ext_data_control_device_v1_set_primary_selection. Once selection
  // is sent, cancellation cannot restore the previous owner and must not clear it.
 }
 static void send(void* data,ext_data_control_source_v1*,const char* mime,int fd){auto* source=static_cast<Source*>(data);new FdWriter(fd,source->data.value(QString::fromUtf8(mime)),source->owner);}
 static void sourceCancelled(void* data,ext_data_control_source_v1* native){auto* source=static_cast<Source*>(data);auto* owner=source->owner;if(owner->copyToken==source->token)owner->finishCopy({"clipboard_copy_failed","The compositor cancelled result ownership",{}});owner->sources.remove(native);ext_data_control_source_v1_destroy(native);delete source;}
 void copy(const CopyPayload& payload,std::function<void(Error)> cb){events();if(!available()){cb({"clipboard_unavailable","Native Wayland data-control result copying is unavailable",{}});return;}if(copyReply){cb({"clipboard_copy_failed","Another result copy is awaiting ownership acknowledgement",{}});return;}
  QHash<QString,QByteArray> data;if(payload.kind=="image"&&!payload.encodedImage.isEmpty())data["image/png"]=payload.encodedImage;
  else if(payload.kind=="files"&&!payload.files.isEmpty()){QByteArray uris;for(const auto& file:payload.files){uris.append(QUrl::fromLocalFile(file).toEncoded());uris.append("\r\n");}data["text/uri-list"]=uris;}
  else if(payload.kind=="text"){const auto bytes=payload.text.toUtf8();data["text/plain;charset=utf-8"]=bytes;data["text/plain"]=bytes;data["UTF8_STRING"]=bytes;}
  else{cb({"source_unsupported","Unsupported native clipboard result type",{}});return;}
  copyToken="application/x-putgd-result-"+QUuid::createUuid().toString(QUuid::Id128);data[copyToken]={};pendingCopyData=std::move(data);copyReply=std::move(cb);copyRevision=revision;pendingCopyEpoch=copyEpoch;copyCommitted=false;copyTimer.start(5000);
  // A non-mutating compositor fence drains ownership events before the final
  // eligibility check. New invocations can veto this unsent stage immediately.
  copyFence=wl_display_sync(display);static const wl_callback_listener fenced={[](void* data,wl_callback* callback,uint32_t){auto* self=static_cast<Impl*>(data);wl_callback_destroy(callback);self->copyFence=nullptr;self->commitCopy();}};wl_callback_add_listener(copyFence,&fenced,this);flush();
 }
};
DataControlClipboard::DataControlClipboard(QObject* parent):QObject(parent),d_(std::make_unique<Impl>(*this)){}
DataControlClipboard::~DataControlClipboard()=default;
xkb_keymap* DataControlClipboard::keyboardMap()const{return d_->keyboardMap;}
bool DataControlClipboard::available()const{return d_->available();}
quint64 DataControlClipboard::revision()const{d_->events();return d_->revision;}
std::optional<quint64> DataControlClipboard::lastCopyRevision()const{return d_->lastCopyRevision;}
QJsonObject DataControlClipboard::status()const{const QString protocol=d_->protocol?d_->protocol:"none";return {{"available",available()},{"types",QJsonArray{"files","image","text"}},{"import",protocol},{"result",protocol+"-clipboard-only"},{"protocol_version",d_->manager?int(wl_proxy_get_version(reinterpret_cast<wl_proxy*>(d_->manager))):0},{"error",d_->error}};}
void DataControlClipboard::acquire(const ResourceLimits& limits,std::function<void(PayloadResult)> cb,quint64 expected){d_->acquire(limits,std::move(cb),expected);}
void DataControlClipboard::copy(const CopyPayload& payload,std::function<void(Error)> cb){d_->copy(payload,std::move(cb));}
void DataControlClipboard::cancel(){d_->cancel();}
void DataControlClipboard::invalidateCopyEligibility(){d_->invalidateCopy();}
}
