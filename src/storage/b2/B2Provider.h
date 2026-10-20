#pragma once
#include "B2Common.h"
#include "core/StorageProvider.h"
#include "secrets/CredentialStore.h"
#include <atomic>
namespace putgd {
// Backblaze B2 through its native API (#282). Every request runs in its own
// putgd-b2-worker process, so the resident links no network code. Calls block the
// calling worker thread, as Proton's do; the resident never calls them on its UI thread.
class B2Provider final : public StorageProvider {
public:
    explicit B2Provider(CredentialStore&, b2::UploadPolicy = {});
    ~B2Provider() override;
    bool reportsUploadProgress() const override { return true; }
    bool reconcilesDeleteProgress() const override { return true; }
    // Each request is its own process; uploads to one destination overlap.
    int maximumConcurrentRequests(const Destination&) const override { return 8; }
    QJsonObject diagnostics() const override;
    ProviderCapabilities capabilities(const Destination&) const override;
    QString serializationKey(const Destination&, const QJsonObject&, const QString&) const override;
    Error validateSettings(const QJsonObject&) const override;
    void checkName(const Destination&, const QString&, std::function<void(NameCheck)>) override;
    void upload(const UploadRequest&, std::function<void(UploadResult)>) override;
    void stat(const StorageReceipt&, std::function<void(StatResult)>) override;
    void reconcile(const UploadRequest&, std::function<void(StatResult)>) override;
    void remove(const DeleteRequest&, DurableCheckpoint, std::function<void(DeleteResult)>) override;
    void abandonUpload(const Destination&, const QJsonObject& journal, std::function<void(Error)>) override;
    // Explicit setup: a synthetic upload, a fetch through the link put.gd would hand
    // out, guarded removal by version ID and a large-file start/cancel probe.
    void testDestination(const Destination&, DurableCheckpoint, std::function<void(QJsonObject, Error)>);
    // The worker executable: the installed sibling, or the build tree's.
    static QString workerPath();
private:
    QJsonObject call(const QString& method, const Destination&, QJsonObject params, Error&, bool* uncertain = nullptr,
                     std::shared_ptr<std::atomic_bool> cancellation = {}, std::function<void(const UploadProgress&)> progress = {},
                     DurableCheckpoint checkpoint = {});
    CredentialStore& credentials_;
    b2::UploadPolicy policy_;
    std::atomic_int active_{0};
    std::atomic_bool stopping_{false};
};
}
