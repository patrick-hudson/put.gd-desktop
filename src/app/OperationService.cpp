#include "OperationService.h"
#include "client/SessionClient.h"
#include "core/BuildInfo.h"
#include "core/Log.h"
#include "core/RunMarker.h"
#include "core/ReleaseLink.h"
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
#include <QScopeGuard>
#include <QTimer>
#include <QUuid>
#include <chrono>
#include <thread>
#include <vector>
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
const QLoggingCategory& providerLog(const QString& provider) {return provider=="s3"?logS3():provider=="proton-drive"?logProton():logOperation();}
qint64 artifactBytes(const Operation& op) {qint64 total=0;for(const auto& item:op.items)total+=item.artifact.bytes;return total;}
QJsonObject itemFields(const Operation& op,const OperationItem& item) {return {{"operationId",op.id},{"itemId",item.artifact.id},{"provider",op.destination.providerId}};}
// A diagnostics upload (#123) has no action route: it goes to the destination
// it was accepted for and asks for a link.
ActionSettings routeOf(const Operation& op) {
 if(op.action==OperationService::diagnosticsAction)return {"upload",op.destination.profileId,{},"share"};
 return op.settings.actions.value(op.action);
}
// On-demand previews (#190) answer within historyPreviewReplyMs: at most this
// long queued, then a local copy check and a decode of up to 10 s each.
constexpr qsizetype maximumQueuedPreviews=3;
constexpr qint64 maximumPreviewWaitMs=20000;
constexpr int maximumPreviewVerifyMs=10000;
static_assert(maximumPreviewWaitMs+maximumPreviewVerifyMs+10000<historyPreviewReplyMs);
// A local copy stands in for a cleaned-up artifact only while it is the same
// file: a regular file with the artifact's size and SHA-256.
Error sameFile(const Artifact& copy,const std::shared_ptr<std::atomic_bool>& cancelled,int deadlineMs) {
 const Error missing{"preview_source_missing","Neither the stored file nor a local copy is available",{}};
 const QFileInfo info(copy.path);if(!info.isFile()||info.isSymLink()||info.size()!=copy.bytes||copy.sha256.isEmpty())return missing;
 QFile file(copy.path);if(!file.open(QIODevice::ReadOnly))return missing;
 QCryptographicHash hash(QCryptographicHash::Sha256);QByteArray chunk(1<<20,Qt::Uninitialized);QElapsedTimer clock;clock.start();
 for(;;) {
  if(cancelled->load())return {"cancelled","Preview cancelled",{}};
  if(clock.elapsed()>deadlineMs)return {"preview_timeout","The local copy took too long to check",{}};
  const auto read=file.read(chunk.data(),chunk.size());if(read<0)return missing;if(read==0)break;
  hash.addData(QByteArrayView(chunk.constData(),read));
 }
 if(QString::fromLatin1(hash.result().toHex())!=copy.sha256)return {"preview_source_changed","The local copy is no longer the stored file",{}};
 return {};
}
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
 uploadSlots_.setCapacity(settings_.snapshot().limits.concurrentUploads);
 connect(&settings_,&SettingsService::accepted,this,[this]{uploadSlots_.setCapacity(settings_.snapshot().limits.concurrentUploads);pump();});
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
 cleanupTimer_.setInterval(cleanupSchedule_.intervalMs);
 connect(&cleanupTimer_,&QTimer::timeout,this,&OperationService::cleanupExpired);
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
  if(ready_) {cleanupTimer_.start();QTimer::singleShot(cleanupSchedule_.startupDelayMs,this,&OperationService::cleanupExpired);}
 });
}
OperationService::~OperationService() {shutdown();}
void OperationService::shutdown() {if(stopping_)return;stopping_=true;posterCancel_->store(true);cleanupTimer_.stop();if(cleanupStop_)cleanupStop_->store(true);heapReclamationTimer_.stop();observationTimer_.stop();for(auto flag:active_) flag->store(true);{std::lock_guard lock(itemCancelsMutex_);for(const auto& items:itemCancels_)for(const auto& flag:items)flag->store(true);}uploadSlots_.close();if(recording_)recording_->shutdown();if(editor_)editor_->cancel();invalidateCopyEligibility();source_.cancelAcquisition();for(auto* thread:threads_) {thread->wait();delete thread;}threads_.clear();}
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
std::shared_ptr<DestinationLease> OperationService::destinationLease(const Destination& d) {
 // Proton serializes per account, so the credential reference names the destination.
 const auto key=d.providerId+"\n"+QString::fromUtf8(QJsonDocument(d.credentialReference).toJson(QJsonDocument::Compact));
 std::lock_guard guard(locksMutex_);
 for(auto it=leases_.begin();it!=leases_.end();) {if(it.value().expired())it=leases_.erase(it);else ++it;}
 auto lease=leases_.value(key).lock();
 if(!lease){lease=std::make_shared<DestinationLease>();leases_.insert(key,lease);}
 return lease;
}
std::shared_ptr<std::atomic_bool> OperationService::itemCancellation(const QString& operation,const QString& item) {
 std::lock_guard lock(itemCancelsMutex_);auto& flag=itemCancels_[operation][item];
 if(!flag)flag=std::make_shared<std::atomic_bool>(false);
 return flag;
}
void OperationService::forgetItemCancellations(const QString& operation) {std::lock_guard lock(itemCancelsMutex_);itemCancels_.remove(operation);}
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
QString OperationService::recordingActiveId() const {return recording_->activeId();}
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
 // Screen recording is its own session: the tray opens or raises it, a global
 // shortcut toggles it (prepare, start, stop) so the keyboard can end a recording.
 if(action==Settings::recordingActionName()) {
  const auto mode=options["mode"].toString("open");
  if((!options.isEmpty()&&!(options.size()==1&&options.contains("mode")))||(mode!="open"&&mode!="toggle")){reply({"invalid_request","The record action takes mode open or toggle",{}},{});return;}
  if(editor_){reply({"editor_active","An edit is open; finish or cancel it before recording",{{"operationId",editingId_}}},{});return;}
  recording_->invoke(mode,reply);return;
 }
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
  logging::info(logOperation(),"Operation accepted",{{"operationId",op.id},{"action",op.action},{"output",op.destination.providerId.isEmpty()?"local":"upload"},{"provider",op.destination.providerId},{"profileId",op.destination.profileId}});
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
void OperationService::uploadDiagnostics(const QString& path,const QString& profileId,JsonReply accepted,std::function<void(const Operation&)> finished) {
 if(!ready_) {accepted(startupError_?startupError_:Error{"initializing","Journal recovery is in progress",{}},{});return;}
 auto snapshot=settings_.snapshot();
 if(!snapshot.profiles.contains(profileId)){accepted({"profile_not_found","No destination has this ID",{{"profileId",profileId}}},{});return;}
 if(active_.size()>=snapshot.limits.pendingOperations+snapshot.limits.concurrentUploads) {accepted({"queue_full","Pending operation limit reached",{}},{});return;}
 // This operation's own settings: the package keeps its file name in history,
 // is never archived a second time, and a name clash never overwrites.
 auto document=snapshot.document;
 auto archive=document["archive"].toObject();archive["keepUploads"]=false;document["archive"]=archive;
 auto naming=document["naming"].toObject();naming["mode"]="original";naming["collisionPolicy"]="rename";document["naming"]=naming;
 const auto profile=snapshot.profiles.value(profileId);
 Operation op;op.id=uuid();op.action=diagnosticsAction;op.settings=Settings::fromValidated(document);op.settingsRevision=settings_.revision();
 op.created=QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);op.clipboardRevision=source_.clipboardRevision();
 op.destination={profile["provider"].toString(),profileId,settings_.revision(),profile["settings"].toObject(),profile["credentials"].toObject()};
 auto flag=std::make_shared<std::atomic_bool>(false);active_.insert(op.id,flag);
 if(finished)finishWatchers_.insert(op.id,std::move(finished));
 const auto database=databasePath();
 background([database,op](Error& e) {HistoryStore store(database);e=store.saveOperation(op);return op.publicJson();},[this,op,flag,path,accepted](Error e,QJsonObject result) mutable {
  if(e) {finishWatchers_.remove(op.id);releaseActive(op.id);accepted(e,{});return;}
  observe(op.publicJson());accepted({},{{"operationId",op.id},{"operation",result}});emit changed(op.id);emit observationsChanged();
  logging::info(logOperation(),"Operation accepted",{{"operationId",op.id},{"action",op.action},{"output","upload"},{"provider",op.destination.providerId},{"profileId",op.destination.profileId}});
  ClipboardPayload payload;payload.kind="files";payload.files={path};
  op.state="acquiring";enqueue({op,std::move(payload),true,flag,{}});
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
 const auto recentColors=settings_.snapshot().editorRecentColors;const auto appearance=editorAppearance_;
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
    *edited=EditorJob(staging,helper).withRecentColors(recentColors).withAppearance(appearance).runFile(file,maximum,destination,outputBytes,session);
    if(edited->error.code=="source_unsupported")edited->error={"edit_source_unsupported","The editor opens static PNG or JPEG files",{}};
    return QJsonObject{};
   }
   *edited=EditorJob(staging,helper).withRecentColors(recentColors).withAppearance(appearance).run(*image,destination,outputBytes,session);
   return QJsonObject{};
  },[=,this](Error,QJsonObject) mutable {
   if(editingId_==op.id){editor_.reset();editingId_.clear();}
   if(edited->recentColors)rememberEditorColors(*edited->recentColors);
   if(flag->load()||edited->error.code=="cancelled"){op.cancelled=true;op.state="cancelled";op.error=flag->load()?Error{}:edited->error;finish(op);return;}
   if(edited->error){op.error=edited->error;op.state="failed";finish(op);return;}
   ClipboardPayload finalized;finalized.kind="png";finalized.bytes=std::move(edited->png);finalized.mime="image/png";
   op.state="acquiring";op.phase="queued";
   // The editor is gone, but the sanitized bytes are not an artifact yet.
   // Persist the handoff before queueing: readers and crash recovery must no
   // longer treat this operation as an open, unsubmitted editor.
   background([path,op](Error& e){HistoryStore store(path);e=store.saveOperation(op);return QJsonObject{};},
    [this,op,flag,finalized=std::move(finalized)](Error e,QJsonObject) mutable {
     if(e){op.error=e;op.state="failed";finish(op);return;}
     enqueue({op,std::move(finalized),true,flag,{}});
    });
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
 logging::info(logOperation(),"Operation accepted",{{"operationId",op.id},{"action",op.action},{"output",upload?"upload":"local"},{"provider",op.destination.providerId},{"profileId",op.destination.profileId}});
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
 if(job.acquire) {op.phase="preparing";if(!save())return op;
  // An upload route rejects files its destination can never store before copying them.
  qint64 objectLimit=0;{const auto output=op.settings.actions.value(op.action);if(output.destination!="local"&&!output.profileId.isEmpty())if(auto destination=providers_.find(op.destination.providerId))objectLimit=destination->capabilities(op.destination).maximumObjectBytes;}
  // A fixed waiting-uploads budget is checked against bytes already stored, so such
  // imports run one at a time. Without one, free disk is the only bound and its
  // reservation is atomic: a new action's import never waits behind a large copy.
  std::unique_lock acquisitionGuard(acquisitionMutex_,std::defer_lock);if(op.settings.limits.pendingArtifactBytes<ResourceLimits::unlimitedBytes)acquisitionGuard.lock();
  auto acquired=artifacts.acquire(op.id,job.payload,op.settings.limits,op.action,op.settings,QDateTime::fromString(op.created,Qt::ISODateWithMs),objectLimit);
  job.payload={};job.payloadLease.reset(); // Retry/upload ownership now belongs to the immutable disk artifact.
  if(acquired.error) {op.error=acquired.error;op.state="failed";save();logging::warning(logOperation(),"Acquisition failed",{{"operationId",op.id},{"error",logging::errorFields(op.error)}});return op;}
  for(const auto& a:acquired.artifacts) {OperationItem item;item.phase="stored";item.artifact=a;item.artifact.previewPath=artifacts.preview(a,op.settings.previewCacheBytes,op.settings.limits.imagePixels);item.publicationId=uuid();op.items.append(item);}op.state="stored";if(!save())return op;
  QJsonArray mimes;for(const auto& item:op.items)if(!mimes.contains(item.artifact.mime))mimes.append(item.artifact.mime);
  logging::info(logOperation(),"Artifacts acquired",{{"operationId",op.id},{"items",op.items.size()},{"byteCount",artifactBytes(op)},{"mimeTypes",mimes}});
 }
 // Archive every acquired item before any item in this batch is published.
 op.phase="saving_local";
 const auto route=routeOf(op);
 const bool local=route.destination=="local"||route.profileId.isEmpty();
 for(auto& item:op.items) if((local||op.settings.archive.keepUploads)&&item.local.status!="saved") {item.phase="saving_local";observe(op.publicJson());item.local=LocalArchive::save(item.artifact,op.settings.archive);item.phase="stored";if(!saveItem(item))return op;}
 if(local) {for(auto& item:op.items) item.uploadStatus="local_only";op.state="completed";for(const auto& item:op.items) if(item.local.status!="saved")op.state="failed";save();return op;}
 auto provider=providers_.find(op.destination.providerId);if(!provider) {op.error={"provider_unavailable","Restore access to the original destination",{}};op.state="failed";save();return op;}
 const auto capabilities=provider->capabilities(op.destination);
 auto publicationMode=routeOf(op).publicationMode;
 if(publicationMode=="default")publicationMode=provider->defaultPublicationMode(op.destination);
 if(publicationMode=="private"&&!capabilities.privateUploads){op.error={"unsupported_capability","This provider cannot perform a private upload",{}};op.state="failed";save();return op;}
 if(!capabilities.conditionalCreate||(op.settings.collisionPolicy=="overwrite"&&!capabilities.conditionalReplace)) {op.error={"unsupported_capability","Provider cannot enforce this collision policy",{}};op.state="failed";save();return op;}
 bool needsNames=job.acquire;for(const auto& item:op.items)if(!terminalUpload(item.uploadStatus)&&item.candidate.isEmpty())needsNames=true;
 if(needsNames) {QList<Artifact> list;for(const auto& item:op.items)list.append(item.artifact);auto names=UploadNaming::batch(list,op.settings.namingMode,op.settings.collisionPolicy,capabilities.maximumNameBytes);QJsonArray chosen;
  for(int i=0;i<op.items.size();++i) {
   if(!op.items[i].candidate.isEmpty())continue;
   // A diagnostics package ignores naming templates: an unguessable name.
   if(op.action==diagnosticsAction)names[i]={"putgd-diagnostics-"+uuid()+".zip","pending"};
   op.items[i].candidate=names[i].name;if(names[i].status!="pending")op.items[i].uploadStatus=names[i].status;chosen.append(QJsonObject{{"itemId",op.items[i].artifact.id},{"name",names[i].name},{"status",names[i].status}});
  }
  logging::info(logOperation(),"Upload names chosen",{{"operationId",op.id},{"namingMode",op.settings.namingMode},{"collisionPolicy",op.settings.collisionPolicy},{"names",chosen}});
  if(!save())return op;
 }
 // A provider that takes one request at a time (Proton locks its account) is leased
 // to this operation for all of its provider work, preflight to sharing.
 const bool parallel=provider->maximumConcurrentRequests(op.destination)>1;
 struct Lease{std::shared_ptr<DestinationLease> held;~Lease(){if(held)held->release();}} lease;
 if(!parallel) {
  const auto destination=destinationLease(op.destination);op.phase="queued";observe(op.publicJson());
  if(!destination->acquire([&job]{return job.cancelled->load();})){op.cancelled=true;op.state="cancelled";save();return op;}
  lease.held=destination;
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
  for(int attempts=0;attempts<32&&!job.cancelled->load();++attempts) {
   auto check=storageRequest<NameCheck>([&](auto cb) {provider->checkName(op.destination,item.candidate,cb,job.cancelled);});
   if(job.cancelled->load())break;
   if(check.error||check.status=="inconclusive"||check.objectReference.isEmpty()) {item.error=check.error?check.error:Error{"existence_unknown","Name availability is inconclusive",{}};item.uploadStatus="failed";auto fields=itemFields(op,item);fields["status"]=check.status;fields["error"]=logging::errorFields(item.error);logging::warning(providerLog(op.destination.providerId),"Name check failed",fields);break;}
   if(check.status=="present"&&op.settings.collisionPolicy=="skip") {item.uploadStatus="skipped";break;}
   if(check.status=="present"&&op.settings.collisionPolicy=="rename") {item.candidate=UploadNaming::renamedCandidate(item.candidate,capabilities.maximumNameBytes);if(!saveItem(item))return op;continue;}
   item.objectReference=check.objectReference;item.concurrencyToken=check.concurrencyToken;item.condition=check.status=="present"?"match":"absent";
   if(item.condition=="match"&&item.concurrencyToken.isEmpty()) {item.error={"precondition_missing","Provider returned no replacement precondition",{}};item.uploadStatus="failed";break;}
   item.uploadStatus="stored";targets[QJsonDocument(item.objectReference).toJson(QJsonDocument::Compact)].append(index);checked=true;break;
  }
  if(job.cancelled->load()) {item.uploadStatus="cancelled";item.phase="cancelled";item.error={"cancelled","Destination check cancelled; Retry uploads this file",{}};}
  else if(!checked&&item.uploadStatus=="stored") {item.uploadStatus="failed";item.error={"collision_exhausted","No available candidate after 32 checks",{}};}
  if(!saveItem(item))return op;
 }
 for(const auto& group:targets) if(group.size()>1) {if(op.settings.collisionPolicy=="overwrite") {for(int i:group)op.items[i].uploadStatus="conflict";}else if(op.settings.collisionPolicy=="skip") {for(int n=1;n<group.size();++n)op.items[group[n]].uploadStatus="skipped";}else {for(int n=1;n<group.size();++n) {auto& item=op.items[group[n]];item.uploadStatus="failed";item.error={"name_alias_conflict","Provider candidates resolve to one target",{}};}}}
 if(!save())return op;
 // Files upload in parallel up to "Uploads at once", shared fairly by every
 // operation (#28). Each file holds one slot for its whole transfer, so a large
 // multipart upload keeps one slot and never starves the others. Workers own a
 // local copy of their item; `op` changes only under itemsMutex, and every save
 // writes the whole operation from there, so a checkpoint never mixes item states.
 std::mutex itemsMutex;std::atomic_bool persisted=true;
 std::function<Error(const OperationItem&,int)> persist=[&](const OperationItem& item,int index)->Error {
  std::lock_guard lock(itemsMutex);op.items[index]=item;HistoryStore durable(databasePath());return durable.checkpointOperation(op,{item.artifact.id},&providers_);
 };
 auto show=[&](const OperationItem& item,int index){std::lock_guard lock(itemsMutex);op.items[index]=item;observe(op.publicJson());};
 auto commit=[&](const OperationItem& item,int index)->bool {
  if(auto e=persist(item,index)){std::lock_guard lock(itemsMutex);op.error=e;op.state="failed";persisted=false;return false;}
  {std::lock_guard lock(itemsMutex);observe(op.publicJson());}
  if(item.linkAvailable()) {
   QPointer<OperationService> owner(this);
   QMetaObject::invokeMethod(this,[owner,id=op.id,itemId=item.artifact.id,index,url=item.receipt.directUrl]{if(owner&&!owner->stopping_)owner->uploadLinkReady(id,itemId,index,url);},Qt::QueuedConnection);
  }
  return true;
 };
 auto settled=[&](int index){QPointer<OperationService> owner(this);QMetaObject::invokeMethod(this,[owner,id=op.id,index]{if(owner&&!owner->stopping_)owner->uploadItemSettled(id,index);},Qt::QueuedConnection);};
 // A sharing provider (Proton Drive) makes the link in the sharing step below,
 // after the transfer. Such a file settles there, so each_link ordering waits
 // for its link instead of passing it and stranding the link (#478).
 auto linkFollows=[&](const OperationItem& item){return capabilities.sharing&&item.uploadStatus=="uploaded"&&item.receipt.directUrl.isEmpty();};
 auto settleTransfer=[&](int index){OperationItem item;{std::lock_guard lock(itemsMutex);item=op.items[index];}if(!linkFollows(item))settled(index);};
 auto transfer=[&](int index,const std::shared_ptr<std::atomic_bool>& cancel) {
  OperationItem item;{std::lock_guard lock(itemsMutex);item=op.items[index];}
  ArtifactStore artifacts(root_+"/artifacts");
  auto lock=objectLock(op.destination,item.objectReference,item.candidate);std::unique_lock guard(*lock);
  if(item.uploadStatus=="unknown") {
   item.phase="reconciling";show(item,index);
   auto stat=storageRequest<StatResult>([&](auto cb) {auto request=item.uploadRequest(op.destination,op.requestTimeoutMs);request.cancellation=cancel;provider->reconcile(request,cb);});
   if(stat.status=="own"&&!stat.receipt.validate()&&stat.receipt.publicationId==item.publicationId&&stat.receipt.destination.toJson()==op.destination.toJson()) {item.receipt=stat.receipt;item.uploadStatus="uploaded";item.error={};item.uploadJournal={};if(!commit(item,index))return;return;}
   {auto fields=itemFields(op,item);fields["status"]=stat.status;if(stat.error)fields["error"]=logging::errorFields(stat.error);logging::write(providerLog(op.destination.providerId),stat.status=="own"||stat.status=="missing"?QtInfoMsg:QtWarningMsg,"Uncertain upload reconciled",fields);}
   if(stat.status!="missing") {item.phase="unknown";item.error=stat.error?stat.error:Error{"publication_unknown","Resolve ambiguous publication before uploading again",{}};if(!commit(item,index))return;return;}
   item.uploadStatus="stored";if(auto error=artifacts.verify(item.artifact)) {item.uploadStatus="failed";item.error=error;commit(item,index);return;}if(!commit(item,index))return;
  }
  for(int attempts=0;attempts<32;++attempts) {
   item.uploadStatus="uploading";item.phase="uploading";item.uploadProgress.reset();if(!commit(item,index))return;
   struct ProgressState{std::mutex mutex;bool closed=false;std::optional<UploadProgress> latest;};
   auto activity=std::make_shared<StorageActivity>(*this,!provider->reportsUploadProgress());
   auto progress=std::make_shared<ProgressState>();QPointer<OperationService> owner(this);
   {auto fields=itemFields(op,item);fields["attempt"]=attempts+1;fields["name"]=item.candidate;fields["byteCount"]=item.artifact.bytes;fields["mime"]=item.artifact.mime;logging::info(providerLog(op.destination.providerId),"Upload attempt",fields);}
   QElapsedTimer attemptClock;attemptClock.start();
   // Idle slots this file may borrow for extra multipart parts (#174), returned when it ends.
   struct Loan{UploadSlots& pool;quint64 id=0;~Loan(){if(id)pool.closeLoan(id);}} loan{uploadSlots_};
   auto uploaded=awaitProvider<UploadResult>([&](auto cb) {
    auto request=item.uploadRequest(op.destination,op.requestTimeoutMs);request.cancellation=cancel;
    if(parallel)request.extraLanes=[&loan,this](int wanted){return uploadSlots_.lend(loan.id,wanted);};
    // Per-part records are debug-only; a multipart completion is kept at info (#148).
    request.trace=[fields=itemFields(op,item),log=&providerLog(op.destination.providerId)](const QJsonObject& record){
     const bool complete=record["event"]=="complete";if(!complete&&!logging::debug())return;
     auto values=fields;for(auto it=record.begin();it!=record.end();++it)if(it.key()!="event")values[it.key()]=it.value();
     logging::write(*log,complete?QtInfoMsg:QtDebugMsg,complete?QStringLiteral("Multipart upload completed"):QStringLiteral("Multipart part uploaded"),values);
    };
    // A resumable (multipart) upload journals its remote upload before relying on it.
    // This worker waits on the provider meanwhile, so the item is not touched concurrently.
    request.checkpoint=[&item,&persist,index](const QJsonObject& journal,std::function<void(Error)> ack){item.uploadJournal=journal;ack(persist(item,index));};
    // Overlapping items each update their own entry of the shared live operation.
    request.progress=[owner,progress,activity,&op,&itemsMutex,index](const UploadProgress& value){
     if(!value.valid())return;
     std::lock_guard lock(progress->mutex);if(progress->closed||!owner)return;progress->latest=value;activity->set(value.phase!="preparing");
     std::lock_guard items(itemsMutex);op.items[index].phase=value.phase;op.items[index].uploadProgress=value;
     // The operation shows the furthest phase any of its files has reached.
     bool transferring=value.phase!="preparing";for(qsizetype other=0;other<op.items.size()&&!transferring;++other)transferring=other!=index&&QStringList{"uploading","finalizing"}.contains(op.items[other].phase);
     op.phase=transferring&&value.phase=="preparing"?QStringLiteral("uploading"):value.phase;owner->observe(op.publicJson());
    };
    provider->upload(request,cb);
   });
   {std::lock_guard lock(progress->mutex);progress->closed=true;activity->set(false);item.uploadProgress=progress->latest;}
   {
    auto fields=itemFields(op,item);fields["attempt"]=attempts+1;fields["status"]=uploaded.status;fields["elapsedMs"]=attemptClock.elapsed();
    // "unknown" means the request may have taken effect: it is reconciled, never resent blindly.
    fields["uncertain"]=uploaded.status=="unknown";
    if(uploaded.error)fields["error"]=logging::errorFields(uploaded.error);
    logging::write(providerLog(op.destination.providerId),uploaded.status=="uploaded"||uploaded.status=="collision"?QtInfoMsg:QtWarningMsg,
     uploaded.status=="uploaded"?QStringLiteral("Upload finished"):uploaded.status=="collision"?QStringLiteral("Upload name collision"):QStringLiteral("Upload failed"),fields);
   }
   item.phase=uploaded.status;
   item.error=uploaded.error;
   if(uploaded.status=="uploaded") {
    auto e=uploaded.receipt.validate();if(!e&&(uploaded.receipt.publicationId!=item.publicationId||uploaded.receipt.destination.toJson()!=op.destination.toJson()))e={"receipt_invalid","Receipt does not match the accepted publication identity",{}};
    if(e) {item.uploadStatus="unknown";item.error=e;}else {item.receipt=uploaded.receipt;item.uploadStatus="uploaded";item.error={};item.uploadJournal={};}if(!commit(item,index))return;break;
   }
   if(uploaded.status=="unknown") {item.uploadStatus="unknown";if(!commit(item,index))return;break;}
   // Cancelling one file leaves it pending for Retry; the rest of the batch continues.
   if(uploaded.status=="failed"&&cancel->load()&&!job.cancelled->load()) {item.uploadStatus="cancelled";item.phase="cancelled";item.error={"cancelled","Upload cancelled; Retry uploads this file again",{}};commit(item,index);break;}
   if(uploaded.status!="collision") {item.uploadStatus="failed";if(!commit(item,index))return;break;}
   // Reconcile our publication before changing any proposed target after a race.
   item.phase="reconciling";show(item,index);
   auto stat=storageRequest<StatResult>([&](auto cb) {auto request=item.uploadRequest(op.destination,op.requestTimeoutMs);request.cancellation=cancel;provider->reconcile(request,cb);});
   if(stat.status=="own"&&!stat.receipt.validate()&&stat.receipt.publicationId==item.publicationId&&stat.receipt.destination.toJson()==op.destination.toJson()) {item.receipt=stat.receipt;item.uploadStatus="uploaded";item.error={};item.uploadJournal={};if(!commit(item,index))return;break;}
   const bool unpublishedDraft=stat.status=="missing"&&uploaded.error.code=="upload_draft_conflict";
   if(stat.status!="foreign"&&!unpublishedDraft) {item.uploadStatus="unknown";item.phase="unknown";item.error=stat.error?stat.error:Error{"publication_unknown","Collision publication could not be reconciled",{}};if(!commit(item,index))return;break;}
   if(op.settings.collisionPolicy=="skip") {item.uploadStatus="skipped";if(!commit(item,index))return;break;}
   if(op.settings.collisionPolicy!="rename") {item.uploadStatus="failed";item.error={"precondition_changed","Original replacement precondition changed",{}};if(!commit(item,index))return;break;}
   item.candidate=UploadNaming::renamedCandidate(item.candidate,capabilities.maximumNameBytes);item.objectReference={};item.concurrencyToken={};item.condition="absent";item.uploadStatus="stored";item.phase="preflight";if(!commit(item,index))return;
   auto check=storageRequest<NameCheck>([&](auto cb) {provider->checkName(op.destination,item.candidate,cb,cancel);});
   if(cancel->load()) {item.uploadStatus="cancelled";item.phase="cancelled";item.error={"cancelled","Destination check cancelled; Retry uploads this file",{}};commit(item,index);break;}
   if(check.status!="absent"||check.error||check.objectReference.isEmpty()) {item.uploadStatus="failed";item.error={"existence_unknown","Renamed candidate could not be reserved",{}};if(!commit(item,index))return;break;}item.objectReference=check.objectReference;if(!commit(item,index))return;
   guard.unlock();lock=objectLock(op.destination,item.objectReference,item.candidate);guard=std::unique_lock<std::mutex>(*lock);
   if(attempts==31) {item.uploadStatus="failed";item.error={"collision_exhausted","Collision search exhausted",{}};commit(item,index);}
  }
 };
 op.phase="uploading";
 std::vector<std::thread> workers;
 for(int index=0;index<op.items.size()&&persisted;++index) {
  OperationItem item;{std::lock_guard lock(itemsMutex);item=op.items[index];}
  if(terminalUpload(item.uploadStatus)||item.uploadStatus=="failed") {settleTransfer(index);continue;}
  if(job.cancelled->load()) {op.cancelled=true;break;}
  const auto cancel=itemCancellation(op.id,item.artifact.id);
  // Waiting for a slot: the operation reads as queued until one of its files transfers.
  {std::lock_guard lock(itemsMutex);item.phase="queued";op.items[index]=item;bool transferring=false;for(const auto& other:op.items)transferring=transferring||QStringList{"uploading","finalizing","preparing"}.contains(other.phase);if(!transferring)op.phase="queued";observe(op.publicJson());}
  const bool admitted=uploadSlots_.acquire(op.id,[&job,cancel]{return job.cancelled->load()||cancel->load();});
  {std::lock_guard lock(itemsMutex);op.phase="uploading";}
  if(!admitted) {
   if(job.cancelled->load()) {op.cancelled=true;break;}
   item.uploadStatus="cancelled";item.phase="cancelled";item.error={"cancelled","Upload cancelled before it started; Retry uploads this file",{}};commit(item,index);settled(index);continue;
  }
  if(!parallel) {transfer(index,cancel);uploadSlots_.release(op.id);settleTransfer(index);continue;}
  workers.emplace_back([&,index,cancel]{transfer(index,cancel);uploadSlots_.release(op.id);settleTransfer(index);});
 }
 for(auto& worker:workers)worker.join();
 forgetItemCancellations(op.id);
 if(!persisted)return op;
 // Persist the uploaded receipt before the separate sharing mutation. Sharing
 // failure leaves an uploaded file and resumes only the link step on explicit retry.
 for(int index=0;index<op.items.size();++index) {
  auto& item=op.items[index];
  // A file whose link this step makes settles when the step ends for it,
  // whichever way: its link, if any, has been offered by then (#478).
  const bool deferredSettle=linkFollows(item);
  const auto settleShared=qScopeGuard([&]{if(deferredSettle)settled(index);});
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
  // A diagnostics package's share link expires (#123).
  const auto options=op.action==diagnosticsAction&&item.shareIntentOptions.isEmpty()?QJsonObject{{"expirationDays",diagnosticsShareDays}}:item.shareIntentOptions;
  QJsonObject doc{{"id",shareId},{"historyId",item.artifact.id},{"action","create"},{"state","running"},{"status","pending"},{"receipt",item.receipt.toJson()},{"progress",QJsonObject{}},{"options",options}};
  if(item.shareStatus=="unknown")doc["action"]="inspect";
  if(auto e=store.saveSharing(doc)){item.error=e;op.state="partial";saveItem(item);return op;}
  const auto path=root_+"/history.sqlite";
  auto checkpoint=[path,&doc](const QJsonObject& progress,std::function<void(Error)> ack){
   auto proposed=doc;proposed["progress"]=progress;HistoryStore durable(path);const auto e=durable.saveSharing(proposed);if(!e)doc=std::move(proposed);ack(e);
  };
  ShareRequest request{shareId,doc["action"].toString(),item.receipt,doc["options"].toObject(),doc["progress"].toObject(),job.cancelled};
  auto result=storageRequest<ShareResult>([&](auto cb){provider->share(request,checkpoint,cb);});
  if(!shareResultMatchesReceipt(result,item.receipt)){result.status="unknown";result.receipt=item.receipt;result.receipt.directUrl.clear();result.expiresAt.clear();result.error={"share_identity_changed","Sharing returned a different publication identity",{}};}
  if(result.status!="shared")result.receipt.directUrl.clear();
  item.receipt=result.receipt;item.shareStatus=result.status;item.phase=result.status;item.error=result.error;
  {auto fields=itemFields(op,item);fields["action"]=request.action;fields["status"]=result.status;if(result.error)fields["error"]=logging::errorFields(result.error);logging::write(providerLog(op.destination.providerId),result.status=="shared"||result.status=="private"?QtInfoMsg:QtWarningMsg,"Sharing finished",fields);}
  doc["state"]="completed";doc["status"]=result.status;doc["receipt"]=result.receipt.toJson();doc["progress"]=result.progress;doc["error"]=result.error.toJson();
  if(!result.expiresAt.isEmpty())doc["expiresAt"]=result.expiresAt;
  if(auto e=store.saveSharing(doc)){
   item.error=e;op.state="partial";
   QPointer<OperationService> owner(this);QMetaObject::invokeMethod(this,[owner,doc,e]{if(owner)emit owner->sharingStopped(doc,e);},Qt::QueuedConnection);
   return op;
  }
  const auto committed=store.sharing(shareId);if(result.status=="shared"||result.status=="revoked"||result.status=="changed"){item.shareIntent.clear();item.shareIntentOptions={};}else if(committed["action"]=="create"){item.shareIntent="create";item.shareIntentOptions=committed["options"].toObject();}
  if(!saveItem(item))return op;
 }
 op.cancelled=op.cancelled||job.cancelled->load();
 op.state=op.cancelled?"cancelled":"completed";for(const auto& item:op.items) if(item.uploadStatus=="failed"||item.uploadStatus=="unknown"||item.uploadStatus=="cancelled"||item.shareStatus=="failed"||item.shareStatus=="unknown"||item.shareStatus=="changed"||item.shareIntent=="create")op.state=op.cancelled?"cancelled":"partial";
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
 // Its link is copied only when asked for (diagnostics.bundle copyLink).
 if(op.action==diagnosticsAction&&op.copyStatus=="pending")op.copyStatus="suppressed";
 if(editingId_==op.id){if(editor_)editor_->cancel();editor_.reset();editingId_.clear();}
 op.phase=op.state;
 if(selectingId_==op.id&&!preparedOwners_.contains(op.id))selectingId_.clear();
 if((op.cancelled||op.state=="failed"||op.state=="interrupted")&&op.copyStatus=="pending")op.copyStatus="no_result";
 const auto path=databasePath();background([path,op](Error& e) {HistoryStore store(path);e=store.saveOperation(op);return e?QJsonObject{}:store.operation(op.id,&e).toJson();},[this,id=op.id,reply](Error e,QJsonObject document) {
  clearObservation(id);emit changed(id);emit observationsChanged();
  if(e) {logging::error(logHistory(),"Cannot record the operation result",{{"operationId",id},{"error",logging::errorFields(e)}});abandonUploadCopies(id);releaseActive(id);if(reply)reply(e,{});return;}
  const auto op=Operation::fromJson(document);
  {
   QJsonObject fields{{"operationId",op.id},{"action",op.action},{"state",op.state},{"provider",op.destination.providerId},{"items",op.items.size()},{"byteCount",artifactBytes(op)}};
   const auto created=QDateTime::fromString(op.created,Qt::ISODateWithMs);if(created.isValid())fields["durationMs"]=created.msecsTo(QDateTime::currentDateTimeUtc());
   QJsonObject outcomes;for(const auto& item:op.items)outcomes[item.uploadStatus]=outcomes[item.uploadStatus].toInt()+1;fields["uploadStatus"]=outcomes;
   if(op.error)fields["error"]=logging::errorFields(op.error);
   QJsonArray failures;for(const auto& item:op.items)if(item.error)failures.append(QJsonObject{{"itemId",item.artifact.id},{"uploadStatus",item.uploadStatus},{"error",logging::errorFields(item.error)}});
   if(!failures.isEmpty())fields["itemErrors"]=failures;
   logging::write(logOperation(),op.state=="completed"||op.state=="cancelled"?QtInfoMsg:QtWarningMsg,"Operation finished",fields);
  }
  if(!Operation::activeState(op.state))queuePosters(op);
  if(!Operation::activeState(op.state))if(auto watcher=finishWatchers_.take(id))watcher(op);
  if(uploadCopies_.contains(id)){finishUploadCopies(op,reply);return;}
  if(op.cancelled||op.state=="failed"||op.state=="interrupted"||op.action==diagnosticsAction) {emit resultReady(op.id);releaseActive(op.id);if(reply)reply({},op.publicJson());return;}
  performCopy(op,false,[this,id=op.id,reply](Error error,QJsonObject result) {releaseActive(id);if(reply)reply(error,result);});
 });
}

