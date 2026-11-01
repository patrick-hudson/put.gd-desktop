#pragma once
#include "core/HistoryStore.h"
#include "core/EditorJob.h"
#include "RecordingService.h"
#include "core/SettingsService.h"
#include "UploadSlots.h"
#include <QElapsedTimer>
#include <QObject>
#include <QQueue>
#include <QThread>
#include <QSet>
#include <QTimer>
#include <QLockFile>
#include <atomic>
#include <mutex>
#include <future>
#include <optional>
namespace putgd {
using JsonReply=std::function<void(Error,QJsonObject)>;
struct CopyPayload { QString kind,text;QStringList files;QByteArray encodedImage; };
// Calls originate on the session thread. Acquire/copy callbacks may arrive on any
// thread, including after cancellation; services marshal and lifetime-guard them.
// cancelAcquisition requests cancellation and need not synchronously drain callbacks.
// The adapter outlives its services and never reads clipboard payloads for revision queries.
class SourceAdapter {
public:
 using PreparedAcquisition=std::function<void(const ResourceLimits&,std::function<void(PayloadResult)>)>;
 virtual ~SourceAdapter()=default;
 // The resident supervisor sets the one live UI helper before it can show a
 // window, and clears it on exit. Native adapters exclude it at invocation.
 virtual void setUiProcess(qint64) {}
 void setCaptureAppearance(const QJsonObject& appearance) { captureAppearance_=appearance; }
 void setCaptureOutcome(const QString& outcome) { captureOutcome_=outcome; }
 // Region screenshots only (#629): freeze every monitor before selection and
 // cut the result from those frames. Recording selection always stays live.
 void setRegionScreenshotFirst(bool frozen) { regionScreenshotFirst_=frozen; }
 virtual quint64 clipboardRevision() const=0;
 virtual QJsonObject capabilities() const=0;
 // Freeze native target identity/layout on the session thread before persistence
 // yields. Pixel acquisition waits until both journal checkpoints are durable.
 virtual PreparedAcquisition prepareAcquisition(const QString& action) {
  return [this,action](const ResourceLimits& limits,std::function<void(PayloadResult)> reply) {
   acquire(action,limits,std::move(reply));
  };
 }
 // Preparation may show region selection, but must not read pixels until the
 // returned continuation is invoked after both durable journal checkpoints.
 virtual PreparedAcquisition prepareAcquisition(const QString& action,const ResourceLimits&) {
  return prepareAcquisition(action);
 }
 // Early cancellation/failure reports metadata only; ordinary payload delivery
 // still belongs to the durable continuation. Legacy adapters need no preview.
 virtual PreparedAcquisition prepareAcquisition(const QString& action,const ResourceLimits& limits,
                                               std::function<void(Error)>) {
  return prepareAcquisition(action,limits);
 }
 // Optional source release reports that an independent prepared helper drained.
 // Legacy adapters keep interactive ownership until ordinary completion.
 virtual PreparedAcquisition prepareAcquisition(const QString& action,const ResourceLimits& limits,
                                               std::function<void(Error)> terminal,std::function<void()> released) {
  // Keep legacy completion ownership. An adapter with an independent prepared
  // helper must override this overload and report release only after drain.
  if(released)released();
  return prepareAcquisition(action,limits,std::move(terminal));
 }
 virtual void acquire(const QString&,const ResourceLimits&,std::function<void(PayloadResult)>)=0;
 virtual void cancelAcquisition()=0;
 // Revision acknowledged by the last successful app copy, not a later owner query.
 // Adapters without an acknowledgement cannot advance automatic-copy eligibility.
 virtual std::optional<quint64> lastCopyRevision() const {return {};}
 // Cancel unsent asynchronous commits when superseded, paused for an import,
 // cancelled, or shut down. Report copy_suppressed only when no write committed.
 virtual void invalidateCopyEligibility() {}
 virtual void copy(const CopyPayload&,std::function<void(Error)>)=0;
protected:
 // Non-secret presentation context; adapters copy it when freezing each request.
 QJsonObject captureAppearance_;
 QString captureOutcome_;
 bool regionScreenshotFirst_=false;
};
class UnavailableSource final:public SourceAdapter {
public:
 quint64 clipboardRevision() const override {return 0;}
 QJsonObject capabilities() const override;
 void acquire(const QString&,const ResourceLimits&,std::function<void(PayloadResult)>) override;
 void cancelAcquisition() override {}
 void copy(const CopyPayload&,std::function<void(Error)>) override;
};
class OperationService final:public QObject {
 Q_OBJECT
public:
 OperationService(QString dataRoot,SettingsService&,ProviderRegistry&,SourceAdapter&,QObject* parent=nullptr);
 ~OperationService() override;
 // `edit` accepts options {source: region|window|desktop|clipboard|file, path}.
 void invoke(const QString& action,JsonReply,const QJsonObject& options={});
 // Relays one editor.* command to the open editor for this operation.
 void editorCommand(const QString& id,const QString& command,const QJsonObject& params,JsonReply);
 QString editingOperation() const {return editingId_;}
 // Relays one recording.* command to the shared recording service.
 void recordingCommand(const QString& command,const QJsonObject& params,JsonReply);
 QString recordingActiveId() const;
 // Tests inject a fake backend; production installs the helper supervisor.
 void setRecordingBackend(std::unique_ptr<RecordingBackend> backend) {recording_->setBackend(std::move(backend));}
 // The interface's resolved scheme ("dark" or "light"), passed to each editor
 // helper so Edit image matches the other windows (#198's rule for putgd-record).
 void setEditorAppearance(const QString& scheme) {editorAppearance_=scheme=="light"?QStringLiteral("light"):QStringLiteral("dark");if(editor_)editor_->setAppearance(editorAppearance_);}
 RecordingService& recording() {return *recording_;}
 // Tests may run a fixture helper; production always uses the sibling putgd-ui.
 void setEditorHelper(QString path) {editorHelper_=std::move(path);}
 // The internal action of a diagnostics package upload (#123), outside the
 // sharing actions: recorded in history and deletable like any upload.
 static constexpr const char* diagnosticsAction="diagnostics";
 static constexpr int diagnosticsShareDays=7;
 // Uploads one finished diagnostics package to a destination through the
 // normal pipeline as putgd-diagnostics-<uuid>.zip: no naming template, no
 // local archive copy, never an overwrite, a link requested (expiring after
 // diagnosticsShareDays where the provider makes share links). `accepted`
 // answers when the operation is durable; `finished` gets its terminal state.
 void uploadDiagnostics(const QString& path,const QString& profileId,JsonReply accepted,std::function<void(const Operation&)> finished);
 // Files and folders chosen with "Upload with put.gd" in a file manager (#1105),
 // outside the sharing actions like a diagnostics upload: the installation is
 // checked first, then every path, folders are expanded, and the destination is
 // resolved once and stored with the operation. Never a local-only save. Links
 // are copied as for a capture. Answers with the operation ID once it is durable.
 static constexpr const char* fileManagerAction="files";
 void uploadFiles(const QStringList& paths,const QString& profileId,const QString& installation,JsonReply);
 // Refuses upload.files with a retryable error (initializing) while it answers one,
 // e.g. while the startup read of saved credentials is still running.
 void setUploadGate(std::function<Error()> gate) {uploadGate_=std::move(gate);}
 // This resident's installation class ("system" or "appimage"); tests may set it.
 void setInstallation(QString installation) {installation_=std::move(installation);}
 QString installation() const {return installation_;}
 void inspect(const QString& id,JsonReply);
 void recent(JsonReply);
 // Session-thread snapshot of bounded live observations; no SQL or provider IO.
 QJsonArray activeObservations() const;
 bool storageActive() const {return storageRequests_>0;}
 Settings settingsSnapshot() const {return settings_.snapshot();}
 // The tray needs only the desktop report, without querying storage providers.
 QJsonObject shortcutStatus() const {return source_.capabilities()["shortcuts"].toObject();}
 QString settingsRevision() const {return settings_.revision();}
 // With an item ID, cancels only that file's upload; the batch's other files continue.
 void cancel(const QString& id,JsonReply,const QString& itemId={});
 void retry(const QString& id,JsonReply);
 void retryLocalSave(const QString& id,JsonReply);
 void copyResult(const QString& id,JsonReply);
 void copyHistoryImage(const QString& id,JsonReply);
 void copyHistoryUrl(const QString& url,JsonReply,quint64 ticket=0);
 // A history item's missing preview (#190): an image's or a video's that was
 // never made, failed or was removed by cleanup, made from the retained
 // artifact or a verified local copy through the one-at-a-time poster queue.
 // Answers with the preview path, or why there is none.
 // retry: an explicit Retry preview forgets an earlier failure for this item.
 void makePreview(const QString& historyId,JsonReply,bool retry=false);
 quint64 invalidateCopyEligibility();
 void cleanup(JsonReply);
 // Applies retention.failedArtifactDays without the user (#202): a minute after
 // journal recovery and then hourly. A pass removes at most `batch` expired
 // operations' artifacts (aborting their retained uploads, #146), and follows up a
 // minute later when more remain. A Retry or a manual cleanup that arrives stops the
 // pass at the next operation and runs once it ends; shutdown stops it too.
 void cleanupExpired();
 struct CleanupSchedule {int startupDelayMs=60*1000,intervalMs=60*60*1000,followUpMs=60*1000,batch=50;};
 // Tests substitute the schedule (before ready) and the clock retention compares against.
 void setCleanupScheduleForTesting(CleanupSchedule schedule) {cleanupSchedule_=schedule;cleanupTimer_.setInterval(schedule.intervalMs);}
 void setClockForTesting(std::function<QDateTime()> clock) {clock_=std::move(clock);}
 void shutdown();
 QJsonObject capabilities() const;
 QJsonObject diagnostics() const;
 // The active configuration/data roots and any retained earlier layout (#56).
 void setLocations(QJsonObject locations) {locations_=std::move(locations);}
 QString databasePath() const {return root_+"/history.sqlite";}
 // Free space, system reserve and in-flight imports on the upload store's filesystem.
 QJsonObject artifactDisk() const {return ArtifactStore::diskBudget(root_+"/artifacts").toJson();}
 ProviderRegistry& providers() {return providers_;}
 // Shared services run SQL, encoding, and file/network work on workers.
 void background(std::function<QJsonObject(Error&)>,JsonReply);
 std::shared_ptr<std::mutex> objectLock(const Destination&,const QJsonObject&,const QString& candidate);
 // A provider that takes one request at a time for a destination (its credential
 // reference) is leased to one operation's provider work at a time.
 std::shared_ptr<DestinationLease> destinationLease(const Destination&);
 bool ready() const {return ready_;}
signals:
 void initialized();
 void changed(const QString& id);
 // One notification for a live observation batch, independent of durable results.
 void observationsChanged();
 void storageActivityChanged();
 // An attempt failed outside normal terminal-result delivery (e.g. local retry).
 // Deliver its durable snapshot with the signal: inspecting it asynchronously
 // can report an already acknowledged failure after a subsequent retry.
 void operationError(const QString& operationId,const QJsonObject& operation);
 // A terminal/copy result is durably observable; progress never emits this.
 void resultReady(const QString& id);
 void sharingStopped(const QJsonObject& document,const putgd::Error& error);
 // An automatic cleanup kept uploads the destination refused to abort that the user
 // has not heard of yet (#226): {count, bytes, buckets, labels, providers, error, hint, message}.
 void incompleteUploadsKept(const QJsonObject& notice);
 // upload.files found no destination to upload to (#1105): Settings should open.
 void destinationRequired();
private:
 // Scoped provider calls count requests, independently of queue/encoding/journal work.
 class StorageActivity {
 public:
  explicit StorageActivity(OperationService& owner,bool active=true):owner_(owner) {set(active);}
  ~StorageActivity() {set(false);}
  void set(bool active) {if(active!=active_){active_=active;owner_.storageActivityDelta(active?1:-1);}}
 private:
  OperationService& owner_;bool active_=false;
 };
 void storageActivityDelta(int);
 template<class T,class Start> T storageRequest(Start);
 // Aborts the remote uploads failed items' journals still hold once their artifacts
 // retire (#146); counts them in `outcome` and records refusals on the items.
 void abandonUploads(HistoryStore&,Operation&,QJsonObject& outcome);
 // One cleanup pass, run on a worker. A manual pass also retires finished uploads
 // and clears the preview cache; an automatic one applies only the retention age.
 struct CleanupPass {bool automatic=false;int failedArtifactDays=0,batch=0;QDateTime now;QStringList activeIds;std::shared_ptr<std::atomic_bool> stop;};
 QJsonObject cleanArtifacts(const CleanupPass&,Error&);
 void startCleanup(bool automatic,JsonReply);
 void finishCleanup(const QJsonObject& outcome);
 // Words the pass's newly kept uploads for the user, the log and diagnostics (#226).
 void announceIncompleteUploads(QJsonObject& outcome,bool automatic);
 QJsonObject lastIncompleteNotice_;
 QTimer cleanupTimer_;
 std::function<QDateTime()> clock_=[]{return QDateTime::currentDateTimeUtc();};
 bool automaticCleanup_=false;CleanupSchedule cleanupSchedule_;
 std::shared_ptr<std::atomic_bool> cleanupStop_;
 // Retry and manual cleanup requests that arrived during an automatic pass.
 QList<std::function<void()>> afterCleanup_;
 QJsonObject lastCleanup_;
 struct Job {Operation op;ClipboardPayload payload;bool acquire=false;std::shared_ptr<std::atomic_bool> cancelled;std::shared_ptr<qint64> payloadLease;QStringList origins={};};
 void observe(const QJsonObject&);
 QJsonObject currentObservation(QJsonObject) const;
 void clearObservation(const QString&);
 void enqueue(Job);
 void pump();
 bool heapReclamationIdle() const;
 void scheduleHeapReclamation();
 Operation run(Job);
 void finish(Operation,JsonReply={});
 // #112: video poster frames, made one item at a time after its operation
 // ends, so a slow or failing decode never delays its upload, link or copied
 // result. #190: a preview History asks for on selection goes ahead of those
 // and answers when it is made.
 struct PosterTask {QString operationId,itemId;bool onDemand=false;qint64 budget=0,maximumPixels=0;QElapsedTimer waiting;QList<JsonReply> replies;};
 void queuePosters(const Operation&);
 void pumpPosters();
 void settlePreview(const PosterTask&,Error,QJsonObject);
 QList<PosterTask> posterQueue_;bool posterRunning_=false;
 // Items whose preview could not be made in this run: selecting one again
 // answers at once instead of decoding the same file again.
 QHash<QString,Error> previewFailures_;
 std::shared_ptr<std::atomic_bool> posterCancel_=std::make_shared<std::atomic_bool>(false);
 void releaseActive(const QString&);
 void performCopy(Operation,bool explicitCopy,JsonReply,quint64 ticket=0);
 // Which guard refused a copy, for the putgd.clipboard log record (#473): the
 // clipboard revision moved, the epoch/invocation moved (`epochGuard` names
 // what that means at the call site), the operation was cancelled or the
 // resident is stopping. Empty while the copy is still eligible.
 static QJsonObject copyGuard(bool stopping,bool cancelled,const char* epochGuard,quint64 epoch,quint64 currentEpoch,quint64 revision,quint64 currentRevision);
 // A copy_suppressed answer from the native clipboard itself (ownership moved
 // before the result was acknowledged); its message never carries the payload.
 static QJsonObject nativeCopyGuard(const Error&);
 void deliverCopy(const CopyPayload&,std::function<void(Error)>,std::function<bool()> eligible={},bool upload=false);
 struct CopyPreparationState {quint64 revision=0;std::shared_ptr<std::atomic_bool> cancelled=std::make_shared<std::atomic_bool>(false);};
 using CopyPreparation=std::function<void(std::function<void(Error)>)>;
 void queueCopy(std::function<CopyPayload()>,std::function<void(Error)>,std::function<bool()>,bool,CopyPreparation={},QStringList={},std::shared_ptr<CopyPreparationState> = {});
 void interruptUploadCopy(const QString& id={});
 void pumpCopies();
 struct PendingCopy;struct UploadCopy;struct UploadCopyGroup;
 void beginUploadCopies(const Operation&,bool importing=false);
 void startEditing(Operation,std::shared_ptr<std::atomic_bool>,ClipboardPayload);
 QString editorDestination(const Operation&) const;
 void abandonUploadCopies(const QString&);
 // A batch item finished its upload stage, with or without a link (each_link ordering).
 void uploadItemSettled(const QString&,int);
 void flushUploadLinks(const std::shared_ptr<UploadCopy>&);
 void queueLinkCopy(const std::shared_ptr<UploadCopy>&,const QString& item,int index);
 bool detachCompletedUploadCopies(const QString&,JsonReply&);
 void uploadLinkReady(const QString&,const QString&,int,const QString&);
 void finishUploadCopies(Operation,JsonReply);
 void settleUploadGroup(const std::shared_ptr<UploadCopyGroup>&);
 void recordUploadCopy(const std::shared_ptr<UploadCopy>&);
 static void refreshUploadLinks(UploadCopy&,const Operation&);
 void acknowledgeCopyRevision(quint64,quint64);
 QHash<QString,std::shared_ptr<UploadCopy>> uploadCopies_;
 std::shared_ptr<UploadCopyGroup> collectingUploadGroup_;
 QQueue<std::shared_ptr<PendingCopy>> copies_;
 std::shared_ptr<PendingCopy> currentCopy_;
 QSet<QString> clipboardImports_;
 quint64 uploadCopyEpoch_=0;
 QString root_;
 QJsonObject locations_;
 std::unique_ptr<QLockFile> journalOwner_;
 SettingsService& settings_;
 void rememberEditorColors(const QStringList&);
 ProviderRegistry& providers_;
 SourceAdapter& source_;
 QList<QThread*> threads_;
 QQueue<Job> queue_;
 std::shared_ptr<std::atomic<qint64>> payloadBytes_=std::make_shared<std::atomic<qint64>>(0);
 QHash<QString,std::shared_ptr<std::atomic_bool>> active_;
 QHash<QString,std::function<void(const Operation&)>> finishWatchers_;
 QHash<QString,std::weak_ptr<std::mutex>> locks_;
 QHash<QString,std::weak_ptr<DestinationLease>> leases_;
 std::mutex locksMutex_;
 // Upload slots shared by every operation, and each active item's own cancellation.
 UploadSlots uploadSlots_;
 std::shared_ptr<std::atomic_bool> itemCancellation(const QString& operation,const QString& item);
 void forgetItemCancellations(const QString& operation);
 mutable std::mutex itemCancelsMutex_;QHash<QString,QHash<QString,std::shared_ptr<std::atomic_bool>>> itemCancels_;

