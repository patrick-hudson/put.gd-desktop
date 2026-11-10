#pragma once
#include "HttpCommon.h"
#include "core/StorageProvider.h"
#include "secrets/CredentialStore.h"
#include <atomic>
namespace putgd {
// A custom HTTP uploader (#1361): the user's own request, or one imported from a ShareX
// .sxcu file. Every request runs in its own putgd-http-worker process, so the resident
// links no network code. Calls block the calling worker thread, as B2's do.
//
// The server names what it stores. There is no conditional create, no stat and no
// versions: the collision policy does not apply, an upload whose reply was lost cannot
// be checked, and deletion exists only for an upload that returned a deletion URL.
class HttpUploaderProvider final : public StorageProvider {
public:
    explicit HttpUploaderProvider(CredentialStore&);
    ~HttpUploaderProvider() override;
    bool reportsUploadProgress() const override { return true; }
    bool reconcilesDeleteProgress() const override { return true; }
    // Each request is its own process; uploads to one destination overlap.
    int maximumConcurrentRequests(const Destination&) const override { return 4; }
    QJsonObject diagnostics() const override;
    ProviderCapabilities capabilities(const Destination&) const override;
    ProviderCapabilities receiptCapabilities(const StorageReceipt&) const override;
    QJsonObject diagnosticSettings(const QJsonObject& settings) const override;
    QString serializationKey(const Destination&, const QJsonObject&, const QString&) const override;
    Error validateSettings(const QJsonObject&) const override;
    void checkName(const Destination&, const QString&, std::function<void(NameCheck)>, std::shared_ptr<std::atomic_bool> cancellation = {}) override;
    void upload(const UploadRequest&, std::function<void(UploadResult)>) override;
    void stat(const StorageReceipt&, std::function<void(StatResult)>) override;
    void reconcile(const UploadRequest&, std::function<void(StatResult)>) override;
    void remove(const DeleteRequest&, DurableCheckpoint, std::function<void(DeleteResult)>) override;
    // Explicit setup: a synthetic upload, a fetch of the returned link, and deletion
    // when the reply carried a deletion URL. The result says which of these happened.
    void testDestination(const Destination&, DurableCheckpoint, std::function<void(QJsonObject, Error)>);
    // Empty when every value the request needs is loaded; otherwise what is missing.
    Error readiness(const Destination&) const;
    // The worker executable: the installed sibling, or the build tree's.
    static QString workerPath();
private:
    QJsonObject call(const QString& method, const Destination&, QJsonObject params, Error&, bool* uncertain = nullptr,
                     std::shared_ptr<std::atomic_bool> cancellation = {}, std::function<void(const UploadProgress&)> progress = {},
                     DurableCheckpoint checkpoint = {});
    CredentialStore& credentials_;
    std::atomic_int active_{0};
    std::atomic_bool stopping_{false};
};
}
