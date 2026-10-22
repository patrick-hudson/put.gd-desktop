#pragma once
// The provider-neutral storage contract (docs/storage-providers.md) as synchronous
// calls made from operation worker threads. Every call completes exactly once
// with a truthful status; ambiguous outcomes are "unknown", never guessed.
#include "core/Operation.h"
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>

namespace putgd {
using CancelFlag = std::shared_ptr<std::atomic_bool>;
using ProgressReport = std::function<void(const UploadProgress&)>;
// Persists provider progress before a destructive step; the step may follow only on success.
using DurableCheckpoint = std::function<Error(const json::Object& progress)>;
struct ProviderCapabilities {
    bool conditionalCreate = false, conditionalReplace = false, conditionalDelete = false, versionAddressing = false, stat = false, remove = false, urls = false;
    int maximumNameBytes = 255;
    long long maximumObjectBytes = 0; // 0 = unknown/unbounded
    std::string deletionEffect;
    bool sharing = false, privateUploads = false;
    json::Object toJson() const {
        return {{"conditionalCreate", conditionalCreate}, {"conditionalReplace", conditionalReplace}, {"conditionalDelete", conditionalDelete}, {"versionAddressing", versionAddressing},
            {"stat", stat}, {"remove", remove}, {"urls", urls}, {"maximumNameBytes", maximumNameBytes}, {"maximumObjectBytes", double(maximumObjectBytes)},
            {"deletionEffect", deletionEffect}, {"sharing", sharing}, {"privateUploads", privateUploads}};
    }
};
struct NameCheck { std::string status; json::Object objectReference; std::string concurrencyToken; Error error; }; // absent, present, inconclusive
struct UploadRequest {
    Destination destination; Artifact artifact;
    std::string candidate, publicationId, condition; // absent or match
    json::Object objectReference; std::string concurrencyToken;
    std::optional<int> requestTimeoutMs;
    // Opaque provider-owned journal of a resumable upload (an S3 multipart upload
    // ID), last acknowledged through `checkpoint`. Empty starts fresh.
    json::Object resume;
    // A resumable upload awaits this durable acknowledgement before it relies on
    // remote state it created; an empty journal means none remains to resume or abort.
    DurableCheckpoint checkpoint;
};
struct UploadResult { std::string status; StorageReceipt receipt; Error error; }; // uploaded, collision, failed, unknown
struct StatResult { std::string status; StorageReceipt receipt; Error error; };   // own, foreign, missing, unknown
struct DeleteRequest { std::string id; StorageReceipt receipt; json::Object progress; };
struct DeleteResult { std::string status; json::Object progress; Error error; };
// Link management (docs/proton-drive.md): one durable sharing operation per mutation.
struct ShareRequest {
    std::string id, action; // create, revoke, inspect
    StorageReceipt receipt;
    json::Object options, progress; // non-secret options and opaque driver progress
    CancelFlag cancellation;
};
struct ShareResult {
    std::string status; StorageReceipt receipt; json::Object progress; Error error; // shared, private, revoked, changed, unknown, failed
    std::string expiresAt; // provider-confirmed link expiry (ISO 8601 UTC), also once passed; empty when none
};
bool shareResultMatchesReceipt(const ShareResult&, const StorageReceipt& original);
// Provider account setup (connectAccount, inspectAccount, createFolder, storeSharePassword);
// `event` receives transient UI events such as {"event":"auth","url":...}.
using SetupEvent = std::function<void(const json::Object&)>;
class StorageProvider {
public:
    virtual ~StorageProvider() = default;
    virtual std::string id() const = 0;
    virtual bool available() const { return true; }
    virtual std::string unavailableReason() const { return {}; }
    virtual bool reportsUploadProgress() const { return false; }
    virtual std::string defaultPublicationMode(const Destination&) const { return "share"; }
    virtual int maximumConcurrentRequests(const Destination&) const { return 4; }
    virtual ProviderCapabilities capabilities(const Destination&) const = 0;
    virtual std::string serializationKey(const Destination&, const json::Object& objectReference, const std::string& candidate) const = 0;
    virtual Error validateSettings(const json::Object&) const = 0;
    virtual Error validateCredentials(const json::Object&) const { return {}; }
    // credential_setup_required when the destination's credentials are not cached.
    virtual Error readiness(const Destination&) const = 0;
    virtual NameCheck checkName(const Destination&, const std::string& candidate, CancelFlag) = 0;
    virtual UploadResult upload(const UploadRequest&, ProgressReport, CancelFlag) = 0;
    virtual StatResult stat(const StorageReceipt&, CancelFlag) = 0;
    virtual StatResult reconcile(const UploadRequest&, CancelFlag) = 0;
    virtual DeleteResult remove(const DeleteRequest&, DurableCheckpoint, CancelFlag) = 0;
    virtual json::Object testDestination(const Destination&, DurableCheckpoint, Error&) = 0;
    // Removes the remote state a failed upload's journal (UploadRequest::resume) still
    // holds, such as an S3 multipart upload's parts, once no Retry can resume it (#146).
    // Best effort; an empty error means nothing of it remains.
    virtual Error abandonUpload(const Destination&, const json::Object& /*journal*/) { return {}; }
    virtual ShareResult share(const ShareRequest& r, DurableCheckpoint, CancelFlag) { return {"failed", r.receipt, {}, {"sharing_unsupported", "This provider cannot manage share links", {}}, {}}; }
    virtual json::Object setup(const std::string& kind, const Destination&, const json::Object&, Error& error, SetupEvent) { error = {"setup_unavailable", "This provider has no account setup", {{"kind", kind}}}; return {}; }
    // A longer current request deadline an explicit Retry may adopt; nullopt keeps the accepted one.
    virtual std::optional<int> retryRequestTimeout(const Destination&, const json::Object& /*currentProfile*/) const { return std::nullopt; }
    virtual json::Object diagnostics() const { return {}; }
};
inline bool shareResultMatchesReceipt(const ShareResult& result, const StorageReceipt& original) {
    static const char* const statuses[] = {"shared", "revoked", "private", "changed", "unknown", "failed"};
    bool known = false; for (const auto* s : statuses) if (result.status == s) known = true;
    return known && !result.receipt.validate() && result.receipt.destination.toJson() == original.destination.toJson() && result.receipt.objectReference == original.objectReference &&
        result.receipt.concurrencyToken == original.concurrencyToken && result.receipt.publicationId == original.publicationId;
}
class ProviderRegistry {
public:
    void add(std::shared_ptr<StorageProvider> provider) { providers_[provider->id()] = std::move(provider); }
    std::shared_ptr<StorageProvider> find(const std::string& id) const { const auto it = providers_.find(id); return it == providers_.end() ? nullptr : it->second; }
    Error validate(const std::string& id, const json::Object& settings) const {
        const auto p = find(id);
        if (!p) return {"unknown_provider", "No registered storage provider has this identifier", {{"provider", id}}};
        return p->validateSettings(settings);
    }
    Error validateCredentials(const std::string& id, const json::Object& credentials) const { const auto p = find(id); return p ? p->validateCredentials(credentials) : Error{}; }
    json::Object capabilities() const {
        json::Object result;
        // The Linux shape (every capability field) plus availability, so shared UIs can show why a provider is off.
        for (const auto& pair : providers_) { auto o = pair.second->capabilities({}).toJson(); o["available"] = pair.second->available(); o["reason"] = pair.second->unavailableReason(); result[pair.first] = o; }
        return result;
    }
    json::Object diagnostics() const { json::Object r; for (const auto& pair : providers_) r[pair.first] = pair.second->diagnostics(); return r; }
    std::vector<std::string> ids() const { std::vector<std::string> r; for (const auto& pair : providers_) r.push_back(pair.first); return r; }
private:
    std::map<std::string, std::shared_ptr<StorageProvider>> providers_;
};
}