 QTimer heapReclamationTimer_,observationTimer_;
 mutable std::mutex observationsMutex_;QHash<QString,QJsonObject> observations_;QSet<QString> observationsDirty_;bool observationScheduled_=false;
 bool heapReclamationPending_=false;
 quint64 heapReclamationCount_=0;
 qint64 lastHeapReclamationUsec_=0;
 int running_=0,storageRequests_=0;
 quint64 invocation_=0;
 QString selectingId_;
 // At most one editor session exists; it holds no pixels on the session thread.
 QString editingId_,editorHelper_,editorAppearance_=QStringLiteral("dark");std::shared_ptr<EditorSession> editor_;
 Error acceptRecording(Operation&);
 std::unique_ptr<RecordingService> recording_;
 QSet<QString> preparedOwners_,finishedPrepared_;
 std::mutex acquisitionMutex_;
 bool ready_=false,stopping_=false,cleanupInProgress_=false;
 QString installation_;
 std::function<Error()> uploadGate_;
 Error startupError_;
};
// Callback adapters complete once, including ambiguous outcomes. Waiting occurs
// exclusively on a worker, so the user session remains responsive.
template<class T,class Start> T awaitProvider(Start start) {
 auto promise=std::make_shared<std::promise<T>>();auto future=promise->get_future();
 start([promise](T value) {promise->set_value(std::move(value));});return future.get();
}
template<class T,class Start> T OperationService::storageRequest(Start start) {
 StorageActivity activity(*this);
 return awaitProvider<T>(std::move(start));
}

}
