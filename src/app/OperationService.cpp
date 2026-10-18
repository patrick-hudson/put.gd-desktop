#include "OperationService.h"
#include "core/BuildInfo.h"
#include <QElapsedTimer>
#ifdef __GLIBC__
#include <malloc.h>
#endif
#include "core/UploadNaming.h"
#include "core/ImageJob.h"
#include "core/ImageFormat.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QPointer>
#include <QTimer>
#include <QUuid>
#ifdef Q_OS_UNIX
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#else
#include <QBuffer>
#include <QImageReader>
#endif
namespace putgd {
namespace {
QString uuid() {return QUuid::createUuid().toString(QUuid::WithoutBraces);}
Error unsupported() {return {"source_unsupported","Native desktop adapter is not available",{}};}
bool terminalUpload(const QString& s) {return s=="uploaded"||s=="skipped"||s=="conflict"||s=="local_only";}
}
QJsonObject UnavailableSource::capabilities() const {return {{"region",false},{"window",false},{"desktop",false},{"clipboardImport",QJsonArray{}},{"clipboardResult",QJsonArray{}},{"shortcuts",false},{"reason","native_adapter_unavailable"}};}
void UnavailableSource::acquire(const QString&,const ResourceLimits&,std::function<void(PayloadResult)> cb) {cb({{},unsupported()});}
void UnavailableSource::copy(const CopyPayload&,std::function<void(Error)> cb) {cb(unsupported());}
OperationService::OperationService(QString root,SettingsService& settings,ProviderRegistry& providers,SourceAdapter& source,QObject* parent):QObject(parent),root_(std::move(root)),settings_(settings),providers_(providers),source_(source),heapReclamationTimer_(this),observationTimer_(this) {
 observationTimer_.setSingleShot(true);observationTimer_.setInterval(250);
 connect(&observationTimer_,&QTimer::timeout,this,[this]{
  QSet<QString> ids;{std::lock_guard lock(observationsMutex_);ids=std::exchange(observationsDirty_,{});observationScheduled_=false;}
  for(const auto& id:ids)emit changed(id);
  if(!ids.isEmpty())emit observationsChanged();
 });
 RecordingService::Host host;
 host.settings=[this]{return settings_.snapshot();};
 host.accept=[this](Operation& op){return acceptRecording(op);};
 host.persist=[this](const Operation& op,std::function<void(Error)> done){const auto path=databasePath();background([path,op](Error& e){HistoryStore store(path);e=store.saveOperation(op);return QJsonObject{};},[done](Error e,QJsonObject){done(e);});};
 host.observe=[this](const Operation& op){observe(op.publicJson());emit changed(op.id);emit observationsChanged();};
 host.admit=[this](Operation op,ClipboardPayload payload,std::function<void()> released){
  auto flag=active_.value(op.id);if(!flag){released();return;}
  Job job{op,std::move(payload),true,flag,{}};
  // The staged export stays until the artifact store has streamed it.
  job.payloadLease=std::shared_ptr<qint64>(new qint64(0),[released](qint64* value){released();delete value;});
  enqueue(std::move(job));
 };
 host.finish=[this](Operation op){finish(op);};
 recording_=std::make_unique<RecordingService>(root_+"/recording",std::move(host),this);
 connect(recording_.get(),&RecordingService::changed,this,[this](const QString& id){emit changed(id);emit observationsChanged();});
 heapReclamationTimer_.setSingleShot(true);
 connect(&heapReclamationTimer_,&QTimer::timeout,this,[this] {
  if(!heapReclamationPending_||!heapReclamationIdle())return;
#ifdef __GLIBC__
  // Return only allocator-owned free pages. Live Qt widgets and clipboard MIME
  // remain owned by their existing objects; no cache or native theme is purged.
  QElapsedTimer elapsed;elapsed.start();
  malloc_trim(0);
  lastHeapReclamationUsec_=elapsed.nsecsElapsed()/1000;
  ++heapReclamationCount_;
#endif
  heapReclamationPending_=false;
 });
 const auto path=databasePath(),artifactsRoot=root_+"/artifacts";
 background([this,path,artifactsRoot](Error& e) {
  if(!QDir().mkpath(root_)||!QFile::setPermissions(root_,QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner)) {e={"history_io","Cannot create private journal storage",{}};return QJsonObject{};}
  journalOwner_=std::make_unique<QLockFile>(root_+"/resident.lock");journalOwner_->setStaleLockTime(0);
  if(!journalOwner_->tryLock(0)) {e={"journal_busy","Another resident owns this operation journal",{}};return QJsonObject{};}
  // Unsubmitted editor sources and recording exports never survive their resident.
  EditorJob::discardStaging(root_+"/editing");
  RecordingService::discardStaging(root_+"/recording");
  HistoryStore store(path);e=store.error();if(e)return QJsonObject{};e=store.indexPublications(providers_);if(e)return QJsonObject{};ArtifactStore artifacts(artifactsRoot);artifacts.discardIncomplete();const auto recovered=store.recover(&e);
  for(auto op:recovered)if(op.items.isEmpty()) {auto batch=artifacts.recover(op.id);if(batch.error) {op.error=batch.error;e=store.saveOperation(op);if(e)break;}else if(!batch.artifacts.isEmpty()) {for(const auto& artifact:batch.artifacts) {OperationItem item;item.artifact=artifact;item.publicationId=uuid();op.items.append(item);}op.error={"operation_interrupted","Acquired artifacts retained; retry explicitly",{}};e=store.saveOperation(op);if(e)break;}}
  return QJsonObject{};
 },[this](Error e,QJsonObject) {
  startupError_=e;ready_=!e;
#ifdef __GLIBC__
  if(ready_) {heapReclamationPending_=true;scheduleHeapReclamation();}
#endif
  emit initialized();
 });
}
OperationService::~OperationService() {shutdown();}
void OperationService::shutdown() {if(stopping_)return;stopping_=true;heapReclamationTimer_.stop();observationTimer_.stop();for(auto flag:active_) flag->store(true);if(recording_)recording_->shutdown();if(editor_)editor_->cancel();invalidateCopyEligibility();source_.cancelAcquisition();for(auto* thread:threads_) {thread->wait();delete thread;}threads_.clear();}
bool OperationService::heapReclamationIdle() const {
 return !stopping_&&threads_.isEmpty()&&queue_.isEmpty()&&running_==0&&active_.isEmpty()&&copies_.isEmpty()&&!currentCopy_&&selectingId_.isEmpty()&&payloadBytes_->load()==0&&!(recording_&&recording_->active());
}
void OperationService::scheduleHeapReclamation() {
 if(heapReclamationPending_&&heapReclamationIdle())heapReclamationTimer_.start(2000);
}
void OperationService::background(std::function<QJsonObject(Error&)> work,JsonReply reply) {
 heapReclamationTimer_.stop();
 QPointer<OperationService> self(this);auto* thread=QThread::create([self,work=std::move(work),reply=std::move(reply)] {Error error;auto result=work(error);if(self) QMetaObject::invokeMethod(self,[self,reply,error,result] {if(self&&!self->stopping_) reply(error,result);},Qt::QueuedConnection);});
 threads_.append(thread);connect(thread,&QThread::finished,this,[this,thread] {threads_.removeOne(thread);thread->deleteLater();scheduleHeapReclamation();});thread->start();
}
std::shared_ptr<std::mutex> OperationService::objectLock(const Destination& d,const QJsonObject& object,const QString& name) {
 // Adapters supply their real destination/object identity, independent of profile revisions.
 auto provider=providers_.find(d.providerId);
 const auto key=provider?provider->serializationKey(d,object,name):d.providerId+name;
 std::lock_guard guard(locksMutex_);
 for(auto it=locks_.begin();it!=locks_.end();) {
  if(it.value().expired())it=locks_.erase(it);
  else ++it;
 }
 auto lock=locks_.value(key).lock();
 if(!lock) {
  lock=std::make_shared<std::mutex>();
  locks_.insert(key,lock);
 }
 return lock;
}
void OperationService::storageActivityDelta(int delta) {
 QPointer<OperationService> owner(this);
 QMetaObject::invokeMethod(this,[owner,delta] {
  if(!owner||owner->stopping_)return;
  const bool before=owner->storageActive();owner->storageRequests_+=delta;
  if(before!=owner->storageActive())emit owner->storageActivityChanged();
 },Qt::QueuedConnection);
}
// Coalesce transient progress without rewriting the durable publication journal.
// The map has at most the existing active-operation limit; one timer wake is
// queued regardless of how often a provider reports. No payload bytes are copied.
void OperationService::observe(const QJsonObject& document){
 const auto id=document["id"].toString();bool schedule=false;
 {std::lock_guard lock(observationsMutex_);const auto existing=observations_.constFind(id);if(existing!=observations_.cend()&&*existing==document)return;observations_[id]=document;observationsDirty_.insert(id);if(!observationScheduled_){observationScheduled_=true;schedule=true;}}
 if(schedule){QPointer<OperationService> owner(this);QMetaObject::invokeMethod(this,[owner]{if(owner&&!owner->stopping_)owner->observationTimer_.start();},Qt::QueuedConnection);}
}
QJsonArray OperationService::activeObservations() const {
 QJsonArray result;std::lock_guard lock(observationsMutex_);
 for(const auto& document:observations_)if(Operation::activeState(document["state"].toString()))result.append(document);
 return result;
}
void OperationService::clearObservation(const QString& id){std::lock_guard lock(observationsMutex_);observations_.remove(id);observationsDirty_.remove(id);}
QJsonObject OperationService::currentObservation(QJsonObject document)const{
 if(QStringList{"completed","partial","failed","cancelled","interrupted"}.contains(document["state"].toString()))return document;
 QJsonObject live;{std::lock_guard lock(observationsMutex_);live=observations_.value(document["id"].toString());}if(live.isEmpty())return document;
 document["phase"]=live["phase"];auto items=document["items"].toArray();QHash<QString,QJsonObject> observedItems;
 for(const auto& value:live["items"].toArray()){const auto observed=value.toObject();const auto id=observed["artifact"].toObject()["id"].toString();if(!observedItems.contains(id))observedItems.insert(id,observed);}
 for(qsizetype i=0;i<items.size();++i){auto item=items[i].toObject();const auto observed=observedItems.constFind(item["artifact"].toObject()["id"].toString());if(observed!=observedItems.cend()){item["phase"]=(*observed)["phase"];item["uploadProgress"]=(*observed)["uploadProgress"]; }items[i]=item;}
 document["items"]=items;return document;
}
void OperationService::invoke(const QString& action,JsonReply reply,const QJsonObject& options) {
 if(!ready_) {reply(startupError_?startupError_:Error{"initializing","Journal recovery is in progress",{}},{});return;}
 if(!Settings::actionNames().contains(action)) {reply({"invalid_action","Unknown sharing action",{}},{});return;}
 // Edit & upload acquires from a chosen source, by default the native region
 // selector; every other action is its own source and takes no options.
 const auto source=action=="edit"?options["source"].toString("region"):action;
 const auto sourcePath=options["path"].toString();
 if((action!="edit"&&!options.isEmpty())||(action=="edit"&&(!QStringList{"region","window","desktop","clipboard","file"}.contains(source)||(source=="file")!=options.contains("path")||(source=="file"&&!QDir::isAbsolutePath(sourcePath))))){reply({"invalid_request","Edit sources are region, window, desktop, clipboard, or file with an absolute path",{}},{});return;}
 if(action=="edit"&&editor_){editor_->request("raise",{},[](Error,QJsonObject){});reply({"editor_active","An edit is already open; finish or cancel it first",{{"operationId",editingId_}}},{});return;}
 if(action=="edit"&&recording_->active()){reply({"recording_active","A recording is active; stop or cancel it before editing",{{"operationId",recording_->activeId()}}},{});return;}
 const bool interactive=source!="clipboard"&&source!="file";
 auto snapshot=settings_.snapshot();if(active_.size()>=snapshot.limits.pendingOperations+snapshot.limits.concurrentUploads) {reply({"queue_full","Pending operation limit reached",{}},{});return;}
 if(interactive&&!selectingId_.isEmpty()) {reply({"busy_selecting","Another interactive acquisition is active",{}},{});return;}
 const auto route=snapshot.actions.value(action);
 const bool upload=route.destination=="upload"&&!route.profileId.isEmpty();
 if(!upload&&!snapshot.archive.keepUploads){reply({"output_required","No output is enabled. Choose a destination in Settings → Capture or enable Keep a local copy in Settings → General.",{}},{});return;}
 Operation op;op.id=uuid();op.action=action;op.settings=snapshot;op.settingsRevision=settings_.revision();op.created=QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);op.invocation=upload?++invocation_:invalidateCopyEligibility();op.clipboardRevision=source_.clipboardRevision();
 if(upload) {const auto profile=snapshot.profiles.value(route.profileId);op.destination={profile["provider"].toString(),route.profileId,settings_.revision(),profile["settings"].toObject(),profile["credentials"].toObject()};}
 auto flag=std::make_shared<std::atomic_bool>(false);active_.insert(op.id,flag);if(upload)beginUploadCopies(op,source=="clipboard");if(interactive)selectingId_=op.id;if(action=="edit"){editingId_=op.id;editor_=std::make_shared<EditorSession>();}
#ifdef __GLIBC__
 heapReclamationPending_=true;
#endif
 // Preview reports terminal metadata only. Latch it on the session thread so
 // it can stop either checkpoint continuation without ever delivering pixels.
 auto earlyError=std::make_shared<Error>();QPointer<OperationService> owner(this);
 auto earlyTerminal=[owner,id=op.id,earlyError](Error error) {
  if(!owner||!error)return;
  auto latch=[owner,id,earlyError,error=std::move(error)]() mutable {
   if(owner&&!owner->stopping_&&owner->active_.contains(id)&&!*earlyError)*earlyError=std::move(error);
  };
  if(QThread::currentThread()==owner->thread())latch();
  else QMetaObject::invokeMethod(owner,std::move(latch),Qt::QueuedConnection);
 };
 preparedOwners_.insert(op.id);
 auto released=[owner,id=op.id,earlyError,flag] {
  if(!owner)return;
  auto release=[owner,id,earlyError,flag] {
   if(!owner||owner->stopping_||!owner->active_.contains(id)||!owner->preparedOwners_.remove(id))return;
   const bool finished=owner->finishedPrepared_.remove(id);
   // A legacy adapter reports no independent helper during preparation. Keep
   // its ordinary interactive reservation; early terminal/drain can release it.
   if(owner->selectingId_==id&&(*earlyError||flag->load()||finished))owner->selectingId_.clear();
   if(finished)owner->active_.remove(id);
  };
  if(QThread::currentThread()==owner->thread())release();
  else QMetaObject::invokeMethod(owner,std::move(release),Qt::QueuedConnection);
 };
 SourceAdapter::PreparedAcquisition acquisition;
 if(source=="file"){
  // A file is read and decoded by the editor stage; no native source is involved.
  acquisition=[sourcePath](const ResourceLimits&,std::function<void(PayloadResult)> deliver){ClipboardPayload payload;payload.kind="files";payload.files={sourcePath};deliver({payload,{}});};
  released();
 }else acquisition=source_.prepareAcquisition(source,snapshot.limits,std::move(earlyTerminal),std::move(released));
 const auto path=databasePath();background([path,op](Error& e) {HistoryStore store(path);e=store.saveOperation(op);return op.publicJson();},[this,op,flag,reply,acquisition,earlyError](Error e,QJsonObject result) mutable {
  if(e) {abandonUploadCopies(op.id);releaseActive(op.id);if(selectingId_==op.id&&!preparedOwners_.contains(op.id))selectingId_.clear();if(editingId_==op.id){editor_.reset();editingId_.clear();}reply(e,{});return;}
  observe(op.publicJson());reply({},{{"operationId",op.id},{"operation",result}});emit changed(op.id);emit observationsChanged();
  // Acceptance stays durable and observable before acquiring pixels. Region
  // preview may already be visible; a terminal event skips the remaining work.
  QTimer::singleShot(0,this,[this,op,flag,acquisition,earlyError]() mutable {
   if(flag->load()) {op.state="cancelled";op.cancelled=true;finish(op);return;}
   if(*earlyError) {op.error=*earlyError;op.cancelled=op.error.code=="cancelled"||op.error.code=="source_cancelled";op.state=op.cancelled?"cancelled":"failed";finish(op);return;}
   op.state="acquiring";op.phase="acquiring";const auto path=databasePath();background([path,op](Error& e) {HistoryStore store(path);e=store.saveOperation(op);return QJsonObject{};},[this,op,flag,acquisition,earlyError](Error e,QJsonObject) mutable {
    if(e) {op.error=e;op.state="failed";finish(op);return;}
    observe(op.publicJson());
    if(flag->load()) {op.state="cancelled";op.cancelled=true;finish(op);return;}
    if(*earlyError) {op.error=*earlyError;op.cancelled=op.error.code=="cancelled"||op.error.code=="source_cancelled";op.state=op.cancelled?"cancelled":"failed";finish(op);return;}
    QPointer<OperationService> guarded(this);
    acquisition(op.settings.limits,[guarded,op,flag](PayloadResult r) {
     if(!guarded)return;
     QMetaObject::invokeMethod(guarded,[guarded,op,flag,r=std::move(r)]() mutable {
      if(!guarded||guarded->stopping_)return;
      if(guarded->selectingId_==op.id&&!guarded->preparedOwners_.contains(op.id))guarded->selectingId_.clear();
      if(r.error||flag->load()) {op.error=r.error;op.cancelled=flag->load()||r.error.code=="cancelled"||r.error.code=="source_cancelled";op.state=op.cancelled?"cancelled":"failed";guarded->finish(op);return;}
      guarded->clipboardImports_.remove(op.id);
      if(op.action=="edit")guarded->startEditing(op,flag,std::move(r.payload));
      else guarded->enqueue({op,std::move(r.payload),true,flag,{}});
      guarded->pumpCopies();
     },Qt::QueuedConnection);
    });
   });
  });
 });
}
void OperationService::enqueue(Job job) {
#ifdef __GLIBC__
 if(job.acquire)heapReclamationPending_=true;
#endif
 if(job.cancelled->load()){job.payload={};job.op.cancelled=true;job.op.state="cancelled";finish(job.op);return;}
 if(job.payload.kind=="image"){
  const auto bytes=job.payload.image.sizeInBytes();
  const auto used=payloadBytes_->load();
  if(bytes>job.op.settings.limits.pendingImageBytes-used){
   job.payload={};job.op.error={"image_memory_limit","Queued image memory is full; wait for encoding or increase Queued image memory",{}};job.op.state="failed";finish(job.op);return;
  }
  payloadBytes_->fetch_add(bytes);
  job.payloadLease=std::shared_ptr<qint64>(new qint64(bytes),[counter=payloadBytes_](qint64* value){counter->fetch_sub(*value);delete value;});
 }
 job.op.phase="queued";observe(job.op.publicJson());queue_.enqueue(std::move(job));pump();
}
QString OperationService::editorDestination(const Operation& op) const {
 const auto route=op.settings.actions.value(op.action);
 if(route.destination!="upload"||route.profileId.isEmpty())return "Save to the local archive";
 const auto label=op.settings.profiles.value(route.profileId)["label"].toString();
 return label.isEmpty()?QStringLiteral("Upload"):QStringLiteral("Upload to %1").arg(label.left(160));
}
// The acquired source stays private to the editor stage. Only the editor's
// validated PNG becomes an artifact, preview, archive or publication input.
void OperationService::startEditing(Operation op,std::shared_ptr<std::atomic_bool> flag,ClipboardPayload payload) {
 auto session=editor_;
 if(!session||editingId_!=op.id||flag->load()){op.cancelled=true;op.state="cancelled";finish(op);return;}
 if(!(payload.kind=="image"||(payload.kind=="files"&&payload.files.size()==1))){op.error={"edit_source_unsupported","The editor needs exactly one static PNG or JPEG image",{}};op.state="failed";finish(op);return;}
 op.state="editing";op.phase="editing";
 const auto path=databasePath(),staging=root_+"/editing",helper=editorHelper_,destination=editorDestination(op);
 const auto recentColors=settings_.snapshot().editorRecentColors;
 const auto outputBytes=std::min<qint64>(op.settings.limits.maxFileBytes,qint64(512)*1024*1024);
 auto image=std::make_shared<QImage>(payload.kind=="image"?std::move(payload.image):QImage());
 const auto file=payload.kind=="files"?payload.files.first():QString();payload={};
 auto edited=std::make_shared<EditorResult>();
 background([path,op](Error& e){HistoryStore store(path);e=store.saveOperation(op);return QJsonObject{};},[=,this](Error e,QJsonObject) mutable {
  if(e){op.error=e;op.state="failed";finish(op);return;}
  observe(op.publicJson());emit changed(op.id);emit observationsChanged();
  background([=](Error&){
   if(!file.isEmpty()){
    // Only the header is inspected here; the editor helper decodes the file once.
    QFile input(file);
    if(!QFileInfo(file).isFile()||!input.open(QIODevice::ReadOnly)){edited->error={"source_unavailable","Cannot read the image file",{}};return QJsonObject{};}
    const auto maximum=std::min<qint64>(op.settings.limits.maxFileBytes,256*1024*1024);
    if(input.size()<1||input.size()>maximum){edited->error={"edit_input_too_large","This image file is too large to edit",{}};return QJsonObject{};}
    const auto header=input.read(64*1024);input.close();
    // Static PNG or JPEG by signature; an animated PNG declares acTL before its image data.
    const bool png=imageFormatHint(header)=="png",jpeg=header.startsWith(QByteArray::fromHex("ffd8ff"));
    const auto frames=header.indexOf("acTL"),data=header.indexOf("IDAT");
    if((!png&&!jpeg)||(png&&frames>=0&&(data<0||frames<data))){edited->error={"edit_source_unsupported","The editor opens static PNG or JPEG files",{}};return QJsonObject{};}
    *edited=EditorJob(staging,helper).withRecentColors(recentColors).runFile(file,maximum,destination,outputBytes,session);
    if(edited->error.code=="source_unsupported")edited->error={"edit_source_unsupported","The editor opens static PNG or JPEG files",{}};
    return QJsonObject{};
   }
   *edited=EditorJob(staging,helper).withRecentColors(recentColors).run(*image,destination,outputBytes,session);
   return QJsonObject{};
  },[=,this](Error,QJsonObject) mutable {
   if(editingId_==op.id){editor_.reset();editingId_.clear();}
   if(edited->recentColors)rememberEditorColors(*edited->recentColors);
   if(flag->load()||edited->error.code=="cancelled"){op.cancelled=true;op.state="cancelled";op.error=flag->load()?Error{}:edited->error;finish(op);return;}
   if(edited->error){op.error=edited->error;op.state="failed";finish(op);return;}
   ClipboardPayload finalized;finalized.kind="png";finalized.bytes=std::move(edited->png);finalized.mime="image/png";
   op.state="acquiring";op.phase="queued";
   enqueue({op,std::move(finalized),true,flag,{}});
  });
 });
}
// Custom colors picked in the editor persist as a settings preference. A
// conflicting concurrent save simply loses this update; colors are a convenience.
void OperationService::rememberEditorColors(const QStringList& colors) {
 if(colors==settings_.snapshot().editorRecentColors||!settings_.metadata()["validation"].toObject()["ok"].toBool())return;
 settings_.patchAsync({{"preferences",QJsonObject{{"editorRecentColors",QJsonArray::fromStringList(colors)}}}},settings_.revision(),[](Result){});
}
// Recording shares the editor's exclusivity: one interactive private review at a time.
Error OperationService::acceptRecording(Operation& op) {
 if(!ready_)return startupError_?startupError_:Error{"initializing","Journal recovery is in progress",{}};
 if(editor_)return {"editor_active","An edit is open; finish or cancel it before recording",{{"operationId",editingId_}}};
 if(!selectingId_.isEmpty())return {"busy_selecting","Another interactive acquisition is active",{}};
 auto snapshot=settings_.snapshot();if(active_.size()>=snapshot.limits.pendingOperations+snapshot.limits.concurrentUploads)return {"queue_full","Pending operation limit reached",{}};
 const auto route=snapshot.actions.value(Settings::recordingActionName());
 const bool upload=route.destination=="upload"&&!route.profileId.isEmpty();
 if(!upload&&!snapshot.archive.keepUploads)return {"output_required","No output is enabled. Choose a destination in Settings → Capture or enable Keep a local copy in Settings → General.",{}};
 op.id=uuid();op.action=Settings::recordingActionName();op.settings=snapshot;op.settingsRevision=settings_.revision();op.created=QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);op.invocation=upload?++invocation_:invalidateCopyEligibility();op.clipboardRevision=source_.clipboardRevision();
 if(upload) {const auto profile=snapshot.profiles.value(route.profileId);op.destination={profile["provider"].toString(),route.profileId,settings_.revision(),profile["settings"].toObject(),profile["credentials"].toObject()};}
 auto flag=std::make_shared<std::atomic_bool>(false);active_.insert(op.id,flag);if(upload)beginUploadCopies(op,false);
