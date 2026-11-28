#include "OperationService.h"
#include "HistoryService.h"
#include "StorageLocations.h"
#include "DestinationTestJournal.h"
#include "core/CommandText.h"
#include "core/ImagePages.h"
#include "client/SessionProtocol.h"
#include "core/BuildInfo.h"
#include "core/Log.h"
#include "core/RunMarker.h"
#include "core/ReleaseLink.h"
#include <QBuffer>
#include <QSet>
#include <QSysInfo>
#include <QElapsedTimer>
#include <QImageReader>
#ifdef __GLIBC__
#include <malloc.h>
#endif
#include "core/UploadNaming.h"
#include "core/EncryptJob.h"
#include "core/HostEnvironment.h"
#include "FileUploads.h"
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
#include <QSet>
#include <QImageReader>
#endif
namespace putgd {
namespace {
// Another name for a taken candidate. A fresh random-characters name (#1729) can repeat another
// item's pending name in the batch, so it is drawn again, up to 32 times, until it does not.
QString renamedAwayFrom(const Settings& settings,const QString& name,int maximumBytes,const QSet<QString>& taken) {
 const auto* fresh=settings.namingMode=="generated"?&settings.randomName:nullptr;
 auto renamed=UploadNaming::renamedCandidate(name,maximumBytes,fresh);
 for(int draws=1;draws<32&&taken.contains(renamed);++draws)renamed=UploadNaming::renamedCandidate(name,maximumBytes,fresh);
 // Still another item's name: fall back to the UUID suffix, which does not repeat.
 if(taken.contains(renamed))renamed=UploadNaming::renamedCandidate(name,maximumBytes);
 return renamed;
}
QSet<QString> otherCandidates(const QList<OperationItem>& items,int index) {
 QSet<QString> names;for(int other=0;other<items.size();++other)if(other!=index)names.insert(items[other].candidate);return names;
}
// Clean library pages leave this process once it has been quiet for this long.
constexpr int imagePageDelayMs=30*1000;
QString uuid() {return QUuid::createUuid().toString(QUuid::WithoutBraces);}
Error unsupported() {return {"source_unsupported","Screen capture and clipboard access aren't available on this desktop.",{}};}
bool terminalUpload(const QString& s) {return s=="uploaded"||s=="skipped"||s=="conflict"||s=="local_only";}
const QLoggingCategory& providerLog(const QString& provider) {return provider=="s3"||provider=="cloudflare-r2"?logS3():provider=="proton-drive"?logProton():logOperation();}
// #2118: why a destination whose encrypted-page check hasn't passed can't encrypt, and what to do.
// Every per-upload place says it the same way: the editor, recording review, putgd-cli and MCP.
QString encryptCheckMessage(const QJsonObject& check,const QJsonObject& profile) {
 const auto label=profile.value("label").toString().trimmed();const auto name=label.isEmpty()?QStringLiteral("This destination"):label;
 const auto state=check.value("state").toString();
 if(state=="failed")return QStringLiteral("%1 didn't pass the encrypted-page check: %2 Fix that, then select Test upload and link in Settings → Destinations.").arg(name,check.value("reason").toString());
 if(state=="changed")return QStringLiteral("%1 changed after its encrypted-page check. Select Test upload and link in Settings → Destinations again.").arg(name);
 return QStringLiteral("%1 needs one passed Test upload and link before it can encrypt. It checks that encrypted pages open over https. Select it in Settings → Destinations.").arg(name);
}
// #2079: an encrypted file opens only from its link, which a private upload doesn't get.
QString encryptNeedsLink() {return QStringLiteral("Encrypted files open only from their link, and Sharing is set to Private upload. Change Sharing in Settings → Capture, or turn off Encrypt to upload it privately.");}
qint64 artifactBytes(const Operation& op) {qint64 total=0;for(const auto& item:op.items)total+=item.artifact.bytes;return total;}
QJsonObject itemFields(const Operation& op,const OperationItem& item) {return {{"operationId",op.id},{"itemId",item.artifact.id},{"provider",op.destination.providerId}};}
// A diagnostics upload (#123) has no action route: it goes to the destination
// it was accepted for and asks for a link.
// A file-manager upload (#1105) goes to the destination and publication mode
// resolved when it was accepted, stored in its own captureDefaults.
ActionSettings routeOf(const Operation& op) {
 if(op.action==OperationService::diagnosticsAction)return {"upload",op.destination.profileId,{},"share"};
 if(op.action==OperationService::fileManagerAction)return {"upload",op.destination.profileId,{},op.settings.document["captureDefaults"].toObject()["publicationMode"].toString("default")};
 return op.settings.actions.value(op.action);
}
QJsonObject uploadReviewOptions(const Operation& op,const ProviderRegistry& providers) {
 const auto route=routeOf(op);
 if(route.destination!="upload"||op.destination.profileId.isEmpty())return {{"destination","local"},{"sharingMode","local"},{"links",false},{"summary",op.settings.archive.keepUploads?"Save to the local archive":"No output enabled · Save a copy is available"}};
 const auto profile=op.settings.profiles.value(op.destination.profileId);
 auto mode=route.publicationMode;
 if(mode=="default")if(auto provider=providers.find(op.destination.providerId))mode=provider->defaultPublicationMode(op.destination);
 const auto label=profile["label"].toString(op.destination.profileId);
 // A destination that gives no links (an SFTP server without a Link address, #1871) never promises one.
 const bool links=commandtext::givesLinks(profile);
 const auto summary=mode=="private"?QStringLiteral("Private upload to %1").arg(label):links?QStringLiteral("Upload to %1 · Shared link").arg(label):QStringLiteral("Upload to %1").arg(label);
 return {{"destination",label},{"sharingMode",mode},{"summary",summary},{"links",links}};
}
// On-demand previews (#190) answer within historyPreviewReplyMs: at most this
// long queued, then a local copy check and a decode of up to 10 s each.
constexpr qsizetype maximumQueuedPreviews=3;
constexpr qint64 maximumPreviewWaitMs=20000;
constexpr int maximumPreviewVerifyMs=10000;
static_assert(maximumPreviewWaitMs+maximumPreviewVerifyMs+10000<historyPreviewReplyMs);
// Regeneration uses a retained artifact or local copy only while its size
// and SHA-256 still match the stored file.
Error sameFile(const Artifact& copy,const std::shared_ptr<std::atomic_bool>& cancelled,int deadlineMs) {
 const Error missing{"preview_source_missing","Neither the stored file nor a local copy is available",{}};
 const QFileInfo info(copy.path);if(!info.isFile()||info.isSymLink()||info.size()!=copy.bytes||copy.sha256.isEmpty())return missing;
 QFile file(copy.path);if(!file.open(QIODevice::ReadOnly))return missing;
 QCryptographicHash hash(QCryptographicHash::Sha256);QByteArray chunk(1<<20,Qt::Uninitialized);QElapsedTimer clock;clock.start();
 for(;;) {
  if(cancelled->load())return {"cancelled","Preview cancelled",{}};
  if(clock.elapsed()>deadlineMs)return {"preview_timeout","Checking the local file took too long.",{}};
  const auto read=file.read(chunk.data(),chunk.size());if(read<0)return missing;if(read==0)break;
  hash.addData(QByteArrayView(chunk.constData(),read));
 }
 if(QString::fromLatin1(hash.result().toHex())!=copy.sha256)return {"preview_source_changed","The local file changed since it was uploaded.",{}};
 return {};
}
}
QJsonObject UnavailableSource::capabilities() const {return {{"region",false},{"window",false},{"desktop",false},{"clipboardImport",QJsonArray{}},{"clipboardResult",QJsonArray{}},{"shortcuts",false},{"reason","native_adapter_unavailable"}};}
void UnavailableSource::acquire(const QString&,const ResourceLimits&,std::function<void(PayloadResult)> cb) {cb({{},unsupported()});}
void UnavailableSource::copy(const CopyPayload&,std::function<void(Error)> cb) {cb(unsupported());}
OperationService::OperationService(QString root,SettingsService& settings,ProviderRegistry& providers,SourceAdapter& source,QObject* parent):QObject(parent),root_(std::move(root)),settings_(settings),providers_(providers),source_(source),heapReclamationTimer_(this),observationTimer_(this),installation_(host::installationClass()) {
 connect(&settings_,&SettingsService::accepted,this,[this](const QString&){emit settingsAccepted();});
 observationTimer_.setSingleShot(true);observationTimer_.setInterval(250);
 connect(&observationTimer_,&QTimer::timeout,this,[this]{
  QSet<QString> ids;{std::lock_guard lock(observationsMutex_);ids=std::exchange(observationsDirty_,{});observationScheduled_=false;}
  for(const auto& id:ids)emit changed(id);
  if(!ids.isEmpty())emit observationsChanged();
 });
 uploadSlots_.setCapacity(settings_.snapshot().limits.concurrentUploads);
 source_.setRegionScreenshotFirst(settings_.snapshot().regionSelectionMode=="screenshot_first");
 connect(&settings_,&SettingsService::accepted,this,[this]{uploadSlots_.setCapacity(settings_.snapshot().limits.concurrentUploads);source_.setRegionScreenshotFirst(settings_.snapshot().regionSelectionMode=="screenshot_first");pump();});
 RecordingService::Host host;
 host.reviewOptions=[this](const Operation& op){
  auto options=uploadReviewOptions(op,providers_);
  // #2079: the review's Encrypt toggle starts from the destination's setting.
  // Shown for an upload route, whatever the destination is called.
  if(routeOf(op).destination=="upload"&&!op.destination.profileId.isEmpty()){const auto encrypt=encryptOptions(op);QJsonObject start{{"available",encrypt["available"]},{"on",encrypt["on"]}};if(encrypt.contains("reason"))start["reason"]=encrypt["reason"];options["encrypt"]=start;}
  return options;
 };
 host.encrypt=[this](const QString& id,bool on){recordEncrypt(id,on);};
 host.encryptRefusal=[this](const Operation& op)->Error{
  const auto options=encryptOptions(op);
  // #2118: only the encrypted-page check missing is encrypt_check_required, as on every other path.
  if(options["checkRequired"].toBool())return {"encrypt_check_required",options["reason"].toString()+QStringLiteral(" Or upload it without encryption."),{}};
  if(!options["available"].toBool())return {"encrypt_unavailable",options["reason"].toString(QStringLiteral("Encrypted links aren't available for this upload."))+" Upload it without encryption.",{}};
  // A saved passphrase that isn't loaded is said now, as the upload would say it after the export.
  if(options["passphrase"].toBool()){QString loaded;{std::lock_guard lock(encryptHooksMutex_);if(encryptPassphrase_)loaded=encryptPassphrase_(op.destination.profileId,op.settings.profiles.value(op.destination.profileId));}
   if(loaded.isEmpty())return {"encrypt_passphrase_locked","The Encrypted links passphrase isn't loaded. Unlock it in Destinations, then upload again.",{}};}
  return {};
 };
 host.encryptChoice=[this](const Operation& op){return encryptOptions(op)["on"].toBool();};
 host.settings=[this]{return settings_.snapshot();};
 host.accept=[this](Operation& op){return acceptRecording(op);};
 host.persist=[this](const Operation& op,std::function<void(Error)> done){const auto path=databasePath();background([path,op](Error& e){HistoryStore store(path);e=store.saveOperation(op);return QJsonObject{};},[done](Error e,QJsonObject){done(e);});};
 host.observe=[this](const Operation& op){observe(op.publicJson());emit changed(op.id);emit observationsChanged();};
 host.admit=[this](Operation op,ClipboardPayload payload,std::function<void()> released){
  auto flag=active_.value(op.id);if(!flag){released();return;}
  // #2079: an upload request that said encrypt decides; the review toggle's choice doesn't override it.
  if(op.encrypt.contains("requested")){std::lock_guard lock(encryptHooksMutex_);encryptChoices_.remove(op.id);}
  Job job{op,std::move(payload),true,flag,{}};
  // The staged export stays until the artifact store has streamed it.
  job.payloadLease=std::shared_ptr<qint64>(new qint64(0),[released](qint64* value){released();delete value;});
  admitUpload(std::move(job));
 };
 host.finish=[this](Operation op){finish(op);};
 host.remember=[this](const QJsonObject& preferences){settings_.patchAsync({{"preferences",preferences}},settings_.revision(),[](Result){});};
 auto scrollHost=host;
 scrollHost.accept=[this](Operation& op){return acceptScroll(op);};
 scrollHost.admit=[this,admit=host.admit](Operation op,ClipboardPayload payload,std::function<void()> released){
  if(op.action=="edit") {editingId_=op.id;editor_=std::make_shared<EditorSession>();scrollEditorReleases_[op.id]=std::move(released);if(op.settings.captureStyle.enabled)styledEdits_.insert(op.id);startEditing(op,active_.value(op.id),std::move(payload));}
  // One scrolling capture at a time: the mode is the one its invoke or scroll.prepare set (#1804).
  else {if(!sensitiveModes_.contains(op.id))sensitiveModes_.insert(op.id,scrollSensitiveMode_);admit(op,std::move(payload),std::move(released));}
 };
 scroll_=std::make_unique<ScrollCaptureService>(root_+"/scroll",std::move(scrollHost),this);
 TextService::Host textHost;
 textHost.settings=host.settings;textHost.persist=host.persist;textHost.observe=host.observe;
 textHost.accept=[this](Operation& op,const TextService::Acceptance& acceptance){return acceptText(op,acceptance);};
 textHost.finish=[this](Operation op){finishText(std::move(op));};
 textHost.copy=[this](const Operation& op,const QString& text,std::function<void(Error)> done){copyRecognizedText(op,text,std::move(done));};
 textHost.background=[this](std::function<void()> work,std::function<void()> done){background([work=std::move(work)](Error&){work();return QJsonObject{};},[done=std::move(done)](Error,QJsonObject){done();});};
 textHost.reclaim=[this]{requestHeapReclamation();};
 text_=std::make_unique<TextService>(std::move(textHost),this);
 connect(text_.get(),&TextService::finished,this,&OperationService::textFinished);
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
  imagePagesPending_=true;
#endif
  heapReclamationPending_=false;
  scheduleHeapReclamation();
 });
 imagePageTimer_.setSingleShot(true);
 connect(&imagePageTimer_,&QTimer::timeout,this,[this] {
  if(!imagePagesPending_||!heapReclamationIdle())return;
  // Runs after the quiet that follows an interaction, so none of its own pages fault back mid-way.
  QElapsedTimer elapsed;elapsed.start();
  const auto release=releaseCleanImagePages();
  lastImagePageReleaseUsec_=elapsed.nsecsElapsed()/1000;lastImagePageReleaseBytes_=release.bytes;
  ++imagePageReleaseCount_;
  imagePagesPending_=false;
 });
 recoverJournal();
}
void OperationService::recoverJournal() {
 if(stopping_)return;
 const auto path=databasePath(),artifactsRoot=root_+"/artifacts";
 background([this,path,artifactsRoot](Error& e) {
  if(!QDir().mkpath(root_)||!QFile::setPermissions(root_,QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner)) {e={"history_io","Couldn't create put.gd's data folder.",{}};return QJsonObject{};}
  if(!journalOwner_||!journalOwner_->isLocked()) {journalOwner_=std::make_unique<QLockFile>(root_+"/resident.lock");journalOwner_->setStaleLockTime(0);}
  if(!journalOwner_->isLocked()&&!journalOwner_->tryLock(0)) {
   // Qt removes a well-formed lock whose owner is gone; one it cannot verify as stale (malformed or unreadable) is left
   // alone, with the manual step, and no owner is ever named (#1576).
   e=journalOwner_->error()==QLockFile::LockFailedError?StorageLocations::journalLockBusy("journal_busy",root_+"/resident.lock"):Error{"history_io","Cannot create the journal lock file",{}};return QJsonObject{};
  }
  // Unsubmitted editor sources and recording exports never survive their resident.
  EditorJob::discardStaging(root_+"/editing");
  // #1820: covered copies of a selection's files wait here only until their upload acquires them.
  EditorJob::discardStaging(root_+"/covered");
  EditorJob::discardStaging(root_+"/styled");
  RecordingService::discardStaging(root_+"/recording");
  // Earlier destination tests' journals lose their signed links before any test is admitted (#1680).
  scrubDestinationTestJournals(root_+"/destination-tests");
  destinationTestsReady_=true;
  HistoryStore store(path);e=store.error();if(e)return QJsonObject{};e=store.indexPublications(providers_);if(e)return QJsonObject{};ArtifactStore artifacts(artifactsRoot);artifacts.discardIncomplete();const auto recovered=store.recover(&e);
  // A recovery that failed partway is retried whole: a later write below must not clear its error (#1576).
  if(e)return QJsonObject{};
  for(auto op:recovered)if(op.items.isEmpty()) {auto batch=artifacts.recover(op.id);if(batch.error) {op.error=batch.error;e=store.saveOperation(op);if(e)break;}else if(!batch.artifacts.isEmpty()) {for(const auto& artifact:std::as_const(batch.artifacts)) {OperationItem item;item.artifact=artifact;item.publicationId=uuid();op.items.append(item);}op.error={"operation_interrupted","Upload was interrupted before it finished. Select Retry upload in History.",{}};e=store.saveOperation(op);if(e)break;}}
  return QJsonObject{};
 },[this](Error e,QJsonObject) {
  startupError_=e;ready_=!e;
#ifdef __GLIBC__
  if(ready_) {heapReclamationPending_=true;scheduleHeapReclamation();}
#endif
  emit initialized();
  if(ready_) {cleanupTimer_.start();QTimer::singleShot(cleanupSchedule_.startupDelayMs,this,&OperationService::cleanupExpired);}
  else if(!stopping_) {
   const int pause=std::min(recoveryRetryMs_<<std::min(recoveryAttempts_,4),30000);
   ++recoveryAttempts_;
   QTimer::singleShot(pause,this,&OperationService::recoverJournal);
  }
 });
}
OperationService::~OperationService() {shutdown();}
void OperationService::shutdown() {if(stopping_)return;stopping_=true;posterCancel_->store(true);cleanupTimer_.stop();if(cleanupStop_)cleanupStop_->store(true);heapReclamationTimer_.stop();imagePageTimer_.stop();observationTimer_.stop();for(auto flag:std::as_const(active_)) flag->store(true);{std::lock_guard lock(itemCancelsMutex_);for(const auto& items:std::as_const(itemCancels_))for(const auto& flag:items)flag->store(true);}uploadSlots_.close();if(scroll_)scroll_->shutdown();if(text_)text_->shutdown();if(recording_)recording_->shutdown();if(editor_)editor_->cancel();for(const auto& check:std::as_const(checkSessions_))check->cancel();checkQueue_.clear();invalidateCopyEligibility();source_.cancelAcquisition();for(auto* thread:threads_) {thread->wait();delete thread;}threads_.clear();}
bool OperationService::heapReclamationIdle() const {
 return !stopping_&&threads_.isEmpty()&&queue_.isEmpty()&&running_==0&&active_.isEmpty()&&copies_.isEmpty()&&!currentCopy_&&selectingId_.isEmpty()&&payloadBytes_->load()==0&&!(recording_&&recording_->active());
}
void OperationService::scheduleHeapReclamation() {
 if(!heapReclamationIdle())return;
 if(heapReclamationPending_)heapReclamationTimer_.start(2000);
 else if(imagePagesPending_)imagePageTimer_.start(imagePageDelayMs);
}
void OperationService::requestHeapReclamation() {
 if(stopping_)return;
 heapReclamationPending_=true;scheduleHeapReclamation();
}
void OperationService::requestImagePageRelease() {
 if(stopping_)return;
 imagePagesPending_=true;
 // A pending trim schedules the release itself; work that is still running
 // schedules it when it drains.
 if(!heapReclamationPending_&&!imagePageTimer_.isActive()&&heapReclamationIdle())imagePageTimer_.start(imagePageDelayMs);
}
void OperationService::background(std::function<QJsonObject(Error&)> work,JsonReply reply) {
 heapReclamationTimer_.stop();imagePageTimer_.stop();
 QPointer<OperationService> self(this);auto* thread=QThread::create([self,work=std::move(work),reply=std::move(reply)] {Error error;auto result=work(error);if(self) QMetaObject::invokeMethod(self,[self,reply,error,result] {if(self&&!self->stopping_) reply(error,result);},Qt::QueuedConnection);});
 threads_.append(thread);connect(thread,&QThread::finished,this,[this,thread=QPointer<QThread>(thread)] {
  // shutdown() may have waited for and deleted it already.
  if(!thread)return;
  threads_.removeOne(thread);thread->deleteLater();
  // Every worker opens its own journal connection and frees its page cache and
  // query results on exit, read-only ones included (#1184). Arm the idle gate
  // here so no caller has to remember to; failed startup has nothing to return.
  if(ready_&&!stopping_)heapReclamationPending_=true;
  scheduleHeapReclamation();
 });thread->start();
}
std::shared_ptr<std::timed_mutex> OperationService::objectLock(const Destination& d,const QJsonObject& object,const QString& name) {
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
  lock=std::make_shared<std::timed_mutex>();
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
QJsonArray OperationService::activeObservations() const {
 QJsonArray result;std::lock_guard lock(observationsMutex_);
 for(const auto& document:observations_)if(Operation::activeState(document["state"].toString()))result.append(document);
 return result;
}
void OperationService::clearObservation(const QString& id){std::lock_guard lock(observationsMutex_);observations_.remove(id);observationsDirty_.remove(id);}
QJsonObject OperationService::currentObservation(QJsonObject document)const{
 // A finished operation that still holds its slot says so: Retry waits for it to clear (#1578).
 if(settling(document["id"].toString()))document["settling"]=true;
 if(QStringList{"completed","partial","failed","cancelled","interrupted"}.contains(document["state"].toString()))return document;
 QJsonObject live;{std::lock_guard lock(observationsMutex_);live=observations_.value(document["id"].toString());}if(live.isEmpty())return document;
 document["phase"]=live["phase"];if(live.contains("sensitive"))document["sensitive"]=live["sensitive"]; // #1804: kinds and counts
 auto items=document["items"].toArray();QHash<QString,QJsonObject> observedItems;
 for(const auto& value:live["items"].toArray()){const auto observed=value.toObject();const auto id=observed["artifact"].toObject()["id"].toString();if(!observedItems.contains(id))observedItems.insert(id,observed);}
 for(qsizetype i=0;i<items.size();++i){auto item=items[i].toObject();const auto observed=observedItems.constFind(item["artifact"].toObject()["id"].toString());if(observed!=observedItems.cend()){item["phase"]=(*observed)["phase"];item["uploadProgress"]=(*observed)["uploadProgress"]; }items[i]=item;}
 document["items"]=items;return document;
}
void OperationService::invoke(const QString& action,JsonReply reply,const QJsonObject& requested,bool agent) {
 // A tray click's xdg-activation token brings an open editor forward (#657);
 // only the tray passes it, session requests cannot.
 auto stripped=requested;const auto activationToken=stripped.take("activationToken").toString();
 // #1804: who answers a sensitive text finding: a person at a prompt, or the caller through operation.decide.
 const auto sensitiveValue=stripped.take("sensitiveText");
 const QJsonObject options=stripped; // const: operator[] must not insert
 emit explicitRequest();
 if(!ready_) {reply(notReadyError(),{});return;}
 if(!sensitiveValue.isUndefined()&&sensitiveValue!="prompt"&&sensitiveValue!="report"){reply({"invalid_request","sensitiveText is prompt or report",{}},{});return;}
 const auto sensitiveMode=sensitiveValue.toString(QStringLiteral("report"));
 // Screen recording is its own session: the tray opens or raises it, a global
 // shortcut toggles it (prepare, start, stop) so the keyboard can end a recording.
 if(action=="scroll"||(action=="edit"&&options["source"]=="scroll")){if((action=="scroll"&&!options.isEmpty())||(action=="edit"&&options.size()!=1)){reply({"invalid_request","Invalid scroll action parameters",{}},{});return;}if(!scroll_->active())scrollSensitiveMode_=sensitiveMode; /* a running capture keeps the mode it started with */scroll_->invoke(action=="edit",reply);return;}
 if(scroll_->active()){reply({"scroll_active","A scrolling capture is already running. Stop or cancel it first.",{{"operationId",scroll_->activeId()}}},{});return;}
 if(action==Settings::recordingActionName()) {
  const auto mode=options["mode"].toString("open");
  if((!options.isEmpty()&&!(options.size()==1&&options.contains("mode")))||(mode!="open"&&mode!="toggle")){reply({"invalid_request","The record action takes mode open or toggle",{}},{});return;}
  if(editor_){reply({"editor_active","An image edit is already open. Finish or cancel it before recording.",{{"operationId",editingId_}}},{});return;}
  recording_->invoke(mode,reply);return;
 }
 // Copy text (#1428) is route-free and never reaches the upload pipeline: a
 // region selected as for Capture region, read by the helper, then one copy.
 if(action==Settings::textActionName()) {
  const auto delay=options["delaySeconds"];
  if((!options.isEmpty()&&!(options.size()==1&&options.contains("delaySeconds")))||(!delay.isUndefined()&&(!delay.isDouble()||delay.toDouble()!=delay.toInt()||!Settings::captureDelayChoices().contains(delay.toInt())))){reply({"invalid_request","The text action takes delaySeconds 0, 3, 5 or 10",{}},{});return;}
  QJsonObject params{{"source","region"},{"copy",true},{"agent",agent}};if(delay.toInt()>0)params["delaySeconds"]=delay.toInt();
  recognizeText(params,std::move(reply));return;
 }
 return invokeCapture(action,std::move(reply),options,activationToken,sensitiveMode);
}
void OperationService::invokeCapture(const QString& requestedAction,JsonReply reply,const QJsonObject& requested,const QString& activationToken,const QString& sensitiveMode) {
 // Repeat last region (#1363) is a region capture of a remembered rectangle: it
 // has its own key and entry, and the region action's output, name and history.
 const bool repeat=requestedAction==Settings::repeatActionName();
 const QString action=repeat?QStringLiteral("region"):requestedAction;
 if(!Settings::actionNames().contains(action)) {reply({"invalid_action","Unknown sharing action",{}},{});return;}
 // delaySeconds (#1363): region, window, monitor and desktop captures, a repeat
 // and an edit of a screen capture can start after 3, 5 or 10 seconds; 0 is immediate.
 auto stripped=requested;const auto delayValue=stripped.take("delaySeconds");
 // #1893: captureStyle true opens a file's edit with the capture style on (Edit again of a styled capture's original).
 const auto styleValue=stripped.take("captureStyle");
 // #2079: encrypt true or false overrides the destination's Encrypted links setting for this upload.
 const auto encryptValue=stripped.take("encrypt");
 const QJsonObject options=stripped; // const: operator[] must not insert
 const int delay=delayValue.toInt();
 if(!delayValue.isUndefined()&&(!Settings::delayableAction(requestedAction)||!delayValue.isDouble()||delayValue.toDouble()!=delay||!Settings::captureDelayChoices().contains(delay))){reply({"invalid_request","delaySeconds is 0, 3, 5 or 10, for region, window, monitor, desktop, repeat and edit captures",{}},{});return;}
 // Edit & upload acquires from a chosen source, by default the native region
 // selector; every other action is its own source and takes no other options.
 const auto source=repeat?requestedAction:action=="edit"?options["source"].toString("region"):action;
 const auto sourcePath=options["path"].toString();
 if((action!="edit"&&!options.isEmpty())||(action=="edit"&&(!QStringList{"region","window","desktop","clipboard","file"}.contains(source)||(source=="file")!=options.contains("path")||(source=="file"&&!QDir::isAbsolutePath(sourcePath))))){reply({"invalid_request","Edit sources are region, window, desktop, clipboard, or file with an absolute path",{}},{});return;}
 // An edit of the clipboard or a file reads no screen, so it has nothing to delay.
 if(!styleValue.isUndefined()&&(action!="edit"||source!="file"||!styleValue.isBool())){reply({"invalid_request","captureStyle is true or false, for an edit of a file",{}},{});return;}
 if(!encryptValue.isUndefined()&&!encryptValue.isBool()){reply({"invalid_request","encrypt is true or false",{}},{});return;}
 if(action=="edit"&&delay>0&&!Settings::delayableEditSource(source)){reply({"invalid_request","delaySeconds applies to an edit of a region, window or desktop capture",{}},{});return;}
 if(action=="edit"&&editor_){editor_->request("raise",activationToken.isEmpty()?QJsonObject{}:QJsonObject{{"activationToken",activationToken}},[](Error,QJsonObject){});reply({"editor_active","An image edit is already open. Finish or cancel it first.",{{"operationId",editingId_}}},{});return;}
 if(action=="edit"&&recording_->active()){reply({"recording_active","A recording is already running. Stop or cancel it before editing.",{{"operationId",recording_->activeId()}}},{});return;}
 // #2118: encrypt true where the destination's encrypted-page check hasn't passed captures nothing,
 // once the request is otherwise valid.
 if(encryptValue.toBool())if(auto refusal=encryptRefusal(action)){reply(refusal,{});return;}
 const bool interactive=source!="clipboard"&&source!="file";
 auto snapshot=settings_.snapshot();if(active_.size()>=snapshot.limits.pendingOperations+snapshot.limits.concurrentUploads) {reply({"queue_full","Too many actions are in progress. Wait for some to finish, or raise Queued actions in Settings → Advanced → Resource limits.",{}},{});return;}
 if(interactive&&!selectingId_.isEmpty()) {reply({"busy_selecting","Another capture selection or countdown is already in progress. Finish or cancel it first.",{}},{});return;}
 const auto route=snapshot.actions.value(action);
 const bool upload=route.destination=="upload"&&!route.profileId.isEmpty();
 if(!upload&&!snapshot.archive.keepUploads&&!(action=="edit"&&(source=="clipboard"||source=="file"))){reply({"output_required","No output is enabled. Choose a destination or turn on Keep a local copy in Settings → Capture.",{}},{});return;}
 Operation op;op.id=uuid();op.action=action;op.settings=snapshot;op.settingsRevision=settings_.revision();op.created=QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);op.invocation=upload?++invocation_:invalidateCopyEligibility();op.clipboardRevision=source_.clipboardRevision();
 if(upload) {const auto profile=snapshot.profiles.value(route.profileId);op.destination={profile["provider"].toString(),route.profileId,settings_.revision(),profile["settings"].toObject(),profile["credentials"].toObject()};}
 if(encryptValue.isBool())op.encrypt={{"requested",encryptValue.toBool()}};
 auto flag=std::make_shared<std::atomic_bool>(false);active_.insert(op.id,flag);sensitiveModes_.insert(op.id,sensitiveMode);if(upload)beginUploadCopies(op,source=="clipboard");if(interactive)selectingId_=op.id;if(action=="edit"){editingId_=op.id;editor_=std::make_shared<EditorSession>();{std::lock_guard lock(encryptHooksMutex_);encryptOpen_.insert(op.id);}if((interactive&&snapshot.captureStyle.enabled)||styleValue.toBool())styledEdits_.insert(op.id);}
 // An edit uploads only when its editor is done; every other upload follows its capture at once.
 if(upload&&action!="edit")anticipate(op);
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
   if(finished){owner->active_.remove(id);owner->settled(id);}
  };
  if(QThread::currentThread()==owner->thread())release();
  else QMetaObject::invokeMethod(owner,std::move(release),Qt::QueuedConnection);
 };
 SourceAdapter::PreparedAcquisition acquisition;
 // A delayed capture prepares nothing until its countdown ends, so no adapter
 // holds its reservation yet. When the operation ends before the acquisition is
 // ever started (a cancel in that gap, a failed journal save), nothing else would
 // release it, and every later interactive capture would be busy_selecting.
 std::function<void()> releaseUnused;
 if(source=="file"){
  // A file is read and decoded by the editor stage; no native source is involved.
  acquisition=[sourcePath](const ResourceLimits&,std::function<void(PayloadResult)> deliver){ClipboardPayload payload;payload.kind="files";payload.files={sourcePath};deliver({payload,{}});};
  released();
 }else {
  const auto output=uploadReviewOptions(op,providers_);
  source_.setCaptureOutcome(action=="edit"?QStringLiteral("Opens the image editor"):upload?QStringLiteral("Uploads right away to %1 · %2").arg(output["destination"].toString(),output["sharingMode"]=="private"?QStringLiteral("Private upload"):output["links"].toBool(true)?QStringLiteral("Shared link"):QStringLiteral("Upload only")):QStringLiteral("Saves locally"));
  const bool screenshotFirst=snapshot.regionSelectionMode=="screenshot_first";
  source_.setRegionScreenshotFirst(screenshotFirst);
  if(const auto refused=SourceAdapter::knownPermissionRefusal(source_.capabilities(),source);refused) {
   // The compositor is known to refuse: fail with its error now, as the
   // capture would after a selection, and show no selector or countdown.
   earlyTerminal(refused);released();
   acquisition=[refused](const ResourceLimits&,std::function<void(PayloadResult)> deliver){deliver({{},refused});};
  }else if(delay>0&&!(source=="region"&&!screenshotFirst)) {
   // The countdown runs first and the target is frozen when it ends: the window
   // then active, the monitor then under the pointer, the screen as it then is
   // (in screenshot-first mode, before the freeze). Nothing is prepared, shown
   // or read until both journal saves are durable and the countdown has ended.
   const auto outcome=source_.captureOutcome();
   releaseUnused=released;
   acquisition=[owner,source,delay,screenshotFirst,outcome,flag,earlyTerminal=std::function<void(Error)>(std::move(earlyTerminal)),released=std::function<void()>(std::move(released))](const ResourceLimits& limits,std::function<void(PayloadResult)> deliver) mutable {
    if(!owner){deliver({{},{"source_unavailable","Capture stopped because put.gd is shutting down.",{}}});return;}
    owner->source_.delayed(delay,[owner,source,screenshotFirst,outcome,limits,flag,earlyTerminal=std::move(earlyTerminal),released=std::move(released),deliver=std::move(deliver)](Error error) mutable {
     // A cancel that arrives after the countdown, while the lock is being asked
     // about, finds nothing to stop: it must still keep every pixel unread.
     if(!owner||owner->stopping_||flag->load()){if(released)released();deliver({{},{"cancelled","Capture cancelled",{}}});return;}
     if(error){if(released)released();deliver({{},std::move(error)});return;}
     owner->source_.setCaptureOutcome(outcome);owner->source_.setRegionScreenshotFirst(screenshotFirst);owner->source_.setCaptureDelay(0);
     owner->source_.prepareAcquisition(source,limits,std::move(earlyTerminal),std::move(released))(limits,std::move(deliver));
    });
   };
  }else {
   // A live region is chosen first; its adapter then counts down before the pixels.
   source_.setCaptureDelay(delay);
   acquisition=source_.prepareAcquisition(source,snapshot.limits,std::move(earlyTerminal),std::move(released));
   source_.setCaptureDelay(0);
  }
 }
 const auto path=databasePath();background([path,op](Error& e) {HistoryStore store(path);e=store.saveOperation(op);return op.publicJson();},[this,op,flag,reply,acquisition,earlyError,releaseUnused](Error e,QJsonObject result) mutable {
  if(e) {if(releaseUnused)releaseUnused();abandonUploadCopies(op.id);releaseActive(op.id);if(selectingId_==op.id&&!preparedOwners_.contains(op.id))selectingId_.clear();if(editingId_==op.id){editor_.reset();editingId_.clear();}reply(e,{});return;}
  observe(op.publicJson());reply({},{{"operationId",op.id},{"operation",result}});emit changed(op.id);emit observationsChanged();
  logging::info(logOperation(),"Operation accepted",{{"operationId",op.id},{"action",op.action},{"output",op.destination.providerId.isEmpty()?"local":"upload"},{"provider",op.destination.providerId},{"profileId",op.destination.profileId}});
  // Acceptance stays durable and observable before acquiring pixels. Region
  // preview may already be visible; a terminal event skips the remaining work.
  QTimer::singleShot(0,this,[this,op,flag,acquisition,earlyError,releaseUnused]() mutable {
   // The release comes before finish(), which frees the selection only once no owner is left.
   if(flag->load()) {if(releaseUnused)releaseUnused();op.advance(OperationState::Cancelled);op.cancelled=true;finish(op);return;}
   if(*earlyError) {if(releaseUnused)releaseUnused();op.error=*earlyError;op.cancelled=op.error.code=="cancelled"||op.error.code=="source_cancelled";op.advance(op.cancelled?OperationState::Cancelled:OperationState::Failed);finish(op);return;}
   op.advance(OperationState::Acquiring);op.phase="acquiring";const auto path=databasePath();background([path,op](Error& e) {HistoryStore store(path);e=store.saveOperation(op);return QJsonObject{};},[this,op,flag,acquisition,earlyError,releaseUnused](Error e,QJsonObject) mutable {
    if(e) {if(releaseUnused)releaseUnused();op.error=e;op.advance(OperationState::Failed);finish(op);return;}
    observe(op.publicJson());
    if(flag->load()) {if(releaseUnused)releaseUnused();op.advance(OperationState::Cancelled);op.cancelled=true;finish(op);return;}
    if(*earlyError) {if(releaseUnused)releaseUnused();op.error=*earlyError;op.cancelled=op.error.code=="cancelled"||op.error.code=="source_cancelled";op.advance(op.cancelled?OperationState::Cancelled:OperationState::Failed);finish(op);return;}
    QPointer<OperationService> guarded(this);
    acquisition(op.settings.limits,[guarded,op,flag](PayloadResult r) {
     if(!guarded)return;
     QMetaObject::invokeMethod(guarded,[guarded,op,flag,r=std::move(r)]() mutable {
      if(!guarded||guarded->stopping_)return;
      if(guarded->selectingId_==op.id&&!guarded->preparedOwners_.contains(op.id))guarded->selectingId_.clear();
      if(r.error||flag->load()) {op.error=r.error;op.cancelled=flag->load()||r.error.code=="cancelled"||r.error.code=="source_cancelled";op.advance(op.cancelled?OperationState::Cancelled:OperationState::Failed);guarded->finish(op);return;}
      guarded->clipboardImports_.remove(op.id);
      if(op.action=="edit")guarded->startEditing(op,flag,std::move(r.payload));
      else guarded->admitUpload({op,std::move(r.payload),true,flag,{}});
      guarded->pumpCopies();
     },Qt::QueuedConnection);
    });
   });
  });
 });
}
void OperationService::uploadDiagnostics(const QString& path,const QString& profileId,JsonReply accepted,std::function<void(const Operation&)> finished) {
 if(!ready_) {accepted(notReadyError(),{});return;}
 auto snapshot=settings_.snapshot();
 if(!snapshot.profiles.contains(profileId)){accepted({"profile_not_found","No destination has this ID",{{"profileId",profileId}}},{});return;}
 if(active_.size()>=snapshot.limits.pendingOperations+snapshot.limits.concurrentUploads) {accepted({"queue_full","Too many actions are in progress. Wait for some to finish, or raise Queued actions in Settings → Advanced → Resource limits.",{}},{});return;}
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
  op.advance(OperationState::Acquiring);enqueue({op,std::move(payload),true,flag,{}});
 });
}
void OperationService::uploadFiles(const QStringList& paths,const QString& profileId,const QString& installation,JsonReply reply,const QString& sensitiveMode,const QJsonObject& encrypt) {
 // The installation identity comes first: one install's menu entry never
 // uploads through another install's resident, whatever it asks for.
 if(!installation.isEmpty())if(auto error=FileUploads::matchInstallation(installation,installation_)){logging::warning(logOperation(),"File upload refused",{{"action",fileManagerAction},{"error",logging::errorFields(error)}});reply(error,{});return;}
 if(!ready_) {reply(notReadyError(),{});return;}
 if(uploadGate_)if(auto error=uploadGate_()){reply(error,{});return;}
 const auto snapshot=settings_.snapshot();const auto revision=settings_.revision();
 if(active_.size()>=snapshot.limits.pendingOperations+snapshot.limits.concurrentUploads) {reply({"queue_full","Too many actions are in progress. Wait for some to finish, or raise Queued actions in Settings → Advanced → Resource limits.",{}},{});return;}
 const auto route=FileUploads::resolve(snapshot,profileId);
 Destination destination;
 if(!route.error){const auto profile=snapshot.profiles.value(route.profileId);destination={profile["provider"].toString(),route.profileId,revision,profile["settings"].toObject(),profile["credentials"].toObject()};}
 // Paths are checked before the destination is reported, so a bad file is named
 // even without one; the destination's largest object bounds the files when known.
 auto selected=std::make_shared<FileUploads::Selection>();const auto artifactsRoot=root_+"/artifacts";
 background([this,paths,snapshot,destination,selected,artifactsRoot](Error& e){
  qint64 objectLimit=0;
  if(!destination.providerId.isEmpty())if(auto provider=providers_.find(destination.providerId))objectLimit=provider->capabilities(destination).maximumObjectBytes;
  *selected=FileUploads::select(paths,snapshot,fileManagerAction,objectLimit,ArtifactStore::diskBudget(artifactsRoot));
  e=selected->error;return QJsonObject{};
 },[this,snapshot,revision,route,destination,selected,reply,sensitiveMode,encrypt](Error e,QJsonObject) mutable {
  if(!e)e=route.error;
  if(e){
   logging::warning(logOperation(),"File upload refused",{{"action",fileManagerAction},{"error",logging::errorFields(e)},{"requested",selected->files.size()}});
   if(e.code=="destination_required")emit destinationRequired();
   reply(e,{});return;
  }
  if(active_.size()>=snapshot.limits.pendingOperations+snapshot.limits.concurrentUploads) {reply({"queue_full","Too many actions are in progress. Wait for some to finish, or raise Queued actions in Settings → Advanced → Resource limits.",{}},{});return;}
  Operation op;op.id=uuid();op.action=fileManagerAction;op.settings=Settings::fromValidated(FileUploads::routedDocument(snapshot,route));op.settingsRevision=revision;
  op.created=QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);op.invocation=++invocation_;op.clipboardRevision=source_.clipboardRevision();op.destination=destination;
  if(encrypt.contains("enabled"))op.encrypt={{"requested",encrypt["enabled"].toBool()}};
  const auto encryptPassphrase=encrypt["passphrase"].toString();
  // The passphrase itself is never kept; a Retry that still has to encrypt refuses without it.
  if(!encryptPassphrase.isEmpty())op.encrypt["passphraseFrom"]="request";
  auto flag=std::make_shared<std::atomic_bool>(false);active_.insert(op.id,flag);sensitiveModes_.insert(op.id,sensitiveMode);beginUploadCopies(op,false);anticipate(op);
  ClipboardPayload payload;payload.kind="files";payload.files=selected->files;payload.zipFolders=selected->zipFolders;
  const auto origins=selected->origins;const auto database=databasePath();
  background([database,op](Error& e) {HistoryStore store(database);e=store.saveOperation(op);return op.publicJson();},[this,op,flag,payload,origins,reply,encryptPassphrase](Error e,QJsonObject result) mutable {
   if(e) {abandonUploadCopies(op.id);releaseActive(op.id);reply(e,{});return;}
   observe(op.publicJson());reply({},{{"operationId",op.id},{"operation",result}});emit changed(op.id);emit observationsChanged();
   logging::info(logOperation(),"Operation accepted",{{"operationId",op.id},{"action",op.action},{"output","upload"},{"provider",op.destination.providerId},{"profileId",op.destination.profileId},{"items",payload.files.size()},{"zipFolders",payload.zipFolders},{"encrypt",op.encrypt}});
   op.advance(OperationState::Acquiring);op.phase="acquiring";Job job{op,std::move(payload),true,flag,{},origins};job.encryptPassphrase=encryptPassphrase;admitUpload(std::move(job));
  });
 });
}
namespace {
// A PNG or JPEG by its signature: what the editor helper can open and check.
// A file's identity as the sensitive text check saw it: device, inode, size and modification time.
QString fileIdentity(const QString& path){
 struct stat s{};if(::stat(QFile::encodeName(path).constData(),&s)!=0)return {};
 return QStringLiteral("%1:%2:%3:%4.%5").arg(s.st_dev).arg(s.st_ino).arg(s.st_size).arg(s.st_mtim.tv_sec).arg(s.st_mtim.tv_nsec);
}
bool checkableImage(const QString& path){
 const QFileInfo info(path);if(!info.isFile()||info.size()<8)return false;
 QFile file(path);if(!file.open(QIODevice::ReadOnly))return false;
 const auto head=file.read(8);return head.startsWith("\x89PNG\r\n\x1a\n")||head.startsWith("\xff\xd8\xff");
}
QJsonObject sensitiveLog(const QJsonObject& findings){
 // Kinds and counts only: what was matched never leaves the helper.
 QJsonObject fields{{"complete",findings["complete"].toBool(true)}};
 const auto found=findings["found"].toObject();for(auto it=found.begin();it!=found.end();++it)fields[it.key()]=it.value();
 if(findings.contains("reason"))fields["reason"]=findings["reason"];
 return fields;
}
}
// #1804: an image capture, clipboard image or file-manager image going to a destination,
// with Settings → Capture → Check captures for sensitive text before uploading on.
bool OperationService::needsSensitiveCheck(const Job& job) const {
 const auto& op=job.op;
 if(!op.settings.sensitiveTextCheck||op.settings.sensitiveTextCategories.isEmpty()||job.sharingOnly||!job.acquire)return false;
 static const QStringList actions{"region","window","monitor","desktop","clipboard","scroll",fileManagerAction};
 if(!actions.contains(op.action))return false;
 const auto route=routeOf(op);
 if(route.destination!="upload"||route.profileId.isEmpty()||op.destination.profileId.isEmpty())return false;
 if(job.payload.kind=="image")return !job.payload.image.isNull();
 // A scrolling capture arrives as its encoded PNG.
 if(job.payload.kind=="png")return !job.payload.bytes.isEmpty();
 // Which files are images is read on the check's worker.
 return job.payload.kind=="files"&&!job.payload.files.isEmpty();
}
void OperationService::admitUpload(Job job) {
 if(!needsSensitiveCheck(job)){enqueue(std::move(job));return;}
 // #2079: the jobs a check rebuilds carry the request's passphrase on to enqueue().
 if(!job.encryptPassphrase.isEmpty())checkPassphrases_.insert(job.op.id,job.encryptPassphrase);
 if(job.cancelled->load()){job.payload={};job.op.cancelled=true;job.op.advance(OperationState::Cancelled);finish(job.op);return;}
 job.op.phase="checking";observe(job.op.publicJson());emit changed(job.op.id);emit observationsChanged();
 checkQueue_.enqueue(std::move(job));pumpChecks();
}
void OperationService::pumpChecks() {
 // One check at a time: a capture taken while a prompt is open waits, checking.
 while(!stopping_&&checkingId_.isEmpty()&&!checkQueue_.isEmpty()){
  auto job=checkQueue_.dequeue();
  if(job.cancelled->load()){job.payload={};job.op.cancelled=true;job.op.advance(OperationState::Cancelled);finish(job.op);continue;}
  runCheck(std::move(job));
 }
}
void OperationService::runCheck(Job job) {
 const auto id=job.op.id;checkingId_=id;checkOperations_.insert(id,job.op);
 // #2079: Open in editor offers the Encrypt toggle too, until the checked image uploads.
 {std::lock_guard lock(encryptHooksMutex_);encryptOpen_.insert(id);}
 auto session=std::make_shared<EditorSession>();checkSessions_.insert(id,session);
 const bool prompt=sensitiveModes_.value(id)=="prompt";
 QPointer<OperationService> owner(this);
 session->setPhaseHandler([owner,id](const QString& phase,const QJsonObject& findings){
  QMetaObject::invokeMethod(QCoreApplication::instance(),[owner,id,phase,findings]{if(owner&&!owner->stopping_)owner->checkPhase(id,phase,findings);},Qt::QueuedConnection);
 });
 // After Open in editor the helper is an ordinary editor: the same copy paths as Edit & upload.
 session->setCopyHandler([owner,id](QByteArray png,std::function<void(Error)> done){
  QMetaObject::invokeMethod(QCoreApplication::instance(),[owner,id,png=std::move(png),done=std::move(done)]() mutable {
   if(!owner||owner->stopping_){done({"editor_closed","The editor closed before the image was copied",{}});return;}
   owner->copyEditedImage(id,std::move(png),std::move(done));
  },Qt::QueuedConnection);
 });
 session->setTextCopyHandler([owner,id](QString text,std::function<void(Error)> done){
  QMetaObject::invokeMethod(QCoreApplication::instance(),[owner,id,text=std::move(text),done=std::move(done)]() mutable {
   if(!owner||owner->stopping_){done({"editor_closed","The editor closed before the text was copied",{}});return;}
   owner->copyEditedText(id,std::move(text),std::move(done));
  },Qt::QueuedConnection);
 });
 session->setAppearance(editorAppearance_);
 const auto& op=job.op;
 const auto staging=root_+"/editing",destination=editorDestination(op),appearance=editorAppearance_;
 const bool links=uploadReviewOptions(op,providers_)["links"].toBool(false);
 const auto recentColors=settings_.snapshot().editorRecentColors;const auto languages=op.settings.textRecognitionLanguages;const bool allCores=op.settings.unhingedMode;
 const auto terms=op.settings.quickRedactTerms,patterns=op.settings.quickRedactPatterns,categories=op.settings.sensitiveTextCategories,cover=op.settings.sensitiveTextCover;
 const auto captureStyle=op.settings.captureStyle;const auto styleLimit=std::min(op.settings.limits.imagePixels,op.settings.limits.imageBytes/4);
 const auto coveredRoot=root_+"/covered/"+id;
 // #2079: Open in editor offers the Encrypt toggle beside Upload, as Capture region & edit does.
 const auto encryptNow=encryptOptions(op);
 const QJsonObject encryptStart=routeOf(op).destination=="upload"&&!op.destination.profileId.isEmpty()?QJsonObject{{"available",encryptNow["available"]},{"on",encryptNow["on"]},{"reason",encryptNow["reason"]}}:QJsonObject{};
 session->setEncryptHandler([owner=QPointer<OperationService>(this),id](bool on){
  QMetaObject::invokeMethod(QCoreApplication::instance(),[owner,id,on]{if(owner&&!owner->stopping_)owner->recordEncrypt(id,on);},Qt::QueuedConnection);
 });
 const auto outputBytes=std::min<qint64>(op.settings.limits.maxFileBytes,qint64(512)*1024*1024);
 const auto inputBytes=std::min<qint64>(op.settings.limits.maxFileBytes,256*1024*1024);
 logging::info(logOperation(),"Sensitive text check started",{{"operationId",id},{"action",op.action},{"mode",prompt?"prompt":"report"}});
 auto shared=std::make_shared<Job>(std::move(job));auto outcome=std::make_shared<EditorResult>();checkJobs_.insert(id,shared);
 auto clock=std::make_shared<QElapsedTimer>();clock->start();
 background([=](Error&){
  // #1893: Open in editor (the sensitive prompt) of a capture opens with its capture style on; a selected file's starts plain.
  const auto make=[&](EditorJob::Check check){return EditorJob(staging).withRecentColors(recentColors).withAppearance(appearance).withTextLanguages(languages).withAllCores(allCores).withRedactList(terms,patterns).withLinks(links).withEncrypt(encryptStart).withStyle(captureStyle,captureStyle.enabled&&QStringList{"region","window","monitor","desktop","scroll"}.contains(shared->op.action),styleLimit).withCheck(std::move(check));};
  if(shared->payload.kind=="image"){
   // The original stays here for Upload anyway; the helper reads a staged copy.
   QImage copy=shared->payload.image;*outcome=make({categories,prompt,true,{},cover}).run(copy,destination,outputBytes,session);
   return QJsonObject{};
  }
  if(shared->payload.kind=="png"){*outcome=make({categories,prompt,true,{},cover}).runEncoded(shared->payload.bytes,destination,outputBytes,session);return QJsonObject{};}
  // Each image with its place in the selection: a file chosen twice is answered, and covered, once per place (#1898).
  QStringList images;QList<int> places;
  for(int place=0;place<shared->payload.files.size();++place)if(checkableImage(shared->payload.files[place])){images.append(shared->payload.files[place]);places.append(place);}
  if(images.isEmpty()){outcome->check="clean";return QJsonObject{};}
  // The editor edits one image: Open in editor is offered only for a selection of one; Redact and upload for every file.
  const bool single=shared->payload.files.size()==1&&images.size()==1;
  for(qsizetype index=0;index<images.size();++index){
   if(session->cancelled()){outcome->error={"cancelled","Capture cancelled",{}};break;}
   EditorJob::Check check{categories,prompt,single,{},cover};
   if(!single)check.file=QStringLiteral("File %1 of %2: %3").arg(index+1).arg(images.size()).arg(QFileInfo(images[index]).fileName());
   // A file chosen twice that changed between its checks matches neither: its uncovered place fails as changed (#1898).
   const auto identity=fileIdentity(images[index]);
   {const auto known=shared->checkedFiles.constFind(images[index]);
    shared->checkedFiles.insert(images[index],known!=shared->checkedFiles.cend()&&*known!=identity?QStringLiteral("-"):identity);}
   *outcome=make(check).runFile(images[index],inputBytes,destination,outputBytes,session);
   // #1820: a file covered automatically uploads as its covered PNG, staged privately in its place; the selection goes on.
   if(!outcome->png.isEmpty()&&!outcome->covered.isEmpty()){
    const auto folder=QStringLiteral("%1/%2").arg(coveredRoot).arg(index);
    // Within the 255-byte name limit after ".png" is added, cut at a whole character (#1898).
    auto base=QFileInfo(images[index]).completeBaseName();
    while(base.toUtf8().size()>200){base.chop(1);if(!base.isEmpty()&&base.back().isHighSurrogate())base.chop(1);}
    const auto name=base+QStringLiteral(".png");
    QFile staged(folder+"/"+name);
    if(!QDir().mkpath(folder)||!QFile::setPermissions(coveredRoot,QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner)||!staged.open(QIODevice::WriteOnly|QIODevice::NewOnly)||staged.write(outcome->png)!=outcome->png.size()||!staged.flush()){
     staged.close();QFile::remove(staged.fileName());
     outcome->png.clear();outcome->error={"image_job_io","Couldn't save the redacted image for upload. Check free disk space and try again.",{}};break;
    }
    staged.close();
    for(auto it=outcome->covered.begin();it!=outcome->covered.end();++it)shared->covered[it.key()]=shared->covered[it.key()].toInt()+it.value().toInt();
    const auto place=QString::number(places[index]);
    shared->replacedFiles.insert(place,staged.fileName());shared->originalIdentities.insert(place,identity);
    outcome->png.clear();outcome->covered={};outcome->check="upload";
    continue;
   }
   // Upload anyway goes on to check the files after this one; anything else ends the selection.
   if(outcome->check!="clean"&&outcome->check!="upload")break;
  }
  return QJsonObject{};
 },[owner,id,shared,outcome,clock](Error,QJsonObject){if(owner)owner->checkFinished(id,shared,outcome,clock->elapsed());});
}
void OperationService::checkPhase(const QString& id,const QString& phase,const QJsonObject& findings) {
 if(!checkOperations_.contains(id))return;
 auto& op=checkOperations_[id];
 if(phase=="prompt"){
  // Each prompt of a file selection is numbered, so a decision can name the one it answers.
  const int prompt=op.sensitive["prompt"].toInt()+1;
  op.phase="sensitive_text_found";op.sensitive=findings;op.sensitive["prompt"]=prompt;
  if(!findings["complete"].toBool(true))unfinishedChecks_.insert(id);
  auto fields=sensitiveLog(findings);fields["operationId"]=id;
  logging::info(logOperation(),"Sensitive text check needs a decision",fields);
  observe(op.publicJson());emit changed(id);emit observationsChanged();
  if(sensitiveModes_.value(id)=="prompt")emit sensitivePromptShown(id);
  return;
 }
 if(phase=="checking"){
  // A decision was taken: until the next prompt of a file selection, if any, nothing waits on one.
  if(op.phase=="sensitive_text_found"){op.phase="checking";observe(op.publicJson());emit changed(id);emit observationsChanged();}
  return;
 }
 if(phase=="editing"){
  // Open in editor: an edit now, outside the one-at-a-time check slot.
  op.advance(OperationState::Editing);op.phase="editing";
  logging::info(logOperation(),"Sensitive text check opened the editor",{{"operationId",id}});
  // Only the edited PNG can upload now: let the original go. The worker is inside the editor
  // and reads the payload no more. The journal keeps its acquiring row, which recovery ends.
  if(const auto job=checkJobs_.value(id);job&&job->payload.kind!="files"){job->payload.image=QImage();job->payload.bytes.clear();}
  observe(op.publicJson());emit changed(id);emit observationsChanged();
  if(checkingId_==id){checkingId_.clear();pumpChecks();}
 }
}
void OperationService::checkFinished(const QString& id,std::shared_ptr<Job> shared,std::shared_ptr<EditorResult> outcome,qint64 elapsedMs) {
 auto job=std::move(*shared);
 if(checkOperations_.contains(id))job.op=checkOperations_.take(id);
 checkSessions_.remove(id);checkJobs_.remove(id);
 if(stopping_)return;
 if(outcome->recentColors)rememberEditorColors(*outcome->recentColors);
 auto& op=job.op;
 {
  auto fields=sensitiveLog(op.sensitive);fields["operationId"]=id;fields["durationMs"]=double(elapsedMs);
  fields["outcome"]=!outcome->png.isEmpty()?QStringLiteral("redacted"):outcome->error?QStringLiteral("failed"):outcome->check;
  if(outcome->error)fields["error"]=logging::errorFields(outcome->error);
  logging::info(logOperation(),"Sensitive text check ended",fields);
 }
 const auto next=[this,id]{if(checkingId_==id)checkingId_.clear();pumpChecks();};
 const bool unfinished=unfinishedChecks_.remove(id);
 // A prompt's covered counts stand only where those covers upload (set again below): never after Open in
 // editor's edit, a Cancel or a failure.
 op.sensitive.remove("covered");
 // #1820: what the check covered on its own, summed over a selection; reported only for a check that finished.
 const auto recordCovered=[&op,unfinished](const QJsonObject& covered){
  if(covered.isEmpty())return;
  if(op.sensitive.isEmpty())op.sensitive={{"found",QJsonObject{}},{"complete",true}};
  op.sensitive.remove("prompt");op.sensitive["covered"]=covered;if(unfinished)op.sensitive["complete"]=false;
  auto fields=sensitiveLog(op.sensitive);fields["operationId"]=op.id;for(auto it=covered.begin();it!=covered.end();++it)fields["covered_"+it.key()]=it.value();
  logging::info(logOperation(),"Sensitive text covered automatically",fields);
 };
 // Covered copies of selected files that will not upload now.
 const auto dropCovered=[&job]{discardCovered(job.replacedFiles);job.replacedFiles.clear();};
 const auto cancelled=job.cancelled->load()||outcome->error.code=="cancelled";
 if(cancelled){dropCovered();job.payload={};op.cancelled=true;op.advance(OperationState::Cancelled);finish(op);next();return;}
 if(!outcome->png.isEmpty()){
  // Open in editor: only the editor's sanitized PNG goes on, as for Edit & upload.
  // #1820: or the check's own covered PNG, with no prompt or after Upload anyway, or (#1898) after Redact and
  // upload with every suggestion covered; the original then stays in memory until it is saved locally, with
  // Keep a local copy on.
  ClipboardPayload finalized;finalized.kind="png";finalized.bytes=std::move(outcome->png);finalized.mime="image/png";
  // #1893: Open in editor's edit drew the style the user chose; the check's own covers get it below.
  // An editor opened only after Open in editor, which moved the operation to editing.
  const bool fromEditor=op.state()==OperationState::Editing;
  ClipboardPayload original;
  if(!outcome->covered.isEmpty()){recordCovered(outcome->covered);if(op.settings.archive.keepUploads)original=std::move(job.payload);}
  job.payload={};
  // The original is written out as a private PNG with the journal row, off this thread, and its raster freed:
  // nothing uncounted waits in memory behind other uploads.
  const auto originalPath=original.kind.isEmpty()?QString():root_+"/covered/"+id+"-original.png";
  op.advance(OperationState::Acquiring);op.phase="queued";
  const auto path=databasePath();auto flag=job.cancelled;
  background([path,op,original=std::move(original),originalPath,root=root_](Error& e) mutable {
   HistoryStore store(path);e=store.saveOperation(op);
   QJsonObject written;
   if(!originalPath.isEmpty()){
    QByteArray png=original.kind=="png"?std::move(original.bytes):QByteArray();
    if(original.kind=="image"){QBuffer buffer(&png);buffer.open(QIODevice::WriteOnly);if(!original.image.save(&buffer,"PNG"))png.clear();}
    original={};
    // The same reserve of free disk as any import: the PNG is written only where it fits.
    const auto reserved=png.isEmpty()?nullptr:ArtifactStore::reserveDisk(root,png.size());
    QFile file(originalPath);bool ok=false;
    if(reserved&&QDir().mkpath(QFileInfo(originalPath).absolutePath())&&file.open(QIODevice::WriteOnly|QIODevice::Truncate)&&file.setPermissions(QFile::ReadOwner|QFile::WriteOwner)&&file.write(png)==png.size()&&file.flush())ok=true;
    file.close();if(!ok)QFile::remove(originalPath);
    written["original"]=ok?originalPath:QStringLiteral("-");
   }
   return written;
  },[this,op,flag,finalized=std::move(finalized),fromEditor](Error e,QJsonObject written) mutable {
   if(e){if(const auto kept=written["original"].toString();!kept.isEmpty()&&kept!="-")QFile::remove(kept);op.error=e;op.advance(OperationState::Failed);finish(op);return;}
   Job covered{op,std::move(finalized),true,flag,{}};covered.originalPath=written["original"].toString();covered.fromEditor=fromEditor;
   enqueue(std::move(covered));
  });
  next();return;
 }
 if(outcome->error){
  dropCovered();
  // The helper could not check (it is missing, failed or gave no answer in time): nothing is uploaded.
  // With Keep a local copy on, the capture is still kept, as Cancel keeps it.
  if(op.settings.archive.keepUploads&&op.state()!=OperationState::Editing&&(!job.payload.image.isNull()||!job.payload.bytes.isEmpty()||!job.payload.files.isEmpty())){
   logging::warning(logOperation(),"Sensitive text check failed; saving the capture locally only",{{"operationId",id},{"error",logging::errorFields(outcome->error)}});
   // Added to a note the capture already has (a scrolling capture's), never in place of it.
   const auto why=QStringLiteral("Couldn't check for sensitive text, so the upload didn't start.");op.note=op.note.isEmpty()?why:op.note+" "+why;
   job.localOnly=true;op.destination={};enqueue(std::move(job));next();return;
  }
  job.payload={};
  op.error={"sensitive_check_failed",QStringLiteral("Couldn't check for sensitive text, so the upload didn't start. %1").arg(outcome->error.message),{{"cause",outcome->error.code}}};
  op.advance(OperationState::Failed);finish(op);next();return;
 }
 if(outcome->check=="clean"||outcome->check=="upload"){
  // #1820: a selection's covered files upload in their places; the originals are saved locally with Keep a local copy on.
  if(!job.replacedFiles.isEmpty()){
   recordCovered(job.covered);
   // Replacements are keyed by place in the selection. A file covered at every place it was chosen uploads only
   // covered copies, so it is not compared again; one also chosen uncovered still is.
   QSet<QString> uncovered,coveredPaths;
   for(int index=0;index<job.payload.files.size();++index){
    const auto& file=job.payload.files[index];const auto place=QString::number(index);
    if(!job.replacedFiles.contains(place)){uncovered.insert(file);continue;}
    coveredPaths.insert(file);
    if(op.settings.archive.keepUploads)job.originalFiles.append({index,file,job.originalIdentities.value(place)});
   }
   for(const auto& file:std::as_const(coveredPaths))if(!uncovered.contains(file))job.checkedFiles.remove(file);
   for(int index=0;index<job.payload.files.size();++index)job.payload.files[index]=job.replacedFiles.value(QString::number(index),job.payload.files[index]);
  }
  enqueue(std::move(job));next();return;
 }
 dropCovered();
 // Cancel, or an agent's decision that never came: nothing is uploaded; the local copy follows Keep a local copy.
 if(op.settings.archive.keepUploads){job.localOnly=true;op.destination={};enqueue(std::move(job));next();return;}
 job.payload={};op.cancelled=true;op.advance(OperationState::Cancelled);finish(op);next();
}
// #1893: region, window, monitor, desktop, Repeat last region (a region capture) and scrolling captures,
// newly acquired, with Settings → Capture → Capture style on. Never an edit, a file, the clipboard or a retry.
bool OperationService::needsStyle(const Job& job) {
 const auto& op=job.op;
 if(!op.settings.captureStyle.enabled||!job.acquire||job.sharingOnly||job.fromEditor)return false;
 static const QStringList actions{"region","window","monitor","desktop","scroll"};
 if(!actions.contains(op.action))return false;
 return (job.payload.kind=="image"&&!job.payload.image.isNull())||(job.payload.kind=="png"&&!job.payload.bytes.isEmpty());
}
void OperationService::styleCapture(Job& job,Operation& op) {
 // The same PNG the artifact store would encode; the image-job child decodes, styles and encodes it.
 QByteArray png;
 if(job.payload.kind=="png")png=job.payload.bytes;
 else{QBuffer buffer(&png);buffer.open(QIODevice::WriteOnly);if(!job.payload.image.save(&buffer,"PNG"))png.clear();}
 if(png.isEmpty())return; // the store reports the encoding failure as before
 QElapsedTimer clock;clock.start();
 Error error;QString note;auto styled=ImageJob().style(png,op.settings.captureStyle,op.settings.limits,error,&note,job.cancelled);
 if(error){
  if(error.code=="cancelled")return;
  // A style problem never fails or blocks a capture: it goes out as captured, with a note.
  const auto why=error.code=="input_excessive"?QStringLiteral("Saved without capture style because the styled image exceeds your image limits."):QStringLiteral("Couldn't apply capture style, so the unstyled capture was kept.");
  op.note=op.note.isEmpty()?why:op.note+" "+why;
  logging::warning(logOperation(),"Capture style was not applied",{{"operationId",op.id},{"error",logging::errorFields(error)}});
  return;
 }
 // The capture as it was, for run() to store instead if the store refuses the larger styled PNG.
 job.unstyled=png;
 // With Keep a local copy on, the capture as it was is kept beside the styled copy, as for automatic covers.
 // A covered capture already keeps its original from before covering.
 if(op.settings.archive.keepUploads&&job.originalPath.isEmpty()){
  const auto originalPath=root_+"/styled/"+op.id+"-original.png";
  const auto reserved=ArtifactStore::reserveDisk(root_,png.size());
  QFile file(originalPath);bool ok=false;
  if(reserved&&QDir().mkpath(QFileInfo(originalPath).absolutePath())&&QFile::setPermissions(QFileInfo(originalPath).absolutePath(),QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner)&&file.open(QIODevice::WriteOnly|QIODevice::Truncate)&&file.setPermissions(QFile::ReadOwner|QFile::WriteOwner)&&file.write(png)==png.size()&&file.flush())ok=true;
  file.close();if(!ok)QFile::remove(originalPath);
  job.originalPath=ok?originalPath:QStringLiteral("-");job.styleOriginal=true;
 }
 job.payload={};job.payloadLease.reset();
 job.payload.kind="png";job.payload.bytes=std::move(styled);job.payload.mime="image/png";
 // Recorded on the operation only once the styled PNG is stored (run()).
 job.style={{"applied",true}};
 if(note=="picture_missing")job.style["pictureMissing"]=true;
 logging::info(logOperation(),"Capture style applied",{{"operationId",op.id},{"durationMs",double(clock.elapsed())},{"bytes",double(job.payload.bytes.size())}});
}
void OperationService::keepOriginals(Job& job,Operation& op) {
 // Straight to the archive, outside the artifact store: never uploaded, never counted as a waiting upload.
 QJsonArray paths;int failed=0;
 const auto keep=[&](Artifact artifact,const QString& path,const QString& name){
  const QFileInfo named(name);artifact.path=path;artifact.name=named.completeBaseName()+QStringLiteral("-original")+(named.suffix().isEmpty()?QString():"."+named.suffix());
  const auto copy=LocalArchive::save(artifact,op.settings.archive);if(copy.status=="saved")paths.append(copy.path);else ++failed;
 };
 if(!job.originalPath.isEmpty()){
  // A capture: its PNG, written privately when the check ended; removed once copied.
  if(job.originalPath!="-"&&!op.items.isEmpty())keep(op.items.first().artifact,job.originalPath,op.items.first().artifact.name);else ++failed;
  if(job.originalPath!="-")QFile::remove(job.originalPath);
  job.originalPath.clear();
 }
 // A selected file: the user's own file, only while it is still the one that was checked.
 for(const auto& [index,path,identity]:std::as_const(job.originalFiles)){
  if(index>=op.items.size()||identity.isEmpty()||fileIdentity(path)!=identity){++failed;continue;}
  keep(op.items[index].artifact,path,QFileInfo(path).fileName());
 }
 job.originalFiles.clear();
 const QJsonObject original{{"status",failed?"failed":"saved"},{"paths",paths}};
 if(job.styleOriginal)op.style["original"]=original;else op.sensitive["original"]=original;
 if(failed)logging::warning(logOperation(),"The original of a covered upload was not saved locally",{{"operationId",op.id},{"saved",paths.size()},{"failed",failed}});
}
void OperationService::discardCovered(const QHash<QString,QString>& replaced) {
 // Each covered copy sits alone in covered/<operation>/<index>/: the file, then its two folders if empty.
 for(const auto& path:replaced){QFile::remove(path);const QFileInfo file(path);QDir().rmdir(file.absolutePath());QDir().rmdir(QFileInfo(file.absolutePath()).absolutePath());}
}
bool OperationService::sensitivePromptWaiting(const QString& id) const {return sensitivePromptNumber(id)>0;}
int OperationService::sensitivePromptNumber(const QString& id) const {
 const auto session=checkSessions_.value(id);
 if(!session||sensitiveModes_.value(id)!="prompt")return -1;
 return session->waitingPrompt();
}
bool OperationService::raiseSensitivePrompt(const QString& activationToken) {
 for(auto it=checkOperations_.cbegin();it!=checkOperations_.cend();++it)if(it->phase=="sensitive_text_found"&&raiseSensitivePrompt(it.key(),activationToken))return true;
 return false;
}
bool OperationService::raiseSensitivePrompt(const QString& id,const QString& activationToken) {
 const auto session=checkSessions_.value(id);
 if(!session||session->phase()!="prompt"||sensitiveModes_.value(id)!="prompt")return false;
 session->request("raisePrompt",activationToken.isEmpty()?QJsonObject{}:QJsonObject{{"activationToken",activationToken}},[](Error,QJsonObject){});
 return true;
}
void OperationService::decideSensitive(const QString& id,const QString& choice,JsonReply reply,int prompt) {
 if(!QStringList{"redact_upload","edit","upload","cancel"}.contains(choice)){reply({"invalid_request","choice is redact_upload, edit, upload or cancel",{}},{});return;}
 const auto session=checkSessions_.value(id);
 const bool queued=std::any_of(checkQueue_.cbegin(),checkQueue_.cend(),[&](const Job& job){return job.op.id==id;});
 if(queued||(session&&session->phase()=="checking")||(session&&session->phase()=="starting")){reply({"sensitive_check_running","The capture is still being checked for sensitive text",{{"operationId",id}}},{});return;}
 if(!session||session->phase()!="prompt"){reply({"sensitive_decision_unavailable","This operation is not waiting for a sensitive text decision",{{"operationId",id}}},{});return;}
 QPointer<OperationService> owner(this);
 // The session checks the prompt number under the lock that queues the decision, so it can't reach the next file's prompt.
 session->decide(choice,prompt,[owner,id,reply](Error error,QJsonObject result){
  QMetaObject::invokeMethod(QCoreApplication::instance(),[owner,id,reply,error,result]() mutable {
   if(!owner||owner->stopping_)return;
   if(!error)result["operationId"]=id;
   reply(error,result);
  },Qt::QueuedConnection);
 });
}
void OperationService::enqueue(Job job) {
 // #2079: the Encrypt toggle's choice joins the operation before anything of it is stored, and the
 // toggle closes: from here on the upload has decided, and a later choice is refused.
 {std::lock_guard lock(encryptHooksMutex_);if(const auto choice=encryptChoices_.constFind(job.op.id);choice!=encryptChoices_.cend())job.op.encrypt["requested"]=*choice;encryptChoices_.remove(job.op.id);encryptOpen_.remove(job.op.id);}
 if(const auto passphrase=checkPassphrases_.take(job.op.id);job.encryptPassphrase.isEmpty())job.encryptPassphrase=passphrase;
 // The Encrypt choice is final now: an upload that waited for it gets its destination warm-up.
 if(job.acquire&&!job.localOnly&&!anticipated_.contains(job.op.id)&&!job.op.destination.providerId.isEmpty())anticipate(job.op,true);
#ifdef __GLIBC__
 if(job.acquire)heapReclamationPending_=true;
#endif
 // #1820: a job dropped here never reaches run(), so its staged covered copies and original go with it.
 if(job.cancelled->load()){discardCovered(job.replacedFiles);if(!job.originalPath.isEmpty()&&job.originalPath!="-")QFile::remove(job.originalPath);job.payload={};job.op.cancelled=true;job.op.advance(OperationState::Cancelled);finish(job.op);return;}
 if(job.payload.kind=="image"){
  const auto bytes=job.payload.image.sizeInBytes();
  const auto used=payloadBytes_->load();
  if(bytes>job.op.settings.limits.pendingImageBytes-used){
   job.payload={};job.op.error={"image_memory_limit","Memory for queued images is full. Wait for current images to finish, or raise Memory for queued images in Settings → Advanced → Resource limits.",{}};job.op.advance(OperationState::Failed);finish(job.op);return;
  }
  payloadBytes_->fetch_add(bytes);
  job.payloadLease=std::shared_ptr<qint64>(new qint64(bytes),[counter=payloadBytes_](qint64* value){counter->fetch_sub(*value);delete value;});
 }
 job.op.phase="queued";observe(job.op.publicJson());queue_.enqueue(std::move(job));pump();
}
QString OperationService::editorDestination(const Operation& op) const {
 return uploadReviewOptions(op,providers_)["summary"].toString();
}
// The acquired source stays private to the editor stage. Only the editor's
// validated PNG becomes an artifact, preview, archive or publication input.
void OperationService::startEditing(Operation op,std::shared_ptr<std::atomic_bool> flag,ClipboardPayload payload) {
 auto session=editor_;
 if(!session||editingId_!=op.id||flag->load()){op.cancelled=true;op.advance(OperationState::Cancelled);finish(op);return;}
 if(!(payload.kind=="image"||(payload.kind=="files"&&payload.files.size()==1))){op.error={"edit_source_unsupported","The editor needs exactly one static PNG or JPEG image",{}};op.advance(OperationState::Failed);finish(op);return;}
 op.advance(OperationState::Editing);op.phase="editing";
 const auto path=databasePath(),staging=root_+"/editing",destination=editorDestination(op);
 const bool links=uploadReviewOptions(op,providers_)["links"].toBool(false);
 const auto route=routeOf(op);const Error submissionError=(route.destination!="upload"||route.profileId.isEmpty())&&!op.settings.archive.keepUploads?Error{"output_required","No output is enabled. Choose a destination or turn on Keep a local copy in Settings → Capture.",{}}:Error{};
 const auto recentColors=settings_.snapshot().editorRecentColors;const auto appearance=editorAppearance_;const auto textLanguages=op.settings.textRecognitionLanguages;const bool allCores=op.settings.unhingedMode;const auto redactTerms=op.settings.quickRedactTerms,redactPatterns=op.settings.quickRedactPatterns;
 const auto outputBytes=std::min<qint64>(op.settings.limits.maxFileBytes,qint64(512)*1024*1024);
 const auto style=op.settings.captureStyle;const bool styleStart=styledEdits_.remove(op.id);const auto styleLimit=std::min(op.settings.limits.imagePixels,op.settings.limits.imageBytes/4);
 auto image=std::make_shared<QImage>(payload.kind=="image"?std::move(payload.image):QImage());
 const auto file=payload.kind=="files"?payload.files.first():QString();payload={};
 auto edited=std::make_shared<EditorResult>();
 // Copy image: the editor's checked PNG goes to the clipboard through the
 // native adapter as an explicit copy. It is not an artifact, a history item
 // or an upload input, and nothing of it is kept here after delivery.
 session->setCopyHandler([owner=QPointer<OperationService>(this),id=op.id](QByteArray png,std::function<void(Error)> done){
  QMetaObject::invokeMethod(QCoreApplication::instance(),[owner,id,png=std::move(png),done=std::move(done)]() mutable {
   if(!owner||owner->stopping_){done({"editor_closed","The editor closed before the image was copied",{}});return;}
   owner->copyEditedImage(id,std::move(png),std::move(done));
  },Qt::QueuedConnection);
 });
 // #2079: the Encrypt toggle beside Upload, starting from the destination's setting; each change is this upload's choice.
 const auto encryptOptionsNow=encryptOptions(op);
 const QJsonObject encryptStart=routeOf(op).destination=="upload"&&!op.destination.profileId.isEmpty()?QJsonObject{{"available",encryptOptionsNow["available"]},{"on",encryptOptionsNow["on"]},{"reason",encryptOptionsNow["reason"]}}:QJsonObject{};
 session->setEncryptHandler([owner=QPointer<OperationService>(this),id=op.id](bool on){
  QMetaObject::invokeMethod(QCoreApplication::instance(),[owner,id,on]{if(owner&&!owner->stopping_)owner->recordEncrypt(id,on);},Qt::QueuedConnection);
 });
 session->setTextCopyHandler([owner=QPointer<OperationService>(this),id=op.id](QString text,std::function<void(Error)> done){
  QMetaObject::invokeMethod(QCoreApplication::instance(),[owner,id,text=std::move(text),done=std::move(done)]() mutable {
   if(!owner||owner->stopping_){done({"editor_closed","The editor closed before the text was copied",{}});return;}
   owner->copyEditedText(id,std::move(text),std::move(done));
  },Qt::QueuedConnection);
 });
 background([path,op](Error& e){HistoryStore store(path);e=store.saveOperation(op);return QJsonObject{};},[=,this](Error e,QJsonObject) mutable {
  if(e){op.error=e;op.advance(OperationState::Failed);finish(op);return;}
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
    *edited=EditorJob(staging).withRecentColors(recentColors).withAppearance(appearance).withTextLanguages(textLanguages).withAllCores(allCores).withRedactList(redactTerms,redactPatterns).withSubmissionError(submissionError).withLinks(links).withEncrypt(encryptStart).withStyle(style,styleStart,styleLimit).runFile(file,maximum,destination,outputBytes,session);
    if(edited->error.code=="source_unsupported")edited->error={"edit_source_unsupported","The editor opens static PNG or JPEG files",{}};
    return QJsonObject{};
   }
   *edited=EditorJob(staging).withRecentColors(recentColors).withAppearance(appearance).withTextLanguages(textLanguages).withAllCores(allCores).withRedactList(redactTerms,redactPatterns).withSubmissionError(submissionError).withLinks(links).withEncrypt(encryptStart).withStyle(style,styleStart,styleLimit).run(*image,destination,outputBytes,session);
   return QJsonObject{};
  },[=,this](Error,QJsonObject) mutable {
   if(editingId_==op.id){editor_.reset();editingId_.clear();}
   if(edited->recentColors)rememberEditorColors(*edited->recentColors);
   if(flag->load()||edited->error.code=="cancelled"){op.cancelled=true;op.advance(OperationState::Cancelled);op.error=flag->load()?Error{}:edited->error;finish(op);return;}
   if(edited->error){op.error=edited->error;op.advance(OperationState::Failed);finish(op);return;}
   ClipboardPayload finalized;finalized.kind="png";finalized.bytes=std::move(edited->png);finalized.mime="image/png";
   op.advance(OperationState::Acquiring);op.phase="queued";
   // The editor is gone, but the sanitized bytes are not an artifact yet.
   // Persist the handoff before queueing: readers and crash recovery must no
   // longer treat this operation as an open, unsubmitted editor.
   background([path,op](Error& e){HistoryStore store(path);e=store.saveOperation(op);return QJsonObject{};},
    [this,op,flag,finalized=std::move(finalized)](Error e,QJsonObject) mutable {
     if(e){op.error=e;op.advance(OperationState::Failed);finish(op);return;}
     enqueue({op,std::move(finalized),true,flag,{}});
    });
  });
 });
}
void OperationService::copyEditedText(const QString& id,QString text,std::function<void(Error)> done) {
 if(editingId_!=id&&!checkSessions_.contains(id)){done({"editor_closed","The editor closed before the text was copied",{}});return;}
 // An explicit copy from the moment it is asked for: a later explicit copy, or
 // the user changing the clipboard before it lands, suppresses it.
 const auto ticket=invalidateCopyEligibility();const auto revision=source_.clipboardRevision();
 CopyPayload payload;payload.kind="text";payload.text=std::move(text);
 // Never the text itself in a log: only that one was copied.
 deliverCopy(payload,[id,done=std::move(done)](Error error){if(!error)logging::info(logOperation(),"Recognized text copied",{{"operationId",id}});done(error);},[this,ticket,revision]{return ticket==invocation_&&revision==source_.clipboardRevision();});
}
void OperationService::copyEditedImage(const QString& id,QByteArray png,std::function<void(Error)> done) {
 if(editingId_!=id&&!checkSessions_.contains(id)){done({"editor_closed","The editor closed before the image was copied",{}});return;}
 // An explicit copy supersedes pending automatic link copies, like history.copyImage.
 const auto ticket=invalidateCopyEligibility();const auto bytes=png.size();
#ifdef __GLIBC__
 heapReclamationPending_=true;
#endif
 CopyPayload payload;payload.kind="image";payload.encodedImage=std::move(png);
 deliverCopy(payload,[id,bytes,done=std::move(done)](Error error){
  if(!error)logging::info(logOperation(),"Edited image copied",{{"operationId",id},{"bytes",double(bytes)}});
  done(error);
 },[this,ticket]{return ticket==invocation_;});
}
// Custom colors picked in the editor persist as a settings preference. A
// conflicting concurrent save simply loses this update; colors are a convenience.
void OperationService::rememberEditorColors(const QStringList& colors) {
 if(colors==settings_.snapshot().editorRecentColors||!settings_.metadata()["validation"].toObject()["ok"].toBool())return;
 settings_.patchAsync({{"preferences",QJsonObject{{"editorRecentColors",QJsonArray::fromStringList(colors)}}}},settings_.revision(),[](Result){});
}
void OperationService::setCaptureDelayPreference(int seconds) {
 if(!Settings::captureDelayChoices().contains(seconds)||seconds==settings_.snapshot().captureDelaySeconds||!settings_.metadata()["validation"].toObject()["ok"].toBool())return;
 settings_.patchAsync({{"preferences",QJsonObject{{"captureDelaySeconds",seconds}}}},settings_.revision(),[](Result){});
}
// Recording shares the editor's exclusivity: one interactive private review at a time.
Error OperationService::acceptScroll(Operation& op) {
 if(recording_->active())return {"recording_active","A recording is already running. Stop or cancel it first.",{}};
 const auto action=op.action;
 auto e=acceptRecording(op);if(e)return e;
 op.action=action;const auto route=op.settings.actions.value(action);op.destination={};
 if(route.destination=="upload"&&!route.profileId.isEmpty()){const auto profile=op.settings.profiles.value(route.profileId);op.destination={profile["provider"].toString(),route.profileId,settings_.revision(),profile["settings"].toObject(),profile["credentials"].toObject()};}
 return {};
}
Error OperationService::acceptText(Operation& op,const TextService::Acceptance& acceptance) {
 if(!ready_)return notReadyError();
 // An interactive source holds the one interactive-acquisition slot, as a capture does.
 if(acceptance.interactive&&scroll_&&scroll_->active())return {"scroll_active","A scrolling capture is already running. Stop or cancel it first.",{{"operationId",scroll_->activeId()}}};
 if(acceptance.interactive&&!selectingId_.isEmpty())return {"busy_selecting","Another capture selection or countdown is already in progress. Finish or cancel it first.",{}};
 // Every source counts against the pending-operation limit. No output is needed.
 const auto snapshot=settings_.snapshot();if(active_.size()>=snapshot.limits.pendingOperations+snapshot.limits.concurrentUploads)return {"queue_full","Too many actions are in progress. Wait for some to finish, or raise Queued actions in Settings → Advanced → Resource limits.",{}};
 op.id=uuid();op.action=Settings::textActionName();op.settings=snapshot;op.settingsRevision=settings_.revision();op.created=QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
 // An explicit copy: it takes the ticket and the expected clipboard revision now,
 // which also moves the upload-copy epoch past any pending link copy. With
 // copy false the clipboard and every pending copy are left alone.
 op.invocation=acceptance.copy?invalidateCopyEligibility():0;op.clipboardRevision=source_.clipboardRevision();op.destination={};
 active_.insert(op.id,std::make_shared<std::atomic_bool>(false));if(acceptance.interactive)selectingId_=op.id;
#ifdef __GLIBC__
 heapReclamationPending_=true;
#endif
 // Never the source, a path or anything recognized: only that a text operation began.
 logging::info(logOperation(),"Operation accepted",{{"operationId",op.id},{"action",op.action},{"output","none"}});
 return {};
}
void OperationService::copyRecognizedText(const Operation& op,const QString& text,std::function<void(Error)> done) {
 const auto ticket=op.invocation;const auto revision=op.clipboardRevision;
 // Suppressed when the user changed the clipboard after acceptance, or a later explicit copy took the ticket.
 const auto eligible=[this,ticket,revision]{return ticket!=0&&ticket==invocation_&&revision==source_.clipboardRevision();};
 if(!eligible()){done({"copy_suppressed","The clipboard changed before the text was copied",{}});return;}
 CopyPayload payload;payload.kind="text";payload.text=text;
 deliverCopy(payload,std::move(done),eligible);
}
void OperationService::finishText(Operation op) {
 op.phase=op.stateName();if(selectingId_==op.id&&!preparedOwners_.contains(op.id))selectingId_.clear();
 if(clipboardImports_.remove(op.id))pumpCopies();
 // A completed or cancelled text operation leaves no journal row; a failed one is
 // kept and listed. Nothing recognized is in either.
 const bool kept=op.state()==OperationState::Failed||op.state()==OperationState::Interrupted;const auto path=databasePath();
 background([path,op,kept](Error& e) {HistoryStore store(path);e=kept?store.saveOperation(op):store.removeTextOperation(op.id);return QJsonObject{};},[this,op](Error e,QJsonObject) {
  clearObservation(op.id);emit changed(op.id);emit observationsChanged();
  if(e)logging::error(logHistory(),"Cannot record the operation result",{{"operationId",op.id},{"error",logging::errorFields(e)}});
  QJsonObject fields{{"operationId",op.id},{"action",op.action},{"state",op.stateName()},{"copyStatus",op.copyStatus}};
  if(op.error)fields["error"]=logging::errorFields(op.error);
  logging::write(logOperation(),op.state()==OperationState::Completed||op.state()==OperationState::Cancelled?QtInfoMsg:QtWarningMsg,"Operation finished",fields);
  releaseActive(op.id);
 });
}
namespace {
Error textSourceUnsupported(){return {"source_unsupported","Text recognition reads an image, or one PNG or JPEG file",{}};}
// A PNG is decoded here by the built-in reader; a JPEG goes to the image worker.
QImage decodeTextImage(const QByteArray& bytes,const ResourceLimits& limits,Error& error) {
 if(imageFormatHint(bytes)=="png") {
  // Detection off: Qt 6.4.2 and 6.8.2 load every image plugin otherwise.
  auto copy=bytes;QBuffer buffer(&copy);buffer.open(QIODevice::ReadOnly);QImageReader reader(&buffer,"png");reader.setAutoDetectImageFormat(false);
  const auto size=reader.size();
  if(!size.isValid()){error=textSourceUnsupported();return {};}
  if(qint64(size.width())*size.height()>std::min<qint64>(limits.imagePixels,textjob::maximumInputPixels)||qint64(size.width())*size.height()>limits.imageBytes/4){error=textjob::failure("input_excessive");return {};}
  auto image=reader.read();if(image.isNull()){error=textSourceUnsupported();return {};}
  if(image.sizeInBytes()>limits.imageBytes){error=textjob::failure("input_excessive");return {};}
  return image;
 }
 if(!bytes.startsWith("\xff\xd8\xff")){error=textSourceUnsupported();return {};}
 auto bounded=limits;bounded.imagePixels=std::min<qint64>(limits.imagePixels,textjob::maximumInputPixels);
 return ImageJob().decode(bytes,bounded,error);
}
// One local regular file, opened once and never followed through a link.
QByteArray readTextImageFile(const QString& path,const ResourceLimits& limits,Error& error) {
 const Error unreadable{"source_unavailable","Cannot read this image file",{}};
#ifdef Q_OS_UNIX
 const int fd=::open(QFile::encodeName(path).constData(),O_RDONLY|O_CLOEXEC|O_NONBLOCK|O_NOFOLLOW);struct stat identity{};
 if(fd<0||::fstat(fd,&identity)||!S_ISREG(identity.st_mode)){if(fd>=0)::close(fd);error=unreadable;return {};}
 QFile file;if(!file.open(fd,QIODevice::ReadOnly,QFileDevice::AutoCloseHandle)){::close(fd);error=unreadable;return {};}
#else
 QFile file(path);if(!QFileInfo(file).isFile()||!file.open(QIODevice::ReadOnly)){error=unreadable;return {};}
#endif
 if(file.size()<1){error=textSourceUnsupported();return {};}
 if(file.size()>limits.imageBytes){error=textjob::failure("input_excessive");return {};}
 const auto bytes=file.read(limits.imageBytes+1);
 if(bytes.size()!=file.size()){error=unreadable;return {};}
 return bytes;
}
// What a source delivered, as pixels: a capture's image, or exactly one image file.
TextService::Load textPayloadLoader(ClipboardPayload delivered,ResourceLimits limits) {
 auto payload=std::make_shared<ClipboardPayload>(std::move(delivered));
 return [payload,limits](Error& error)->QImage {
  if(payload->kind=="image"&&!payload->image.isNull())return std::exchange(payload->image,{});
  if(!payload->bytes.isEmpty()&&payload->mime.startsWith("image/"))return decodeTextImage(std::exchange(payload->bytes,{}),limits,error);
  if(payload->kind=="files"&&payload->files.size()==1&&QDir::isAbsolutePath(payload->files.first())) {
   const auto bytes=readTextImageFile(payload->files.first(),limits,error);
   return error?QImage{}:decodeTextImage(bytes,limits,error);
  }
  error=textSourceUnsupported();return {};
 };
}
Error adapterSourceError(const QJsonObject& entry) {
 const auto reported=entry["error"];const auto object=reported.toObject();
 const auto code=entry["code"].toString(object["code"].toString("source_unavailable"));
 const auto message=reported.isString()?reported.toString():object["message"].toString();
 return {code,message.isEmpty()?QStringLiteral("This desktop cannot read this source"):message,{}};
}
}
QHash<QString,Error> OperationService::textSourceErrors() const {
 QHash<QString,Error> errors;const auto report=source_.capabilities();
 // An adapter that reports no capture table is asked at acquisition instead.
 if(report["capture"].isObject()) {
  const auto capture=report["capture"].toObject();
  // A region for text is the adapter's own `text` action; one that has not
  // learned it yet has no entry.
  const auto region=capture[Settings::textActionName()];
  if(!region.isObject())errors.insert("region",{"source_unsupported","Copy text from a region is not available on this desktop yet",{}});
  else if(!region.toObject()["available"].toBool())errors.insert("region",adapterSourceError(region.toObject()));
  for(const auto* source:{"window","monitor","desktop"}){const auto entry=capture[QLatin1String(source)].toObject();if(!entry["available"].toBool())errors.insert(QLatin1String(source),adapterSourceError(entry));}
 }
 if(report["clipboard"].isObject()&&!report["clipboard"].toObject()["available"].toBool())errors.insert("clipboard",adapterSourceError(report["clipboard"].toObject()));
 return errors;
}
void OperationService::recognizeText(const QJsonObject& params,JsonReply reply) {
 emit explicitRequest();
 const auto source=params["source"].toString();const bool screen=QStringList{"region","window","monitor","desktop"}.contains(source);
 if(screen&&scroll_->active()){reply({"scroll_active","A scrolling capture is already running. Stop or cancel it first.",{{"operationId",scroll_->activeId()}}},{});return;}
 // A source this desktop cannot read answers with its adapter's error; nothing else is read in its place.
 if(const auto unavailable=textSourceErrors();unavailable.contains(source)){reply(unavailable.value(source),{});return;}
 const auto snapshot=settings_.snapshot();const auto limits=snapshot.limits;const int delay=screen?params["delaySeconds"].toInt():0;
 TextService::Request request;request.source=source;request.copy=params["copy"].toBool();request.keepResult=params["keepResult"].toBool();request.interactive=screen;request.agent=params["agent"].toBool();
 for(const auto& language:params["languages"].toArray())request.languages.append(language.toString());
 QPointer<OperationService> owner(this);auto id=std::make_shared<QString>();
 // Hands what the adapter delivered to the service, on the session thread.
 const auto handOver=[owner,id,limits](TextService::Deliver deliver){
  return [owner,id,limits,deliver](PayloadResult result){
   if(!owner)return;
   QMetaObject::invokeMethod(owner.data(),[owner,id,limits,deliver,result=std::move(result)]() mutable {
    if(!owner||owner->stopping_)return;
    // The selection is over: the next interactive capture may start while this one is read.
    if(owner->selectingId_==*id&&!owner->preparedOwners_.contains(*id))owner->selectingId_.clear();
    if(owner->clipboardImports_.remove(*id))owner->pumpCopies();
    if(result.error)deliver(result.error,{});else deliver({},textPayloadLoader(std::move(result.payload),limits));
   },Qt::QueuedConnection);
  };
 };
 if(screen) {
  // A region is the adapter's `text` action: selected as a region is, never remembered for Repeat.
  const auto action=source=="region"?Settings::textActionName():source;const bool screenshotFirst=snapshot.regionSelectionMode=="screenshot_first";
  const QString outcome=QStringLiteral("Copies text");
  auto prepared=std::make_shared<SourceAdapter::PreparedAcquisition>();
  request.prepare=[this,owner,id,prepared,action,source,delay,screenshotFirst,outcome,limits](const Operation& op,TextService::Deliver deliver) {
   *id=op.id;
   const std::function<void(Error)> early=[deliver](Error error){if(error)deliver(std::move(error),{});};
   // The adapter reports when its prepared helper has drained; the selection slot is held until then.
   const std::function<void()> released=[owner,id=op.id] {
    if(!owner)return;
    auto release=[owner,id] {
     if(!owner||owner->stopping_||!owner->preparedOwners_.remove(id))return;
     if(owner->finishedPrepared_.remove(id)){if(owner->selectingId_==id)owner->selectingId_.clear();owner->active_.remove(id);owner->settled(id);}
    };
    if(QThread::currentThread()==owner->thread())release();else QMetaObject::invokeMethod(owner.data(),std::move(release),Qt::QueuedConnection);
   };
   source_.setCaptureOutcome(outcome);source_.setRegionScreenshotFirst(screenshotFirst);
   if(delay>0&&!(source=="region"&&!screenshotFirst)) {
    // The countdown runs first and the target is frozen when it ends, as for a capture.
    *prepared=[owner,id=op.id,action,delay,screenshotFirst,outcome,early,released](const ResourceLimits& limits,std::function<void(PayloadResult)> done) {
     if(!owner){done({{},{"source_unavailable","Capture stopped because put.gd is shutting down.",{}}});return;}
     owner->source_.delayed(delay,[owner,id,action,screenshotFirst,outcome,early,released,limits,done=std::move(done)](Error error) mutable {
      if(!owner||owner->stopping_||!owner->active_.contains(id)){done({{},{"cancelled","Capture cancelled",{}}});return;}
      if(error){done({{},std::move(error)});return;}
      owner->source_.setCaptureOutcome(outcome);owner->source_.setRegionScreenshotFirst(screenshotFirst);owner->source_.setCaptureDelay(0);
      owner->preparedOwners_.insert(id);
      owner->source_.prepareAcquisition(action,limits,early,released)(limits,std::move(done));
     });
    };
   }else {
    // A live region is chosen first; its adapter then counts down before the pixels.
    preparedOwners_.insert(op.id);
    source_.setCaptureDelay(delay);
    *prepared=source_.prepareAcquisition(action,limits,early,released);
    source_.setCaptureDelay(0);
   }
  };
  request.acquire=[prepared,limits,handOver](TextService::Deliver deliver){(*prepared)(limits,handOver(std::move(deliver)));};
  request.cancelAcquisition=[owner]{if(owner)owner->source_.cancelAcquisition();};
 }else if(source=="clipboard") {
  // Reserved as a clipboard import is: no pending copy replaces the clipboard before it is read.
  auto prepared=std::make_shared<SourceAdapter::PreparedAcquisition>();
  request.prepare=[this,id,prepared](const Operation& op,TextService::Deliver){*id=op.id;clipboardImports_.insert(op.id);*prepared=source_.prepareAcquisition("clipboard");};
  request.acquire=[prepared,limits,handOver](TextService::Deliver deliver){(*prepared)(limits,handOver(std::move(deliver)));};
 }else if(source=="file") {
  const auto path=params["path"].toString();
  request.acquire=[path,limits](TextService::Deliver deliver){
   deliver({},[path,limits](Error& error)->QImage{const auto bytes=readTextImageFile(path,limits,error);return error?QImage{}:decodeTextImage(bytes,limits,error);});
  };
 }else {
  // History: the item's retained local copy, checked against its recorded hash. Nothing is downloaded.
  const auto database=databasePath();const auto historyId=params["historyId"].toString();
  request.acquire=[database,historyId,limits](TextService::Deliver deliver){
   deliver({},[database,historyId,limits](Error& error)->QImage{
    QJsonObject data;{HistoryStore store(database);data=store.detail(historyId,&error);}
    if(error)return {};
    const auto local=data["local"].toObject();
    if(local["status"]!="saved"||!data["mime"].toString().startsWith("image/")){error={"image_unavailable","Copy text needs a local copy of this image. Turn on Keep a local copy in Settings → Capture for future captures.",{}};return {};}
    const auto bytes=readTextImageFile(local["path"].toString(),limits,error);
    if(error){error={"artifact_missing","Couldn't read the local copy of this image.",{}};return {};}
    if(bytes.size()!=data["bytes"].toInteger()||QString::fromLatin1(QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex())!=data["sha256"].toString()){error={"artifact_changed","The local copy of this image changed since it was saved.",{}};return {};}
    return decodeTextImage(bytes,limits,error);
   });
  };
 }
 text_->recognize(std::move(request),std::move(reply));
}
void OperationService::textCommand(const QString& command,const QJsonObject& params,JsonReply reply) {
 const auto id=params["id"].toString();
 if(command=="inspect"){QJsonObject result;const auto error=text_->inspect(id,result);reply(error,result);}
 else if(command=="cancel")text_->cancel(id,std::move(reply));
 else reply({"not_implemented","This text recognition method is not built yet",{}},{});
}
Error OperationService::acceptRecording(Operation& op) {
 if(scroll_&&scroll_->active())return {"scroll_active","A scrolling capture is already running. Stop or cancel it first.",{}};
 if(!ready_)return notReadyError();
 if(editor_)return {"editor_active","An image edit is already open. Finish or cancel it before recording.",{{"operationId",editingId_}}};
 if(!selectingId_.isEmpty())return {"busy_selecting","Another capture selection or countdown is already in progress. Finish or cancel it first.",{}};
 auto snapshot=settings_.snapshot();if(active_.size()>=snapshot.limits.pendingOperations+snapshot.limits.concurrentUploads)return {"queue_full","Too many actions are in progress. Wait for some to finish, or raise Queued actions in Settings → Advanced → Resource limits.",{}};
 const auto acceptedAction=op.action.isEmpty()?Settings::recordingActionName():op.action;
 const auto route=snapshot.actions.value(acceptedAction);
 const bool upload=route.destination=="upload"&&!route.profileId.isEmpty();
 if(!upload&&!snapshot.archive.keepUploads)return {"output_required","No output is enabled. Choose a destination or turn on Keep a local copy in Settings → Capture.",{}};
 op.id=uuid();op.action=acceptedAction;op.settings=snapshot;op.settingsRevision=settings_.revision();op.created=QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);op.invocation=upload?++invocation_:invalidateCopyEligibility();op.clipboardRevision=source_.clipboardRevision();
 if(upload) {const auto profile=snapshot.profiles.value(route.profileId);op.destination={profile["provider"].toString(),route.profileId,settings_.revision(),profile["settings"].toObject(),profile["credentials"].toObject()};}
 auto flag=std::make_shared<std::atomic_bool>(false);active_.insert(op.id,flag);if(upload)beginUploadCopies(op,false);
 if(upload){std::lock_guard lock(encryptHooksMutex_);encryptOpen_.insert(op.id);}
