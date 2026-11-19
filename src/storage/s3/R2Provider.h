#pragma once
#include "S3Provider.h"
#include <memory>
namespace putgd {
// Cloudflare R2 (#1678, #1679, #1691): its own destination type over the S3 provider. Each
// request reaches the S3 driver with r2::s3Settings() of the destination (the account
// endpoint, region auto, path addressing, ACL omit) under provider "s3"; receipts come back
// with the Cloudflare R2 destination, so History and later deletions route here.
class R2Provider final : public StorageProvider {
public:
    explicit R2Provider(std::shared_ptr<S3Provider> s3);
    // The destination and recorded upload as the S3 provider handles them.
    static Destination s3Destination(const Destination&);
    static StorageReceipt s3Receipt(const StorageReceipt&);
    bool reportsUploadProgress() const override { return true; }
    bool reconcilesDeleteProgress() const override { return true; }
    int maximumConcurrentRequests(const Destination&) const override;
    QJsonObject diagnostics() const override { return {}; }
    ProviderCapabilities capabilities(const Destination&) const override;
    ProviderCapabilities receiptCapabilities(const StorageReceipt&) const override;
    QString serializationKey(const Destination&, const QJsonObject&, const QString&) const override;
    Error validateSettings(const QJsonObject&) const override;
    Error validateCredentials(const QJsonObject&) const override;
    void checkName(const Destination&, const QString&, std::function<void(NameCheck)>, std::shared_ptr<std::atomic_bool> cancellation = {}) override;
    void upload(const UploadRequest&, std::function<void(UploadResult)>) override;
    void unlockLinkCredentials(const StorageReceipt&, std::function<void(Error)>) override;
    void refreshLink(const StorageReceipt&, std::function<void(StatResult)>) override;
    void refreshLinkForCopy(const StorageReceipt&, std::shared_ptr<std::atomic_bool> kill, std::function<void(StatResult)>) override;
    void stat(const StorageReceipt&, std::function<void(StatResult)>) override;
    void statForRemoval(const StorageReceipt&, std::shared_ptr<std::atomic_bool> kill, std::function<void(StatResult)>) override;
    void reconcile(const UploadRequest&, std::function<void(StatResult)>) override;
    void remove(const DeleteRequest&, DurableCheckpoint, std::function<void(DeleteResult)>) override;
    void abandonUpload(const Destination&, const QJsonObject& journal, std::function<void(Error)>) override;
    void testDestination(const Destination&, DurableCheckpoint, std::function<void(QJsonObject, Error)>);
private:
    std::shared_ptr<S3Provider> s3_;
};
}
