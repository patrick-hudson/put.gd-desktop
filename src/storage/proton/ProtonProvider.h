#pragma once
#include "core/StorageProvider.h"
#include <QProcessEnvironment>
#include <atomic>
namespace putgd {
class ProtonProvider final : public StorageProvider {
public:
 bool reportsUploadProgress() const override {return true;}
 using SetupEvent=std::function<void(const QJsonObject&)>;
 static constexpr int MaximumRequestTimeoutMs=86400000;
 static QJsonObject defaults();
 std::optional<int> retryRequestTimeout(const Destination&,const QJsonObject&) const override;
 void shutdown(){stopping_.store(true);}
 ProviderCapabilities capabilities(const Destination&) const override;
 QString defaultPublicationMode(const Destination&) const override;
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
 QJsonObject call(const QString&,const Destination&,QJsonObject,Error&,
                 std::shared_ptr<std::atomic_bool> = {},SetupEvent={},bool* outcomeUncertain=nullptr,
                 std::function<void(const UploadProgress&)> progress={});
 std::atomic_int active_{0};std::atomic_bool stopping_{false};
};
}