#ifdef __GLIBC__
 heapReclamationPending_=true;
#endif
 return {};
}
void OperationService::recordingCommand(const QString& command,const QJsonObject& params,JsonReply reply) {
 if(!ready_&&command!="capabilities") {reply(startupError_?startupError_:Error{"initializing","Journal recovery is in progress",{}},{});return;}
 recording_->command(command,params,std::move(reply));
}
void OperationService::editorCommand(const QString& id,const QString& command,const QJsonObject& params,JsonReply reply) {
 if(!editor_||editingId_!=id){reply({"editor_unavailable","No open editor has this operation ID",{}},{});return;}
 QPointer<OperationService> owner(this);
 editor_->request(command,params,[owner,id,reply](Error error,QJsonObject result){
  if(!owner)return;
  QMetaObject::invokeMethod(owner,[owner,id,reply,error,result]() mutable {
   if(!owner||owner->stopping_)return;
   if(!error)result["operationId"]=id;
   reply(error,result);
  },Qt::QueuedConnection);
 });
}
void OperationService::pump() {
 while(!queue_.isEmpty()&&running_<queue_.head().op.settings.limits.concurrentUploads&&!stopping_) {auto job=queue_.dequeue();++running_;background([this,job=std::move(job)](Error&) mutable {return run(std::move(job)).toJson();},[this](Error,QJsonObject doc) {--running_;auto op=Operation::fromJson(doc);finish(op);pump();});}
}
Operation OperationService::run(Job job) {
 auto op=job.op;HistoryStore store(databasePath());ArtifactStore artifacts(root_+"/artifacts");
 auto save=[&]() {auto e=store.saveOperation(op,&providers_);if(e) {op.error=e;op.state="failed";}else observe(op.publicJson());return !e;};
 auto saveItem=[&](const OperationItem& item) {
  auto e=store.checkpointOperation(op,{item.artifact.id},&providers_);
  if(e){op.error=e;op.state="failed";return false;}
  observe(op.publicJson());
  if(item.linkAvailable()) {
   int index=0;while(index<op.items.size()&&op.items[index].artifact.id!=item.artifact.id)++index;
   QPointer<OperationService> owner(this);
   QMetaObject::invokeMethod(this,[owner,id=op.id,itemId=item.artifact.id,index,url=item.receipt.directUrl]{if(owner&&!owner->stopping_)owner->uploadLinkReady(id,itemId,index,url);},Qt::QueuedConnection);
  }
  return true;
 };
 if(job.cancelled->load()){job.payload={};job.payloadLease.reset();op.cancelled=true;op.state="cancelled";save();return op;}
 if(job.acquire) {op.phase="preparing";if(!save())return op;std::lock_guard acquisitionGuard(acquisitionMutex_);auto acquired=artifacts.acquire(op.id,job.payload,op.settings.limits,op.action,op.settings,QDateTime::fromString(op.created,Qt::ISODateWithMs));
  job.payload={};job.payloadLease.reset(); // Retry/upload ownership now belongs to the immutable disk artifact.
  if(acquired.error) {op.error=acquired.error;op.state="failed";save();return op;}
  for(const auto& a:acquired.artifacts) {OperationItem item;item.phase="stored";item.artifact=a;item.artifact.previewPath=artifacts.preview(a,op.settings.previewCacheBytes,op.settings.limits.imagePixels);item.publicationId=uuid();op.items.append(item);}op.state="stored";if(!save())return op;
 }
 // Archive every acquired item before any item in this batch is published.
 op.phase="saving_local";
 const auto route=op.settings.actions.value(op.action);
 const bool local=route.destination=="local"||route.profileId.isEmpty();
 for(auto& item:op.items) if((local||op.settings.archive.keepUploads)&&item.local.status!="saved") {item.phase="saving_local";observe(op.publicJson());item.local=LocalArchive::save(item.artifact,op.settings.archive);item.phase="stored";if(!saveItem(item))return op;}
 if(local) {for(auto& item:op.items) item.uploadStatus="local_only";op.state="completed";for(const auto& item:op.items) if(item.local.status!="saved")op.state="failed";save();return op;}
 auto provider=providers_.find(op.destination.providerId);if(!provider) {op.error={"provider_unavailable","Restore access to the original destination",{}};op.state="failed";save();return op;}
 const auto capabilities=provider->capabilities(op.destination);
 auto publicationMode=op.settings.actions.value(op.action).publicationMode;
 if(publicationMode=="default")publicationMode=provider->defaultPublicationMode(op.destination);
 if(publicationMode=="private"&&!capabilities.privateUploads){op.error={"unsupported_capability","This provider cannot perform a private upload",{}};op.state="failed";save();return op;}
 if(!capabilities.conditionalCreate||(op.settings.collisionPolicy=="overwrite"&&!capabilities.conditionalReplace)) {op.error={"unsupported_capability","Provider cannot enforce this collision policy",{}};op.state="failed";save();return op;}
 bool needsNames=job.acquire;for(const auto& item:op.items)if(!terminalUpload(item.uploadStatus)&&item.candidate.isEmpty())needsNames=true;
 if(needsNames) {QList<Artifact> list;for(const auto& item:op.items)list.append(item.artifact);auto names=UploadNaming::batch(list,op.settings.namingMode,op.settings.collisionPolicy,capabilities.maximumNameBytes);for(int i=0;i<op.items.size();++i) {if(!op.items[i].candidate.isEmpty())continue;op.items[i].candidate=names[i].name;if(names[i].status!="pending")op.items[i].uploadStatus=names[i].status;}
  if(!save())return op;
 }
 op.state="running";op.phase="preflight";if(!save())return op;
 // Check all pending names before the first PUT, including opaque references that
 // may identify one target despite differing proposed display names.
 QHash<QByteArray,QList<int>> targets;
 for(int index=0;index<op.items.size();++index) {
  auto& item=op.items[index];if(terminalUpload(item.uploadStatus)||item.uploadStatus=="unknown")continue;
  if(job.cancelled->load())break;
  item.phase="preflight";observe(op.publicJson());
  if(auto e=artifacts.verify(item.artifact)) {item.error=e;item.uploadStatus="failed";if(!saveItem(item))return op;continue;}
  if(!item.objectReference.isEmpty()&&!item.condition.isEmpty()&&item.uploadStatus!="failed") {targets[QJsonDocument(item.objectReference).toJson(QJsonDocument::Compact)].append(index);continue;}
  bool checked=false;
  for(int attempts=0;attempts<32;++attempts) {
   auto check=storageRequest<NameCheck>([&](auto cb) {provider->checkName(op.destination,item.candidate,cb);});
   if(check.error||check.status=="inconclusive"||check.objectReference.isEmpty()) {item.error=check.error?check.error:Error{"existence_unknown","Name availability is inconclusive",{}};item.uploadStatus="failed";break;}
   if(check.status=="present"&&op.settings.collisionPolicy=="skip") {item.uploadStatus="skipped";break;}
   if(check.status=="present"&&op.settings.collisionPolicy=="rename") {item.candidate=UploadNaming::renamedCandidate(item.candidate,capabilities.maximumNameBytes);if(!saveItem(item))return op;continue;}
   item.objectReference=check.objectReference;item.concurrencyToken=check.concurrencyToken;item.condition=check.status=="present"?"match":"absent";
   if(item.condition=="match"&&item.concurrencyToken.isEmpty()) {item.error={"precondition_missing","Provider returned no replacement precondition",{}};item.uploadStatus="failed";break;}
   item.uploadStatus="stored";targets[QJsonDocument(item.objectReference).toJson(QJsonDocument::Compact)].append(index);checked=true;break;
  }
  if(!checked&&item.uploadStatus=="stored") {item.uploadStatus="failed";item.error={"collision_exhausted","No available candidate after 32 checks",{}};}
  if(!saveItem(item))return op;
 }
 for(const auto& group:targets) if(group.size()>1) {if(op.settings.collisionPolicy=="overwrite") {for(int i:group)op.items[i].uploadStatus="conflict";}else if(op.settings.collisionPolicy=="skip") {for(int n=1;n<group.size();++n)op.items[group[n]].uploadStatus="skipped";}else {for(int n=1;n<group.size();++n) {auto& item=op.items[group[n]];item.uploadStatus="failed";item.error={"name_alias_conflict","Provider candidates resolve to one target",{}};}}}
 if(!save())return op;
 for(auto& item:op.items) {
  if(job.cancelled->load()) {op.cancelled=true;break;}
  if(terminalUpload(item.uploadStatus)||item.uploadStatus=="failed")continue;
  auto lock=objectLock(op.destination,item.objectReference,item.candidate);std::unique_lock guard(*lock);
  if(item.uploadStatus=="unknown") {
   op.phase="reconciling";item.phase="reconciling";observe(op.publicJson());
   auto stat=storageRequest<StatResult>([&](auto cb) {auto request=item.uploadRequest(op.destination,op.requestTimeoutMs);request.cancellation=job.cancelled;provider->reconcile(request,cb);});
   if(stat.status=="own"&&!stat.receipt.validate()&&stat.receipt.publicationId==item.publicationId&&stat.receipt.destination.toJson()==op.destination.toJson()) {item.receipt=stat.receipt;item.uploadStatus="uploaded";item.error={};if(!saveItem(item))return op;continue;}
   if(stat.status!="missing") {item.phase="unknown";item.error=stat.error?stat.error:Error{"publication_unknown","Resolve ambiguous publication before uploading again",{}};if(!saveItem(item))return op;continue;}
   item.uploadStatus="stored";if(auto error=artifacts.verify(item.artifact)) {item.uploadStatus="failed";item.error=error;if(!saveItem(item))return op;continue;}if(!saveItem(item))return op;
  }
  for(int attempts=0;attempts<32;++attempts) {
   item.uploadStatus="uploading";item.phase="uploading";item.uploadProgress.reset();op.phase="uploading";if(!saveItem(item))return op;
   struct ProgressState{std::mutex mutex;bool closed=false;std::optional<UploadProgress> latest;QJsonObject document;};
   auto activity=std::make_shared<StorageActivity>(*this,!provider->reportsUploadProgress());
   auto progress=std::make_shared<ProgressState>();progress->document=op.publicJson();const auto artifactId=item.artifact.id;QPointer<OperationService> owner(this);
   auto uploaded=awaitProvider<UploadResult>([&](auto cb) {
    auto request=item.uploadRequest(op.destination,op.requestTimeoutMs);request.cancellation=job.cancelled;
    request.progress=[owner,progress,activity,artifactId](const UploadProgress& value){
     if(!value.valid())return;
     std::lock_guard lock(progress->mutex);if(progress->closed||!owner)return;progress->latest=value;activity->set(value.phase!="preparing");
     progress->document["phase"]=value.phase;auto items=progress->document["items"].toArray();for(qsizetype i=0;i<items.size();++i){auto observed=items[i].toObject();if(observed["artifact"].toObject()["id"].toString()==artifactId){observed["phase"]=value.phase;observed["uploadProgress"]=value.toJson();items[i]=observed;break;}}progress->document["items"]=items;owner->observe(progress->document);
    };
    provider->upload(request,cb);
   });
   {std::lock_guard lock(progress->mutex);progress->closed=true;activity->set(false);item.uploadProgress=progress->latest;}
   item.phase=uploaded.status;
   item.error=uploaded.error;
   if(uploaded.status=="uploaded") {
    auto e=uploaded.receipt.validate();if(!e&&(uploaded.receipt.publicationId!=item.publicationId||uploaded.receipt.destination.toJson()!=op.destination.toJson()))e={"receipt_invalid","Receipt does not match the accepted publication identity",{}};
    if(e) {item.uploadStatus="unknown";item.error=e;}else {item.receipt=uploaded.receipt;item.uploadStatus="uploaded";item.error={};}if(!saveItem(item))return op;break;
   }
   if(uploaded.status=="unknown") {item.uploadStatus="unknown";if(!saveItem(item))return op;break;}
   if(uploaded.status!="collision") {item.uploadStatus="failed";if(!saveItem(item))return op;break;}
   // Reconcile our publication before changing any proposed target after a race.
   op.phase="reconciling";item.phase="reconciling";observe(op.publicJson());
   auto stat=storageRequest<StatResult>([&](auto cb) {auto request=item.uploadRequest(op.destination,op.requestTimeoutMs);request.cancellation=job.cancelled;provider->reconcile(request,cb);});
   if(stat.status=="own"&&!stat.receipt.validate()&&stat.receipt.publicationId==item.publicationId&&stat.receipt.destination.toJson()==op.destination.toJson()) {item.receipt=stat.receipt;item.uploadStatus="uploaded";item.error={};if(!saveItem(item))return op;break;}
   const bool unpublishedDraft=stat.status=="missing"&&uploaded.error.code=="upload_draft_conflict";
   if(stat.status!="foreign"&&!unpublishedDraft) {item.uploadStatus="unknown";item.phase="unknown";item.error=stat.error?stat.error:Error{"publication_unknown","Collision publication could not be reconciled",{}};if(!saveItem(item))return op;break;}
   if(op.settings.collisionPolicy=="skip") {item.uploadStatus="skipped";if(!saveItem(item))return op;break;}
   if(op.settings.collisionPolicy!="rename") {item.uploadStatus="failed";item.error={"precondition_changed","Original replacement precondition changed",{}};if(!saveItem(item))return op;break;}
   item.candidate=UploadNaming::renamedCandidate(item.candidate,capabilities.maximumNameBytes);item.objectReference={};item.concurrencyToken={};item.condition="absent";item.uploadStatus="stored";item.phase="preflight";op.phase="preflight";if(!saveItem(item))return op;
   auto check=storageRequest<NameCheck>([&](auto cb) {provider->checkName(op.destination,item.candidate,cb);});
   if(check.status!="absent"||check.error||check.objectReference.isEmpty()) {item.uploadStatus="failed";item.error={"existence_unknown","Renamed candidate could not be reserved",{}};if(!saveItem(item))return op;break;}item.objectReference=check.objectReference;if(!saveItem(item))return op;
   guard.unlock();lock=objectLock(op.destination,item.objectReference,item.candidate);guard=std::unique_lock<std::mutex>(*lock);
   if(attempts==31) {item.uploadStatus="failed";item.error={"collision_exhausted","Collision search exhausted",{}};saveItem(item);}
  }
 }
 // Persist the uploaded receipt before the separate sharing mutation. Sharing
 // failure leaves an uploaded file and resumes only the link step on explicit retry.
 for(auto& item:op.items) {
  if(item.uploadStatus!="uploaded"||job.cancelled->load())continue;
  if(!capabilities.sharing)continue;
  auto lock=objectLock(item.receipt.destination,item.receipt.objectReference,{});std::unique_lock guard(*lock);
  const auto current=store.detail(item.artifact.id);
  if(current.contains("shareStatus")){item.shareStatus=current["shareStatus"].toString();item.shareIntent=current["shareIntent"].toString();item.shareIntentOptions=current["shareIntentOptions"].toObject();item.receipt=StorageReceipt::fromJson(current["receipt"].toObject());}
  if(!QStringList{"not_requested","unknown"}.contains(item.shareStatus)&&!(QStringList{"private","failed"}.contains(item.shareStatus)&&item.shareIntent=="create"))continue;
  if(publicationMode=="private"&&item.shareStatus!="unknown"&&item.shareIntent!="create"){item.shareStatus="private";if(!saveItem(item))return op;continue;}
  if(!item.receipt.directUrl.isEmpty())continue;
  op.phase="sharing";item.phase="sharing";if(!saveItem(item))return op;
  const auto shareId=uuid();
  QJsonObject doc{{"id",shareId},{"historyId",item.artifact.id},{"action","create"},{"state","running"},{"status","pending"},{"receipt",item.receipt.toJson()},{"progress",QJsonObject{}},{"options",item.shareIntentOptions}};
  if(item.shareStatus=="unknown")doc["action"]="inspect";
  if(auto e=store.saveSharing(doc)){item.error=e;op.state="partial";saveItem(item);return op;}
  const auto path=root_+"/history.sqlite";
  auto checkpoint=[path,&doc](const QJsonObject& progress,std::function<void(Error)> ack){
   auto proposed=doc;proposed["progress"]=progress;HistoryStore durable(path);const auto e=durable.saveSharing(proposed);if(!e)doc=std::move(proposed);ack(e);
  };
  ShareRequest request{shareId,doc["action"].toString(),item.receipt,doc["options"].toObject(),doc["progress"].toObject(),job.cancelled};
  auto result=storageRequest<ShareResult>([&](auto cb){provider->share(request,checkpoint,cb);});
  if(!shareResultMatchesReceipt(result,item.receipt)){result.status="unknown";result.receipt=item.receipt;result.receipt.directUrl.clear();result.error={"share_identity_changed","Sharing returned a different publication identity",{}};}
  if(result.status!="shared")result.receipt.directUrl.clear();
  item.receipt=result.receipt;item.shareStatus=result.status;item.phase=result.status;item.error=result.error;
  doc["state"]="completed";doc["status"]=result.status;doc["receipt"]=result.receipt.toJson();doc["progress"]=result.progress;doc["error"]=result.error.toJson();
  if(auto e=store.saveSharing(doc)){
   item.error=e;op.state="partial";
   QPointer<OperationService> owner(this);QMetaObject::invokeMethod(this,[owner,doc,e]{if(owner)emit owner->sharingStopped(doc,e);},Qt::QueuedConnection);
   return op;
  }
  const auto committed=store.sharing(shareId);if(result.status=="shared"||result.status=="revoked"||result.status=="changed"){item.shareIntent.clear();item.shareIntentOptions={};}else if(committed["action"]=="create"){item.shareIntent="create";item.shareIntentOptions=committed["options"].toObject();}
  if(!saveItem(item))return op;
 }
 op.cancelled=op.cancelled||job.cancelled->load();
 op.state=op.cancelled?"cancelled":"completed";for(const auto& item:op.items) if(item.uploadStatus=="failed"||item.uploadStatus=="unknown"||item.shareStatus=="failed"||item.shareStatus=="unknown"||item.shareStatus=="changed"||item.shareIntent=="create")op.state=op.cancelled?"cancelled":"partial";
 op.phase=op.state;save();return op;
}
void OperationService::releaseActive(const QString& id) {
 // Durable completion and source drain can arrive in either order. Retain the
 // bounded operation slot until both have happened; never unlock a live helper.
 if(preparedOwners_.contains(id))finishedPrepared_.insert(id);
 else active_.remove(id);
}
void OperationService::finish(Operation op,JsonReply reply) {
 clipboardImports_.remove(op.id);pumpCopies();
 if(editingId_==op.id){if(editor_)editor_->cancel();editor_.reset();editingId_.clear();}
 op.phase=op.state;
 if(selectingId_==op.id&&!preparedOwners_.contains(op.id))selectingId_.clear();
 if((op.cancelled||op.state=="failed"||op.state=="interrupted")&&op.copyStatus=="pending")op.copyStatus="no_result";
 const auto path=databasePath();background([path,op](Error& e) {HistoryStore store(path);e=store.saveOperation(op);return e?QJsonObject{}:store.operation(op.id,&e).toJson();},[this,id=op.id,reply](Error e,QJsonObject document) {
  clearObservation(id);emit changed(id);emit observationsChanged();
  if(e) {abandonUploadCopies(id);releaseActive(id);if(reply)reply(e,{});return;}
  const auto op=Operation::fromJson(document);
  if(uploadCopies_.contains(id)){finishUploadCopies(op,reply);return;}
  if(op.cancelled||op.state=="failed"||op.state=="interrupted") {emit resultReady(op.id);releaseActive(op.id);if(reply)reply({},op.publicJson());return;}
  performCopy(op,false,[this,id=op.id,reply](Error error,QJsonObject result) {releaseActive(id);if(reply)reply(error,result);});
 });
}

