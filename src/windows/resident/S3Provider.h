#pragma once
// The S3 provider facade: validates settings without IO, keeps credentials in
// the resident's store, and runs every network request in a transient
// `putgd --s3-worker` child (docs/s3.md), so no HTTP or TLS code is mapped in
// the resident between uploads.
#include "resident/CredentialStore.h"
#include "resident/Provider.h"
#include <mutex>

namespace putgd {
class S3Provider final : public StorageProvider {
public:
    explicit S3Provider(CredentialStore& credentials) : credentials_(credentials) {}
    std::string id() const override { return "s3"; }
    bool reportsUploadProgress() const override { return true; }
    ProviderCapabilities capabilities(const Destination&) const override;
    std::string serializationKey(const Destination&, const json::Object& objectReference, const std::string& candidate) const override;
    Error validateSettings(const json::Object&) const override;
    Error validateCredentials(const json::Object&) const override;
    Error readiness(const Destination&) const override;
    NameCheck checkName(const Destination&, const std::string& candidate, CancelFlag) override;
    UploadResult upload(const UploadRequest&, ProgressReport, CancelFlag) override;
    StatResult stat(const StorageReceipt&, CancelFlag) override;
    StatResult reconcile(const UploadRequest&, CancelFlag) override;
    DeleteResult remove(const DeleteRequest&, DurableCheckpoint, CancelFlag) override;
    json::Object testDestination(const Destination&, DurableCheckpoint, Error&) override;
    json::Object diagnostics() const override;
private:
    struct Outcome { json::Object result; Error error; };
    Outcome run(const Destination&, json::Object request, int calls, int checkpoints, long long hashedBytes, ProgressReport, DurableCheckpoint, CancelFlag);
    CredentialStore& credentials_;
    mutable std::mutex mutex_;
    int active_ = 0; unsigned long long started_ = 0, failed_ = 0;
};
}
