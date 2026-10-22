#include "resident/S3Provider.h"
#include "common/Log.h"
#include "common/Text.h"
#include "core/S3Settings.h"
#include "resident/Helpers.h"
#include "s3/Multipart.h"
#include <algorithm>
#include <climits>

namespace putgd {
namespace {
namespace mp = s3::multipart;
StorageReceipt receiptFrom(const json::Value& v) { return v.isObject() ? StorageReceipt::fromJson(v.objectRef()) : StorageReceipt{}; }
Error errorFrom(const json::Value& v) { return v.isObject() ? Error::fromJson(v.objectRef()) : Error{}; }
}
ProviderCapabilities S3Provider::capabilities(const Destination& d) const {
    ProviderCapabilities c;
    c.conditionalCreate = c.conditionalReplace = c.conditionalDelete = c.versionAddressing = c.stat = c.remove = c.urls = true;
    const auto prefix = s3::objectKey(d.settings, "");
    c.maximumNameBytes = std::max(16, std::min(255, 1024 - int(prefix.size())));
    // Files over the multipart threshold upload in parts, up to S3's object limit (docs/s3.md).
    c.maximumObjectBytes = mp::maximumObjectBytes;
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
    // Clamped domains keep the arithmetic far below 2^63; the total is capped at INT_MAX ms (24.8 days).
    const auto s = d.settings;
    const auto timeout = [&](const char* key, long long fallback) { return std::clamp<long long>(s.value(key).toInteger(fallback), 1, INT_MAX); };
    const long long perCall = std::max<long long>(timeout("requestTimeoutMs", 60000), timeout("connectTimeoutMs", 5000) + timeout("lowSpeedTimeSeconds", 30) * 1000) + 1000;
    const long long hashing = std::max<long long>(60000, (std::clamp<long long>(hashedBytes, 0, mp::maximumObjectBytes) + 16383) / 16384 * 4000);
    const long long budget = std::min<long long>(perCall * calls + 300000LL * checkpoints + 60000 + hashing, INT_MAX);
    // Actual cancellation starts a separate cleanup grace: one call plus five seconds, 30-120 s.
    const int cancelGrace = int(std::clamp<long long>(perCall + 5000, 30000, 120000));
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
    }, cancel, 60000, "request", request, cancelGrace);
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
    // A new large upload starts as a multipart plan. An existing journal keeps its
    // own part size: its uploaded parts were cut with it.
    auto resume = r.resume;
    if (resume.empty()) if (const auto part = mp::partBytes(r.artifact.bytes)) resume = {{"schemaVersion", 1}, {"partBytes", double(part)}};
    if (!resume.empty()) request["resume"] = resume;
    // A multipart upload makes up to three attempts per part plus its create,
    // listing, completion and abort calls, and journals its upload ID.
    const long long parts = std::clamp<long long>(mp::partCount(r.artifact.bytes, std::clamp<long long>(resume.value("partBytes").toInteger(0), 0, mp::maximumPartBytes)), 0, mp::maximumParts);
    const int calls = int(6 + parts * 3 + (parts ? 16 : 0));
    DurableCheckpoint journal = [save = r.checkpoint](const json::Object& progress) { return save ? save(progress) : Error{}; };
    auto o = run(r.destination, std::move(request), calls, parts ? 4 : 0, r.artifact.bytes, std::move(progress), std::move(journal), cancel);
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
    auto o = run(d, {{"kind", "testDestination"}}, 20, 12, 0, {}, std::move(checkpoint), {});
    if (o.error) { error = o.error; return {}; }
    if (o.result.value("status") != "ok") error = errorFrom(o.result.value("error"));
    if (o.result.value("status") != "ok" && !error) error = {"destination_test_failed", "The connection test did not pass every step", {}};
    return o.result;
}
Error S3Provider::abandonUpload(const Destination& d, const json::Object& journal) {
    if (journal.value("uploadId").toString().empty()) return {};
    auto o = run(d, {{"kind", "abandon"}, {"journal", journal}}, 3, 0, 0, {}, {}, {});
    if (o.error) return o.error;
    if (o.result.value("status") == "aborted") return {};
    const auto error = errorFrom(o.result.value("error"));
    return error ? error : Error{"s3_multipart_abort_failed", "The incomplete multipart upload could not be removed", {}};
}
json::Object S3Provider::diagnostics() const {
    std::lock_guard lock(mutex_);
    return {{"activeWorkers", active_}, {"startedWorkers", double(started_)}, {"failedWorkers", double(failed_)}, {"transport", "winhttp"}, {"credentials", credentials_.diagnostics()}};
}
}