#ifdef __GLIBC__
 heapReclamationPending_=true;
#endif
 logging::info(logOperation(),"Operation accepted",{{"operationId",op.id},{"action",op.action},{"output",upload?"upload":"local"},{"provider",op.destination.providerId},{"profileId",op.destination.profileId}});
 return {};
}
void OperationService::recordingCommand(const QString& command,const QJsonObject& params,JsonReply reply) {
 if(command=="prepare"&&scroll_->active()){reply({"scroll_active","A scrolling capture is already running. Stop or cancel it first.",{{"operationId",scroll_->activeId()}}},{});return;}
 // recording.prepare opens the recording selector; the other commands drive the session it opened.
 if(command=="prepare")emit explicitRequest();
 if(!ready_&&command!="capabilities"&&command!="windows"&&command!="microphones") {reply(notReadyError(),{});return;}
 recording_->command(command,params,std::move(reply));
}
void OperationService::editorCommand(const QString& id,const QString& command,const QJsonObject& params,JsonReply reply) {
 // #1804: Open in editor turns a sensitive text check into an editor with the same ID.
 const auto session=editor_&&editingId_==id?editor_:checkSessions_.value(id);
 if(!session||(session!=editor_&&session->phase()!="editing")){reply({"editor_unavailable","No open editor has this operation ID",{}},{});return;}
 QPointer<OperationService> owner(this);
 session->request(command,params,[owner,id,reply](Error error,QJsonObject result){
  if(!owner)return;
  QMetaObject::invokeMethod(owner,[owner,id,reply,error,result]() mutable {
   if(!owner||owner->stopping_)return;
   if(!error)result["operationId"]=id;
   reply(error,result);
  },Qt::QueuedConnection);
 });
}
void OperationService::pump() {
 while(!queue_.isEmpty()&&running_<queue_.head().op.settings.limits.concurrentUploads&&!stopping_) {auto job=queue_.dequeue();++running_;if(!job.op.destination.profileId.isEmpty())emit uploadStarting(job.op);background([this,job=std::move(job)](Error&) mutable {return run(std::move(job)).toJson();},[this](Error,QJsonObject doc) {--running_;auto op=Operation::fromJson(doc);finish(op);pump();});}
}
bool OperationService::encryptAvailableFor(const QString& action) const {return encryptStateFor(action)["available"].toBool();}
Error OperationService::encryptRefusal(const QString& action,const QString& profileId,const QString& installation) const {
 QJsonObject state;
 if(action==fileManagerAction) {
  // Another installation's menu entry is refused for that first, as uploadFiles refuses it,
  // whatever this resident's destinations would say.
  if(!installation.isEmpty())if(auto error=FileUploads::matchInstallation(installation,installation_))return error;
  // The route upload.files takes: its profileId, else the file manager's destination.
  Operation op;op.action=fileManagerAction;const auto snapshot=settings_.snapshot();const auto route=FileUploads::resolve(snapshot,profileId);if(route.error)return {};
  op.settings=Settings::fromValidated(FileUploads::routedDocument(snapshot,route));const auto profile=snapshot.profiles.value(route.profileId);
  op.destination={profile["provider"].toString(),route.profileId,{},profile["settings"].toObject(),profile["credentials"].toObject()};state=encryptOptions(op);
 } else state=encryptStateFor(action==Settings::repeatActionName()?QStringLiteral("region"):action);
 if(!state["checkRequired"].toBool())return {};
 return {"encrypt_check_required",state["reason"].toString()+QStringLiteral(" Or upload it without encryption."),{}};
}
QJsonObject OperationService::encryptStateFor(const QString& action) const {
 Operation op;op.action=action;op.settings=settings_.snapshot();
 const auto route=op.settings.actions.value(action);
 if(route.destination!="upload"||route.profileId.isEmpty())return {{"available",false}};
 const auto profile=op.settings.profiles.value(route.profileId);
 op.destination={profile["provider"].toString(),route.profileId,{},profile["settings"].toObject(),profile["credentials"].toObject()};
 auto out=encryptOptions(op);out["profileId"]=route.profileId;return out;
}
bool OperationService::encryptWanted(const Operation& op) const {
 if(op.action==diagnosticsAction)return false;
 if(op.encrypt.contains("requested"))return op.encrypt.value("requested").toBool();
 return op.settings.profiles.value(op.destination.profileId).value("encrypt").toObject().value("mode").toString(QStringLiteral("off"))!="off";
}
QJsonObject OperationService::encryptOptions(const Operation& op) const {
 const auto route=routeOf(op);
 const auto setting=op.settings.profiles.value(op.destination.profileId).value("encrypt").toObject().value("mode").toString(QStringLiteral("off"));
 QJsonObject out{{"setting",setting},{"passphrase",setting=="passphrase"},{"available",false}};
 bool on=op.encrypt.contains("requested")?op.encrypt.value("requested").toBool():setting!="off";
 {std::lock_guard lock(encryptHooksMutex_);if(const auto choice=encryptChoices_.constFind(op.id);choice!=encryptChoices_.cend())on=*choice;
  if(route.destination!="upload"||op.destination.profileId.isEmpty()){out["on"]=false;out["reason"]="Encrypted links need an upload destination.";return out;}
  const auto provider=providers_.find(op.destination.providerId);
  const auto unavailable=provider?provider->capabilities(op.destination).encryptUnavailable:QStringLiteral("This destination is unavailable.");
  const auto check=encryptCheck_?encryptCheck_(op.destination.profileId,op.settings.profiles.value(op.destination.profileId)):QJsonObject{{"state","untested"}};
  auto mode=route.publicationMode;if(mode=="default"&&provider)mode=provider->defaultPublicationMode(op.destination);
  if(!unavailable.isEmpty())out["reason"]=unavailable;
  else if(mode=="private")out["reason"]=encryptNeedsLink();
  else if(check.value("state")!="passed"){out["reason"]=encryptCheckMessage(check,op.settings.profiles.value(op.destination.profileId));out["checkRequired"]=true;}
  else out["available"]=true;}
 out["on"]=on;return out;
}
void OperationService::setEncrypt(const QString& id,bool enabled,JsonReply reply) {
 const auto found=active_.constFind(id);
 if(found==active_.cend()){reply({"operation_not_found","That operation has finished, or there is none with this ID.",{}},{});return;}
 {std::lock_guard lock(encryptHooksMutex_);if(!encryptOpen_.contains(id)){reply({"encrypt_too_late","This upload has already started, so Encrypt can't change now.",{{"operationId",id}}},{});return;}}
 // A change counts only while the window that shows the toggle is open and editing: a sensitive
 // text check's editor not yet opened, a recording not yet in review and an edit already
 // submitted all show (or showed) another state.
 if(checkSessions_.contains(id)&&checkSessions_.value(id)->phase()!="editing"){reply({"encrypt_unavailable","Encrypt can change once the editor is open. Select Open in editor, or start the capture with Encrypt on.",{{"operationId",id}}},{});return;}
 if(recording_&&recording_->active()&&recording_->activeId()==id&&!recording_->reviewing()&&!recording_->exporting()){reply({"recording_state","Encrypt can change once the recording is in review.",{{"operationId",id}}},{});return;}
 const bool editorOpen=editor_&&editingId_==id&&editor_->phase()=="editing";
 const bool checkOpen=checkSessions_.contains(id)&&checkSessions_.value(id)->phase()=="editing";
 const bool reviewOpen=recording_&&recording_->active()&&recording_->activeId()==id;
 if(!editorOpen&&!checkOpen&&!reviewOpen){reply({"encrypt_too_late","This upload has already started, so Encrypt can't change now.",{{"operationId",id}}},{});return;}
 // The open editor's own toggle, as a click sets it: its change comes back through recordEncrypt.
 if(editorOpen||checkOpen){editorCommand(id,"encrypt",{{"on",enabled}},[reply,id,enabled](Error e,QJsonObject state){if(e){reply(e,{});return;}reply({},{{"operationId",id},{"encrypt",enabled},{"editor",state}});});return;}
 // The recording review's own toggle, likewise; its change comes back as the review's encrypt event.
 // While an export runs nothing changes; before review the choice is kept and the window starts from it.
 if(recording_&&recording_->active()&&recording_->activeId()==id) {
  if(recording_->exporting()){reply({"recording_state","Encrypt can't change while an export runs.",{{"operationId",id}}},{});return;}
  if(recording_->reviewing()){recordingCommand("review",{{"id",id},{"action","encrypt"},{"on",enabled}},[reply,id,enabled](Error e,QJsonObject state){if(e){reply(e,{});return;}reply({},{{"operationId",id},{"encrypt",enabled},{"review",state}});});return;}
 }
 recordEncrypt(id,enabled);
 reply({},{{"operationId",id},{"encrypt",enabled}});
}
void OperationService::recordEncrypt(const QString& id,bool enabled) {
 if(!active_.contains(id))return;
 {std::lock_guard lock(encryptHooksMutex_);if(!encryptOpen_.contains(id))return;encryptChoices_.insert(id,enabled);}
 logging::info(logOperation(),"Encrypt choice set",{{"operationId",id},{"enabled",enabled}});
}
// Encrypted links (#2079). An upload is encrypted when it asked to be (op.encrypt.requested), else when its
// destination's Encrypted links setting is on. An encrypted item uploads an encrypted page under a random name
// in place of its file; the file itself stays the item's artifact for previews and local copies.
bool OperationService::encryptItems(Operation& op,const ProviderCapabilities& capabilities,const QString& publicationMode,const QString& requestPassphrase,const std::shared_ptr<std::atomic_bool>& cancelled,const std::function<bool(const OperationItem&)>& saveItem) {
 const auto profile=op.settings.profiles.value(op.destination.profileId);
 const auto setting=profile.value("encrypt").toObject().value("mode").toString(QStringLiteral("off"));
 if(!encryptWanted(op))return true;
 QJsonObject check;QString passphrase=requestPassphrase;
 {std::lock_guard lock(encryptHooksMutex_);if(encryptCheck_)check=encryptCheck_(op.destination.profileId,profile);if(passphrase.isEmpty()&&setting=="passphrase"&&encryptPassphrase_)passphrase=encryptPassphrase_(op.destination.profileId,profile);}
 for(auto& item:op.items) {
  // A file a Retry sends again (stored, failed or cancelled) and never encrypted is encrypted now.
  if(!item.encrypt.isEmpty()||terminalUpload(item.uploadStatus)||item.uploadStatus=="unknown"||item.uploadStatus=="uploading"||!item.receipt.directUrl.isEmpty())continue;
  if(cancelled->load())return true;
  // Encrypting asked for and impossible here: the file doesn't go out unencrypted.
  Error refusal;
  if(!capabilities.encryptUnavailable.isEmpty())refusal={"encrypt_unavailable",capabilities.encryptUnavailable+" Turn off Encrypt to upload it.",{}};
  // An encrypted file opens only from its link, and a private upload gets none.
  else if(publicationMode=="private")refusal={"encrypt_needs_link",encryptNeedsLink(),{}};
  else if(check.value("state")!="passed")refusal={"encrypt_check_required",encryptCheckMessage(check,profile)+QStringLiteral(" Or turn off Encrypt to upload it as an ordinary link."),{}};
  else if(op.encrypt.value("passphraseFrom")=="request"&&requestPassphrase.isEmpty())refusal={"encrypt_passphrase_missing","This upload's passphrase came with the request and isn't kept, so it can't be encrypted now. Upload the file again with its passphrase.",{}};
  else if(setting=="passphrase"&&passphrase.isEmpty())refusal={"encrypt_passphrase_locked","The Encrypted links passphrase isn't loaded. Unlock it in Destinations, then select Retry upload.",{}};
  // The format's limits: a file encrypting can't take fails before anything is sent. It is never
  // uploaded unencrypted; the user turns off Encrypt and uploads again for an ordinary link.
  const bool split=item.artifact.bytes>encryptInlineLimit_;
  if(!refusal&&item.artifact.bytes>encryptTotalLimit_)refusal={"encrypt_too_large","This file is too large to encrypt (2 GB limit). Turn off Encrypt to upload it as an ordinary link.",{}};
  else if(!refusal&&split&&(!capabilities.encryptSplit||capabilities.serverNamesObjects))refusal={"encrypt_split_unavailable","Files over 64 MB are encrypted as a page and a data file beside it, which this destination's links can't serve. Turn off Encrypt to upload it as an ordinary link.",{}};
  if(refusal) {item.uploadStatus="failed";item.phase="failed";item.error=refusal;logging::info(logOperation(),"Upload not encrypted, so not sent",{{"operationId",op.id},{"itemId",item.artifact.id},{"code",refusal.code},{"bytes",double(item.artifact.bytes)}});if(!saveItem(item))return false;continue;}
  item.phase="encrypting";observe(op.publicJson());
  // The page and its data file share one random stem, as the design shows (k3Xq….html, k3Xq….bin).
  const auto pageName=EncryptJob::randomName("html"),binName=split?pageName.chopped(5)+".bin":QString();
  const EncryptJob encrypter(encryptHelper_);
  auto result=encrypter.run(item.artifact,QFileInfo(item.artifact.path).absolutePath(),split?"split":"inline",pageName,binName,passphrase,cancelled);
  if(result.error) {
   if(result.error.code=="cancelled"){item.uploadStatus="cancelled";item.phase="cancelled";item.error={"cancelled","Upload cancelled before it started. Select Retry upload to try again.",{}};}
   else {item.uploadStatus="failed";item.phase="failed";item.error=result.error;}
   logging::warning(logOperation(),"Encryption failed",{{"operationId",op.id},{"itemId",item.artifact.id},{"error",logging::errorFields(result.error)}});
   if(!saveItem(item))return false;continue;
  }
  QJsonObject encrypt{{"status","encrypted"},{"form",split?"split":"inline"},{"key",result.key},{"passphrase",!passphrase.isEmpty()},{"page",result.page.toJson()}};
  if(split)encrypt["data"]=QJsonObject{{"artifact",result.data.toJson()},{"candidate",binName},{"uploadStatus","stored"},{"publicationId",uuid()}};
  item.encrypt=encrypt;item.candidate=pageName;item.phase="stored";
  logging::info(logOperation(),"Upload encrypted",{{"operationId",op.id},{"itemId",item.artifact.id},{"form",split?"split":"inline"},{"passphrase",!passphrase.isEmpty()},{"pageBytes",double(result.page.bytes)}});
  if(!saveItem(item))return false;
 }
 return true;
}
Operation OperationService::run(Job job) {
 auto op=job.op;HistoryStore store(databasePath());ArtifactStore artifacts(root_+"/artifacts");
 // Any History write of this run that fails with history_io marks the operation, even if a later write succeeds:
 // a checkpoint it skipped can leave a row that only a restart's recovery fixes (#1579).
 const auto refused=[this,id=op.id](const Error& e){if(e.code=="history_io"){QPointer<OperationService> owner(this);QMetaObject::invokeMethod(this,[owner,id]{if(owner)owner->rememberUnsavedHistory(id);},Qt::QueuedConnection);}return e;};
 auto save=[&]() {auto e=refused(store.saveOperation(op,&providers_));if(e) {op.error=e;op.advance(OperationState::Failed);}else observe(op.publicJson());return !e;};
 auto saveItem=[&](const OperationItem& item) {
  auto e=refused(store.checkpointOperation(op,{item.artifact.id},&providers_));
  if(e){op.error=e;op.advance(OperationState::Failed);return false;}
  observe(op.publicJson());
  if(item.linkAvailable()) {
   int index=0;while(index<op.items.size()&&op.items[index].artifact.id!=item.artifact.id)++index;
   QPointer<OperationService> owner(this);
   QMetaObject::invokeMethod(this,[owner,id=op.id,itemId=item.artifact.id,index,url=item.link(),count=int(op.items.size())]{if(owner&&!owner->stopping_)owner->uploadLinkReady(id,itemId,index,url,count);},Qt::QueuedConnection);
  }
  return true;
 };
 // #1820: covered copies of selected files are read once, by the acquisition below; then they go, whatever happens.
 const std::shared_ptr<void> coveredCopies(nullptr,[replaced=job.replacedFiles,original=job.originalPath](void*){discardCovered(replaced);if(!original.isEmpty()&&original!="-")QFile::remove(original);});
 // #1893: the capture before its style, written below; removed once copied, whatever happens.
 QString styledOriginal;const std::shared_ptr<void> styledCopy(nullptr,[&styledOriginal](void*){if(!styledOriginal.isEmpty()&&styledOriginal!="-")QFile::remove(styledOriginal);});
 if(job.cancelled->load()){job.payload={};job.payloadLease.reset();op.cancelled=true;op.advance(OperationState::Cancelled);save();return op;}
 if(job.acquire) {op.phase="preparing";if(!save())return op;
  // An upload route rejects files its destination can never store before copying them.
  qint64 objectLimit=0;{const auto output=routeOf(op);if(output.destination!="local"&&!output.profileId.isEmpty())if(auto destination=providers_.find(op.destination.providerId))objectLimit=destination->capabilities(op.destination).maximumObjectBytes;}
  // A fixed waiting-uploads budget is checked against bytes already stored, so such
  // imports run one at a time. Without one, free disk is the only bound and its
  // reservation is atomic: a new action's import never waits behind a large copy.
  std::unique_lock acquisitionGuard(acquisitionMutex_,std::defer_lock);if(op.settings.limits.pendingArtifactBytes<ResourceLimits::unlimitedBytes)acquisitionGuard.lock();
  // #1804: a file that changed after its sensitive text check is not uploaded unchecked.
  for(auto it=job.checkedFiles.cbegin();it!=job.checkedFiles.cend();++it)if(!job.localOnly&&fileIdentity(it.key())!=it.value()){
   job.payload={};job.payloadLease.reset();op.error={"sensitive_check_failed",QFileInfo(it.key()).fileName()+": changed after the sensitive text check, so the upload didn't start. Upload it again to recheck.",{{"cause","file_changed"}}};
   op.advance(OperationState::Failed);save();return op;
  }
  // #1893: a still capture goes on its capture style before it is stored. Off, nothing here runs.
  if(needsStyle(job)){
   styleCapture(job,op);if(job.styleOriginal)styledOriginal=job.originalPath;
   // A capture cancelled while it was being styled stores nothing.
   if(job.cancelled->load()){job.payload={};job.payloadLease.reset();op.cancelled=true;op.advance(OperationState::Cancelled);save();return op;}
  }
  auto acquired=artifacts.acquire(op.id,job.payload,op.settings.limits,op.action,op.settings,QDateTime::fromString(op.created,Qt::ISODateWithMs),objectLimit);
  // #1893: a style problem never fails a capture. When the store refuses the styled PNG for its size or for disk space,
  // the capture as it was is stored instead, once, and History says why.
  if(acquired.error&&!job.unstyled.isEmpty()&&QStringList{"input_excessive","destination_object_too_large","disk_space_insufficient"}.contains(acquired.error.code)){
   logging::warning(logOperation(),"Capture style was not applied",{{"operationId",op.id},{"error",logging::errorFields(acquired.error)}});
   // The unstyled capture is what will be stored: no -original beside it, and its disk space is free for it first.
   // A covered capture's original from before covering stays.
   if(job.styleOriginal){if(!job.originalPath.isEmpty()&&job.originalPath!="-")QFile::remove(job.originalPath);job.originalPath.clear();job.styleOriginal=false;}
   job.style={};
   ClipboardPayload plain;plain.kind="png";plain.bytes=std::move(job.unstyled);plain.mime="image/png";
   acquired=artifacts.acquire(op.id,plain,op.settings.limits,op.action,op.settings,QDateTime::fromString(op.created,Qt::ISODateWithMs),objectLimit);
   if(!acquired.error){
    const auto note=QStringLiteral("Saved without capture style because the styled image was too large to store or upload.");
    op.note=op.note.isEmpty()?note:op.note+" "+note;
   }
  }
  job.unstyled.clear();
  // #1893: the style stands on the operation only now that its PNG is stored.
  if(!acquired.error&&!job.style.isEmpty()){
   op.style=job.style;
   if(job.style["pictureMissing"].toBool()){const auto why=QStringLiteral("The backdrop picture couldn't be read, so put.gd used Graphite.");op.note=op.note.isEmpty()?why:op.note+" "+why;}
  }
  job.payload={};job.payloadLease.reset(); // Retry/upload ownership now belongs to the immutable disk artifact.
  // A file-manager upload names the requested file or folder a late failure came from.
  if(acquired.error&&!job.origins.isEmpty()){
   const auto index=acquired.error.details["itemIndex"].toInt(-1);
   if(index>=0&&index<job.origins.size()){const auto origin=job.origins[index];acquired.error.details["path"]=origin;acquired.error.message=QFileInfo(origin).fileName()+": "+acquired.error.message;}
  }
  if(acquired.error) {op.error=acquired.error;op.advance(OperationState::Failed);save();logging::warning(logOperation(),"Acquisition failed",{{"operationId",op.id},{"error",logging::errorFields(op.error)}});return op;}
  for(const auto& a:std::as_const(acquired.artifacts)) {OperationItem item;item.phase="stored";item.artifact=a;item.artifact.previewPath=artifacts.preview(a,op.settings.previewCacheBytes,op.settings.limits.imagePixels);item.publicationId=uuid();op.items.append(item);}op.advance(OperationState::Stored);if(!save())return op;
  QJsonArray mimes;for(const auto& item:std::as_const(op.items))if(!mimes.contains(item.artifact.mime))mimes.append(item.artifact.mime);
  logging::info(logOperation(),"Artifacts acquired",{{"operationId",op.id},{"items",op.items.size()},{"byteCount",artifactBytes(op)},{"mimeTypes",mimes}});
 }
 // Archive every acquired item before any item in this batch is published.
 op.phase="saving_local";
 const auto route=routeOf(op);
 const bool local=job.localOnly||route.destination=="local"||route.profileId.isEmpty();
 for(auto& item:op.items) if(!job.sharingOnly&&(local||op.settings.archive.keepUploads)&&item.local.status!="saved") {item.phase="saving_local";observe(op.publicJson());item.local=LocalArchive::save(item.artifact,op.settings.archive);item.phase="stored";if(!saveItem(item))return op;}
 // #1820: with Keep a local copy on, a covered upload's original is kept locally too, beside the covered copy.
 if(!job.sharingOnly&&op.settings.archive.keepUploads&&(!job.originalPath.isEmpty()||!job.originalFiles.isEmpty())){keepOriginals(job,op);if(!save())return op;}
 if(local) {for(auto& item:op.items) item.uploadStatus="local_only";bool saved=true;for(const auto& item:std::as_const(op.items)) if(item.local.status!="saved")saved=false;op.advance(saved?OperationState::Completed:OperationState::Failed);save();return op;}
 auto provider=providers_.find(op.destination.providerId);if(!provider) {op.error={"provider_unavailable","Destination is unavailable. Restore access to it in Destinations.",{}};op.advance(OperationState::Failed);save();return op;}
 const auto capabilities=provider->capabilities(op.destination);
 auto publicationMode=routeOf(op).publicationMode;
 if(publicationMode=="default")publicationMode=provider->defaultPublicationMode(op.destination);
 if(publicationMode=="private"&&!capabilities.privateUploads){op.error={"unsupported_capability","This destination doesn't support private uploads. Change Sharing or Destination in Settings → Capture.",{}};op.advance(OperationState::Failed);save();return op;}
 // A server that names its own objects has no name to collide with: the policy does not apply (#1361).
 if(!capabilities.serverNamesObjects&&(!capabilities.conditionalCreate||(op.settings.collisionPolicy=="overwrite"&&!capabilities.conditionalReplace))) {op.error={"unsupported_capability","This destination doesn't support the selected When a name exists setting. Change it in Settings → Capture.",{}};op.advance(OperationState::Failed);save();return op;}
 // #2079: encrypted items get their page and random name before naming; a retry keeps both.
 if(!job.sharingOnly&&!encryptItems(op,capabilities,publicationMode,job.encryptPassphrase,job.cancelled,saveItem))return op;
 const bool encrypting=encryptWanted(op);
 bool needsNames=job.acquire;for(const auto& item:std::as_const(op.items))if(!terminalUpload(item.uploadStatus)&&item.candidate.isEmpty())needsNames=true;
 if(needsNames) {QList<Artifact> list;for(const auto& item:std::as_const(op.items))list.append(item.artifact);auto names=UploadNaming::batch(list,op.settings.namingMode,capabilities.serverNamesObjects?QStringLiteral("keep"):op.settings.collisionPolicy,capabilities.maximumNameBytes,op.settings.randomName);QJsonArray chosen;
  for(int i=0;i<op.items.size();++i) {
   if(!op.items[i].candidate.isEmpty())continue;
   // #2079: an encrypted request's item that encrypting refused keeps its failure; it is never named or sent.
   if(!sendable(op.items[i],encrypting))continue;
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
 // What the provider keeps between this operation's requests ends before the lease passes on.
 struct Lease{std::shared_ptr<DestinationLease> held;std::shared_ptr<void> session;~Lease(){session.reset();if(held)held->release();}} lease;
 if(!parallel) {
  const auto destination=destinationLease(op.destination);op.phase="queued";observe(op.publicJson());
  if(!destination->acquire([&job]{return job.cancelled->load();})){op.cancelled=true;op.advance(OperationState::Cancelled);save();return op;}
  lease.held=destination;lease.session=provider->openSession(op.destination);
 }
 op.advance(OperationState::Running);op.phase="preflight";if(!save())return op;
 // Check all pending names before the first PUT, including opaque references that
 // may identify one target despite differing proposed display names.
 QHash<QByteArray,QList<int>> targets;
 for(int index=0;index<op.items.size();++index) {
  auto& item=op.items[index];if(terminalUpload(item.uploadStatus)||item.uploadStatus=="unknown")continue;
  if(!sendable(item,encrypting))continue;
  if(job.cancelled->load())break;
  item.phase="preflight";observe(op.publicJson());
  if(auto e=artifacts.verify(item.uploadArtifact())) {item.error=e;item.uploadStatus="failed";if(!saveItem(item))return op;continue;}
  if(!item.objectReference.isEmpty()&&!item.condition.isEmpty()&&item.uploadStatus!="failed") {targets[QJsonDocument(item.objectReference).toJson(QJsonDocument::Compact)].append(index);continue;}
  bool checked=false;
  for(int attempts=0;attempts<32&&!job.cancelled->load();++attempts) {
   auto check=storageRequest<NameCheck>([&](auto cb) {provider->checkName(op.destination,item.candidate,cb,job.cancelled);});
   if(job.cancelled->load())break;
   if(check.error||check.status=="inconclusive"||check.objectReference.isEmpty()) {item.error=check.error?check.error:Error{"existence_unknown","Couldn't check whether the file name is already taken at the destination. Check the destination and try again.",{}};item.uploadStatus="failed";auto fields=itemFields(op,item);fields["status"]=check.status;fields["error"]=logging::errorFields(item.error);logging::warning(providerLog(op.destination.providerId),"Name check failed",fields);break;}
   if(check.status=="present"&&op.settings.collisionPolicy=="skip") {item.uploadStatus="skipped";break;}
   if(check.status=="present"&&op.settings.collisionPolicy=="rename") {item.candidate=renamedAwayFrom(op.settings,item.candidate,capabilities.maximumNameBytes,otherCandidates(op.items,index));if(!saveItem(item))return op;continue;}
   item.objectReference=check.objectReference;item.concurrencyToken=check.concurrencyToken;item.condition=check.status=="present"?"match":"absent";
   if(item.condition=="match"&&item.concurrencyToken.isEmpty()) {item.error={"precondition_missing","Couldn't verify the existing file before overwriting it, so the upload didn't start.",{}};item.uploadStatus="failed";break;}
   item.uploadStatus="stored";targets[QJsonDocument(item.objectReference).toJson(QJsonDocument::Compact)].append(index);checked=true;break;
  }
  if(job.cancelled->load()) {item.uploadStatus="cancelled";item.phase="cancelled";item.error={"cancelled","Upload cancelled before it started. Select Retry upload to try again.",{}};}
  else if(!checked&&item.uploadStatus=="stored") {item.uploadStatus="failed";item.error={"collision_exhausted","Couldn't find an unused file name after 32 tries. Rename the file or change When a name exists in Settings → Capture.",{}};}
  if(!saveItem(item))return op;
 }
 for(const auto& group:std::as_const(targets)) if(group.size()>1) {if(op.settings.collisionPolicy=="overwrite") {for(int i:group)op.items[i].uploadStatus="conflict";}else if(op.settings.collisionPolicy=="skip") {for(int n=1;n<group.size();++n)op.items[group[n]].uploadStatus="skipped";}else {for(int n=1;n<group.size();++n) {auto& item=op.items[group[n]];item.uploadStatus="failed";item.error={"name_alias_conflict","Multiple files in this batch map to the same name at the destination. Rename them and try again.",{}};}}}
 if(!save())return op;
 // Files upload in parallel up to "Uploads at once", shared fairly by every
 // operation (#28). Each file holds one slot for its whole transfer, so a large
 // multipart upload keeps one slot and never starves the others. Workers own a
 // local copy of their item; `op` changes only under itemsMutex, and every save
 // writes the whole operation from there, so a checkpoint never mixes item states.
 std::mutex itemsMutex;std::atomic_bool persisted=true;
 std::function<Error(const OperationItem&,int)> persist=[&](const OperationItem& item,int index)->Error {
  std::lock_guard lock(itemsMutex);op.items[index]=item;HistoryStore durable(databasePath());return refused(durable.checkpointOperation(op,{item.artifact.id},&providers_));
 };
 auto show=[&](const OperationItem& item,int index){std::lock_guard lock(itemsMutex);op.items[index]=item;observe(op.publicJson());};
 auto commit=[&](const OperationItem& item,int index)->bool {
  if(auto e=persist(item,index)){std::lock_guard lock(itemsMutex);op.error=e;op.advance(OperationState::Failed);persisted=false;return false;}
  int count=0;{std::lock_guard lock(itemsMutex);observe(op.publicJson());count=int(op.items.size());}
  if(item.linkAvailable()) {
   QPointer<OperationService> owner(this);
   QMetaObject::invokeMethod(this,[owner,id=op.id,itemId=item.artifact.id,index,url=item.link(),count]{if(owner&&!owner->stopping_)owner->uploadLinkReady(id,itemId,index,url,count);},Qt::QueuedConnection);
  }
  return true;
 };
 auto settled=[&](int index){QPointer<OperationService> owner(this);QMetaObject::invokeMethod(this,[owner,id=op.id,index]{if(owner&&!owner->stopping_)owner->uploadItemSettled(id,index);},Qt::QueuedConnection);};
 // A sharing provider (Proton Drive) makes the link in the sharing step below,
 // after the transfer. Such a file settles there, so each_link ordering waits
 // for its link instead of passing it and stranding the link (#478).
 auto linkFollows=[&](const OperationItem& item){return capabilities.sharing&&item.uploadStatus=="uploaded"&&item.receipt.directUrl.isEmpty();};
 auto settleTransfer=[&](int index){OperationItem item;{std::lock_guard lock(itemsMutex);item=op.items[index];}if(!linkFollows(item))settled(index);};
 // #2079: a split encrypted item's data file goes first, under its own random name and
 // conditional create, with the same unknown-outcome rules: it is reconciled before it is
 // ever sent again. Its record lives in item.encrypt.data. False when the item settled here.
 auto sendData=[&](OperationItem& item,int index,const std::shared_ptr<std::atomic_bool>& cancel)->bool {
  auto encrypt=item.encrypt;auto data=encrypt["data"].toObject();
  OperationItem part;part.artifact=Artifact::fromJson(data["artifact"].toObject());part.candidate=data["candidate"].toString();part.publicationId=data["publicationId"].toString();
  part.uploadStatus=data["uploadStatus"].toString("stored");part.objectReference=data["objectReference"].toObject();part.condition=data["condition"].toString();
  part.concurrencyToken=data["concurrencyToken"].toString();part.receipt=StorageReceipt::fromJson(data["receipt"].toObject());part.uploadJournal=data["uploadJournal"].toObject();
  const auto keep=[&]()->bool{
   data["uploadStatus"]=part.uploadStatus;data["objectReference"]=part.objectReference;data["condition"]=part.condition;data["concurrencyToken"]=part.concurrencyToken;
   data["receipt"]=part.receipt.toJson();data["uploadJournal"]=part.uploadJournal;data["error"]=part.error.toJson();encrypt["data"]=data;item.encrypt=encrypt;return commit(item,index);
  };
  const auto settle=[&](const QString& status,const Error& error){item.uploadStatus=status;item.phase=status;item.error=error;keep();return false;};
  const auto own=[&](const StatResult& stat){return stat.status=="own"&&!stat.receipt.validate()&&stat.receipt.publicationId==part.publicationId&&stat.receipt.destination.toJson()==op.destination.toJson();};
  if(part.uploadStatus=="uploaded")return true;
  ArtifactStore artifacts(root_+"/artifacts");
  if(part.uploadStatus=="unknown") {
   item.phase="reconciling";show(item,index);
   auto stat=storageRequest<StatResult>([&](auto cb){auto request=part.uploadRequest(op.destination,op.requestTimeoutMs);request.cancellation=cancel;provider->reconcile(request,cb);});
   if(own(stat)){part.receipt=stat.receipt;part.uploadStatus="uploaded";part.uploadJournal={};part.error={};return keep();}
   if(stat.status!="missing")return settle("unknown",stat.error?stat.error:Error{"publication_unknown","Couldn't confirm whether the encrypted link's data file was uploaded. Check the destination before selecting Retry upload.",{}});
   part.uploadStatus="stored";
  }
  if(auto e=artifacts.verify(part.artifact))return settle("failed",e);
  if(part.objectReference.isEmpty()||part.condition.isEmpty()) {
   item.phase="preflight";show(item,index);
   auto check=storageRequest<NameCheck>([&](auto cb){provider->checkName(op.destination,part.candidate,cb,cancel);});
   if(cancel->load())return settle("cancelled",{"cancelled","Upload cancelled before it started. Select Retry upload to try again.",{}});
   if(check.error||check.objectReference.isEmpty()||check.status=="inconclusive")return settle("failed",check.error?check.error:Error{"existence_unknown","Couldn't check whether the encrypted link's data file name is free at the destination. Try again.",{}});
   // Its name is in the page already: a taken one is never replaced or renamed.
   if(check.status=="present")return settle("failed",{"encrypt_name_taken","The encrypted link's random data file name is already taken at the destination, so the upload didn't finish.",{}});
   part.objectReference=check.objectReference;part.concurrencyToken=check.concurrencyToken;part.condition="absent";if(!keep())return false;
  }
  part.uploadStatus="uploading";item.phase="uploading";if(!keep())return false;
  auto activity=std::make_shared<StorageActivity>(*this,!provider->reportsUploadProgress());
  auto uploaded=awaitProvider<UploadResult>([&](auto cb){
   auto request=part.uploadRequest(op.destination,op.requestTimeoutMs);request.cancellation=cancel;
   request.checkpoint=[&](const QJsonObject& journal,std::function<void(Error)> ack){part.uploadJournal=journal;data["uploadJournal"]=journal;encrypt["data"]=data;item.encrypt=encrypt;ack(persist(item,index));};
   request.progress=[&,activity](const UploadProgress& value){if(!value.valid())return;activity->set(value.phase!="preparing");std::lock_guard lock(itemsMutex);op.items[index].phase=value.phase;op.items[index].uploadProgress=value;observe(op.publicJson());};
   provider->upload(request,cb);
  });
  activity->set(false);
  logging::write(providerLog(op.destination.providerId),uploaded.status=="uploaded"?QtInfoMsg:QtWarningMsg,"Encrypted data file upload",{{"operationId",op.id},{"itemId",item.artifact.id},{"status",uploaded.status},{"byteCount",double(part.artifact.bytes)}});
  if(uploaded.status=="uploaded") {
   const bool matches=!uploaded.receipt.validate()&&uploaded.receipt.publicationId==part.publicationId&&uploaded.receipt.destination.toJson()==op.destination.toJson();
   if(!matches){part.uploadStatus="unknown";return settle("unknown",{"receipt_invalid","The destination's reply didn't match this upload. Check the destination before retrying.",{}});}
   part.receipt=uploaded.receipt;part.uploadStatus="uploaded";part.uploadJournal={};part.error={};return keep();
  }
  if(uploaded.status=="unknown"){part.uploadStatus="unknown";part.error=uploaded.error;return settle("unknown",uploaded.error);}
  if(uploaded.status=="collision") {
   // Ours from an earlier attempt whose reply was lost, or someone else's.
   auto stat=storageRequest<StatResult>([&](auto cb){auto request=part.uploadRequest(op.destination,op.requestTimeoutMs);request.cancellation=cancel;provider->reconcile(request,cb);});
   if(own(stat)){part.receipt=stat.receipt;part.uploadStatus="uploaded";part.uploadJournal={};part.error={};return keep();}
   if(stat.status!="foreign"){part.uploadStatus="unknown";return settle("unknown",stat.error?stat.error:Error{"publication_unknown","Couldn't confirm whether the encrypted link's data file was uploaded. Check the destination before selecting Retry upload.",{}});}
   return settle("failed",{"encrypt_name_taken","The encrypted link's random data file name is already taken at the destination, so the upload didn't finish.",{}});
  }
  // A failure before the provider settled a journaled (multipart) upload leaves it unknown, as for the page.
  if(uploaded.status=="failed"&&!part.uploadJournal.isEmpty()&&!cancel->load()){part.uploadStatus="unknown";part.error=uploaded.error;return settle("unknown",uploaded.error);}
  part.uploadStatus=cancel->load()?QStringLiteral("cancelled"):QStringLiteral("failed");part.error=uploaded.error;
  return settle(part.uploadStatus,cancel->load()?Error{"cancelled","Upload cancelled. Select Retry upload to upload this file.",{}}:uploaded.error);
 };
 auto transfer=[&](int index,const std::shared_ptr<std::atomic_bool>& cancel) {
  OperationItem item;{std::lock_guard lock(itemsMutex);item=op.items[index];}
  if(!sendable(item,encrypting))return;
  ArtifactStore artifacts(root_+"/artifacts");
  auto lock=objectLock(op.destination,item.objectReference,item.candidate);std::unique_lock guard(*lock);
  // What a failed or cancelled attempt leaves the item as.
  QString undone=QStringLiteral("failed");
  if(item.uploadStatus=="unknown"&&capabilities.serverNamesObjects) {
   // Nothing can tell whether the earlier request stored the file. This runs only
   // on an explicit Retry, which sends it again; the server may then hold two copies.
   logging::warning(providerLog(op.destination.providerId),"Uncertain upload sent again; the destination cannot be checked",itemFields(op,item));
   // The first request's copy may still be on the server whatever this resend
   // does, so a resend that does not finish leaves the item unknown, as it was
   // (with this attempt's error), never failed: History keeps saying so and
   // Retry stays the same. Only an upload confirmed here clears it. Nothing is
   // journaled before the attempt itself (uploading reads back as unknown).
   undone=QStringLiteral("unknown");
   item.uploadStatus="stored";item.error={};if(auto error=artifacts.verify(item.uploadArtifact())) {item.uploadStatus=undone;item.error=error;commit(item,index);return;}
  }
  // A provider that settles its own uploads checks the key itself when it resumes a journaled
  // upload; checking it here as well could only overrule its answer (#1627).
  // An unknown item with a journaled multipart upload stays unknown until the provider settles
  // that upload: every settled outcome (uploaded, or a collision or failure after its abort)
  // clears the journal, so a failure that leaves the journal in place happened before it.
  const bool settling=item.uploadStatus=="unknown"&&capabilities.settlesUploads&&!item.uploadJournal.isEmpty();
  if(settling) {
   // First the saved identity: it needs neither the local file nor a new upload, so a missing
   // file, locked credentials or a worker that won't start leave the item unknown with its
   // journal (the check itself fails), never failed. Only the provider's own check may then
   // abort or replace the upload, for a key that is absent or holds another object.
   item.phase="reconciling";show(item,index);
   auto stat=storageRequest<StatResult>([&](auto cb) {auto request=item.uploadRequest(op.destination,op.requestTimeoutMs);request.cancellation=cancel;provider->reconcile(request,cb);});
   if(stat.status=="own"&&!stat.receipt.validate()&&stat.receipt.publicationId==item.publicationId&&stat.receipt.destination.toJson()==op.destination.toJson()) {item.receipt=stat.receipt;item.uploadStatus="uploaded";item.error={};item.uploadJournal={};if(!commit(item,index))return;return;}
   if(stat.status!="missing"&&stat.status!="foreign") {item.phase="unknown";item.error=stat.error?stat.error:Error{"publication_unknown","Couldn't confirm whether the earlier upload succeeded. Check the destination before selecting Retry upload.",{}};commit(item,index);return;}
   if(auto error=artifacts.verify(item.uploadArtifact())) {item.phase="unknown";item.error=error;commit(item,index);return;}
  }
  if(item.uploadStatus=="unknown"&&!settling) {
   item.phase="reconciling";show(item,index);
   auto stat=storageRequest<StatResult>([&](auto cb) {auto request=item.uploadRequest(op.destination,op.requestTimeoutMs);request.cancellation=cancel;provider->reconcile(request,cb);});
   if(stat.status=="own"&&!stat.receipt.validate()&&stat.receipt.publicationId==item.publicationId&&stat.receipt.destination.toJson()==op.destination.toJson()) {item.receipt=stat.receipt;item.uploadStatus="uploaded";item.error={};item.uploadJournal={};if(!commit(item,index))return;return;}
   {auto fields=itemFields(op,item);fields["status"]=stat.status;if(stat.error)fields["error"]=logging::errorFields(stat.error);logging::write(providerLog(op.destination.providerId),stat.status=="own"||stat.status=="missing"?QtInfoMsg:QtWarningMsg,"Uncertain upload reconciled",fields);}
   if(stat.status!="missing") {item.phase="unknown";item.error=stat.error?stat.error:Error{"publication_unknown","Couldn't confirm whether the earlier upload succeeded. Check the destination before selecting Retry upload.",{}};if(!commit(item,index))return;return;}
   item.uploadStatus="stored";if(auto error=artifacts.verify(item.uploadArtifact())) {item.uploadStatus="failed";item.error=error;commit(item,index);return;}if(!commit(item,index))return;
  }
  if(item.encrypted()&&item.encrypt.value("form")=="split"&&!sendData(item,index,cancel))return;
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
    // The sharing step below starts with the first file once the last has been uploaded.
    request.linkFollows=capabilities.sharing&&index==op.items.size()-1&&(publicationMode!="private"||item.shareIntent=="create");
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
    auto e=uploaded.receipt.validate();if(!e&&(uploaded.receipt.publicationId!=item.publicationId||uploaded.receipt.destination.toJson()!=op.destination.toJson()))e={"receipt_invalid","The destination's reply didn't match this upload. Check the destination before retrying.",{}};
    if(e) {item.uploadStatus="unknown";item.error=e;}else {item.receipt=uploaded.receipt;item.uploadStatus="uploaded";item.error={};item.uploadJournal={};}if(!commit(item,index))return;break;
   }
   if(uploaded.status=="unknown") {item.uploadStatus="unknown";if(!commit(item,index))return;break;}
   // A failure before the provider settled the journaled upload (its worker, credentials or the
   // retained file) leaves the item unknown with its journal and this error, never failed.
   if(settling&&uploaded.status=="failed"&&!item.uploadJournal.isEmpty()) {item.uploadStatus="unknown";item.phase="unknown";if(!commit(item,index))return;break;}
   // Cancelling one file leaves it pending for Retry; the rest of the batch continues.
   if(uploaded.status=="failed"&&cancel->load()&&!job.cancelled->load()) {item.uploadStatus=undone=="unknown"?undone:QStringLiteral("cancelled");item.phase="cancelled";item.error={"cancelled","Upload cancelled. Select Retry upload to upload this file.",{}};commit(item,index);break;}
   if(uploaded.status!="collision") {item.uploadStatus=undone;if(!commit(item,index))return;break;}
   // Reconcile our publication before changing any proposed target after a race, unless
   // the provider already did: its collision means another object holds the key (#1627).
   item.phase="reconciling";show(item,index);
   auto stat=capabilities.settlesUploads?StatResult{"foreign",{},{}}:storageRequest<StatResult>([&](auto cb) {auto request=item.uploadRequest(op.destination,op.requestTimeoutMs);request.cancellation=cancel;provider->reconcile(request,cb);});
   if(stat.status=="own"&&!stat.receipt.validate()&&stat.receipt.publicationId==item.publicationId&&stat.receipt.destination.toJson()==op.destination.toJson()) {item.receipt=stat.receipt;item.uploadStatus="uploaded";item.error={};item.uploadJournal={};if(!commit(item,index))return;break;}
   const bool unpublishedDraft=stat.status=="missing"&&uploaded.error.code=="upload_draft_conflict";
   if(stat.status!="foreign"&&!unpublishedDraft) {item.uploadStatus="unknown";item.phase="unknown";item.error=stat.error?stat.error:Error{"publication_unknown","A file with this name already exists, and put.gd couldn't confirm whether it's from this upload. Check the destination before retrying.",{}};if(!commit(item,index))return;break;}
   if(op.settings.collisionPolicy=="skip") {item.uploadStatus="skipped";if(!commit(item,index))return;break;}
   if(op.settings.collisionPolicy!="rename") {item.uploadStatus=undone;item.error={"precondition_changed","Couldn't replace the existing file because it changed at the destination. Check the destination and try again.",{}};if(!commit(item,index))return;break;}
   // The last attempt has no upload left, so no replacement name is looked for.
   if(attempts==31) {item.uploadStatus=undone;item.error={"collision_exhausted","Couldn't find an unused file name after 32 tries.",{}};commit(item,index);break;}
   NameCheck check;
   // #1729: a short random name can be taken too; draw again while the check confirms it is.
   for(int draws=0;draws<32;++draws) {
    QSet<QString> taken;{std::lock_guard items(itemsMutex);taken=otherCandidates(op.items,index);}
    item.candidate=renamedAwayFrom(op.settings,item.candidate,capabilities.maximumNameBytes,taken);item.objectReference={};item.concurrencyToken={};item.condition="absent";item.uploadStatus="stored";item.phase="preflight";if(!commit(item,index))return;
    check=storageRequest<NameCheck>([&](auto cb) {provider->checkName(op.destination,item.candidate,cb,cancel);});
    if(cancel->load()||check.error||check.status!="present")break;
   }
   if(cancel->load()) {item.uploadStatus=undone=="unknown"?undone:QStringLiteral("cancelled");item.phase="cancelled";item.error={"cancelled","Cancelled while checking for an unused file name. Select Retry upload to try again.",{}};commit(item,index);break;}
   if(check.status!="absent"||check.error||check.objectReference.isEmpty()) {item.uploadStatus=undone;item.error=check.status=="present"&&!check.error?Error{"collision_exhausted","Every generated file name was already taken at the destination.",{}}:Error{"existence_unknown","Couldn't check whether the renamed file is available at the destination.",{}};if(!commit(item,index))return;break;}item.objectReference=check.objectReference;if(!commit(item,index))return;
   guard.unlock();lock=objectLock(op.destination,item.objectReference,item.candidate);guard=std::unique_lock<std::timed_mutex>(*lock);
  }
 };
 op.phase="uploading";
 std::vector<std::thread> workers;
 for(int index=0;index<op.items.size()&&persisted;++index) {
  OperationItem item;{std::lock_guard lock(itemsMutex);item=op.items[index];}
  if(terminalUpload(item.uploadStatus)||item.uploadStatus=="failed"||!sendable(item,encrypting)) {settleTransfer(index);continue;}
  if(job.cancelled->load()) {op.cancelled=true;break;}
  const auto cancel=itemCancellation(op.id,item.artifact.id);
  // Waiting for a slot: the operation reads as queued until one of its files transfers.
  {std::lock_guard lock(itemsMutex);item.phase="queued";op.items[index]=item;bool transferring=false;for(const auto& other:std::as_const(op.items))transferring=transferring||QStringList{"uploading","finalizing","preparing"}.contains(other.phase);if(!transferring)op.phase="queued";observe(op.publicJson());}
  const bool admitted=uploadSlots_.acquire(op.id,[&job,cancel]{return job.cancelled->load()||cancel->load();});
  {std::lock_guard lock(itemsMutex);op.phase="uploading";}
  if(!admitted) {
   if(job.cancelled->load()) {op.cancelled=true;break;}
   // An unknown item cancelled before its retry began is still unknown, with its own error.
   if(item.uploadStatus!="unknown"){item.uploadStatus="cancelled";item.error={"cancelled","Upload cancelled before it started. Select Retry upload to try again.",{}};}item.phase="cancelled";commit(item,index);settled(index);continue;
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
  // A file deleted, replaced or changed at the destination since its upload gets no link (#1496).
  // The durable item state stands in when the History row was cleared.
  if(!OperationItem::shareableRemote(current.contains("remoteStatus")?current["remoteStatus"].toString():item.remoteStatus))continue;
  if(!QStringList{"not_requested","unknown"}.contains(item.shareStatus)&&!(QStringList{"private","failed"}.contains(item.shareStatus)&&item.shareIntent=="create"))continue;
  if(publicationMode=="private"&&item.shareStatus!="unknown"&&item.shareIntent!="create"){item.shareStatus="private";if(!saveItem(item))return op;continue;}
  if(!item.receipt.directUrl.isEmpty())continue;
  // An uncertain share is inspected first. On the user's Retry, an inspection that
  // finds the file unshared while the create intent is still kept goes on to make
  // that link in the same run, through the same ownership checks as any create
  // (#1399). A found link is reported, never made twice; a changed or missing file
  // ends the step. A revoke clears the intent, so it is never repeated here, and a
  // new operation (`job.acquire`) never inspects.
  // Nextcloud (#1872): the share ID an earlier step recorded goes with each request, as the last step left it.
  auto carried=HistoryService::recordedShare(item.receipt,current.value("shareProgress").toObject(),QString());
  for(QString action=item.shareStatus=="unknown"?"inspect":"create";!action.isEmpty();) {
   op.phase="sharing";item.phase="sharing";if(!saveItem(item))return op;
   const auto shareId=uuid();
   // A diagnostics package's share link expires (#123).
   const auto options=op.action==diagnosticsAction&&item.shareIntentOptions.isEmpty()?QJsonObject{{"expirationDays",diagnosticsShareDays}}:item.shareIntentOptions;
   QJsonObject doc{{"id",shareId},{"historyId",item.artifact.id},{"action",action},{"state","running"},{"status","pending"},{"receipt",item.receipt.toJson()},{"progress",carried},{"options",options}};
   if(auto e=refused(store.saveSharing(doc))){item.error=e;op.advance(OperationState::Partial);saveItem(item);return op;}
   const auto path=root_+"/history.sqlite";
   auto checkpoint=[path,&doc,refused](const QJsonObject& progress,std::function<void(Error)> ack){
    auto proposed=doc;proposed["progress"]=progress;HistoryStore durable(path);const auto e=refused(durable.saveSharing(proposed));if(!e)doc=std::move(proposed);ack(e);
   };
   ShareRequest request{shareId,doc["action"].toString(),item.receipt,doc["options"].toObject(),doc["progress"].toObject(),job.cancelled};
   auto result=storageRequest<ShareResult>([&](auto cb){provider->share(request,checkpoint,cb);});
   if(!shareResultMatchesReceipt(result,item.receipt)){result.status="unknown";result.receipt=item.receipt;result.receipt.directUrl.clear();result.expiresAt.clear();result.error={"share_identity_changed","Couldn't confirm that the share link belongs to this uploaded file.",{}};}
   if(result.status!="shared")result.receipt.directUrl.clear();
   item.receipt=result.receipt;item.shareStatus=result.status;item.phase=result.status;item.error=result.error;
   {auto fields=itemFields(op,item);fields["action"]=request.action;fields["status"]=result.status;if(result.error)fields["error"]=logging::errorFields(result.error);logging::write(providerLog(op.destination.providerId),result.status=="shared"||result.status=="private"?QtInfoMsg:QtWarningMsg,"Sharing finished",fields);}
   doc["state"]="completed";doc["status"]=result.status;doc["receipt"]=result.receipt.toJson();doc["progress"]=result.progress;doc["error"]=result.error.toJson();
   if(!result.expiresAt.isEmpty())doc["expiresAt"]=result.expiresAt;
   // What this step left is what the next one starts from: a share ID it found gone is gone.
   carried=HistoryService::recordedShare(item.receipt,result.progress,QString());
   if(auto e=refused(store.saveSharing(doc))){
    item.error=e;op.advance(OperationState::Partial);
    QPointer<OperationService> owner(this);QMetaObject::invokeMethod(this,[owner,doc,e]{if(owner)emit owner->sharingStopped(doc,e);},Qt::QueuedConnection);
    return op;
   }
   const auto committed=store.sharing(shareId);if(result.status=="shared"||result.status=="revoked"||result.status=="changed"){item.shareIntent.clear();item.shareIntentOptions={};}else if(committed["action"]=="create"){item.shareIntent="create";item.shareIntentOptions=committed["options"].toObject();}
   if(!saveItem(item))return op;
   action=action=="inspect"&&!job.acquire&&result.status=="private"&&item.shareIntent=="create"&&!job.cancelled->load()?"create":QString{};
  }
 }
 op.cancelled=op.cancelled||job.cancelled->load();
 bool partial=false;for(const auto& item:std::as_const(op.items)) if(item.uploadStatus=="failed"||item.uploadStatus=="unknown"||item.uploadStatus=="cancelled"||item.shareStatus=="failed"||item.shareStatus=="unknown"||item.shareStatus=="changed"||item.shareIntent=="create")partial=true;
 op.advance(op.cancelled?OperationState::Cancelled:partial?OperationState::Partial:OperationState::Completed);
 op.phase=op.stateName();save();return op;
}
// Lets the destination's provider get ready while the operation's content is still being
// chosen, captured or copied. What it readied and the operation never used ends with it.
void OperationService::anticipate(const Operation& op,bool final) {
 // #2079: an encrypted request contacts its destination only once it is encrypted, so one that can't
 // be encrypted reaches nothing at all; it gives up the warm-up. Where Encrypt can still be turned on
 // later (a sensitive text check's editor), the warm-up waits for enqueue(), when the choice is final.
 if(encryptWanted(op))return;
 if(!final&&op.settings.sensitiveTextCheck)return;
 if(const auto provider=providers_.find(op.destination.providerId))if(auto ready=provider->anticipate(op.destination))anticipated_.insert(op.id,std::move(ready));
}
namespace {
Error settlingError() {return {"operation_settling","Previous attempt is still finishing. Wait a moment and try again.",{}};}
}
void OperationService::releaseActive(const QString& id) {
 {std::lock_guard lock(encryptHooksMutex_);encryptChoices_.remove(id);encryptOpen_.remove(id);}
 checkPassphrases_.remove(id);
 anticipated_.remove(id);
 // Durable completion and source drain can arrive in either order. Retain the
 // bounded operation slot until both have happened; never unlock a live helper.
 if(preparedOwners_.contains(id))finishedPrepared_.insert(id);
 else {active_.remove(id);settled(id);}
}
// Settling: the slot is still held, but the outcome is out. finish() has started, or the worker has already
// stored and published the final state (run() saves it before finish() runs).
bool OperationService::settling(const QString& id) const {
 if(!active_.contains(id))return false;
 if(finishing_.contains(id))return true;
 std::lock_guard lock(observationsMutex_);const auto live=observations_.constFind(id);
 return live!=observations_.cend()&&!(*live)["state"].toString().isEmpty()&&!Operation::activeState((*live)["state"].toString());
}
void OperationService::settled(const QString& id) {
 if(finishing_.remove(id)){emit changed(id);emit observationsChanged();}
}
void OperationService::finish(Operation op,JsonReply reply) {
 emit uploadSettled(op);
 sensitiveModes_.remove(op.id);
 if(scrollEditorReleases_.contains(op.id))scrollEditorReleases_.take(op.id)();
 anticipated_.remove(op.id);clipboardImports_.remove(op.id);pumpCopies();
 finishing_.insert(op.id);
 // Its link is copied only when asked for (diagnostics.bundle copyLink).
 if(op.action==diagnosticsAction&&op.copyStatus=="pending")op.copyStatus="suppressed";
 styledEdits_.remove(op.id);
 if(editingId_==op.id){if(editor_)editor_->cancel();editor_.reset();editingId_.clear();}
 op.phase=op.stateName();
 if(selectingId_==op.id&&!preparedOwners_.contains(op.id))selectingId_.clear();
 if((op.cancelled||op.state()==OperationState::Failed||op.state()==OperationState::Interrupted)&&op.copyStatus=="pending")op.copyStatus="no_result";
 // Only a failed outcome write marks the operation; reading the saved row back is not a write (#1579).
 const auto path=databasePath();background([path,op,failReadback=failFinalReadbackForTesting_](Error& e) {HistoryStore store(path);e=store.saveOperation(op);if(e)return QJsonObject{{"writeFailed",true}};if(failReadback){e={"history_io","Synthetic readback failure",{}};return QJsonObject{};}return store.operation(op.id,&e).toJson();},[this,id=op.id,reply](Error e,QJsonObject document) {
  if(e.code=="history_io"&&document["writeFailed"].toBool())rememberUnsavedHistory(id);
  clearObservation(id);emit changed(id);emit observationsChanged();
  if(e) {logging::error(logHistory(),"Cannot record the operation result",{{"operationId",id},{"error",logging::errorFields(e)}});abandonUploadCopies(id);releaseActive(id);if(reply)reply(e,{});return;}
  const auto op=Operation::fromJson(document);
  {
   QJsonObject fields{{"operationId",op.id},{"action",op.action},{"state",op.stateName()},{"provider",op.destination.providerId},{"items",op.items.size()},{"byteCount",artifactBytes(op)}};
   const auto created=QDateTime::fromString(op.created,Qt::ISODateWithMs);if(created.isValid())fields["durationMs"]=created.msecsTo(QDateTime::currentDateTimeUtc());
   QJsonObject outcomes;for(const auto& item:op.items)outcomes[item.uploadStatus]=outcomes[item.uploadStatus].toInt()+1;fields["uploadStatus"]=outcomes;
   if(op.error)fields["error"]=logging::errorFields(op.error);
   QJsonArray failures;for(const auto& item:op.items)if(item.error)failures.append(QJsonObject{{"itemId",item.artifact.id},{"uploadStatus",item.uploadStatus},{"error",logging::errorFields(item.error)}});
   if(!failures.isEmpty())fields["itemErrors"]=failures;
   logging::write(logOperation(),op.state()==OperationState::Completed||op.state()==OperationState::Cancelled?QtInfoMsg:QtWarningMsg,"Operation finished",fields);
  }
  if(!op.active())queuePosters(op);
  if(!op.active())if(auto watcher=finishWatchers_.take(id))watcher(op);
  if(uploadCopies_.contains(id)){finishUploadCopies(op,reply);return;}
  if(op.cancelled||op.state()==OperationState::Failed||op.state()==OperationState::Interrupted||op.action==diagnosticsAction) {emit outcome(op.publicJson());emit resultReady(op.id);releaseActive(op.id);if(reply)reply({},op.publicJson());return;}
  performCopy(op,false,[this,id=op.id,reply](Error error,QJsonObject result) {releaseActive(id);if(reply)reply(error,result);});
 });
}

void OperationService::queuePosters(const Operation& op) {
 if(op.settings.previewCacheBytes<=0)return;
 for(const auto& item:op.items) {
  if(item.artifact.category!="videos"||!item.artifact.previewPath.isEmpty())continue;
  if(std::any_of(posterQueue_.cbegin(),posterQueue_.cend(),[&](const PosterTask& task){return task.itemId==item.artifact.id;}))continue;
  PosterTask task;task.operationId=op.id;task.itemId=item.artifact.id;task.budget=op.settings.previewCacheBytes;task.maximumPixels=op.settings.limits.imagePixels;task.allCores=op.settings.unhingedMode;task.waiting.start();
  posterQueue_.append(std::move(task));
 }
 pumpPosters();
}
void OperationService::makePreview(const QString& id,JsonReply reply,bool retry) {
 if(!ready_){reply(notReadyError(),{});return;}
 if(stopping_){reply({"cancelled","put.gd is shutting down.",{}},{});return;}
 const auto snapshot=settings_.snapshot();
 if(snapshot.previewCacheBytes<=0){reply({"previews_disabled","Thumbnails are turned off. Raise Thumbnail cache in Settings → Advanced → Resource limits to turn them on.",{}},{});return;}
 if(retry)previewFailures_.remove(id);
 if(const auto failed=previewFailures_.find(id);failed!=previewFailures_.end()){reply(failed.value(),{});return;}
 // Asked for again (or already queued after its operation): answer both when it is made.
 for(auto& task:posterQueue_)if(task.itemId==id&&task.onDemand){task.replies.append(std::move(reply));return;}
 const auto onDemand=std::count_if(posterQueue_.cbegin(),posterQueue_.cend(),[](const PosterTask& task){return task.onDemand;});
 if(onDemand>=maximumQueuedPreviews){reply({"preview_busy","Other previews are still loading. Select the item again in a moment.",{}},{});return;}
 PosterTask task;task.itemId=id;task.onDemand=true;task.budget=snapshot.previewCacheBytes;task.maximumPixels=snapshot.limits.imagePixels;task.allCores=snapshot.unhingedMode;task.waiting.start();task.replies.append(std::move(reply));
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
 if(task.onDemand&&task.waiting.elapsed()>maximumPreviewWaitMs){settlePreview(task,{"preview_busy","Other previews are still loading. Select the item again in a moment.",{}},{});pumpPosters();return;}
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
  const auto cached=item.artifact.previewPath;
  if(!cached.isEmpty()&&(QFileInfo(cached).exists()||QFileInfo(cached).isSymLink())){
   const QFileInfo file(cached);QImageReader reader(cached,"png");reader.setAutoDetectImageFormat(false);const auto size=reader.size();
   if(!file.isSymLink()&&file.size()<=8*1024*1024&&size.isValid()&&qint64(size.width())*size.height()<=16000000&&!reader.read().isNull()){result["state"]="ready";result["previewPath"]=cached;return result;}
   // Only this item's cache entry belongs to us; never remove a source or a foreign path.
   const auto expected=QDir(root+"/previews").absoluteFilePath(task.itemId+".png");
   if(QUuid(task.itemId).isNull()||QDir::cleanPath(file.absoluteFilePath())!=QDir::cleanPath(expected)||file.isSymLink()||QFileInfo(file.path()).isSymLink()||file.canonicalFilePath()==QFileInfo(item.artifact.path).canonicalFilePath()||file.canonicalFilePath()==QFileInfo(item.local.path).canonicalFilePath()||!QFile::remove(cached)){e={"preview_cache_invalid","The cached preview is invalid and could not be safely removed",{}};return QJsonObject{};}
  }
  if(op.active()){e={"operation_active","The preview will be ready once the upload finishes.",{}};return QJsonObject{};}
  // The retained artifact, or else the local copy when it is still the same file.
  Artifact source=item.artifact;QString from;
  const QFileInfo stored(source.path);
  if(stored.isFile()&&!stored.isSymLink()&&stored.size()==source.bytes){if(auto mismatch=sameFile(source,cancelled,maximumPreviewVerifyMs)){e=mismatch;return QJsonObject{};}from="artifact";}
  else if(item.local.status=="saved"&&!item.local.path.isEmpty()) {
   source.path=item.local.path;
   if(auto mismatch=sameFile(source,cancelled,maximumPreviewVerifyMs)){e=mismatch;return QJsonObject{};}
   from="local";
  }
  if(from.isEmpty()){e={"preview_source_missing","Neither the stored file nor a local copy is available",{}};return QJsonObject{};}
  ArtifactStore artifacts(root+"/artifacts");Error failure;QString made;
  if(category=="videos")made=artifacts.poster(source,task.budget,task.maximumPixels,cancelled,&failure,task.allCores);
  else if(!cancelled->load()){made=artifacts.preview(source,task.budget,task.maximumPixels);if(made.isEmpty())failure={"preview_failed","The image preview could not be made",{}};}
  else failure={"cancelled","Preview cancelled",{}};
  if(made.isEmpty()){e=failure;return QJsonObject{};}
  result["state"]="made";result["previewPath"]=made;result["source"]=from;result["elapsedMs"]=double(clock.elapsed());
  // Another action may have written the operation while the helper decoded,
  // and a retry may be running it again: add only the preview to a fresh read.
  auto fresh=store.operation(operationId,&e);if(e||fresh.active()){e={};result["stored"]=false;return result;}
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
 if(scroll_&&scroll_->activeId()==id){scroll_->command("cancel",{{"id",id}},std::move(reply));return;}
 // A text operation ends in its service, which also stops a running helper.
 if(text_&&text_->activeId()==id){text_->cancel(id,[](Error,QJsonObject){});reply({},{{"operationId",id},{"cancellationRequested",true},{"effect","Text recognition stops; nothing is copied"}});return;}
 if(recording_&&recording_->activeId()==id){recording_->command("cancel",{{"id",id}},[reply,id](Error error,QJsonObject){reply(error,error?QJsonObject{}:QJsonObject{{"operationId",id},{"cancellationRequested",true},{"effect","The recording and its working clip are discarded"}});});return;}
 if(active_.contains(id)) {active_[id]->store(true);{std::lock_guard lock(itemCancelsMutex_);for(const auto& flag:itemCancels_.value(id))flag->store(true);}uploadSlots_.wake();interruptUploadCopy(id);if(selectingId_==id)source_.cancelAcquisition();if(editingId_==id&&editor_)editor_->cancel();if(const auto check=checkSessions_.value(id))check->cancel();
  for(int index=0;index<checkQueue_.size();++index)if(checkQueue_[index].op.id==id){auto job=checkQueue_.takeAt(index);job.payload={};job.payloadLease.reset();job.op.cancelled=true;job.op.advance(OperationState::Cancelled);finish(job.op);break;}
  for(int index=0;index<queue_.size();++index)if(queue_[index].op.id==id){auto job=queue_.takeAt(index);discardCovered(job.replacedFiles);if(!job.originalPath.isEmpty()&&job.originalPath!="-")QFile::remove(job.originalPath);job.payload={};job.payloadLease.reset();job.op.cancelled=true;job.op.advance(OperationState::Cancelled);finish(job.op);break;}
  reply({},{{"operationId",id},{"cancellationRequested",true},{"effect","Already sent network requests cannot be rolled back"}});return;}
 // A finished operation keeps its outcome: marking it cancelled would hide a live
 // link or a failure in History. Only a row a failed journal write left active is.
 const auto path=databasePath();background([path,id](Error& e) {HistoryStore store(path);auto op=store.operation(id,&e);if(e)return QJsonObject{};if(!op.active()){e={"operation_inactive","This upload has already finished and can't be cancelled.",{{"state",op.stateName()}}};return QJsonObject{};}op.cancelled=true;op.advance(OperationState::Cancelled);op.phase="cancelled";for(auto& item:op.items)item.phase=item.uploadStatus;e=store.saveOperation(op);return op.publicJson();},reply);
}
namespace {
Error unsavedHistoryError() {return {"history_io","Couldn't save this upload's result to History. Restart put.gd to finish saving it, then try again.",{}};}
}
void OperationService::rememberUnsavedHistory(const QString& id) {
 unsavedHistory_.insert(id);
 if(!unsavedHistoryNotified_){unsavedHistoryNotified_=true;emit historyResultsUnsaved();}
}
void OperationService::retry(const QString& id,JsonReply reply,bool sharingOnly) {
 // Its saved row is stale until a restart recovers it: retrying from it could upload again (#1579).
 if(unsavedHistory_.contains(id)){reply(unsavedHistoryError(),{});return;}
 // This operation's own state answers first: a Retry refused as settling or busy is never queued behind cleanup or
 // turned into queue_full (#1578).
 if(active_.contains(id)) {reply(settling(id)?settlingError():Error{"operation_busy","This upload is already running.",{}},{});return;}
 // #2079: Forget key is writing this operation's record; a Retry now could put the key back.
 if(held_.contains(id)){reply({"operation_busy","History is changing this upload. Try again in a moment.",{}},{});return;}
 if(cleanupInProgress_&&automaticCleanup_){cleanupStop_->store(true);afterCleanup_.append([this,id,reply,sharingOnly]{retry(id,reply,sharingOnly);});return;}
 if(cleanupInProgress_){reply({"cleanup_busy","Temporary files are being cleared. Wait a moment and try again.",{}},{});return;}
 if(!ready_) {reply(notReadyError(),{});return;}
 const auto limits=settings_.snapshot().limits;if(active_.size()>=limits.pendingOperations+limits.concurrentUploads) {reply({"queue_full","Too many actions are in progress. Wait for some to finish, or raise Queued actions in Settings → Advanced → Resource limits.",{}},{});return;}
 auto reserved=std::make_shared<std::atomic_bool>(false);active_.insert(id,reserved);const auto path=databasePath();background([path,id](Error& e) {HistoryStore store(path);return store.operation(id,&e).toJson();},[this,reply,id,reserved,path,sharingOnly](Error e,QJsonObject doc) {
  if(e) {active_.remove(id);reply(e,{});return;}auto op=Operation::fromJson(doc);if(op.items.isEmpty()) {active_.remove(id);reply({"artifact_missing","The local file for this upload is gone, so it can't be retried. Start a new capture or upload.",{}},{});return;}
  auto provider=providers_.find(op.destination.providerId);const bool sharing=provider&&provider->capabilities(op.destination).sharing;auto mode=routeOf(op).publicationMode;if(mode=="default"&&provider)mode=provider->defaultPublicationMode(op.destination);
  // The whole operation is checked here, not the History rows a client happened to load.
  if(sharingOnly)for(const auto& i:std::as_const(op.items))if(!terminalUpload(i.uploadStatus)){active_.remove(id);reply({"retry_would_upload","A file in this batch still needs to be uploaded. Select Retry upload instead.",{{"itemId",i.artifact.id}}},{});return;}
  bool pending=false;for(const auto& i:std::as_const(op.items))if(!terminalUpload(i.uploadStatus)||(sharing&&i.uploadStatus=="uploaded"&&(i.shareStatus=="unknown"||(i.shareStatus=="private"&&i.shareIntent=="create")||(mode=="share"&&(i.shareStatus=="not_requested"||(i.shareStatus=="failed"&&i.shareIntent=="create"))))))pending=true;
  if(!pending) {active_.remove(id);performCopy(op,true,reply);return;}
  if(provider)if(auto timeout=provider->retryRequestTimeout(op.destination,settings_.snapshot().profiles.value(op.destination.profileId)))op.requestTimeoutMs=std::max(op.requestTimeoutMs.value_or(0),*timeout);
  if(!op.advance(OperationState::Stored)){active_.remove(id);reply({"operation_busy","This upload is already running.",{}},{});return;}
  op.cancelled=reserved->load();op.error={};op.phase="queued";op.invocation=++invocation_;op.clipboardRevision=source_.clipboardRevision();op.copyStatus="pending";beginUploadCopies(op);
  background([path,op](Error& error){HistoryStore store(path);error=store.saveOperation(op);return QJsonObject{};},[this,reply,op,reserved,sharingOnly](Error error,QJsonObject)mutable{
   if(error){abandonUploadCopies(op.id);releaseActive(op.id);reply(error,{});return;}
   op.cancelled=reserved->load();Job job{op,{},false,reserved,{}};job.sharingOnly=sharingOnly;enqueue(std::move(job));reply({},{{"operationId",op.id},{"retryAccepted",true}});
   logging::info(logOperation(),"Retry accepted",{{"operationId",op.id},{"action",op.action},{"provider",op.destination.providerId},{"items",op.items.size()}});
  });
 });
}
void OperationService::retryLocalSave(const QString& id,JsonReply reply) {
 if(unsavedHistory_.contains(id)){reply(unsavedHistoryError(),{});return;}
 if(active_.contains(id)){reply(settling(id)?settlingError():Error{"operation_busy","This file is already being saved or uploaded.",{}},{});return;}
 if(held_.contains(id)){reply({"operation_busy","History is changing this upload. Try again in a moment.",{}},{});return;}
 if(cleanupInProgress_&&automaticCleanup_){cleanupStop_->store(true);afterCleanup_.append([this,id,reply]{retryLocalSave(id,reply);});return;}
 if(cleanupInProgress_){reply({"cleanup_busy","Temporary files are being cleared. Wait a moment and try again.",{}},{});return;}
 if(!ready_){reply(notReadyError(),{});return;}
 const auto limits=settings_.snapshot().limits;
 if(active_.size()>=limits.pendingOperations+limits.concurrentUploads){reply({"queue_full","Too many actions are in progress. Wait for some to finish, or raise Queued actions in Settings → Advanced → Resource limits.",{}},{});return;}
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
  e=store.saveOperation(op);return QJsonObject{{"operation",op.publicJson()},{"writeFailed",bool(e)}};
 },[this,id,reply](Error e,QJsonObject result) {
  // A failed read of the operation changed nothing; only a failed write of its outcome marks it (#1579).
  // The snapshot stays with the failure: the tray names the files that failed from it.
  if(e.code=="history_io"&&result["writeFailed"].toBool())rememberUnsavedHistory(id);
  const auto r=result["operation"].toObject();
  clearObservation(id);emit changed(id);emit observationsChanged();releaseActive(id);
  bool failed=bool(e);for(const auto& value:r["items"].toArray())failed=failed||value.toObject()["local"].toObject()["status"]=="failed";
  if(failed)emit operationError(id,r);
  if(!e&&!r.isEmpty())emit outcome(r);
  reply(e,r);
 });
}
void OperationService::copyResult(const QString& id,JsonReply reply) {
 // #2079: it rewrites the operation's record, which Forget key may be writing.
 if(held_.contains(id)){reply({"operation_busy","History is changing this upload. Try again in a moment.",{}},{});return;}
 JsonReply pendingReply;
 if(active_.contains(id)&&!detachCompletedUploadCopies(id,pendingReply)) {reply({"operation_busy","Wait for this action to finish before copying its result.",{}},{});return;}
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
  guarded->background([path,value](Error& e) {HistoryStore store(path);e=store.saveOperation(value);return value.publicJson();},[guarded,value,reply,explicitCopy,copy](Error e,QJsonObject result) {if(!guarded)return;emit guarded->changed(value.id);if(!e){if(explicitCopy)emit guarded->copyFinished(result,copy);else emit guarded->outcome(result);emit guarded->resultReady(value.id);}if(reply)reply(e,result);});};
 if(!eligible) {
  op.copyStatus="suppressed";
  record(op,{{"suppression",explicitCopy?copyGuard(false,false,"superseded",ticket,invocation_,0,0):copyGuard(false,false,"newer_action",op.invocation,invocation_,op.clipboardRevision,source_.clipboardRevision())}});return;
 }
 QStringList urls,files;QString localKind;
 for(const auto& item:std::as_const(op.items)) {if(item.linkAvailable())urls.append(item.link());if(item.uploadStatus=="local_only"&&item.local.status=="saved") {files.append(item.local.path);localKind=item.artifact.category;}}
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
   if(!file.open(QIODevice::ReadOnly)) {e={"artifact_missing","Couldn't read the saved file to copy it.",{}};return QJsonObject{};}
   if(file.size()!=artifact.bytes) {e={"artifact_changed","The saved file changed before it could be copied.",{}};return QJsonObject{};}
   const auto bytes=file.read(artifact.bytes+1);
   if(bytes.size()!=artifact.bytes||QString::fromLatin1(QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex())!=artifact.sha256) {
    e={"artifact_changed","The saved file changed before it could be copied.",{}};return QJsonObject{};
   }
   if(localKind=="images") {
    prepared->kind="image";prepared->encodedImage=bytes;
    if(!bytes.startsWith(QByteArray::fromHex("89504e470d0a1a0a")))e={"artifact_invalid","Couldn't copy the saved image because it isn't a valid PNG.",{}};
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
  if(!eligible()){complete({"copy_suppressed","Image wasn't copied because your clipboard changed or another copy started.",{}});return;}
  background([path,id,prepared,preparation](Error& e) {
  HistoryStore store(path);const auto data=store.detail(id,&e);if(e)return QJsonObject{};
  const auto local=data["local"].toObject();
  if(local["status"]!="saved"||!data["mime"].toString().startsWith("image/")){e={"image_unavailable","Copy image needs a local copy of this image. Turn on Keep a local copy in Settings → Capture for future captures.",{}};return QJsonObject{};}
  QFile file(local["path"].toString());
#ifdef Q_OS_UNIX
  const int fd=::open(QFile::encodeName(file.fileName()).constData(),O_RDONLY|O_CLOEXEC|O_NONBLOCK|O_NOFOLLOW);struct stat identity{};
  if(fd<0||::fstat(fd,&identity)||!S_ISREG(identity.st_mode)){if(fd>=0)::close(fd);e={"artifact_missing","Couldn't read the local copy of this image.",{}};return QJsonObject{};}
  if(!file.open(fd,QIODevice::ReadOnly,QFileDevice::AutoCloseHandle)){::close(fd);e={"artifact_missing","Couldn't read the local copy of this image.",{}};return QJsonObject{};}
#else
  if(!QFileInfo(file).isFile()||!file.open(QIODevice::ReadOnly)){e={"artifact_missing","Couldn't read the local copy of this image.",{}};return QJsonObject{};}
#endif
  constexpr qint64 inputLimit=32*1024*1024;
  if(file.size()<1||file.size()>inputLimit){e={"image_limit","This image is too large to copy to the clipboard.",{}};return QJsonObject{};}
  const auto bytes=file.read(inputLimit+1);
  if(bytes.size()!=data["bytes"].toInteger()||QString::fromLatin1(QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex())!=data["sha256"].toString()){e={"artifact_changed","The local copy of this image changed since it was saved.",{}};return QJsonObject{};}
  // Sixteen-bit RGBA uses eight bytes per pixel; retain that precision.
  ResourceLimits limits;limits.imagePixels=16000000;limits.imageBytes=128*1024*1024;limits.maxFileBytes=129*1024*1024;
  // Generic image codecs stay in the transient GUI worker; the resident owns
  // only the bounded full-size PNG, never a preview or a persistent GUI cache.
  prepared->kind="image";
#ifdef Q_OS_UNIX
  prepared->encodedImage=ImageJob().decodePng(bytes,limits,e,preparation->cancelled);
#else
  QBuffer input;input.setData(bytes);input.open(QIODevice::ReadOnly);QImageReader reader(&input);const auto size=reader.size();
  if(!size.isValid()||qint64(size.width())*size.height()>limits.imagePixels){e={"image_limit","This image is too large to copy to the clipboard.",{}};return QJsonObject{};}
  const auto image=reader.read();QBuffer output(&prepared->encodedImage);output.open(QIODevice::WriteOnly);
  if(image.isNull()||image.sizeInBytes()>limits.imageBytes||!image.save(&output,"PNG")||prepared->encodedImage.size()>limits.maxFileBytes)e={"image_read","Couldn't prepare the local image for copying.",{}};
#endif
  return QJsonObject{};
  },[complete,eligible,preparation](Error e,QJsonObject){if(preparation->cancelled->load()||!eligible())e={"copy_suppressed","Image wasn't copied because your clipboard changed or another copy started.",{}};complete(e);});
 },{},preparation);
}
void OperationService::copyHistoryUrl(const QString& url,JsonReply reply,quint64 ticket) {if(ticket==0)ticket=invalidateCopyEligibility();if(ticket!=invocation_) {reply({"copy_suppressed","Link wasn't copied because another copy or action started.",{}},{});return;}if(url.isEmpty()) {reply({"link_unavailable","This item has no available link.",{}},{});return;}CopyPayload p;p.kind="text";p.text=url;deliverCopy(p,[reply](Error e) {reply(e,{{"copied",!e}});},[this,ticket]{return ticket==invocation_;});}
void OperationService::copyHistoryQr(QByteArray png,JsonReply reply,quint64 ticket,quint64 revision) {
 if(ticket!=invocation_||revision!=source_.clipboardRevision()){reply({"copy_suppressed","QR code wasn't copied because your clipboard changed or another copy started.",{}},{});return;}
 CopyPayload payload;payload.kind="image";payload.encodedImage=std::move(png);
 deliverCopy(payload,[reply](Error e){reply(e,{{"copied",!e}});},[this,ticket,revision]{return ticket==invocation_&&revision==source_.clipboardRevision();});
}
QJsonObject OperationService::capabilities() const {
#ifdef Q_OS_UNIX
 const bool editor=QFileInfo(QCoreApplication::applicationDirPath()+"/putgd-ui").isExecutable();
#else
 const bool editor=false;
#endif
 auto sourceCapabilities=source_.capabilities();sourceCapabilities["scroll"]=scroll_->capabilities();
 return {{"source",sourceCapabilities},{"providers",providers_.capabilities()},
  {"editor",QJsonObject{{"available",editor},{"admissionPixels",double(EditorJob::admissionPixels)},{"open",!editingId_.isEmpty()},
   {"sources",QJsonArray{"region","window","desktop","clipboard","file","scroll"}},{"fileFormats",QJsonArray{"png","jpeg"}},{"output","png"},
   {"shapes",QJsonArray{"redact","blur","arrow","rectangle","text","highlight","step","pen","ellipse","line"}},{"blurStyles",QJsonArray{"blur","pixelate"}},
   {"commands",QJsonArray{"inspect","apply","preview","saveCopy","copy","fileDialog","view","encrypt","submit","cancel","raise"}}}},
  {"scroll",scroll_->capabilities()},{"recording",recording_->capabilities()},{"text",text_->capabilities(textSourceErrors())}};
}
QJsonObject OperationService::diagnostics() const {
#ifdef __GLIBC__
 constexpr bool reclaimSupported=true;
#else
 constexpr bool reclaimSupported=false;
#endif
 const auto build=QJsonDocument::fromJson(buildManifestJson()).object();
 const auto logStatus=logging::status();
 return {{"schemaVersion",1},{"build",build},{"release",releaseLink(build).toJson()},{"backend","Qt"},{"qtVersion",QT_VERSION_STR},{"activeOperations",active_.size()},{"text",QJsonObject{{"active",text_->active()},{"resultCacheBytes",double(text_->cachedBytes())}}},{"previews",QJsonObject{{"running",posterRunning_},{"queued",posterQueue_.size()}}},{"uploadSlots",uploadSlots_.diagnostics()},{"journalReady",ready_},{"unsavedHistoryResults",int(unsavedHistory_.size())},{"artifactCleanup",QJsonObject{{"running",cleanupInProgress_},{"automatic",automaticCleanup_},{"intervalMs",cleanupSchedule_.intervalMs},{"last",lastCleanup_.isEmpty()?QJsonValue(QJsonValue::Null):QJsonValue(lastCleanup_)},{"incompleteUploadNotice",lastIncompleteNotice_.isEmpty()?QJsonValue(QJsonValue::Null):QJsonValue(lastIncompleteNotice_)}}},{"capabilities",capabilities()},{"storage",providers_.diagnostics()},{"locations",locations_},
  {"logging",logStatus},{"loggingDegraded",logStatus["degraded"].toBool()},{"uncleanExit",RunMarker::report()},
  {"memory",QJsonObject{{"allocatorReclamationSupported",reclaimSupported},{"allocatorReclamationPending",heapReclamationPending_},{"allocatorReclamationCount",qint64(heapReclamationCount_)},{"lastAllocatorReclamationUsec",lastHeapReclamationUsec_},{"imagePageReleasePending",imagePagesPending_},{"imagePageReleaseCount",qint64(imagePageReleaseCount_)},{"lastImagePageReleaseUsec",lastImagePageReleaseUsec_},{"lastImagePageReleaseBytes",lastImagePageReleaseBytes_}}}};
}
void OperationService::abandonUploads(HistoryStore& store,Operation& op,QJsonObject& outcome) {
 QSet<QString> changed;
 // The user hears of each kept upload once (#226), with at most `bytes` stored in its parts.
 const auto notify=[&](const Error& error,double bytes){
  auto notice=outcome["incompleteUploadNotice"].toObject();
  const auto add=[&](const char* key,const QString& value){auto values=notice[key].toArray();if(!value.isEmpty()&&!values.contains(value))values.append(value);notice[key]=values;};
  notice["count"]=notice["count"].toInt()+1;notice["bytes"]=notice["bytes"].toDouble()+bytes;
  add("providers",op.destination.providerId);add("profiles",op.destination.profileId);add("buckets",op.destination.settings.value("bucket").toString());
  if(!notice.contains("error"))notice["error"]=error.toJson();
  outcome["incompleteUploadNotice"]=notice;
 };
 for(auto& item:op.items) {
  // #2079: a split encrypted link's data file may hold an unfinished multipart upload of its own.
  if(auto data=item.encrypt.value("data").toObject();!data.value("uploadJournal").toObject().value("uploadId").toString().isEmpty()&&data.value("uploadStatus")!="uploaded") {
   auto provider=providers_.find(op.destination.providerId);
   Error error=provider?Error{}:Error{"provider_unavailable","Destination is unavailable. Restore access to it in Destinations.",{}};
   if(provider){auto lock=objectLock(op.destination,data.value("objectReference").toObject(),data.value("candidate").toString());std::unique_lock guard(*lock);
    error=storageRequest<Error>([&](auto cb){provider->abandonUpload(op.destination,data.value("uploadJournal").toObject(),cb);});}
   if(!error){data["uploadJournal"]=QJsonObject{};data.remove("incompleteUploadReported");item.encrypt["data"]=data;changed.insert(item.artifact.id);outcome["abortedUploads"]=outcome["abortedUploads"].toInt()+1;}
   else{
    // Its own once-only mark, beside the data file's journal: the page's upload has its own.
    const bool reported=data.value("incompleteUploadReported").toBool();data["incompleteUploadReported"]=true;item.encrypt["data"]=data;
    item.error.details["incompleteUpload"]=true;item.error.details["abortError"]=error.toJson();changed.insert(item.artifact.id);
    auto kept=outcome["incompleteUploads"].toArray();if(kept.size()<100)kept.append(QJsonObject{{"operationId",op.id},{"itemId",item.artifact.id},{"error",error.toJson()},{"reported",reported}});outcome["incompleteUploads"]=kept;
    if(!reported)notify(error,data.value("uploadJournal").toObject().value("bytes").toDouble(data.value("artifact").toObject().value("bytes").toDouble()));
   }
  }
  if(item.uploadJournal["uploadId"].toString().isEmpty()||item.uploadStatus=="uploaded")continue;
  auto provider=providers_.find(op.destination.providerId);
  Error error=provider?Error{}:Error{"provider_unavailable","Destination is unavailable. Restore access to it in Destinations.",{}};
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
   // The upload's whole size: at most this much is stored in its parts.
   if(!reported)notify(error,item.uploadJournal.value("bytes").toDouble(double(item.artifact.bytes)));
   fields["error"]=logging::errorFields(error);
   logging::warning(providerLog(op.destination.providerId),"Incomplete upload kept",fields);
  }
  changed.insert(item.artifact.id);
 }
 if(!changed.isEmpty())if(auto error=store.checkpointOperation(op,changed,&providers_))logging::warning(logOperation(),"Upload journal not saved",{{"operationId",op.id},{"error",logging::errorFields(error)}});
}
void OperationService::cleanup(JsonReply reply) {
 if(cleanupInProgress_&&automaticCleanup_){cleanupStop_->store(true);afterCleanup_.append([this,reply]{cleanup(reply);});return;}
 if(cleanupInProgress_){reply({"cleanup_busy","Temporary files are being cleared. Wait a moment and try again.",{}},{});return;}
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
  auto pending=std::exchange(afterCleanup_,{});for(const auto& next:std::as_const(pending))next();
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
  :providers.contains("cloudflare-r2")
  ?QStringLiteral("Give put.gd's R2 API token Object Read & Write on the bucket, or add a lifecycle rule to the bucket that aborts incomplete multipart uploads after a few days.")
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
 // The pass frees its journal connection's page cache; hand that back too.
 requestHeapReclamation();
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
  bool successful=true;for(const auto& item:std::as_const(op.items))if(item.uploadStatus=="unknown"||item.uploadStatus=="uploading")successful=false;
  const bool finished=op.state()==OperationState::Completed||op.state()==OperationState::Partial||op.state()==OperationState::Failed||op.state()==OperationState::Cancelled||op.state()==OperationState::Interrupted;
  bool uploaded=true;for(const auto& item:std::as_const(op.items))if(!terminalUpload(item.uploadStatus))uploaded=false;
  bool archivePending=false;for(const auto& item:std::as_const(op.items))if(item.local.status=="failed")archivePending=true;
  const auto date=QDateTime::fromString(op.created,Qt::ISODateWithMs);
  const qint64 age=date.isValid()?date.daysTo(pass.now):-1;
  const bool expired=pass.failedArtifactDays>0&&age>=pass.failedArtifactDays;
  if(finished&&successful&&((!pass.automatic&&uploaded&&!archivePending)||expired)) {
   // Without its artifact no Retry can resume the upload: abort it first, best effort.
   abandonUploads(store,op,outcome);visited.insert(name);
   if(auto error=artifacts.remove(name)) {e=error;break;}++removed;
   logging::info(logOperation(),expired?"Expired artifacts removed":"Artifacts removed",{{"operationId",op.id},{"trigger",trigger},{"state",op.stateName()},{"items",op.items.size()},{"ageDays",age},{"failedArtifactDays",pass.failedArtifactDays}});
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
   {"more",outcome.value("more").toBool()},{"stopped",outcome.value("stopped").toBool()},{"elapsedMs",outcome["elapsedMs"].toInteger()}});
 return outcome;
}
}