void OperationService::recent(JsonReply reply) {const auto path=databasePath();const int limit=settings_.snapshot().recentActivityLimit;background([path,limit](Error& e) {HistoryStore store(path);return QJsonObject{{"operations",store.recent(limit,&e)}};},[this,reply](Error e,QJsonObject data){auto values=data["operations"].toArray();for(qsizetype i=0;i<values.size();++i)values[i]=currentObservation(values[i].toObject());data["operations"]=values;reply(e,data);});}
void OperationService::inspect(const QString& id,JsonReply reply) {const auto path=databasePath();background([path,id](Error& e) {HistoryStore store(path);return store.operation(id,&e).publicJson();},[this,reply](Error e,QJsonObject data){reply(e,currentObservation(data));});}
void OperationService::cancel(const QString& id,JsonReply reply) {
 if(active_.contains(id)) {active_[id]->store(true);interruptUploadCopy(id);if(selectingId_==id)source_.cancelAcquisition();if(editingId_==id&&editor_)editor_->cancel();
  for(int index=0;index<queue_.size();++index)if(queue_[index].op.id==id){auto job=queue_.takeAt(index);job.payload={};job.payloadLease.reset();job.op.cancelled=true;job.op.state="cancelled";finish(job.op);break;}
  reply({},{{"operationId",id},{"cancellationRequested",true},{"effect","Already sent network requests cannot be rolled back"}});return;}
 const auto path=databasePath();background([path,id](Error& e) {HistoryStore store(path);auto op=store.operation(id,&e);if(e)return QJsonObject{};op.cancelled=true;op.state="cancelled";op.phase="cancelled";for(auto& item:op.items)item.phase=item.uploadStatus;e=store.saveOperation(op);return op.publicJson();},reply);
}
void OperationService::retry(const QString& id,JsonReply reply) {
 if(cleanupInProgress_){reply({"cleanup_busy","Wait for artifact cleanup to finish",{}},{});return;}
 if(!ready_) {reply(startupError_?startupError_:Error{"initializing","Journal recovery is in progress",{}},{});return;}
 const auto limits=settings_.snapshot().limits;if(active_.size()>=limits.pendingOperations+limits.concurrentUploads) {reply({"queue_full","Pending operation limit reached",{}},{});return;}
 if(active_.contains(id)) {reply({"operation_busy","Operation is already active",{}},{});return;}
 auto reserved=std::make_shared<std::atomic_bool>(false);active_.insert(id,reserved);const auto path=databasePath();background([path,id](Error& e) {HistoryStore store(path);return store.operation(id,&e).toJson();},[this,reply,id,reserved,path](Error e,QJsonObject doc) {
  if(e) {active_.remove(id);reply(e,{});return;}auto op=Operation::fromJson(doc);if(op.items.isEmpty()) {active_.remove(id);reply({"artifact_missing","No acquired artifacts; invoke a new action",{}},{});return;}
  auto provider=providers_.find(op.destination.providerId);const bool sharing=provider&&provider->capabilities(op.destination).sharing;auto mode=op.settings.actions.value(op.action).publicationMode;if(mode=="default"&&provider)mode=provider->defaultPublicationMode(op.destination);
  bool pending=false;for(const auto& i:op.items)if(!terminalUpload(i.uploadStatus)||(sharing&&i.uploadStatus=="uploaded"&&(i.shareStatus=="unknown"||(i.shareStatus=="private"&&i.shareIntent=="create")||(mode=="share"&&(i.shareStatus=="not_requested"||(i.shareStatus=="failed"&&i.shareIntent=="create"))))))pending=true;
  if(!pending) {active_.remove(id);performCopy(op,true,reply);return;}
  if(provider)if(auto timeout=provider->retryRequestTimeout(op.destination,settings_.snapshot().profiles.value(op.destination.profileId)))op.requestTimeoutMs=std::max(op.requestTimeoutMs.value_or(0),*timeout);
  op.cancelled=reserved->load();op.error={};op.state="stored";op.phase="queued";op.invocation=++invocation_;op.clipboardRevision=source_.clipboardRevision();op.copyStatus="pending";beginUploadCopies(op);
  background([path,op](Error& error){HistoryStore store(path);error=store.saveOperation(op);return QJsonObject{};},[this,reply,op,reserved](Error error,QJsonObject)mutable{
   if(error){abandonUploadCopies(op.id);releaseActive(op.id);reply(error,{});return;}
   op.cancelled=reserved->load();enqueue({op,{},false,reserved,{}});reply({},{{"operationId",op.id},{"retryAccepted",true}});
  });
 });
}
void OperationService::retryLocalSave(const QString& id,JsonReply reply) {
 if(cleanupInProgress_){reply({"cleanup_busy","Wait for artifact cleanup to finish",{}},{});return;}
 if(!ready_){reply(startupError_?startupError_:Error{"initializing","Journal recovery is in progress",{}},{});return;}
 const auto limits=settings_.snapshot().limits;
 if(active_.size()>=limits.pendingOperations+limits.concurrentUploads){reply({"queue_full","Pending operation limit reached",{}},{});return;}
 if(active_.contains(id)){reply({"operation_busy","Operation is active",{}},{});return;}
 active_.insert(id,std::make_shared<std::atomic_bool>(false));
 // A local retry keeps its durable result; only its live observation is active.
 observe({{"id",id},{"state","running"},{"phase","saving_local"},{"created",QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)}});
 emit observationsChanged();
 const auto path=databasePath();
 background([this,path,id](Error& e) {
  HistoryStore store(path);auto op=store.operation(id,&e);if(e)return QJsonObject{};
  ArtifactStore artifacts(root_+"/artifacts");
  for(auto& item:op.items)if(item.local.status!="saved") {
   if(auto error=artifacts.verify(item.artifact)){item.local.status="failed";item.local.error=error;}
   else item.local=LocalArchive::save(item.artifact,op.settings.archive);
  }
  e=store.saveOperation(op);return op.publicJson();
 },[this,id,reply](Error e,QJsonObject r) {
  clearObservation(id);emit changed(id);emit observationsChanged();releaseActive(id);
  bool failed=bool(e);for(const auto& value:r["items"].toArray())failed=failed||value.toObject()["local"].toObject()["status"]=="failed";
  if(failed)emit operationError();
  reply(e,r);
 });
}
void OperationService::copyResult(const QString& id,JsonReply reply) {
 JsonReply pendingReply;
 if(active_.contains(id)&&!detachCompletedUploadCopies(id,pendingReply)) {reply({"operation_busy","Wait for the operation to finish before copying its result",{}},{});return;}
 auto completed=[reply,pendingReply](Error error,QJsonObject result){if(pendingReply)pendingReply(error,result);reply(error,result);};
 const auto ticket=invalidateCopyEligibility();const auto path=databasePath();
 background([path,id](Error& e) {HistoryStore store(path);return store.operation(id,&e).toJson();},[this,completed,ticket](Error e,QJsonObject doc) {if(e)completed(e,{});else performCopy(Operation::fromJson(doc),true,completed,ticket);});
}
void OperationService::performCopy(Operation op,bool explicitCopy,JsonReply reply,quint64 ticket) {
 if(explicitCopy&&ticket==0)ticket=invalidateCopyEligibility();
 const bool eligible=(explicitCopy&&ticket==invocation_)||(!explicitCopy&&(op.invocation==invocation_&&op.clipboardRevision==source_.clipboardRevision()));
 const auto path=databasePath();
 QPointer<OperationService> guarded(this);
 auto record=[guarded,path,reply](Operation value) {if(!guarded)return;guarded->background([path,value](Error& e) {HistoryStore store(path);e=store.saveOperation(value);return value.publicJson();},[guarded,value,reply](Error e,QJsonObject result) {if(!guarded)return;emit guarded->changed(value.id);if(!e)emit guarded->resultReady(value.id);if(reply)reply(e,result);});};
 if(!eligible) {op.copyStatus="suppressed";record(op);return;}
 QStringList urls,files;QString localKind;
 for(const auto& item:op.items) {if(item.linkAvailable())urls.append(item.receipt.directUrl);if(item.uploadStatus=="local_only"&&item.local.status=="saved") {files.append(item.local.path);localKind=item.artifact.category;}}
 if(urls.isEmpty()&&files.isEmpty()) {op.copyStatus="no_result";record(op);return;}
 CopyPayload payload;
 if(!urls.isEmpty()) {payload.kind="text";payload.text=urls.join('\n');}
 else {payload.kind="files";payload.files=files;}
 const auto completeCopy=[op,record](Error error) mutable {
  op.copyStatus=error?(error.code=="copy_suppressed"?"suppressed":"failed"):"copied";
  if(error)op.error=error;
  record(op);
 };
 // File batches always copy URIs. Detached text/image acquisitions copy their MIME.
 if(urls.isEmpty()&&op.items.size()==1&&!op.items[0].artifact.originalFile&&(localKind=="images"||localKind=="text")) {
#ifdef __GLIBC__
  heapReclamationPending_=true;
#endif
  auto artifact=op.items[0].artifact;if(!QFile::exists(artifact.path))artifact.path=op.items[0].local.path;
  const auto ticket=invocation_;const auto revision=source_.clipboardRevision();auto prepared=std::make_shared<CopyPayload>();
  background([artifact,localKind,prepared](Error& e) {
   QFile file(artifact.path);
   if(!file.open(QIODevice::ReadOnly)) {e={"artifact_missing","Cannot read clipboard result",{}};return QJsonObject{};}
   if(file.size()!=artifact.bytes) {e={"artifact_changed","Clipboard result changed",{}};return QJsonObject{};}
   const auto bytes=file.read(artifact.bytes+1);
   if(bytes.size()!=artifact.bytes||QString::fromLatin1(QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex())!=artifact.sha256) {
    e={"artifact_changed","Clipboard result changed",{}};return QJsonObject{};
   }
   if(localKind=="images") {
    prepared->kind="image";prepared->encodedImage=bytes;
    if(!bytes.startsWith(QByteArray::fromHex("89504e470d0a1a0a")))e={"artifact_invalid","Image result is not a PNG",{}};
   }else {prepared->kind="text";prepared->text=QString::fromUtf8(bytes);}
   return QJsonObject{};
  },[this,op,ticket,revision,record,prepared,completeCopy](Error e,QJsonObject) mutable {
   if(e) {op.copyStatus="failed";op.error=e;record(op);return;}
   if(ticket!=invocation_||revision!=source_.clipboardRevision()) {op.copyStatus="suppressed";record(op);return;}
   deliverCopy(*prepared,completeCopy,[this,ticket,revision]{return ticket==invocation_&&revision==source_.clipboardRevision();});
  });return;
 }
 deliverCopy(payload,completeCopy,[this,op,explicitCopy,ticket]{return explicitCopy?ticket==invocation_:(op.invocation==invocation_&&op.clipboardRevision==source_.clipboardRevision());});
}
void OperationService::copyHistoryImage(const QString& id,JsonReply reply) {
 const auto ticket=invalidateCopyEligibility();auto preparation=std::make_shared<CopyPreparationState>();preparation->revision=source_.clipboardRevision();const auto path=databasePath();auto prepared=std::make_shared<CopyPayload>();
#ifdef __GLIBC__
 heapReclamationPending_=true;
#endif
 const auto eligible=[this,ticket,preparation]{return ticket==invocation_&&preparation->revision==source_.clipboardRevision();};
 queueCopy([prepared]{return *prepared;},[reply](Error e){reply(e,{{"copied",!e}});},eligible,false,[this,path,id,prepared,eligible,preparation](std::function<void(Error)> complete) {
  if(!eligible()){complete({"copy_suppressed","A newer clipboard action superseded this image copy",{}});return;}
  background([path,id,prepared,preparation](Error& e) {
  HistoryStore store(path);const auto data=store.detail(id,&e);if(e)return QJsonObject{};
  const auto local=data["local"].toObject();
  if(local["status"]!="saved"||!data["mime"].toString().startsWith("image/")){e={"image_unavailable","A retained local original image is needed to copy image bytes",{}};return QJsonObject{};}
  QFile file(local["path"].toString());
#ifdef Q_OS_UNIX
  const int fd=::open(QFile::encodeName(file.fileName()).constData(),O_RDONLY|O_CLOEXEC|O_NONBLOCK|O_NOFOLLOW);struct stat identity{};
  if(fd<0||::fstat(fd,&identity)||!S_ISREG(identity.st_mode)){if(fd>=0)::close(fd);e={"artifact_missing","Cannot read the retained original image",{}};return QJsonObject{};}
  if(!file.open(fd,QIODevice::ReadOnly,QFileDevice::AutoCloseHandle)){::close(fd);e={"artifact_missing","Cannot read the retained original image",{}};return QJsonObject{};}
#else
  if(!QFileInfo(file).isFile()||!file.open(QIODevice::ReadOnly)){e={"artifact_missing","Cannot read the retained original image",{}};return QJsonObject{};}
#endif
  constexpr qint64 inputLimit=32*1024*1024;
  if(file.size()<1||file.size()>inputLimit){e={"image_limit","This original image is too large to copy safely",{}};return QJsonObject{};}
  const auto bytes=file.read(inputLimit+1);
  if(bytes.size()!=data["bytes"].toInteger()||QString::fromLatin1(QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex())!=data["sha256"].toString()){e={"artifact_changed","The retained original image changed",{}};return QJsonObject{};}
  // Sixteen-bit RGBA uses eight bytes per pixel; retain that precision.
  ResourceLimits limits;limits.imagePixels=16000000;limits.imageBytes=128*1024*1024;limits.maxFileBytes=129*1024*1024;
  // Generic image codecs stay in the transient GUI worker; the resident owns
  // only the bounded full-size PNG, never a preview or a persistent GUI cache.
  prepared->kind="image";
#ifdef Q_OS_UNIX
  prepared->encodedImage=ImageJob().decodePng(bytes,limits,e,preparation->cancelled);
#else
  QBuffer input;input.setData(bytes);input.open(QIODevice::ReadOnly);QImageReader reader(&input);const auto size=reader.size();
  if(!size.isValid()||qint64(size.width())*size.height()>limits.imagePixels){e={"image_limit","This original image is too large to copy safely",{}};return QJsonObject{};}
  const auto image=reader.read();QBuffer output(&prepared->encodedImage);output.open(QIODevice::WriteOnly);
  if(image.isNull()||image.sizeInBytes()>limits.imageBytes||!image.save(&output,"PNG")||prepared->encodedImage.size()>limits.maxFileBytes)e={"image_read","Could not prepare the retained original image",{}};
#endif
  return QJsonObject{};
  },[complete,eligible,preparation](Error e,QJsonObject){if(preparation->cancelled->load()||!eligible())e={"copy_suppressed","A newer clipboard action superseded this image copy",{}};complete(e);});
 },{},preparation);
}
void OperationService::copyHistoryUrl(const QString& url,JsonReply reply,quint64 ticket) {if(ticket==0)ticket=invalidateCopyEligibility();if(ticket!=invocation_) {reply({"copy_suppressed","A newer sharing invocation superseded this copy request",{}},{});return;}if(url.isEmpty()) {reply({"link_unavailable","This record has no confirmed direct URL",{}},{});return;}CopyPayload p;p.kind="text";p.text=url;deliverCopy(p,[reply](Error e) {reply(e,{{"copied",!e}});},[this,ticket]{return ticket==invocation_;});}
QJsonObject OperationService::capabilities() const {
#ifdef Q_OS_UNIX
 const bool editor=QFileInfo(editorHelper_.isEmpty()?QCoreApplication::applicationDirPath()+"/putgd-ui":editorHelper_).isExecutable();
#else
 const bool editor=false;
#endif
 return {{"source",source_.capabilities()},{"providers",providers_.capabilities()},
  {"editor",QJsonObject{{"available",editor},{"admissionPixels",double(EditorJob::admissionPixels)},{"open",!editingId_.isEmpty()},
   {"sources",QJsonArray{"region","window","desktop","clipboard","file"}},{"fileFormats",QJsonArray{"png","jpeg"}},{"output","png"},
   {"shapes",QJsonArray{"redact","blur","arrow","rectangle","text","highlight"}},
   {"commands",QJsonArray{"inspect","apply","preview","saveCopy","submit","cancel","raise"}}}},
  {"recording",recording_->capabilities()}};
}
QJsonObject OperationService::diagnostics() const {
#ifdef __GLIBC__
 constexpr bool reclaimSupported=true;
#else
 constexpr bool reclaimSupported=false;
#endif
 return {{"schemaVersion",1},{"build",QJsonDocument::fromJson(buildManifestJson()).object()},{"backend","Qt"},{"qtVersion",QT_VERSION_STR},{"activeOperations",active_.size()},{"journalReady",ready_},{"capabilities",capabilities()},{"storage",providers_.diagnostics()},
  {"memory",QJsonObject{{"allocatorReclamationSupported",reclaimSupported},{"allocatorReclamationPending",heapReclamationPending_},{"allocatorReclamationCount",qint64(heapReclamationCount_)},{"lastAllocatorReclamationUsec",lastHeapReclamationUsec_}}}};
}
void OperationService::cleanup(JsonReply reply) {if(cleanupInProgress_){reply({"cleanup_busy","Wait for artifact cleanup to finish",{}},{});return;}cleanupInProgress_=true;const auto snapshot=settings_.snapshot();const auto path=databasePath(),root=root_;const auto activeIds=active_.keys();background([snapshot,path,root,activeIds](Error& e) {HistoryStore store(path);ArtifactStore artifacts(root+"/artifacts");e=store.error();if(e)return QJsonObject{};int removed=0;QDir dir(root+"/artifacts");for(const auto& name:dir.entryList(QDir::Dirs|QDir::NoDotAndDotDot)) {if(activeIds.contains(name))continue;Error error;auto op=store.operation(name,&error);if(error)continue;bool successful=true;for(const auto& item:op.items)if(item.uploadStatus=="unknown"||item.uploadStatus=="uploading")successful=false;const bool finished=op.state=="completed"||op.state=="partial"||op.state=="failed"||op.state=="cancelled"||op.state=="interrupted";bool uploaded=true;for(const auto& item:op.items)if(!terminalUpload(item.uploadStatus))uploaded=false;bool archivePending=false;for(const auto& item:op.items)if(item.local.status=="failed")archivePending=true;const auto date=QDateTime::fromString(op.created,Qt::ISODateWithMs);if(finished&&successful&&((uploaded&&!archivePending)||(snapshot.failedArtifactDays>0&&date.daysTo(QDateTime::currentDateTimeUtc())>=snapshot.failedArtifactDays))) {if(auto error=artifacts.remove(name)) {e=error;break;}++removed;}}artifacts.clearPreviews();return QJsonObject{{"removedOperations",removed}};},[this,reply=std::move(reply)](Error e,QJsonObject result){cleanupInProgress_=false;reply(e,result);});}
}
