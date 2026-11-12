#pragma once
#include "core/StorageProvider.h"
#include <QHash>
#include <QProcessEnvironment>
#include <atomic>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
namespace putgd {
class ProtonProvider final : public StorageProvider {
public:
 bool reportsUploadProgress() const override {return true;}
 using SetupEvent=std::function<void(const QJsonObject&)>;
 static constexpr int MaximumRequestTimeoutMs=86400000;
 static QJsonObject defaults();
 std::optional<int> retryRequestTimeout(const Destination&,const QJsonObject&) const override;
 ProtonProvider();
 ~ProtonProvider() override;
 void shutdown(){stopping_.store(true);}
 // Checks the bundled runtime's hash on a thread of its own, ahead of the first request,
 // which otherwise waits most of a second for it. Later calls do nothing.
 void prepare();
 ProviderCapabilities capabilities(const Destination&) const override;
 QString defaultPublicationMode(const Destination&) const override;
 std::shared_ptr<void> openSession(const Destination&) override;
 std::shared_ptr<void> anticipate(const Destination&) override;
 QString serializationKey(const Destination&,const QJsonObject&,const QString&) const override;
 Error validateSettings(const QJsonObject&) const override;
 Error validateCredentials(const QJsonObject&) const override;
 QJsonObject diagnostics() const override;
 void checkName(const Destination&,const QString&,std::function<void(NameCheck)>, std::shared_ptr<std::atomic_bool> cancellation = {}) override;
 void upload(const UploadRequest&,std::function<void(UploadResult)>) override;
 void stat(const StorageReceipt&,std::function<void(StatResult)>) override;
 void reconcile(const UploadRequest&,std::function<void(StatResult)>) override;
 void remove(const DeleteRequest&,DurableCheckpoint,std::function<void(DeleteResult)>) override;
 void share(const ShareRequest&,DurableCheckpoint,std::function<void(ShareResult)>) override;
 // The helper's environment: the host's, as AppRun found it (#518). The helper is a self-contained
 // Bun program that loads the host libsecret; an AppImage's library path would bind that to the
 // image's older GLib. Caller-set Proton variables never reach it.
 static QProcessEnvironment helperEnvironment(const QProcessEnvironment& host,const QString& dataDirectory);
 // Explicit account actions run on the application worker, never during validation.
 QJsonObject setup(const QString&,const Destination&,const QJsonObject&,Error&,SetupEvent={});
private:
 struct Helper;
 // One account's helper, kept between the requests its owning thread makes.
 struct Session;
 std::shared_ptr<Session> session(const QString& reference) const;
 std::unique_ptr<Helper> start(const QString& reference,bool lines,int lockWaitMs,bool mayHash,const std::shared_ptr<std::atomic_bool>& cancelled,Error&);
 std::unique_ptr<Helper> takeStandby(const QString& reference);
 void retire(std::unique_ptr<Helper>);
 // Helpers that wait for a request, until a request takes one. Sessions share it, to pass
 // their helper on when they end.
 struct Waiting;
 std::shared_ptr<Waiting> waiting_;
 std::mutex retiringMutex_;std::condition_variable retired_;int retiring_=0;
 // The storage host each account's last block upload went to, by keychain reference. It is
 // named in the next upload so the helper can open that connection early; never stored.
 std::mutex transferHostsMutex_;QHash<QString,QString> transferHosts_;
 QJsonObject call(const QString&,const Destination&,QJsonObject,Error&,
                 std::shared_ptr<std::atomic_bool> = {},SetupEvent={},bool* outcomeUncertain=nullptr,
                 std::function<void(const UploadProgress&)> progress={});
 // One helper's answer to one request; `call` asks a second helper when the first could not read its session.
 QJsonObject callOnce(const QString&,const Destination&,QJsonObject,Error&,
                 std::shared_ptr<std::atomic_bool>,SetupEvent,bool* outcomeUncertain,
                 std::function<void(const UploadProgress&)> progress);
 std::atomic_int active_{0};std::atomic_bool stopping_{false},prepared_{false};std::thread verifier_;
};
}
