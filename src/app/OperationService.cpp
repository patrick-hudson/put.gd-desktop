#include "OperationService.h"
#include "core/BuildInfo.h"
#include <QElapsedTimer>
#ifdef __GLIBC__
#include <malloc.h>
#endif
#include "core/UploadNaming.h"
#include <QDateTime>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QPointer>
#include <QTimer>
#include <QUuid>
namespace putgd {
namespace {
QString uuid() {return QUuid::createUuid().toString(QUuid::WithoutBraces);}
Error unsupported() {return {"source_unsupported","Native desktop adapter is not available",{}};}
bool terminalUpload(const QString& s) {return s=="uploaded"||s=="skipped"||s=="conflict"||s=="local_only";}
}
QJsonObject UnavailableSource::capabilities() const {return {{"region",false},{"window",false},{"desktop",false},{"clipboardImport",QJsonArray{}},{"clipboardResult",QJsonArray{}},{"shortcuts",false},{"reason","native_adapter_unavailable"}};}
void UnavailableSource::acquire(const QString&,const ResourceLimits&,std::function<void(PayloadResult)> cb) {cb({{},unsupported()});}
void UnavailableSource::copy(const CopyPayload&,std::function<void(Error)> cb) {cb(unsupported());}
OperationService::OperationService(QString root,SettingsService& settings,ProviderRegistry& providers,SourceAdapter& source,QObject* parent):QObject(parent),root_(std::move(root)),settings_(settings),providers_(providers),source_(source),heapReclamationTimer_(this) {
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
void OperationService::shutdown() {if(stopping_)return;stopping_=true;heapReclamationTimer_.stop();for(auto flag:active_) flag->store(true);source_.cancelAcquisition();for(auto* thread:threads_) {thread->wait();delete thread;}threads_.clear();}
bool OperationService::heapReclamationIdle() const {
 return !stopping_&&threads_.isEmpty()&&queue_.isEmpty()&&running_==0&&active_.isEmpty()&&selectingId_.isEmpty()&&payloadBytes_->load()==0;
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
void OperationService::invoke(const QString& action,JsonReply reply) {
 if(!ready_) {reply(startupError_?startupError_:Error{"initializing","Journal recovery is in progress",{}},{});return;}
 if(!Settings::actionNames().contains(action)) {reply({"invalid_action","Unknown sharing action",{}},{});return;}
 auto snapshot=settings_.snapshot();if(active_.size()>=snapshot.limits.pendingOperations+snapshot.limits.concurrentUploads) {reply({"queue_full","Pending operation limit reached",{}},{});return;}
 if(action!="clipboard"&&!selectingId_.isEmpty()) {reply({"busy_selecting","Another interactive acquisition is active",{}},{});return;}
 const auto route=snapshot.actions.value(action);if(route.destination=="upload"&&route.profileId.isEmpty()) {reply({"destination_required","Configure a destination before sharing",{}},{});return;}
 Operation op;op.id=uuid();op.action=action;op.settings=snapshot;op.settingsRevision=settings_.revision();op.created=QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);op.invocation=invalidateCopyEligibility();op.clipboardRevision=source_.clipboardRevision();
 if(route.destination=="upload") {const auto profile=snapshot.profiles.value(route.profileId);op.destination={profile["provider"].toString(),route.profileId,settings_.revision(),profile["settings"].toObject(),profile["credentials"].toObject()};}
 auto flag=std::make_shared<std::atomic_bool>(false);active_.insert(op.id,flag);if(action!="clipboard")selectingId_=op.id;
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
 auto acquisition=source_.prepareAcquisition(action,snapshot.limits,std::move(earlyTerminal),std::move(released));
 const auto path=databasePath();background([path,op](Error& e) {HistoryStore store(path);e=store.saveOperation(op);return op.publicJson();},[this,op,flag,reply,acquisition,earlyError](Error e,QJsonObject result) mutable {
  if(e) {releaseActive(op.id);if(selectingId_==op.id&&!preparedOwners_.contains(op.id))selectingId_.clear();reply(e,{});return;}
  reply({},{{"operationId",op.id},{"operation",result}});emit changed(op.id);
  // Acceptance stays durable and observable before acquiring pixels. Region
  // preview may already be visible; a terminal event skips the remaining work.
  QTimer::singleShot(0,this,[this,op,flag,acquisition,earlyError]() mutable {
   if(flag->load()) {op.state="cancelled";op.cancelled=true;finish(op);return;}
   if(*earlyError) {op.error=*earlyError;op.cancelled=op.error.code=="cancelled"||op.error.code=="source_cancelled";op.state=op.cancelled?"cancelled":"failed";finish(op);return;}
   op.state="acquiring";const auto path=databasePath();background([path,op](Error& e) {HistoryStore store(path);e=store.saveOperation(op);return QJsonObject{};},[this,op,flag,acquisition,earlyError](Error e,QJsonObject) mutable {
    if(e) {op.error=e;op.state="failed";finish(op);return;}
    if(flag->load()) {op.state="cancelled";op.cancelled=true;finish(op);return;}
    if(*earlyError) {op.error=*earlyError;op.cancelled=op.error.code=="cancelled"||op.error.code=="source_cancelled";op.state=op.cancelled?"cancelled":"failed";finish(op);return;}
    QPointer<OperationService> guarded(this);
    acquisition(op.settings.limits,[guarded,op,flag](PayloadResult r) {
     if(!guarded)return;
     QMetaObject::invokeMethod(guarded,[guarded,op,flag,r=std::move(r)]() mutable {
      if(!guarded||guarded->stopping_)return;
      if(guarded->selectingId_==op.id&&!guarded->preparedOwners_.contains(op.id))guarded->selectingId_.clear();
      if(r.error||flag->load()) {op.error=r.error;op.cancelled=flag->load()||r.error.code=="cancelled"||r.error.code=="source_cancelled";op.state=op.cancelled?"cancelled":"failed";guarded->finish(op);return;}
      guarded->enqueue({op,std::move(r.payload),true,flag,{}});
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
 queue_.enqueue(std::move(job));pump();
}
void OperationService::pump() {
 while(!queue_.isEmpty()&&running_<queue_.head().op.settings.limits.concurrentUploads&&!stopping_) {auto job=queue_.dequeue();++running_;background([this,job=std::move(job)](Error&) mutable {return run(std::move(job)).toJson();},[this](Error,QJsonObject doc) {--running_;auto op=Operation::fromJson(doc);finish(op);pump();});}
}
Operation OperationService::run(Job job) {
 auto op=job.op;HistoryStore store(databasePath());ArtifactStore artifacts(root_+"/artifacts");
 auto save=[&]() {auto e=store.saveOperation(op,&providers_);if(e) {op.error=e;op.state="failed";}return !e;};
 if(job.cancelled->load()){job.payload={};job.payloadLease.reset();op.cancelled=true;op.state="cancelled";save();return op;}
 if(job.acquire) {std::lock_guard acquisitionGuard(acquisitionMutex_);auto acquired=artifacts.acquire(op.id,job.payload,op.settings.limits,op.action,op.settings,QDateTime::fromString(op.created,Qt::ISODateWithMs));
  job.payload={};job.payloadLease.reset(); // Retry/upload ownership now belongs to the immutable disk artifact.
  if(acquired.error) {op.error=acquired.error;op.state="failed";save();return op;}
  for(const auto& a:acquired.artifacts) {OperationItem item;item.artifact=a;item.artifact.previewPath=artifacts.preview(a,op.settings.previewCacheBytes,op.settings.limits.imagePixels);item.publicationId=uuid();op.items.append(item);}op.state="stored";if(!save())return op;
 }
 // Archive every acquired item before any item in this batch is published.
 const bool local=op.settings.actions.value(op.action).destination=="local";
 for(auto& item:op.items) if((local||op.settings.archive.keepUploads)&&item.local.status!="saved") {item.local=LocalArchive::save(item.artifact,op.settings.archive);if(!save())return op;}
 if(local) {for(auto& item:op.items) item.uploadStatus="local_only";op.state="completed";for(const auto& item:op.items) if(item.local.status!="saved")op.state="failed";save();return op;}
 auto provider=providers_.find(op.destination.providerId);if(!provider) {op.error={"provider_unavailable","Restore access to the original destination",{}};op.state="failed";save();return op;}
 const auto capabilities=provider->capabilities(op.destination);
 if(!capabilities.conditionalCreate||(op.settings.collisionPolicy=="overwrite"&&!capabilities.conditionalReplace)) {op.error={"unsupported_capability","Provider cannot enforce this collision policy",{}};op.state="failed";save();return op;}
 bool needsNames=job.acquire;for(const auto& item:op.items)if(!terminalUpload(item.uploadStatus)&&item.candidate.isEmpty())needsNames=true;
 if(needsNames) {QList<Artifact> list;for(const auto& item:op.items)list.append(item.artifact);auto names=UploadNaming::batch(list,op.settings.namingMode,op.settings.collisionPolicy,capabilities.maximumNameBytes);for(int i=0;i<op.items.size();++i) {if(!op.items[i].candidate.isEmpty())continue;op.items[i].candidate=names[i].name;if(names[i].status!="pending")op.items[i].uploadStatus=names[i].status;}
  if(!save())return op;
 }
 op.state="running";if(!save())return op;
 // Check all pending names before the first PUT, including opaque references that
 // may identify one target despite differing proposed display names.
 QHash<QByteArray,QList<int>> targets;
 for(int index=0;index<op.items.size();++index) {
  auto& item=op.items[index];if(terminalUpload(item.uploadStatus)||item.uploadStatus=="unknown")continue;
  if(job.cancelled->load())break;
  if(auto e=artifacts.verify(item.artifact)) {item.error=e;item.uploadStatus="failed";save();continue;}
  if(!item.objectReference.isEmpty()&&!item.condition.isEmpty()&&item.uploadStatus!="failed") {targets[QJsonDocument(item.objectReference).toJson(QJsonDocument::Compact)].append(index);continue;}
  bool checked=false;
  for(int attempts=0;attempts<32;++attempts) {
   auto check=awaitProvider<NameCheck>([&](auto cb) {provider->checkName(op.destination,item.candidate,cb);});
   if(check.error||check.status=="inconclusive"||check.objectReference.isEmpty()) {item.error=check.error?check.error:Error{"existence_unknown","Name availability is inconclusive",{}};item.uploadStatus="failed";break;}
   if(check.status=="present"&&op.settings.collisionPolicy=="skip") {item.uploadStatus="skipped";break;}
   if(check.status=="present"&&op.settings.collisionPolicy=="rename") {item.candidate=UploadNaming::renamedCandidate(item.candidate,capabilities.maximumNameBytes);if(!save())return op;continue;}
   item.objectReference=check.objectReference;item.concurrencyToken=check.concurrencyToken;item.condition=check.status=="present"?"match":"absent";
   if(item.condition=="match"&&item.concurrencyToken.isEmpty()) {item.error={"precondition_missing","Provider returned no replacement precondition",{}};item.uploadStatus="failed";break;}
   item.uploadStatus="stored";targets[QJsonDocument(item.objectReference).toJson(QJsonDocument::Compact)].append(index);checked=true;break;
  }
  if(!checked&&item.uploadStatus=="stored") {item.uploadStatus="failed";item.error={"collision_exhausted","No available candidate after 32 checks",{}};}
  if(!save())return op;
 }
 for(const auto& group:targets) if(group.size()>1) {if(op.settings.collisionPolicy=="overwrite") {for(int i:group)op.items[i].uploadStatus="conflict";}else if(op.settings.collisionPolicy=="skip") {for(int n=1;n<group.size();++n)op.items[group[n]].uploadStatus="skipped";}else {for(int n=1;n<group.size();++n) {auto& item=op.items[group[n]];item.uploadStatus="failed";item.error={"name_alias_conflict","Provider candidates resolve to one target",{}};}}}
 if(!save())return op;
 for(auto& item:op.items) {
  if(job.cancelled->load()) {op.cancelled=true;break;}
  if(terminalUpload(item.uploadStatus)||item.uploadStatus=="failed")continue;
  auto lock=objectLock(op.destination,item.objectReference,item.candidate);std::unique_lock guard(*lock);
  if(item.uploadStatus=="unknown") {
   auto stat=awaitProvider<StatResult>([&](auto cb) {provider->reconcile(item.uploadRequest(op.destination),cb);});
   if(stat.status=="own"&&!stat.receipt.validate()&&stat.receipt.publicationId==item.publicationId&&stat.receipt.destination.toJson()==op.destination.toJson()) {item.receipt=stat.receipt;item.uploadStatus="uploaded";item.error={};if(!save())return op;continue;}
   if(stat.status!="missing") {item.error=stat.error?stat.error:Error{"publication_unknown","Resolve ambiguous publication before uploading again",{}};if(!save())return op;continue;}
   item.uploadStatus="stored";if(auto error=artifacts.verify(item.artifact)) {item.uploadStatus="failed";item.error=error;if(!save())return op;continue;}if(!save())return op;
  }
  for(int attempts=0;attempts<32;++attempts) {
   item.uploadStatus="uploading";if(!save())return op;
   auto uploaded=awaitProvider<UploadResult>([&](auto cb) {auto request=item.uploadRequest(op.destination);request.cancellation=job.cancelled;provider->upload(request,cb);});
   item.error=uploaded.error;
   if(uploaded.status=="uploaded") {
    auto e=uploaded.receipt.validate();if(!e&&(uploaded.receipt.publicationId!=item.publicationId||uploaded.receipt.destination.toJson()!=op.destination.toJson()))e={"receipt_invalid","Receipt does not match the accepted publication identity",{}};
    if(e) {item.uploadStatus="unknown";item.error=e;}else {item.receipt=uploaded.receipt;item.uploadStatus="uploaded";item.error={};}if(!save())return op;break;
   }
   if(uploaded.status=="unknown") {item.uploadStatus="unknown";if(!save())return op;break;}
   if(uploaded.status!="collision") {item.uploadStatus="failed";if(!save())return op;break;}
   // Reconcile our publication before changing any proposed target after a race.
   auto stat=awaitProvider<StatResult>([&](auto cb) {provider->reconcile(item.uploadRequest(op.destination),cb);});
   if(stat.status=="own"&&!stat.receipt.validate()&&stat.receipt.publicationId==item.publicationId&&stat.receipt.destination.toJson()==op.destination.toJson()) {item.receipt=stat.receipt;item.uploadStatus="uploaded";item.error={};if(!save())return op;break;}
   if(stat.status!="foreign") {item.uploadStatus="unknown";item.error={"publication_unknown","Collision publication could not be reconciled",{}};if(!save())return op;break;}
   if(op.settings.collisionPolicy=="skip") {item.uploadStatus="skipped";if(!save())return op;break;}
   if(op.settings.collisionPolicy!="rename") {item.uploadStatus="failed";item.error={"precondition_changed","Original replacement precondition changed",{}};if(!save())return op;break;}
   item.candidate=UploadNaming::renamedCandidate(item.candidate,capabilities.maximumNameBytes);item.objectReference={};item.concurrencyToken={};item.condition="absent";item.uploadStatus="stored";if(!save())return op;
   auto check=awaitProvider<NameCheck>([&](auto cb) {provider->checkName(op.destination,item.candidate,cb);});
   if(check.status!="absent"||check.error||check.objectReference.isEmpty()) {item.uploadStatus="failed";item.error={"existence_unknown","Renamed candidate could not be reserved",{}};if(!save())return op;break;}item.objectReference=check.objectReference;if(!save())return op;
   guard.unlock();lock=objectLock(op.destination,item.objectReference,item.candidate);guard=std::unique_lock<std::mutex>(*lock);
   if(attempts==31) {item.uploadStatus="failed";item.error={"collision_exhausted","Collision search exhausted",{}};save();}
  }
 }
 op.cancelled=op.cancelled||job.cancelled->load();
 op.state=op.cancelled?"cancelled":"completed";for(const auto& item:op.items) if(item.uploadStatus=="failed"||item.uploadStatus=="unknown")op.state=op.cancelled?"cancelled":"partial";
 save();return op;
}
void OperationService::releaseActive(const QString& id) {
 // Durable completion and source drain can arrive in either order. Retain the
 // bounded operation slot until both have happened; never unlock a live helper.
 if(preparedOwners_.contains(id))finishedPrepared_.insert(id);
 else active_.remove(id);
}
void OperationService::finish(Operation op,JsonReply reply) {
 if(selectingId_==op.id&&!preparedOwners_.contains(op.id))selectingId_.clear();
 if((op.cancelled||op.state=="failed"||op.state=="interrupted")&&op.copyStatus=="pending")op.copyStatus="no_result";
 const auto path=databasePath();background([path,op](Error& e) {HistoryStore store(path);e=store.saveOperation(op);return e?QJsonObject{}:store.operation(op.id,&e).toJson();},[this,id=op.id,reply](Error e,QJsonObject document) {
  emit changed(id);
  if(e) {releaseActive(id);if(reply)reply(e,{});return;}
  const auto op=Operation::fromJson(document);
  if(op.cancelled||op.state=="failed"||op.state=="interrupted") {releaseActive(op.id);if(reply)reply({},op.publicJson());return;}
  performCopy(op,false,[this,id=op.id,reply](Error error,QJsonObject result) {releaseActive(id);if(reply)reply(error,result);});
 });
}

void OperationService::recent(JsonReply reply) {const auto path=databasePath();const int limit=settings_.snapshot().recentActivityLimit;background([path,limit](Error& e) {HistoryStore store(path);return QJsonObject{{"operations",store.recent(limit,&e)}};},reply);}
void OperationService::inspect(const QString& id,JsonReply reply) {const auto path=databasePath();background([path,id](Error& e) {HistoryStore store(path);return store.operation(id,&e).publicJson();},reply);}
void OperationService::cancel(const QString& id,JsonReply reply) {
 if(active_.contains(id)) {active_[id]->store(true);if(selectingId_==id)source_.cancelAcquisition();
  for(int index=0;index<queue_.size();++index)if(queue_[index].op.id==id){auto job=queue_.takeAt(index);job.payload={};job.payloadLease.reset();job.op.cancelled=true;job.op.state="cancelled";finish(job.op);break;}
  reply({},{{"operationId",id},{"cancellationRequested",true},{"effect","Already sent network requests cannot be rolled back"}});return;}
 const auto path=databasePath();background([path,id](Error& e) {HistoryStore store(path);auto op=store.operation(id,&e);if(e)return QJsonObject{};op.cancelled=true;op.state="cancelled";e=store.saveOperation(op);return op.publicJson();},reply);
}
void OperationService::retry(const QString& id,JsonReply reply) {
 if(!ready_) {reply(startupError_?startupError_:Error{"initializing","Journal recovery is in progress",{}},{});return;}
 const auto limits=settings_.snapshot().limits;if(active_.size()>=limits.pendingOperations+limits.concurrentUploads) {reply({"queue_full","Pending operation limit reached",{}},{});return;}
 if(active_.contains(id)) {reply({"operation_busy","Operation is already active",{}},{});return;}
 auto reserved=std::make_shared<std::atomic_bool>(false);active_.insert(id,reserved);const auto path=databasePath();background([path,id](Error& e) {HistoryStore store(path);return store.operation(id,&e).toJson();},[this,reply,id,reserved](Error e,QJsonObject doc) {
  if(e) {active_.remove(id);reply(e,{});return;}auto op=Operation::fromJson(doc);if(op.items.isEmpty()) {active_.remove(id);reply({"artifact_missing","No acquired artifacts; invoke a new action",{}},{});return;}
  bool pending=false;for(const auto& i:op.items)if(!terminalUpload(i.uploadStatus))pending=true;
  if(!pending) {active_.remove(id);performCopy(op,true,reply);return;}
  op.cancelled=reserved->load();op.error={};enqueue({op,{},false,reserved,{}});reply({},{{"operationId",op.id},{"retryAccepted",true}});
 });
}
void OperationService::retryLocalSave(const QString& id,JsonReply reply) {if(!ready_) {reply(startupError_?startupError_:Error{"initializing","Journal recovery is in progress",{}},{});return;}const auto limits=settings_.snapshot().limits;if(active_.size()>=limits.pendingOperations+limits.concurrentUploads) {reply({"queue_full","Pending operation limit reached",{}},{});return;}if(active_.contains(id)) {reply({"operation_busy","Operation is active",{}},{});return;}active_.insert(id,std::make_shared<std::atomic_bool>(false));const auto path=databasePath();background([this,path,id](Error& e) {HistoryStore store(path);auto op=store.operation(id,&e);if(e)return QJsonObject{};ArtifactStore artifacts(root_+"/artifacts");for(auto& item:op.items)if(item.local.status!="saved") {if(auto error=artifacts.verify(item.artifact)) {item.local.status="failed";item.local.error=error;}else item.local=LocalArchive::save(item.artifact,op.settings.archive);}e=store.saveOperation(op);return op.publicJson();},[this,id,reply](Error e,QJsonObject r) {active_.remove(id);reply(e,r);});}
void OperationService::copyResult(const QString& id,JsonReply reply) {if(active_.contains(id)) {reply({"operation_busy","Wait for the operation to finish before copying its result",{}},{});return;}const auto ticket=invalidateCopyEligibility();const auto path=databasePath();background([path,id](Error& e) {HistoryStore store(path);return store.operation(id,&e).toJson();},[this,reply,ticket](Error e,QJsonObject doc) {if(e)reply(e,{});else performCopy(Operation::fromJson(doc),true,reply,ticket);});}
void OperationService::deliverCopy(const CopyPayload& payload,std::function<void(Error)> callback) {
 QPointer<OperationService> guarded(this);
 source_.copy(payload,[guarded,callback](Error error) {
  if(!guarded)return;
  QMetaObject::invokeMethod(guarded,[guarded,callback,error] {if(guarded&&!guarded->stopping_)callback(error);},Qt::QueuedConnection);
 });
}
void OperationService::performCopy(Operation op,bool explicitCopy,JsonReply reply,quint64 ticket) {
 if(explicitCopy&&ticket==0)ticket=invalidateCopyEligibility();
 const bool eligible=(explicitCopy&&ticket==invocation_)||(!explicitCopy&&(op.invocation==invocation_&&op.clipboardRevision==source_.clipboardRevision()));
 const auto path=databasePath();
 QPointer<OperationService> guarded(this);
 auto record=[guarded,path,reply](Operation value) {if(!guarded)return;guarded->background([path,value](Error& e) {HistoryStore store(path);e=store.saveOperation(value);return value.publicJson();},[guarded,value,reply](Error e,QJsonObject result) {if(!guarded)return;emit guarded->changed(value.id);if(reply)reply(e,result);});};
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
   deliverCopy(*prepared,completeCopy);
  });return;
 }
 deliverCopy(payload,completeCopy);
}
void OperationService::copyHistoryUrl(const QString& url,JsonReply reply,quint64 ticket) {if(ticket==0)ticket=invalidateCopyEligibility();if(ticket!=invocation_) {reply({"copy_suppressed","A newer sharing invocation superseded this copy request",{}},{});return;}if(url.isEmpty()) {reply({"link_unavailable","This record has no confirmed direct URL",{}},{});return;}CopyPayload p;p.kind="text";p.text=url;deliverCopy(p,[reply](Error e) {reply(e,{{"copied",!e}});});}
QJsonObject OperationService::capabilities() const {return {{"source",source_.capabilities()},{"providers",providers_.capabilities()}};}
QJsonObject OperationService::diagnostics() const {
#ifdef __GLIBC__
 constexpr bool reclaimSupported=true;
#else
 constexpr bool reclaimSupported=false;
#endif
 return {{"schemaVersion",1},{"build",QJsonDocument::fromJson(buildManifestJson()).object()},{"backend","Qt"},{"qtVersion",QT_VERSION_STR},{"activeOperations",active_.size()},{"journalReady",ready_},{"capabilities",capabilities()},
  {"memory",QJsonObject{{"allocatorReclamationSupported",reclaimSupported},{"allocatorReclamationPending",heapReclamationPending_},{"allocatorReclamationCount",qint64(heapReclamationCount_)},{"lastAllocatorReclamationUsec",lastHeapReclamationUsec_}}}};
}
void OperationService::cleanup(JsonReply reply) {const auto snapshot=settings_.snapshot();const auto path=databasePath(),root=root_;const auto activeIds=active_.keys();background([snapshot,path,root,activeIds](Error& e) {HistoryStore store(path);ArtifactStore artifacts(root+"/artifacts");e=store.error();if(e)return QJsonObject{};int removed=0;QDir dir(root+"/artifacts");for(const auto& name:dir.entryList(QDir::Dirs|QDir::NoDotAndDotDot)) {if(activeIds.contains(name))continue;Error error;auto op=store.operation(name,&error);if(error)continue;bool successful=true;for(const auto& item:op.items)if(item.uploadStatus=="unknown"||item.uploadStatus=="uploading")successful=false;const bool finished=op.state=="completed"||op.state=="partial"||op.state=="failed"||op.state=="cancelled"||op.state=="interrupted";bool uploaded=true;for(const auto& item:op.items)if(!terminalUpload(item.uploadStatus))uploaded=false;bool archivePending=false;for(const auto& item:op.items)if(item.local.status=="failed")archivePending=true;const auto date=QDateTime::fromString(op.created,Qt::ISODateWithMs);if(finished&&successful&&((uploaded&&!archivePending)||(snapshot.failedArtifactDays>0&&date.daysTo(QDateTime::currentDateTimeUtc())>=snapshot.failedArtifactDays))) {if(auto error=artifacts.remove(name)) {e=error;break;}++removed;}}artifacts.clearPreviews();return QJsonObject{{"removedOperations",removed}};},reply);}
}
