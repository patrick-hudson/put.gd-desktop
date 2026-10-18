#pragma once
#include "core/HistoryStore.h"
#include "core/EditorJob.h"
#include "core/SettingsService.h"
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
 // Tests may run a fixture helper; production always uses the sibling putgd-ui.
 void setEditorHelper(QString path) {editorHelper_=std::move(path);}
 void inspect(const QString& id,JsonReply);
 void recent(JsonReply);
 // Session-thread snapshot of bounded live observations; no SQL or provider IO.
 QJsonArray activeObservations() const;
 bool storageActive() const {return storageRequests_>0;}
 Settings settingsSnapshot() const {return settings_.snapshot();}
 void cancel(const QString& id,JsonReply);
 void retry(const QString& id,JsonReply);
 void retryLocalSave(const QString& id,JsonReply);
 void copyResult(const QString& id,JsonReply);
 void copyHistoryImage(const QString& id,JsonReply);
 void copyHistoryUrl(const QString& url,JsonReply,quint64 ticket=0);
 quint64 invalidateCopyEligibility();
 void cleanup(JsonReply);
 void shutdown();
 QJsonObject capabilities() const;
 QJsonObject diagnostics() const;
 QString databasePath() const {return root_+"/history.sqlite";}
 ProviderRegistry& providers() {return providers_;}
 // Shared services run SQL, encoding, and file/network work on workers.
 void background(std::function<QJsonObject(Error&)>,JsonReply);
 std::shared_ptr<std::mutex> objectLock(const Destination&,const QJsonObject&,const QString& candidate);
 bool ready() const {return ready_;}
signals:
 void initialized();
 void changed(const QString& id);
 // One notification for a live observation batch, independent of durable results.
 void observationsChanged();
 void storageActivityChanged();
 // An attempt failed outside normal terminal-result delivery (e.g. local retry).
 void operationError();
 // A terminal/copy result is durably observable; progress never emits this.
 void resultReady(const QString& id);
 void sharingStopped(const QJsonObject& document,const putgd::Error& error);
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
 struct Job {Operation op;ClipboardPayload payload;bool acquire=false;std::shared_ptr<std::atomic_bool> cancelled;std::shared_ptr<qint64> payloadLease;};
 void observe(const QJsonObject&);
 QJsonObject currentObservation(QJsonObject) const;
 void clearObservation(const QString&);
 void enqueue(Job);
 void pump();
 bool heapReclamationIdle() const;
 void scheduleHeapReclamation();
 Operation run(Job);
 void finish(Operation,JsonReply={});
 void releaseActive(const QString&);
 void performCopy(Operation,bool explicitCopy,JsonReply,quint64 ticket=0);
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
 std::unique_ptr<QLockFile> journalOwner_;
 SettingsService& settings_;
 void rememberEditorColors(const QStringList&);
 ProviderRegistry& providers_;
 SourceAdapter& source_;
 QList<QThread*> threads_;
 QQueue<Job> queue_;
 std::shared_ptr<std::atomic<qint64>> payloadBytes_=std::make_shared<std::atomic<qint64>>(0);
 QHash<QString,std::shared_ptr<std::atomic_bool>> active_;
 QHash<QString,std::weak_ptr<std::mutex>> locks_;
 std::mutex locksMutex_;
 QTimer heapReclamationTimer_,observationTimer_;
 mutable std::mutex observationsMutex_;QHash<QString,QJsonObject> observations_;QSet<QString> observationsDirty_;bool observationScheduled_=false;
 bool heapReclamationPending_=false;
 quint64 heapReclamationCount_=0;
 qint64 lastHeapReclamationUsec_=0;
 int running_=0,storageRequests_=0;
 quint64 invocation_=0;
 QString selectingId_;
 // At most one editor session exists; it holds no pixels on the session thread.
 QString editingId_,editorHelper_;std::shared_ptr<EditorSession> editor_;
 QSet<QString> preparedOwners_,finishedPrepared_;
 std::mutex acquisitionMutex_;
 bool ready_=false,stopping_=false,cleanupInProgress_=false;
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
