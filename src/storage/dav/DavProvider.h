#pragma once
#include "DavCommon.h"
#include "core/StorageProvider.h"
#include "secrets/CredentialStore.h"
#include <atomic>
namespace putgd {
// Nextcloud and plain WebDAV (#1872): one instance per destination type. Every request
// runs in its own putgd-webdav-worker process, so the resident links no network code.
// Calls block the calling worker thread, as B2's do; the resident never calls them on its
// UI thread. Credentials: the login name (Nextcloud) or user, and the app password or
// password, as an access key and secret key in the credential store.
class DavProvider final : public StorageProvider {
public:
    DavProvider(CredentialStore&, QString providerId, qint64 chunkThresholdBytes = dav::chunkThresholdBytes, qint64 chunkBytes = dav::chunkBytes);
    ~DavProvider() override;
    bool reportsUploadProgress() const override { return true; }
    bool reconcilesDeleteProgress() const override { return true; }
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
    void statForRemoval(const StorageReceipt&, std::shared_ptr<std::atomic_bool> kill, std::function<void(StatResult)>) override;
    void reconcile(const UploadRequest&, std::function<void(StatResult)>) override;
    void remove(const DeleteRequest&, DurableCheckpoint, std::function<void(DeleteResult)>) override;
    void share(const ShareRequest&, DurableCheckpoint, std::function<void(ShareResult)>) override;
    void abandonUpload(const Destination&, const QJsonObject& journal, std::function<void(Error)>) override;
    // Explicit setup: a synthetic upload, its link fetched anonymously and compared, and
    // removal with If-Match. Blocks; run it on a worker thread.
    void testDestination(const Destination&, DurableCheckpoint, std::function<void(QJsonObject, Error)>);
    // Setup kinds: connectAccount (Nextcloud Login Flow v2; `event` hears {"event":"signIn",
    // "url":…} for the browser), inspectAccount (the signed-in account and its sharing rules,
    // or the WebDAV folder), createFolder, signOut (revokes a Nextcloud app password and
    // forgets the credentials). The helper work runs through `background`; the credential
    // store is used on the calling thread. A sign-in's app password goes only to the store.
    using Background = std::function<void(std::function<QJsonObject(Error&)>, std::function<void(Error, QJsonObject)>)>;
    // `current` says whether the destination still exists with the same server; a sign-in that
    // finishes after it changed is revoked, never saved.
    void setup(const QString& kind, const Destination&, const QJsonObject& input, const Background&, std::function<void(Error, QJsonObject)> reply,
               std::function<void(QJsonObject)> event, std::shared_ptr<std::atomic_bool> cancelled, std::function<bool(const Destination&)> current = {});
    // History's Unlock saved credentials, before a delete or a link change (#1872).
    void unlockLinkCredentials(const StorageReceipt&, std::function<void(Error)>) override;
    // The worker executable: the installed sibling, or the build tree's.
    static QString workerPath();
private:
    QJsonObject call(const QString& method, const Destination&, QJsonObject params, Error&, bool* uncertain = nullptr,
                     std::shared_ptr<std::atomic_bool> cancellation = {}, std::function<void(const UploadProgress&)> progress = {},
                     DurableCheckpoint checkpoint = {}, std::shared_ptr<std::atomic_bool> kill = {},
                     std::function<void(const QJsonObject&)> notice = {}, bool needsCredentials = true, const Credentials* given = nullptr);
    CredentialStore& credentials_;
    QString id_;
    qint64 chunkThreshold_, chunkBytes_;
    std::atomic_int active_{0};
    std::atomic_bool stopping_{false};
};
}
