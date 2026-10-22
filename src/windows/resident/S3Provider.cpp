#include "resident/S3Provider.h"
#include "common/Log.h"
#include "common/Text.h"
#include "core/S3Settings.h"
#include "resident/Helpers.h"
#include <algorithm>

namespace putgd {
namespace {
constexpr long long singlePutLimit = 5LL * 1024 * 1024 * 1024;
StorageReceipt receiptFrom(const json::Value& v) { return v.isObject() ? StorageReceipt::fromJson(v.objectRef()) : StorageReceipt{}; }
Error errorFrom(const json::Value& v) { return v.isObject() ? Error::fromJson(v.objectRef()) : Error{}; }
}
ProviderCapabilities S3Provider::capabilities(const Destination& d) const {
    ProviderCapabilities c;
    c.conditionalCreate = c.conditionalReplace = c.conditionalDelete = c.versionAddressing = c.stat = c.remove = c.urls = true;
    const auto prefix = s3::objectKey(d.settings, "");
    c.maximumNameBytes = std::max(16, std::min(255, 1024 - int(prefix.size())));
    c.maximumObjectBytes = singlePutLimit;
    c.deletionEffect = "Removes the recorded object (its recorded version in a versioned bucket); browser and CDN caches are not invalidated";
    return c;
}
std::string S3Provider::serializationKey(const Destination& d, const json::Object& reference, const std::string& candidate) const {
    const auto key = reference.value("key").isString() ? reference.value("key").stringRef() : candidate.empty() ? std::string() : s3::objectKey(d.settings, candidate);
    if (key.empty()) return {};
    return text::parseUrl(d.settings.value("endpoint").toString()).authority() + "/" + d.settings.value("bucket").toString() + "/" + key;
}
Error S3Provider::validateSettings(const json::Object& settings) const { return s3::validateSettings(settings); }
Error S3Provider::validateCredentials(const json::Object& credentials) const { return s3::validateCredentials(credentials); }
Error S3Provider::readiness(const Destination& d) const {
    if (auto e = validateSettings(d.settings)) return e;
    if (!credentials_.cached(d.profileId)) return {"credential_setup_required", "Enter or unlock the credentials for this destination in Settings", {{"profileId", d.profileId}}};
    return {};
}
S3Provider::Outcome S3Provider::run(const Destination& d, json::Object request, int calls, int checkpoints, long long hashedBytes, ProgressReport progress, DurableCheckpoint checkpoint, CancelFlag cancel) {
    Outcome outcome;
    const auto credentials = credentials_.cached(d.profileId);
    if (!credentials) { outcome.error = {"credential_setup_required", "Enter or unlock the credentials for this destination in Settings", {{"profileId", d.profileId}}}; return outcome; }
    request["destination"] = d.toJson();
    request["credentials"] = json::Object{{"accessKey", credentials->accessKey}, {"secretKey", credentials->secretKey}, {"sessionToken", credentials->sessionToken}};
    // The worker's time budget follows docs/s3.md: per-call timeouts, checkpoints, margin and hashing allowance.
    const auto s = d.settings;
    const long long perCall = std::max<long long>(s.value("requestTimeoutMs").toInteger(60000), s.value("connectTimeoutMs").toInteger(5000) + s.value("lowSpeedTimeSeconds").toInteger(30) * 1000) + 1000;
    const long long hashing = std::max<long long>(60000, hashedBytes / (16 * 1024) * 4 * 1000);
    const long long budget = std::min<long long>(perCall * calls + 300000LL * checkpoints + 60000 + hashing, 6LL * 3600 * 1000);
    { std::lock_guard lock(mutex_); ++active_; ++started_; }
    auto result = helpers::runSynchronous("s3", {"--s3-worker"}, {}, protocol::startupMs, int(budget), [&](const protocol::Frame& frame, frames::Channel& send) {
        if (frame.type == "progress") {
            if (progress) if (auto p = UploadProgress::fromJson(frame.payload)) progress(*p);
            return true;
        }
        if (frame.type == "checkpoint") {
            Error e = checkpoint ? checkpoint(frame.payload.value("progress").toObject()) : Error{"delete_checkpoint_invalid", "No durable checkpoint is available for this request", {}};
            send.send("checkpointAccepted", json::Object{{"ok", !e}, {"error", e.toJson()}});
            return true;
        }
        return false;
    }, cancel, 60000, "request", request);
    request["credentials"] = json::Object{};
    { std::lock_guard lock(mutex_); --active_; if (result.error) ++failed_; }
    if (result.error) { outcome.error = result.error; return outcome; }
    outcome.result = result.result;
    return outcome;
}
NameCheck S3Provider::checkName(const Destination& d, const std::string& candidate, CancelFlag cancel) {
    auto o = run(d, {{"kind", "checkName"}, {"candidate", candidate}}, 1, 0, 0, {}, {}, cancel);
    if (o.error) return {"inconclusive", {}, {}, o.error};
    return {o.result.value("status").toString("inconclusive"), o.result.value("objectReference").toObject(), o.result.value("concurrencyToken").toString(), errorFrom(o.result.value("error"))};
}
UploadResult S3Provider::upload(const UploadRequest& r, ProgressReport progress, CancelFlag cancel) {
    json::Object request{{"kind", "upload"}, {"artifact", json::Object{{"path", r.artifact.path}, {"bytes", double(r.artifact.bytes)}, {"sha256", r.artifact.sha256}, {"mime", r.artifact.mime}}},
        {"candidate", r.candidate}, {"publicationId", r.publicationId}, {"condition", r.condition}, {"objectReference", r.objectReference}, {"concurrencyToken", r.concurrencyToken}};
    if (r.requestTimeoutMs) request["timeoutMs"] = *r.requestTimeoutMs;
    auto o = run(r.destination, std::move(request), 6, 0, r.artifact.bytes, std::move(progress), {}, cancel);
    if (o.error) {
        const bool sent = o.error.code == "helper_timeout" || o.error.code == "helper_silent" || o.error.code == "helper_exit" || o.error.code == "helper_pipe_closed";
        return {sent ? "unknown" : "failed", {}, sent ? Error{"publication_unknown", "The upload worker stopped before confirming the result; it is reconciled on retry", o.error.toJson()} : o.error};
    }
    return {o.result.value("status").toString("unknown"), receiptFrom(o.result.value("receipt")), errorFrom(o.result.value("error"))};
}
StatResult S3Provider::stat(const StorageReceipt& receipt, CancelFlag cancel) {
    auto o = run(receipt.destination, {{"kind", "stat"}, {"receipt", receipt.toJson()}}, 1, 0, 0, {}, {}, cancel);
    if (o.error) return {"unknown", {}, o.error};
    return {o.result.value("status").toString("unknown"), receiptFrom(o.result.value("receipt")), errorFrom(o.result.value("error"))};
}
StatResult S3Provider::reconcile(const UploadRequest& r, CancelFlag cancel) {
    json::Object request{{"kind", "reconcile"}, {"artifact", json::Object{{"path", r.artifact.path}, {"bytes", double(r.artifact.bytes)}, {"sha256", r.artifact.sha256}, {"mime", r.artifact.mime}}},
        {"candidate", r.candidate}, {"publicationId", r.publicationId}, {"condition", r.condition}, {"objectReference", r.objectReference}, {"concurrencyToken", r.concurrencyToken}};
    auto o = run(r.destination, std::move(request), 1, 0, 0, {}, {}, cancel);
    if (o.error) return {"unknown", {}, o.error};
    return {o.result.value("status").toString("unknown"), receiptFrom(o.result.value("receipt")), errorFrom(o.result.value("error"))};
}
DeleteResult S3Provider::remove(const DeleteRequest& r, DurableCheckpoint checkpoint, CancelFlag cancel) {
    auto o = run(r.receipt.destination, {{"kind", "remove"}, {"receipt", r.receipt.toJson()}, {"deleteId", r.id}, {"progress", r.progress}}, 8, 8, 0, {}, std::move(checkpoint), cancel);
    if (o.error) return {"unknown", {}, o.error.code == "credential_setup_required" ? o.error : Error{"delete_unknown", "The deletion worker stopped before confirming the result", o.error.toJson()}};
    return {o.result.value("status").toString("unknown"), o.result.value("progress").toObject(), errorFrom(o.result.value("error"))};
}
json::Object S3Provider::testDestination(const Destination& d, DurableCheckpoint checkpoint, Error& error) {
    auto o = run(d, {{"kind", "testDestination"}}, 16, 10, 0, {}, std::move(checkpoint), {});
    if (o.error) { error = o.error; return {}; }
    if (o.result.value("status") != "ok") error = errorFrom(o.result.value("error"));
    if (o.result.value("status") != "ok" && !error) error = {"destination_test_failed", "The connection test did not pass every step", {}};
    return o.result;
}
json::Object S3Provider::diagnostics() const {
    std::lock_guard lock(mutex_);
    return {{"activeWorkers", active_}, {"startedWorkers", double(started_)}, {"failedWorkers", double(failed_)}, {"transport", "winhttp"}, {"credentials", credentials_.diagnostics()}};
}
}
