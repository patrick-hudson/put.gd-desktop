#pragma once
#include "core/StorageProvider.h"
#include "S3Multipart.h"
#include "secrets/CredentialStore.h"
namespace putgd {
class S3Provider final : public StorageProvider {
public:
 bool reportsUploadProgress() const override {return true;}
    // The upload policy is a constructor seam for tests; production uses the defaults.
    explicit S3Provider(CredentialStore&, S3UploadPolicy = {});
    ~S3Provider() override;
    QJsonObject diagnostics() const override;
    bool reconcilesDeleteProgress() const override { return true; }
    ProviderCapabilities capabilities(const Destination&) const override;
    QString serializationKey(const Destination&, const QJsonObject&, const QString&) const override;
    Error validateSettings(const QJsonObject&) const override;
    Error readiness(const Destination&) const;
    void checkName(const Destination&, const QString&, std::function<void(NameCheck)>) override;
    void upload(const UploadRequest&, std::function<void(UploadResult)>) override;
    void stat(const StorageReceipt&, std::function<void(StatResult)>) override;
    void reconcile(const UploadRequest&, std::function<void(StatResult)>) override;
    void remove(const DeleteRequest&, DurableCheckpoint, std::function<void(DeleteResult)>) override;
    // Explicit setup operation, synthetic bytes only; public GET has no AWS credentials.
    void testDestination(const Destination&, DurableCheckpoint, std::function<void(QJsonObject, Error)>);
private:
    struct State;
    std::unique_ptr<State> state_;
};
}
