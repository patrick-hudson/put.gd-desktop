#pragma once
// Proton Drive through the bundled SDK runtime helper (docs/proton-drive.md,
// tools/proton/README.md): one `putgd-proton-runtime.exe --stdio` child per
// request, a newline-JSON request on its stdin and bounded events plus one
// completion on its stdout. The helper owns the Proton session in Credential
// Manager (Bun.secrets); the resident holds no Proton secret. Requests for one
// account reference serialize across processes through a lock file because the
// service rotates the refresh token.
#include "resident/Provider.h"
#include <atomic>
#include <mutex>

namespace putgd {
class ProtonProvider final : public StorageProvider {
public:
    explicit ProtonProvider(std::string dataRoot);
    std::string id() const override { return "proton-drive"; }
    bool available() const override;
    std::string unavailableReason() const override;
    bool reportsUploadProgress() const override { return true; }
    std::string defaultPublicationMode(const Destination&) const override;
    int maximumConcurrentRequests(const Destination&) const override { return 1; }
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
    ShareResult share(const ShareRequest&, DurableCheckpoint, CancelFlag) override;
    json::Object testDestination(const Destination&, DurableCheckpoint, Error&) override;
    json::Object setup(const std::string& kind, const Destination&, const json::Object& input, Error&, SetupEvent) override;
    std::optional<int> retryRequestTimeout(const Destination&, const json::Object& currentProfile) const override;
    json::Object diagnostics() const override;
    void shutdown() { stopping_.store(true); }
    // The helper next to the executable and the digest the build was configured with.
    static std::string runtimePath();
    static std::string expectedRuntimeSha256();
private:
    struct Outcome { json::Object result; Error error; bool uncertain = false; };
    Outcome call(const std::string& method, const Destination&, json::Object params, CancelFlag, SetupEvent event, ProgressReport progress);
    bool runtimeVerified() const;
    std::string root_;
    std::atomic_int active_{0};
    std::atomic_bool stopping_{false};
    mutable std::mutex verifyMutex_;
    mutable bool verified_ = false; mutable long long verifiedSize_ = -1; mutable std::string verifiedIdentity_;
    mutable std::atomic<unsigned long long> started_{0}, failed_{0};
};
}