void OperationService::queuePosters(const Operation& op) {
 if(op.settings.previewCacheBytes<=0)return;
 for(const auto& item:op.items) {
  if(item.artifact.category!="videos"||!item.artifact.previewPath.isEmpty())continue;
  if(std::any_of(posterQueue_.cbegin(),posterQueue_.cend(),[&](const PosterTask& task){return task.itemId==item.artifact.id;}))continue;
  PosterTask task;task.operationId=op.id;task.itemId=item.artifact.id;task.budget=op.settings.previewCacheBytes;task.maximumPixels=op.settings.limits.imagePixels;task.waiting.start();
  posterQueue_.append(std::move(task));
 }
 pumpPosters();
}
void OperationService::makePreview(const QString& id,JsonReply reply) {
 if(!ready_){reply({"initializing","Wait for resident journal recovery",{}},{});return;}
 if(stopping_){reply({"cancelled","The resident is stopping",{}},{});return;}
 const auto snapshot=settings_.snapshot();
 if(snapshot.previewCacheBytes<=0){reply({"previews_disabled","Previews are turned off (retention.previewCacheBytes is 0)",{}},{});return;}
 if(const auto failed=previewFailures_.find(id);failed!=previewFailures_.end()){reply(failed.value(),{});return;}
 // Asked for again (or already queued after its operation): answer both when it is made.
 for(auto& task:posterQueue_)if(task.itemId==id&&task.onDemand){task.replies.append(std::move(reply));return;}
 const auto onDemand=std::count_if(posterQueue_.cbegin(),posterQueue_.cend(),[](const PosterTask& task){return task.onDemand;});
 if(onDemand>=maximumQueuedPreviews){reply({"preview_busy","Other previews are being made; select the item again shortly",{}},{});return;}
 PosterTask task;task.itemId=id;task.onDemand=true;task.budget=snapshot.previewCacheBytes;task.maximumPixels=snapshot.limits.imagePixels;task.waiting.start();task.replies.append(std::move(reply));
 // Ahead of after-operation posters, behind earlier selections; an
 // after-operation task for the same item becomes this one.
 posterQueue_.removeIf([&](const PosterTask& queued){return queued.itemId==id;});
 const auto position=std::find_if(posterQueue_.begin(),posterQueue_.end(),[](const PosterTask& queued){return !queued.onDemand;});
 posterQueue_.insert(position,std::move(task));
 pumpPosters();
}
void OperationService::pumpPosters() {
 if(posterRunning_||posterQueue_.isEmpty()||stopping_)return;
 auto task=posterQueue_.takeFirst();
 // The reply deadline counts from the request: a selection that waited this
 // long behind other previews is answered instead of decoded.
 if(task.onDemand&&task.waiting.elapsed()>maximumPreviewWaitMs){settlePreview(task,{"preview_busy","Other previews are being made; select the item again shortly",{}},{});pumpPosters();return;}
 posterRunning_=true;const auto path=databasePath(),root=root_;const auto cancelled=posterCancel_;
 background([path,root,task,cancelled](Error& e) {
  QElapsedTimer clock;clock.start();
  HistoryStore store(path);e=store.error();if(e)return QJsonObject{};
  auto operationId=task.operationId;
  if(operationId.isEmpty()){const auto record=store.detail(task.itemId,&e);if(e)return QJsonObject{};operationId=record["operationId"].toString();}
  auto op=store.operation(operationId,&e);if(e){if(e.code=="operation_not_found")e={"preview_source_missing","The stored file is no longer available",{}};return QJsonObject{};}
  const auto found=std::find_if(op.items.cbegin(),op.items.cend(),[&](const OperationItem& item){return item.artifact.id==task.itemId;});
  if(found==op.items.cend()){e={"history_not_found","History record not found",{}};return QJsonObject{};}
  const auto& item=*found;const auto category=item.artifact.category;
  if(category!="videos"&&category!="images"){e={"preview_unsupported","Previews are made for images and videos only",{}};return QJsonObject{};}
  QJsonObject result{{"id",task.itemId},{"operationId",operationId}};
  if(!item.artifact.previewPath.isEmpty()&&QFileInfo(item.artifact.previewPath).isFile()){result["state"]="ready";result["previewPath"]=item.artifact.previewPath;return result;}
  if(Operation::activeState(op.state)){e={"operation_active","The preview is made when the operation finishes",{}};return QJsonObject{};}
  // The retained artifact, or else the local copy when it is still the same file.
  Artifact source=item.artifact;QString from;
  const QFileInfo stored(source.path);
  if(stored.isFile()&&!stored.isSymLink()&&stored.size()==source.bytes)from="artifact";
  else if(item.local.status=="saved"&&!item.local.path.isEmpty()) {
   source.path=item.local.path;
   if(auto mismatch=sameFile(source,cancelled,maximumPreviewVerifyMs)){e=mismatch;return QJsonObject{};}
   from="local";
  }
  if(from.isEmpty()){e={"preview_source_missing","Neither the stored file nor a local copy is available",{}};return QJsonObject{};}
  ArtifactStore artifacts(root+"/artifacts");Error failure;QString made;
  if(category=="videos")made=artifacts.poster(source,task.budget,task.maximumPixels,cancelled,&failure);
  else if(!cancelled->load()){made=artifacts.preview(source,task.budget,task.maximumPixels);if(made.isEmpty())failure={"preview_failed","The image preview could not be made",{}};}
  else failure={"cancelled","Preview cancelled",{}};
  if(made.isEmpty()){e=failure;return QJsonObject{};}
  result["state"]="made";result["previewPath"]=made;result["source"]=from;result["elapsedMs"]=double(clock.elapsed());
  // Another action may have written the operation while the helper decoded,
  // and a retry may be running it again: add only the preview to a fresh read.
  auto fresh=store.operation(operationId,&e);if(e||Operation::activeState(fresh.state)){e={};result["stored"]=false;return result;}
  QSet<QString> changed;for(auto& entry:fresh.items)if(entry.artifact.id==task.itemId&&entry.artifact.previewPath!=made){entry.artifact.previewPath=made;changed.insert(entry.artifact.id);}
  if(!changed.isEmpty())e=store.checkpointOperation(fresh,changed);
  result["stored"]=!e;return e?QJsonObject{}:result;
 },[this,task](Error e,QJsonObject result) {
  posterRunning_=false;settlePreview(task,e,result);pumpPosters();
 });
}
void OperationService::settlePreview(const PosterTask& task,Error e,QJsonObject result) {
 QJsonObject fields{{"itemId",task.itemId},{"onDemand",task.onDemand},{"waitedMs",double(task.waiting.elapsed())}};
 if(!task.operationId.isEmpty())fields["operationId"]=task.operationId;
 if(e) {
  // A selection that failed is retried only after a restart. An
  // after-operation failure gets one more try when History selects the item,
  // and the transient refusals are never remembered.
  if(task.onDemand&&!QStringList{"cancelled","preview_busy","operation_active","initializing"}.contains(e.code)){if(previewFailures_.size()>=4096)previewFailures_.clear();previewFailures_.insert(task.itemId,e);}
  fields["error"]=logging::errorFields(e);logging::info(logHistory(),"No preview",fields);
 } else {
  for(const auto& key:{"operationId","state","source","elapsedMs","stored"})if(result.contains(key))fields[key]=result[key];
  logging::info(logHistory(),"Preview",fields);
  if(result["state"]=="made")emit changed(result["operationId"].toString());
 }
 for(const auto& reply:task.replies)reply(e,e?QJsonObject{}:result);
}
void OperationService::recent(JsonReply reply) {const auto path=databasePath();const int limit=settings_.snapshot().recentActivityLimit;background([path,limit](Error& e) {HistoryStore store(path);return QJsonObject{{"operations",store.recent(limit,&e)}};},[this,reply](Error e,QJsonObject data){auto values=data["operations"].toArray();for(qsizetype i=0;i<values.size();++i)values[i]=currentObservation(values[i].toObject());data["operations"]=values;reply(e,data);});}
void OperationService::inspect(const QString& id,JsonReply reply) {const auto path=databasePath();background([path,id](Error& e) {HistoryStore store(path);return store.operation(id,&e).publicJson();},[this,reply](Error e,QJsonObject data){reply(e,currentObservation(data));});}
void OperationService::cancel(const QString& id,JsonReply reply,const QString& itemId) {
 if(!itemId.isEmpty()) {
  // One file of an active batch: waiting stops at once, a running transfer is
  // cancelled like an operation's, and the other files continue.
  if(!active_.contains(id)){reply({"operation_inactive","Only an active operation's files can be cancelled one at a time",{}},{});return;}
  itemCancellation(id,itemId)->store(true);uploadSlots_.wake();
  reply({},{{"operationId",id},{"itemId",itemId},{"cancellationRequested",true},{"effect","Only this file stops; already sent network requests cannot be rolled back"}});return;
 }
 // A recording session lives in the recording service, not the upload queue:
 // cancelling its operation ends the capture or review and discards the clip.
 if(recording_&&recording_->activeId()==id){recording_->command("cancel",{{"id",id}},[reply,id](Error error,QJsonObject){reply(error,error?QJsonObject{}:QJsonObject{{"operationId",id},{"cancellationRequested",true},{"effect","The recording and its working clip are discarded"}});});return;}
 if(active_.contains(id)) {active_[id]->store(true);{std::lock_guard lock(itemCancelsMutex_);for(const auto& flag:itemCancels_.value(id))flag->store(true);}uploadSlots_.wake();interruptUploadCopy(id);if(selectingId_==id)source_.cancelAcquisition();if(editingId_==id&&editor_)editor_->cancel();
  for(int index=0;index<queue_.size();++index)if(queue_[index].op.id==id){auto job=queue_.takeAt(index);job.payload={};job.payloadLease.reset();job.op.cancelled=true;job.op.state="cancelled";finish(job.op);break;}
  reply({},{{"operationId",id},{"cancellationRequested",true},{"effect","Already sent network requests cannot be rolled back"}});return;}
 const auto path=databasePath();background([path,id](Error& e) {HistoryStore store(path);auto op=store.operation(id,&e);if(e)return QJsonObject{};op.cancelled=true;op.state="cancelled";op.phase="cancelled";for(auto& item:op.items)item.phase=item.uploadStatus;e=store.saveOperation(op);return op.publicJson();},reply);
}
void OperationService::retry(const QString& id,JsonReply reply) {
 if(cleanupInProgress_&&automaticCleanup_){cleanupStop_->store(true);afterCleanup_.append([this,id,reply]{retry(id,reply);});return;}
 if(cleanupInProgress_){reply({"cleanup_busy","Wait for artifact cleanup to finish",{}},{});return;}
 if(!ready_) {reply(startupError_?startupError_:Error{"initializing","Journal recovery is in progress",{}},{});return;}
 const auto limits=settings_.snapshot().limits;if(active_.size()>=limits.pendingOperations+limits.concurrentUploads) {reply({"queue_full","Pending operation limit reached",{}},{});return;}
 if(active_.contains(id)) {reply({"operation_busy","Operation is already active",{}},{});return;}
 auto reserved=std::make_shared<std::atomic_bool>(false);active_.insert(id,reserved);const auto path=databasePath();background([path,id](Error& e) {HistoryStore store(path);return store.operation(id,&e).toJson();},[this,reply,id,reserved,path](Error e,QJsonObject doc) {
  if(e) {active_.remove(id);reply(e,{});return;}auto op=Operation::fromJson(doc);if(op.items.isEmpty()) {active_.remove(id);reply({"artifact_missing","No acquired artifacts; invoke a new action",{}},{});return;}
  auto provider=providers_.find(op.destination.providerId);const bool sharing=provider&&provider->capabilities(op.destination).sharing;auto mode=routeOf(op).publicationMode;if(mode=="default"&&provider)mode=provider->defaultPublicationMode(op.destination);
  bool pending=false;for(const auto& i:op.items)if(!terminalUpload(i.uploadStatus)||(sharing&&i.uploadStatus=="uploaded"&&(i.shareStatus=="unknown"||(i.shareStatus=="private"&&i.shareIntent=="create")||(mode=="share"&&(i.shareStatus=="not_requested"||(i.shareStatus=="failed"&&i.shareIntent=="create"))))))pending=true;
  if(!pending) {active_.remove(id);performCopy(op,true,reply);return;}
  if(provider)if(auto timeout=provider->retryRequestTimeout(op.destination,settings_.snapshot().profiles.value(op.destination.profileId)))op.requestTimeoutMs=std::max(op.requestTimeoutMs.value_or(0),*timeout);
  op.cancelled=reserved->load();op.error={};op.state="stored";op.phase="queued";op.invocation=++invocation_;op.clipboardRevision=source_.clipboardRevision();op.copyStatus="pending";beginUploadCopies(op);
  background([path,op](Error& error){HistoryStore store(path);error=store.saveOperation(op);return QJsonObject{};},[this,reply,op,reserved](Error error,QJsonObject)mutable{
   if(error){abandonUploadCopies(op.id);releaseActive(op.id);reply(error,{});return;}
   op.cancelled=reserved->load();enqueue({op,{},false,reserved,{}});reply({},{{"operationId",op.id},{"retryAccepted",true}});
   logging::info(logOperation(),"Retry accepted",{{"operationId",op.id},{"action",op.action},{"provider",op.destination.providerId},{"items",op.items.size()}});
  });
 });
}
void OperationService::retryLocalSave(const QString& id,JsonReply reply) {
 if(cleanupInProgress_&&automaticCleanup_){cleanupStop_->store(true);afterCleanup_.append([this,id,reply]{retryLocalSave(id,reply);});return;}
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
 // `copy` carries the copy's own error and suppression guard for the
 // putgd.clipboard record (#473); the operation's error may predate the copy.
 auto record=[guarded,path,reply,explicitCopy](Operation value,QJsonObject copy={}) {if(!guarded)return;
  copy["operationId"]=value.id;copy["action"]=value.action;copy["automatic"]=!explicitCopy;copy["copyStatus"]=value.copyStatus;
  logging::write(logClipboard(),value.copyStatus=="failed"?QtWarningMsg:QtInfoMsg,explicitCopy?"Copy finished":"Automatic copy finished",copy);
  guarded->background([path,value](Error& e) {HistoryStore store(path);e=store.saveOperation(value);return value.publicJson();},[guarded,value,reply](Error e,QJsonObject result) {if(!guarded)return;emit guarded->changed(value.id);if(!e)emit guarded->resultReady(value.id);if(reply)reply(e,result);});};
 if(!eligible) {
  op.copyStatus="suppressed";
  record(op,{{"suppression",explicitCopy?copyGuard(false,false,"superseded",ticket,invocation_,0,0):copyGuard(false,false,"newer_action",op.invocation,invocation_,op.clipboardRevision,source_.clipboardRevision())}});return;
 }
 QStringList urls,files;QString localKind;
 for(const auto& item:op.items) {if(item.linkAvailable())urls.append(item.receipt.directUrl);if(item.uploadStatus=="local_only"&&item.local.status=="saved") {files.append(item.local.path);localKind=item.artifact.category;}}
 if(urls.isEmpty()&&files.isEmpty()) {op.copyStatus="no_result";record(op);return;}
 CopyPayload payload;
 if(!urls.isEmpty()) {payload.kind="text";payload.text=urls.join('\n');}
 else {payload.kind="files";payload.files=files;}
 auto suppression=std::make_shared<QJsonObject>();
 const auto completeCopy=[op,record,suppression](Error error) mutable {
  op.copyStatus=error?(error.code=="copy_suppressed"?"suppressed":"failed"):"copied";
  if(error)op.error=error;
  QJsonObject copy;
  if(error){copy["error"]=logging::errorFields(error);if(error.code=="copy_suppressed")copy["suppression"]=suppression->isEmpty()?nativeCopyGuard(error):*suppression;}
  record(op,copy);
 };
 // File batches always copy URIs. Detached text/image acquisitions copy their MIME;
 // a recording (video, or an animated GIF/WebP) is a file, never a still PNG.
 if(urls.isEmpty()&&op.items.size()==1&&!op.items[0].artifact.originalFile&&op.action!=Settings::recordingActionName()&&(localKind=="images"||localKind=="text")) {
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
  },[this,op,ticket,revision,record,prepared,completeCopy,suppression](Error e,QJsonObject) mutable {
   if(e) {op.copyStatus="failed";op.error=e;record(op,{{"error",logging::errorFields(e)}});return;}
   if(const auto guard=copyGuard(false,false,"newer_action",ticket,invocation_,revision,source_.clipboardRevision());!guard.isEmpty()) {op.copyStatus="suppressed";record(op,{{"suppression",guard}});return;}
   deliverCopy(*prepared,completeCopy,[this,ticket,revision,suppression]{*suppression=copyGuard(false,false,"newer_action",ticket,invocation_,revision,source_.clipboardRevision());return suppression->isEmpty();});
  });return;
 }
 deliverCopy(payload,completeCopy,[this,op,explicitCopy,ticket,suppression]{
  *suppression=explicitCopy?copyGuard(false,false,"superseded",ticket,invocation_,0,0):copyGuard(false,false,"newer_action",op.invocation,invocation_,op.clipboardRevision,source_.clipboardRevision());
  return suppression->isEmpty();
 });
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
   {"commands",QJsonArray{"inspect","apply","preview","saveCopy","fileDialog","submit","cancel","raise"}}}},
  {"recording",recording_->capabilities()}};
}
QJsonObject OperationService::diagnostics() const {
#ifdef __GLIBC__
 constexpr bool reclaimSupported=true;
#else
 constexpr bool reclaimSupported=false;
#endif
 const auto build=QJsonDocument::fromJson(buildManifestJson()).object();
 const auto logStatus=logging::status();
 return {{"schemaVersion",1},{"build",build},{"release",releaseLink(build).toJson()},{"backend","Qt"},{"qtVersion",QT_VERSION_STR},{"activeOperations",active_.size()},{"previews",QJsonObject{{"running",posterRunning_},{"queued",posterQueue_.size()}}},{"uploadSlots",uploadSlots_.diagnostics()},{"journalReady",ready_},{"artifactCleanup",QJsonObject{{"running",cleanupInProgress_},{"automatic",automaticCleanup_},{"intervalMs",cleanupSchedule_.intervalMs},{"last",lastCleanup_.isEmpty()?QJsonValue(QJsonValue::Null):QJsonValue(lastCleanup_)},{"incompleteUploadNotice",lastIncompleteNotice_.isEmpty()?QJsonValue(QJsonValue::Null):QJsonValue(lastIncompleteNotice_)}}},{"capabilities",capabilities()},{"storage",providers_.diagnostics()},{"locations",locations_},
  {"logging",logStatus},{"loggingDegraded",logStatus["degraded"].toBool()},{"uncleanExit",RunMarker::report()},
  {"memory",QJsonObject{{"allocatorReclamationSupported",reclaimSupported},{"allocatorReclamationPending",heapReclamationPending_},{"allocatorReclamationCount",qint64(heapReclamationCount_)},{"lastAllocatorReclamationUsec",lastHeapReclamationUsec_}}}};
}
void OperationService::abandonUploads(HistoryStore& store,Operation& op,QJsonObject& outcome) {
 QSet<QString> changed;
 for(auto& item:op.items) {
  if(item.uploadJournal["uploadId"].toString().isEmpty()||item.uploadStatus=="uploaded")continue;
  auto provider=providers_.find(op.destination.providerId);
  Error error=provider?Error{}:Error{"provider_unavailable","Restore access to the original destination",{}};
  if(provider) {
   auto lock=objectLock(op.destination,item.objectReference,item.candidate);std::unique_lock guard(*lock);
   error=storageRequest<Error>([&](auto cb) {provider->abandonUpload(op.destination,item.uploadJournal,cb);});
  }
  auto fields=itemFields(op,item);
  if(!error) {
   item.uploadJournal={};for(const auto* key:{"incompleteUpload","abortError","incompleteUploadReported"})item.error.details.remove(key);
   outcome["abortedUploads"]=outcome["abortedUploads"].toInt()+1;
   logging::info(providerLog(op.destination.providerId),"Incomplete upload aborted",fields);
  } else {
   // The journal stays, so the next cleanup tries again; the artifact still goes.
   // The user hears of each kept upload once (#226): the flag is saved with the item,
   // so neither the hourly pass nor a restart repeats the notice for it.
   const bool reported=item.error.details.value("incompleteUploadReported").toBool();
   item.error.details["incompleteUpload"]=true;item.error.details["abortError"]=error.toJson();item.error.details["incompleteUploadReported"]=true;
   auto kept=outcome["incompleteUploads"].toArray();
   if(kept.size()<100)kept.append(QJsonObject{{"operationId",op.id},{"itemId",item.artifact.id},{"error",error.toJson()},{"reported",reported}});
   outcome["incompleteUploads"]=kept;
   if(!reported) {
    auto notice=outcome["incompleteUploadNotice"].toObject();
    const auto add=[&](const char* key,const QString& value){auto values=notice[key].toArray();if(!value.isEmpty()&&!values.contains(value))values.append(value);notice[key]=values;};
    notice["count"]=notice["count"].toInt()+1;
    // The upload's whole size: at most this much is stored in its parts.
    notice["bytes"]=notice["bytes"].toDouble()+item.uploadJournal.value("bytes").toDouble(double(item.artifact.bytes));
    add("providers",op.destination.providerId);add("profiles",op.destination.profileId);add("buckets",op.destination.settings.value("bucket").toString());
    if(!notice.contains("error"))notice["error"]=error.toJson();
    outcome["incompleteUploadNotice"]=notice;
   }
   fields["error"]=logging::errorFields(error);
   logging::warning(providerLog(op.destination.providerId),"Incomplete upload kept",fields);
  }
  changed.insert(item.artifact.id);
 }
 if(!changed.isEmpty())if(auto error=store.checkpointOperation(op,changed,&providers_))logging::warning(logOperation(),"Upload journal not saved",{{"operationId",op.id},{"error",logging::errorFields(error)}});
}
void OperationService::cleanup(JsonReply reply) {
 if(cleanupInProgress_&&automaticCleanup_){cleanupStop_->store(true);afterCleanup_.append([this,reply]{cleanup(reply);});return;}
 if(cleanupInProgress_){reply({"cleanup_busy","Wait for artifact cleanup to finish",{}},{});return;}
 startCleanup(false,std::move(reply));
}
void OperationService::cleanupExpired() {
 if(!ready_||stopping_||cleanupInProgress_)return;
 startCleanup(true,{});
}
void OperationService::startCleanup(bool automatic,JsonReply reply) {
 cleanupInProgress_=true;automaticCleanup_=automatic;cleanupStop_=std::make_shared<std::atomic_bool>(false);
 // Retry is refused or waits while cleanup runs, and active operations are skipped,
 // so no upload aborted here is one a Retry is about to resume (#146).
 CleanupPass pass{automatic,settings_.snapshot().failedArtifactDays,cleanupSchedule_.batch,clock_(),active_.keys(),cleanupStop_};
 background([this,pass](Error& e){return cleanArtifacts(pass,e);},[this,automatic,reply=std::move(reply)](Error e,QJsonObject result){
  if(e)logging::warning(logOperation(),"Artifact cleanup failed",{{"trigger",automatic?"automatic":"manual"},{"error",logging::errorFields(e)}});
  else announceIncompleteUploads(result,automatic);
  finishCleanup(result);
  if(reply)reply(e,result);
  // More expired operations than one pass takes: the next pass follows shortly.
  if(automatic&&!e&&result["more"].toBool()&&!stopping_)QTimer::singleShot(cleanupSchedule_.followUpMs,this,&OperationService::cleanupExpired);
  auto pending=std::exchange(afterCleanup_,{});for(const auto& next:pending)next();
 });
}
void OperationService::announceIncompleteUploads(QJsonObject& outcome,bool automatic) {
 auto notice=outcome.value("incompleteUploadNotice").toObject();if(notice.isEmpty())return;
 const auto snapshot=settings_.snapshot();
 QStringList labels,buckets;
 for(const auto& id:notice["profiles"].toArray()){const auto label=snapshot.profiles.value(id.toString()).value("label").toString();if(!label.isEmpty()&&!labels.contains(label))labels<<label;}
 for(const auto& bucket:notice["buckets"].toArray())buckets<<bucket.toString();
 const int count=notice["count"].toInt();const auto bytes=qint64(notice["bytes"].toDouble());
 const auto uploads=count==1?QStringLiteral("an incomplete upload"):QStringLiteral("%1 incomplete uploads").arg(count);
 const auto where=buckets.size()==1?QStringLiteral(" in bucket %1").arg(buckets[0]):buckets.size()>1?QStringLiteral(" in %1 buckets (%2)").arg(buckets.size()).arg(buckets.join(", "))
  :labels.isEmpty()?QString{}:QStringLiteral(" at %1").arg(labels.join(", "));
 const auto size=bytes>0?QStringLiteral(" (up to %1 of stored parts)").arg(ArtifactStore::sizeText(bytes)):QString{};
 auto reason=notice.value("error").toObject().value("message").toString();if(!reason.isEmpty()&&!reason.endsWith('.'))reason+='.';
 const auto providers=notice.value("providers").toArray();
 const auto hint=providers.contains("s3")
  ?QStringLiteral("Allow s3:AbortMultipartUpload for put.gd's access key, or add a lifecycle rule to the bucket that aborts incomplete multipart uploads after a few days.")
  :providers.contains("backblaze-b2")
  ?QStringLiteral("Give put.gd's B2 application key writeFiles, or cancel the unfinished large file in the Backblaze web console.")
  :QStringLiteral("Check that put.gd's account may cancel uploads at the destination.");
 notice["hint"]=hint;notice["labels"]=QJsonArray::fromStringList(labels);notice["notified"]=automatic;notice["at"]=outcome["finished"];
 // S3's own words for a refused abort already name the permission and the lifecycle rule.
 const auto advice=reason.contains("AbortMultipartUpload")?reason
  :(reason.isEmpty()?QString{}:reason+' ')+(count==1?QStringLiteral("Its"):QStringLiteral("Their"))+QStringLiteral(" parts keep using storage. ")+hint;
 notice["message"]=QStringLiteral("%1 could not cancel %2%3%4. %5 put.gd tries again at each cleanup.")
  .arg(automatic?QStringLiteral("Automatic cleanup"):QStringLiteral("Cleanup"),uploads,where,size,advice);
 outcome["incompleteUploadNotice"]=notice;lastIncompleteNotice_=notice;
 logging::warning(logOperation(),"Incomplete uploads left at the destination",{{"trigger",automatic?"automatic":"manual"},{"count",count},{"bytes",double(bytes)},{"providers",notice["providers"]}});
 // A manual pass answers its caller (the History window says it); only an automatic one notifies.
 if(automatic)emit incompleteUploadsKept(notice);
}
void OperationService::finishCleanup(const QJsonObject& outcome) {
 cleanupInProgress_=false;automaticCleanup_=false;cleanupStop_.reset();
 if(!outcome.isEmpty())lastCleanup_=outcome;
}
QJsonObject OperationService::cleanArtifacts(const CleanupPass& pass,Error& e) {
 QElapsedTimer elapsed;elapsed.start();
 const auto trigger=pass.automatic?QStringLiteral("automatic"):QStringLiteral("manual");
 HistoryStore store(databasePath());ArtifactStore artifacts(root_+"/artifacts");e=store.error();if(e)return QJsonObject{};
 int removed=0;QJsonObject outcome{{"trigger",trigger},{"abortedUploads",0},{"incompleteUploads",QJsonArray{}}};QSet<QString> visited;
 const auto stopped=[&]{if(!pass.stop||!pass.stop->load())return false;outcome["stopped"]=true;return true;};
 QDir dir(root_+"/artifacts");
 for(const auto& name:dir.entryList(QDir::Dirs|QDir::NoDotAndDotDot)) {
  if(stopped())break;
  if(pass.automatic&&removed>=pass.batch) {outcome["more"]=true;break;}
  if(pass.activeIds.contains(name))continue;
  Error error;auto op=store.operation(name,&error);if(error)continue;
  bool successful=true;for(const auto& item:op.items)if(item.uploadStatus=="unknown"||item.uploadStatus=="uploading")successful=false;
  const bool finished=op.state=="completed"||op.state=="partial"||op.state=="failed"||op.state=="cancelled"||op.state=="interrupted";
  bool uploaded=true;for(const auto& item:op.items)if(!terminalUpload(item.uploadStatus))uploaded=false;
  bool archivePending=false;for(const auto& item:op.items)if(item.local.status=="failed")archivePending=true;
  const auto date=QDateTime::fromString(op.created,Qt::ISODateWithMs);
  const qint64 age=date.isValid()?date.daysTo(pass.now):-1;
  const bool expired=pass.failedArtifactDays>0&&age>=pass.failedArtifactDays;
  if(finished&&successful&&((!pass.automatic&&uploaded&&!archivePending)||expired)) {
   // Without its artifact no Retry can resume the upload: abort it first, best effort.
   abandonUploads(store,op,outcome);visited.insert(name);
   if(auto error=artifacts.remove(name)) {e=error;break;}++removed;
   logging::info(logOperation(),expired?"Expired artifacts removed":"Artifacts removed",{{"operationId",op.id},{"trigger",trigger},{"state",op.state},{"items",op.items.size()},{"ageDays",age},{"failedArtifactDays",pass.failedArtifactDays}});
  }
 }
 // An abort refused earlier is tried again on every cleanup after its artifact went.
 if(!e)for(const auto& id:store.journaledOperations()) {
  if(stopped())break;
  if(pass.activeIds.contains(id)||visited.contains(id)||QFileInfo::exists(root_+"/artifacts/"+id))continue;
  Error error;auto op=store.operation(id,&error);if(!error)abandonUploads(store,op,outcome);
 }
 if(!pass.automatic)artifacts.clearPreviews();
 outcome["removedOperations"]=removed;outcome["finished"]=pass.now.toString(Qt::ISODateWithMs);outcome["elapsedMs"]=elapsed.elapsed();
 const int incomplete=outcome["incompleteUploads"].toArray().size();
 // An automatic pass that found nothing to do stays out of the log.
 if(!pass.automatic||removed>0||outcome["abortedUploads"].toInt()>0||incomplete>0)
  logging::info(logOperation(),"Artifact cleanup finished",{{"trigger",trigger},{"removedOperations",removed},{"abortedUploads",outcome["abortedUploads"].toInt()},{"incompleteUploads",incomplete},
   {"more",outcome["more"].toBool()},{"stopped",outcome["stopped"].toBool()},{"elapsedMs",outcome["elapsedMs"].toInteger()}});
 return outcome;
}
}
