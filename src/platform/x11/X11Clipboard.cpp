#include "X11Clipboard.h"
#include "X11Reply.h"
#include <QGuiApplication>
#include <QClipboard>
#include <QMimeData>
#include <QBuffer>
#include <QImageReader>
#include <QUrl>
#include <QPointer>
#include <xcb/xfixes.h>
#include <cstring>
namespace putgd {
namespace {using x11::reply;PayloadResult failure(const char* code,const char* message){return {{},{QString::fromLatin1(code),QString::fromLatin1(message),{}}};}}
X11Clipboard::X11Clipboard(QObject* parent):QObject(parent) {
 if(QGuiApplication::platformName()!="xcb")return;
 connection_=xcb_connect(nullptr,nullptr);if(xcb_connection_has_error(connection_))return;
 auto* root=xcb_setup_roots_iterator(xcb_get_setup(connection_)).data;
 window_=xcb_generate_id(connection_);uint32_t mask=XCB_EVENT_MASK_PROPERTY_CHANGE;
 xcb_create_window(connection_,XCB_COPY_FROM_PARENT,window_,root->root,0,0,1,1,0,XCB_WINDOW_CLASS_INPUT_ONLY,XCB_COPY_FROM_PARENT,XCB_CW_EVENT_MASK,&mask);
 clipboard_=atom("CLIPBOARD");targets_=atom("TARGETS");property_=atom("PUTGD_SELECTION");incr_=atom("INCR");
 const auto* fixes=xcb_get_extension_data(connection_,&xcb_xfixes_id);if(!fixes||!fixes->present)return;
 auto version=reply(xcb_xfixes_query_version_reply(connection_,xcb_xfixes_query_version(connection_,5,0),nullptr));if(!version)return;
 fixesEvent_=fixes->first_event;xcb_xfixes_select_selection_input(connection_,window_,clipboard_,XCB_XFIXES_SELECTION_EVENT_MASK_SET_SELECTION_OWNER|XCB_XFIXES_SELECTION_EVENT_MASK_SELECTION_WINDOW_DESTROY|XCB_XFIXES_SELECTION_EVENT_MASK_SELECTION_CLIENT_CLOSE);
 xcb_flush(connection_);notifier_=new QSocketNotifier(xcb_get_file_descriptor(connection_),QSocketNotifier::Read,this);
 connect(notifier_,&QSocketNotifier::activated,this,[this]{events();});timeout_.setSingleShot(true);
 connect(&timeout_,&QTimer::timeout,this,[this]{complete(failure("clipboard_timeout","Clipboard owner did not provide data; copy the source again and retry"));});

 revision();
}
X11Clipboard::~X11Clipboard(){closing_=true;cancel();for(auto* t:decoders_){t->wait();delete t;}if(connection_)xcb_disconnect(connection_);}
bool X11Clipboard::available()const{return notifier_&&connection_&&!xcb_connection_has_error(connection_);}
xcb_atom_t X11Clipboard::atom(const char* name){const QByteArray key(name);const auto found=atoms_.constFind(key);
 if(found!=atoms_.cend())return found.value();
 const auto value=x11::internAtom(connection_,name);
 if(value!=XCB_ATOM_NONE)atoms_.insert(key,value);
 return value;}
quint64 X11Clipboard::revision()const {
 if(!available())return revision_;
 // Ownership queries read no payload and never wait on the selection owner.
 auto* self=const_cast<X11Clipboard*>(this);self->events();
 auto r=reply(xcb_get_selection_owner_reply(connection_,xcb_get_selection_owner(connection_,clipboard_),nullptr));
 // xcb reply reads may queue ownership events after draining the socket. Drain
 // them before freezing a revision, even when the fd is no longer readable.
 self->events();if(r&&r->owner!=owner_){owner_=r->owner;++revision_;}return revision_;
}
void X11Clipboard::acquire(const ResourceLimits& limits,std::function<void(PayloadResult)> cb,quint64 expectedRevision) {
 if(closing_){cb(failure("cancelled","Clipboard adapter is closing"));return;}
 const auto count=pending_.size()+((callback_||decoding_)?1:0);
 if(count>=qint64(limits.pendingOperations)+limits.concurrentUploads){cb(failure("queue_full","Clipboard source queue reached the accepted operation limit"));return;}
 const auto frozen=expectedRevision?expectedRevision:revision();
 pending_.enqueue({limits,std::move(cb),frozen});pump();
}
void X11Clipboard::pump(){
 if(closing_||pumping_||callback_||decoding_)return;
 if(dispatching_){QTimer::singleShot(0,this,[this]{pump();});return;}
 pumping_=true;
 while(!pending_.isEmpty()&&!callback_&&!decoding_)beginImport(pending_.dequeue());
 pumping_=false;
}
void X11Clipboard::beginImport(ImportRequest request){
 auto cb=std::move(request.callback);
 if(!available()){cb(failure("clipboard_unavailable","X11 clipboard ownership monitoring is unavailable"));return;}
 const auto current=revision();if(request.revision!=current){cb(failure("clipboard_changed","Clipboard changed after invocation; invoke sharing again"));return;}
 if(owner_==XCB_WINDOW_NONE){cb(failure("source_unsupported","Clipboard has no owner"));return;}
 ++generation_;limits_=request.limits;callback_=std::move(cb);acquiredRevision_=current;bytes_.clear();incremental_=false;timeout_.start(5000);convert(targets_);
}
void X11Clipboard::nextTarget(){
 if(offered_.isEmpty()){complete(failure("source_unsupported","Clipboard contains no supported source format"));return;}
 const auto target=offered_.takeFirst();imageFormat_=imageFormats_.value(target);convert(target);
}
void X11Clipboard::convert(xcb_atom_t target){target_=target;xcb_delete_property(connection_,window_,property_);xcb_convert_selection(connection_,window_,clipboard_,target,property_,XCB_CURRENT_TIME);xcb_flush(connection_);}
void X11Clipboard::events(){
 if(!available()||dispatching_)return;
 dispatching_=true;
 while(auto* raw=xcb_poll_for_event(connection_)) {
  auto event=reply(raw);const int type=raw->response_type&127;
  if(type==fixesEvent_+XCB_XFIXES_SELECTION_NOTIFY){auto* e=reinterpret_cast<xcb_xfixes_selection_notify_event_t*>(raw);owner_=e->owner;++revision_;if(callback_&&revision_!=acquiredRevision_)complete(failure("clipboard_changed","Clipboard owner changed during acquisition"));}
  else if(type==XCB_SELECTION_NOTIFY&&callback_) {auto* e=reinterpret_cast<xcb_selection_notify_event_t*>(raw);if(e->requestor!=window_||e->selection!=clipboard_||e->target!=target_)continue;if(e->property==XCB_ATOM_NONE){complete(failure("source_unsupported","Clipboard owner rejected the requested data format"));continue;}property(false);}
  else if(type==XCB_PROPERTY_NOTIFY&&callback_&&incremental_) {auto* e=reinterpret_cast<xcb_property_notify_event_t*>(raw);if(e->window==window_&&e->atom==property_&&e->state==XCB_PROPERTY_NEW_VALUE)property(true);}
 }
 if(xcb_connection_has_error(connection_)&&callback_)complete(failure("clipboard_unavailable","X11 connection closed during clipboard acquisition"));
 dispatching_=false;
}
void X11Clipboard::property(bool incremental){
 const qint64 bound=target_==targets_?65536:std::max(limits_.clipboardTextBytes,limits_.imageBytes);
 const uint32_t words=uint32_t(std::min<qint64>((bound+3)/4+1,UINT32_MAX));
 auto r=reply(xcb_get_property_reply(connection_,xcb_get_property(connection_,incremental?1:0,window_,property_,XCB_GET_PROPERTY_TYPE_ANY,0,words),nullptr));
 if(!r){complete(failure("clipboard_unavailable","Cannot read the clipboard selection response"));return;}
 if(r->bytes_after){complete(failure("input_excessive","Clipboard data exceeds resource limits"));return;}
 if(r->type==incr_&&!incremental){if(r->format!=32||xcb_get_property_value_length(r.get())!=4){complete(failure("source_unsupported","Malformed incremental clipboard transfer"));return;}
  uint32_t advertised=0;memcpy(&advertised,xcb_get_property_value(r.get()),4);if(advertised>bound){complete(failure("input_excessive","Clipboard transfer exceeds resource limits"));return;}incremental_=true;bytes_.clear();xcb_delete_property(connection_,window_,property_);xcb_flush(connection_);return;
 }
 const int length=xcb_get_property_value_length(r.get());
 if(target_==targets_){if(r->type!=XCB_ATOM_ATOM||r->format!=32||incremental){complete(failure("source_unsupported","Malformed clipboard target list"));return;}
  const auto* supported=static_cast<xcb_atom_t*>(xcb_get_property_value(r.get()));const int count=length/4;auto has=[&](xcb_atom_t a){for(int i=0;i<count;++i)if(supported[i]==a)return true;return false;};
  imageFormat_.clear();offered_.clear();imageFormats_.clear();
  if(has(atom("text/uri-list")))offered_.append(atom("text/uri-list"));
  auto images=QImageReader::supportedMimeTypes();images.removeAll("image/png");images.prepend("image/png");
  for(const auto& mime:images){const auto a=atom(mime.constData());if(has(a)){imageFormats_[a]=QString::fromLatin1(mime);offered_.append(a);}}
  for(const char* name:{"UTF8_STRING","text/plain;charset=utf-8","text/plain"}){const auto a=atom(name);if(has(a))offered_.append(a);}
  nextTarget();return;
 }
 if(r->format!=8){complete(failure("source_unsupported","Malformed clipboard data format"));return;}
 if(qint64(bytes_.size())+length>bound){complete(failure("input_excessive","Clipboard data exceeds resource limits"));return;}
 bytes_.append(static_cast<const char*>(xcb_get_property_value(r.get())),length);
 if(!incremental)xcb_delete_property(connection_,window_,property_);
 xcb_flush(connection_);
 if(!incremental||length==0)received();
}
void X11Clipboard::received(){
 timeout_.stop();if(revision()!=acquiredRevision_){complete(failure("clipboard_changed","Clipboard changed during acquisition"));return;}
 decoding_=true;const auto generation=generation_;
 const auto data=bytes_;const auto target=target_;const auto limits=limits_;const bool uris=target==atom("text/uri-list"),image=!imageFormat_.isEmpty();auto callback=std::move(callback_);callback_={};incremental_=false;bytes_.clear();
 // Parsing/decoding potentially large owner data must not run on the session thread.
 auto result=std::make_shared<PayloadResult>();auto noLocal=std::make_shared<bool>(false);auto* worker=QThread::create([data,limits,uris,image,result,noLocal] {
  QMimeData mime;if(uris){QList<QUrl> urls;for(auto line:data.split('\n')){line=line.trimmed();if(!line.isEmpty()&&!line.startsWith('#'))urls.append(QUrl::fromEncoded(line));}mime.setUrls(urls);*noLocal=true;for(const auto& url:urls)if(url.isLocalFile())*noLocal=false;}
  else if(image){QBuffer buffer;buffer.setData(data);buffer.open(QIODevice::ReadOnly);QImageReader reader(&buffer);const auto size=reader.size();if(!size.isValid()||qint64(size.width())*size.height()>limits.imagePixels||qint64(size.width())*size.height()*4>limits.imageBytes){*result=failure("input_excessive","Clipboard image dimensions exceed resource limits");return;}mime.setImageData(reader.read());}
  else {if(data.size()>limits.clipboardTextBytes){*result=failure("input_excessive","Clipboard text exceeds resource limits");return;}mime.setText(QString::fromUtf8(data));}
  *result=ClipboardPayload::fromMime(mime,limits);
 });decoders_.append(worker);connect(worker,&QThread::finished,this,[this,worker,result,noLocal,generation,callback=std::move(callback)]() mutable {
  decoders_.removeOne(worker);worker->deleteLater();decoding_=false;
  if(generation!=generation_){callback(failure("cancelled","Clipboard acquisition cancelled"));pump();return;}
  // A remote URI list is not a file source. Use the owner's actual image/text
  // representations before relying on Qt's synthesized URI-list text.
  if(*noLocal&&!offered_.isEmpty()){
   if(revision()!=acquiredRevision_){callback(failure("clipboard_changed","Clipboard changed before importing its next representation"));pump();return;}
   callback_=std::move(callback);bytes_.clear();timeout_.start(5000);nextTarget();return;
  }
  callback(std::move(*result));pump();
 });worker->start();
}
void X11Clipboard::complete(PayloadResult result){timeout_.stop();incremental_=false;bytes_.clear();if(callback_){auto cb=std::move(callback_);callback_={};cb(std::move(result));}pump();}
void X11Clipboard::cancel(){
 ++generation_;auto pending=std::move(pending_);pending_.clear();
 if(callback_)complete(failure("cancelled","Clipboard acquisition cancelled"));
 for(auto& request:pending)request.callback(failure("cancelled","Clipboard acquisition cancelled"));
}
void X11Clipboard::copy(const CopyPayload& payload,std::function<void(Error)> callback){
 if(!available()){callback({"clipboard_unavailable","X11 clipboard is unavailable",{}});return;}
 auto* mime=new QMimeData;
 if(payload.kind=="image")mime->setData("image/png",payload.encodedImage);else if(payload.kind=="files"){QList<QUrl> urls;for(const auto& path:payload.files)urls.append(QUrl::fromLocalFile(path));mime->setUrls(urls);}else if(payload.kind=="text")mime->setText(payload.text);else {delete mime;callback({"source_unsupported","Unsupported clipboard result type",{}});return;}
 QGuiApplication::clipboard()->setMimeData(mime);if(!QGuiApplication::clipboard()->ownsClipboard()){callback({"clipboard_copy_failed","Unable to own the X11 clipboard",{}});return;}++revision_;callback({});
}
}
