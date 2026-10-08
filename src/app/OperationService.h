#pragma once
#include "core/HistoryStore.h"
#include "core/SettingsService.h"
#include <QObject>
#include <QQueue>
#include <QThread>
#include <QLockFile>
#include <atomic>
#include <mutex>
#include <future>
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
 virtual quint64 clipboardRevision() const=0;
 virtual QJsonObject capabilities() const=0;
 // Freeze native target identity/layout on the session thread before persistence
 // yields. Starting acquisition remains deferred until its acceptance is durable.
 virtual PreparedAcquisition prepareAcquisition(const QString& action) {
  return [this,action](const ResourceLimits& limits,std::function<void(PayloadResult)> reply) {
   acquire(action,limits,std::move(reply));
  };
 }
 virtual void acquire(const QString&,const ResourceLimits&,std::function<void(PayloadResult)>)=0;
 virtual void cancelAcquisition()=0;
 // Invalidates in-flight asynchronous clipboard commits when a newer action is accepted.
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
 void invoke(const QString& action,JsonReply);
 void inspect(const QString& id,JsonReply);
 void recent(JsonReply);
 Settings settingsSnapshot() const {return settings_.snapshot();}
 void cancel(const QString& id,JsonReply);
 void retry(const QString& id,JsonReply);
 void retryLocalSave(const QString& id,JsonReply);
 void copyResult(const QString& id,JsonReply);
 void copyHistoryUrl(const QString& url,JsonReply,quint64 ticket=0);
 quint64 invalidateCopyEligibility() {source_.invalidateCopyEligibility();return ++invocation_;}
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
private:
 struct Job {Operation op;ClipboardPayload payload;bool acquire=false;std::shared_ptr<std::atomic_bool> cancelled;};
 void enqueue(Job);
 void pump();
 Operation run(Job);
 void finish(Operation,JsonReply={});
 void performCopy(Operation,bool explicitCopy,JsonReply,quint64 ticket=0);
 void deliverCopy(const CopyPayload&,std::function<void(Error)>);
 QString root_;
 std::unique_ptr<QLockFile> journalOwner_;
 SettingsService& settings_;
 ProviderRegistry& providers_;
 SourceAdapter& source_;
 QList<QThread*> threads_;
 QQueue<Job> queue_;
 QHash<QString,std::shared_ptr<std::atomic_bool>> active_;
 QHash<QString,std::weak_ptr<std::mutex>> locks_;
 std::mutex locksMutex_;
 int running_=0;
 quint64 invocation_=0;
 QString selectingId_;
 std::mutex acquisitionMutex_;
 bool ready_=false,stopping_=false;
 Error startupError_;
};
// Callback adapters complete once, including ambiguous outcomes. Waiting occurs
// exclusively on a worker, so the user session remains responsive.
template<class T,class Start> T awaitProvider(Start start) {
 auto promise=std::make_shared<std::promise<T>>();auto future=promise->get_future();
 start([promise](T value) {promise->set_value(std::move(value));});return future.get();
}
}
