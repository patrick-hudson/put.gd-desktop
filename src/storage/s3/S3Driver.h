#pragma once
#include "core/StorageProvider.h"
#include "secrets/CredentialStore.h"
namespace putgd {
// Private same-build module ABI; no AWS types or host symbol exports.
using CredentialLookup = std::function<std::optional<Credentials>(const QString&, const QJsonObject&)>;
using ReadinessCheck = std::function<Error(const Destination&)>;
class S3Driver {
public:
    virtual ~S3Driver() = default;
    virtual void checkName(const Destination&, const QString&, std::function<void(NameCheck)>) = 0;
    virtual void upload(const UploadRequest&, std::function<void(UploadResult)>) = 0;
    virtual void stat(const StorageReceipt&, std::function<void(StatResult)>) = 0;
    virtual void reconcile(const UploadRequest&, std::function<void(StatResult)>) = 0;
    virtual void remove(const DeleteRequest&, DurableCheckpoint, std::function<void(DeleteResult)>) = 0;
    virtual void testDestination(const Destination&, DurableCheckpoint, std::function<void(QJsonObject, Error)>) = 0;
    // Aborts the multipart upload a retired item's journal names (#146).
    virtual void abandon(const Destination&, const QJsonObject& journal, std::function<void(Error)>) = 0;
};
class NetworkS3Driver final : public S3Driver {
public:
    NetworkS3Driver(CredentialLookup, ReadinessCheck);
    ~NetworkS3Driver() override;
    void checkName(const Destination&, const QString&, std::function<void(NameCheck)>) override;
    void upload(const UploadRequest&, std::function<void(UploadResult)>) override;
    void stat(const StorageReceipt&, std::function<void(StatResult)>) override;
    void reconcile(const UploadRequest&, std::function<void(StatResult)>) override;
    void remove(const DeleteRequest&, DurableCheckpoint, std::function<void(DeleteResult)>) override;
    void testDestination(const Destination&, DurableCheckpoint, std::function<void(QJsonObject, Error)>) override;
    void abandon(const Destination&, const QJsonObject&, std::function<void(Error)>) override;
private:
    struct State;
    std::unique_ptr<State> state_;
};
}
