#include "resident/ProtonProvider.h"
#include "common/Files.h"
#include "common/Log.h"
#include "common/Paths.h"
#include "common/Process.h"
#include "common/Sha256.h"
#include "common/Text.h"
#include "common/Uuid.h"
#include "core/ProtonSettings.h"
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <thread>

#ifndef PUTGD_PROTON_RUNTIME_SHA256
#define PUTGD_PROTON_RUNTIME_SHA256 ""
#endif

namespace putgd {
namespace {
constexpr size_t maximumRequestBytes = 128 * 1024;
// Bound bursts and buffered frames while allowing sustained coalesced progress (the Linux ProtonOutputBudget).
class OutputBudget {
public:
    static constexpr long long maximumBurstBytes = 2 * 1024 * 1024;
    bool consume(long long bytes, long long elapsedMs) {
        if (bytes < 0 || bytes > maximumBurstBytes || elapsedMs < lastMs_) return false;
        const auto credit = std::min(elapsedMs - lastMs_, fullRefillMs) * refillBytesPerSecond + fraction_;
        remaining_ = std::min<long long>(maximumBurstBytes, remaining_ + credit / 1000);
        fraction_ = credit % 1000; lastMs_ = elapsedMs;
        if (bytes > remaining_) return false;
        remaining_ -= bytes; return true;
    }
private:
    static constexpr long long refillBytesPerSecond = 1024;
    static constexpr long long fullRefillMs = maximumBurstBytes * 1000 / refillBytesPerSecond;
    long long remaining_ = maximumBurstBytes, lastMs_ = 0, fraction_ = 0;
};
Error parseError(const json::Value& value) {
    if (!value.isObject()) return {};
    auto code = value["code"].toString();
    if (code == "request_timeout") code = "proton_request_timeout";
    return {code, value["message"].toString(), value["details"].toObject()};
}
StorageReceipt receiptFrom(const json::Value& v) { return v.isObject() ? StorageReceipt::fromJson(v.objectRef()) : StorageReceipt{}; }
long long nowMs() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
bool preMutationFailure(const std::string& code) {
    static const char* const codes[] = {"invalid_request", "authentication_required", "account_changed", "artifact_changed", "keychain_unavailable", "invalid_destination", "invalid_receipt",
        "keychain_required", "unsupported_request", "unsupported_condition", "unsupported_safe_removal", "unsafe_data_directory", "destination_unavailable"};
    for (const auto* c : codes) if (code == c) return true;
    return false;
}
struct Handle { HANDLE h = INVALID_HANDLE_VALUE; ~Handle() { if (h != INVALID_HANDLE_VALUE) CloseHandle(h); } };
}
ProtonProvider::ProtonProvider(std::string dataRoot) : root_(std::move(dataRoot)) {}
std::string ProtonProvider::runtimePath() { return paths::join(paths::join(paths::executableDirectory(), "proton"), "putgd-proton-runtime.exe"); }
std::string ProtonProvider::expectedRuntimeSha256() { return PUTGD_PROTON_RUNTIME_SHA256; }
bool ProtonProvider::runtimeVerified() const {
    const auto expected = expectedRuntimeSha256();
    if (expected.empty()) return false;
    const auto path = runtimePath();
    const auto size = files::size(path);
    if (size <= 0) return false;
    const auto identity = files::identity(path);
    const auto key = std::to_string(identity.volume) + ":" + std::to_string(identity.index);
    std::lock_guard lock(verifyMutex_);
    if (verifiedSize_ == size && verifiedIdentity_ == key) return verified_;
    // Hash the helper once per installed file; a replaced file is hashed again.
    verified_ = files::sha256(path) == expected; verifiedSize_ = size; verifiedIdentity_ = key;
    if (!verified_) log::warning("proton", "The Proton runtime does not match the build's manifest", {});
    return verified_;
}
bool ProtonProvider::available() const { return runtimeVerified(); }
std::string ProtonProvider::unavailableReason() const { return runtimeVerified() ? std::string() : "proton_runtime_unavailable"; }
std::string ProtonProvider::defaultPublicationMode(const Destination& d) const { return d.settings.value("shareByDefault").toBool(true) ? "share" : "private"; }
ProviderCapabilities ProtonProvider::capabilities(const Destination&) const {
    ProviderCapabilities c;
    c.conditionalCreate = c.stat = c.urls = c.sharing = c.privateUploads = true;
    c.maximumNameBytes = 255;
    // The pinned Drive SDK sets no file size limit; Proton enforces plan limits server-side (destination_file_too_large_for_plan).
    c.maximumObjectBytes = 0;
    c.deletionEffect = "Safe remote deletion is unavailable: Proton does not expose an atomic revision-guarded file deletion. Revoke share links separately.";
    return c;
}
std::string ProtonProvider::serializationKey(const Destination& d, const json::Object& ref, const std::string& candidate) const { return proton::serializationKey(d.settings, ref, candidate); }
Error ProtonProvider::validateSettings(const json::Object& s) const { return proton::validateSettings(s); }
Error ProtonProvider::validateCredentials(const json::Object& c) const { return proton::validateCredentials(c); }
Error ProtonProvider::readiness(const Destination& d) const {
    if (auto e = validateSettings(d.settings)) return e;
    if (!runtimeVerified()) return {"proton_runtime_unavailable", "Install a matching put.gd package with the bundled Proton runtime", {}};
    if (d.settings.value("accountRootUid").toString().empty() || d.settings.value("folderUid").toString().empty())
        return {"proton_setup_required", "Connect your Proton account and choose an upload folder in Settings", {{"profileId", d.profileId}}};
    return {};
}
std::optional<int> ProtonProvider::retryRequestTimeout(const Destination& d, const json::Object& profile) const {
    return proton::retryRequestTimeout(d.profileId, d.providerId, d.credentialReference, d.settings, profile);
}
json::Object ProtonProvider::diagnostics() const {
    const auto path = runtimePath();
    return {{"activeHelpers", active_.load()}, {"protocolVersion", 1}, {"started", double(started_.load())}, {"failed", double(failed_.load())},
        {"runtime", json::Object{{"bundled", !expectedRuntimeSha256().empty()}, {"present", files::isRegularFile(path)}, {"verified", runtimeVerified()}}}};
}

ProtonProvider::Outcome ProtonProvider::call(const std::string& method, const Destination& destination, json::Object params, CancelFlag cancelled, SetupEvent event, ProgressReport progress) {
    Outcome out;
    auto fail = [&](const char* code, std::string message, json::Object details = {}) { out.error = {code, std::move(message), std::move(details)}; return out; };
    if (stopping_.load()) return fail("operation_cancelled", "The application is stopping");
    if (!runtimeVerified()) return fail("proton_runtime_unavailable", "Install a matching put.gd package with the bundled Proton runtime");
    const auto& reference = destination.credentialReference;
    if (reference.value("mode") != "keychain" || reference.value("reference").toString().empty()) return fail("proton_credentials_invalid", "Proton requires an explicit account keychain reference");
    // Separate helper processes share a rotating auth session: serialize the whole
    // request by account reference, including setup and token refresh.
    const auto lockRoot = paths::join(root_, "proton-accounts");
    if (!files::makePrivateDirectory(lockRoot)) return fail("proton_lock_unavailable", "Private account coordination is unavailable");
    const auto lockPath = paths::join(lockRoot, crypto::Sha256::hexDigest(reference.value("reference").stringRef()) + ".lock");
    Handle lock;
    for (const auto start = nowMs(); lock.h == INVALID_HANDLE_VALUE;) {
        lock.h = CreateFileW(paths::wide(lockPath).c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
        if (lock.h != INVALID_HANDLE_VALUE) break;
        const auto error = GetLastError();
        if ((error != ERROR_SHARING_VIOLATION && error != ERROR_ACCESS_DENIED) || nowMs() - start > 2000) return fail("proton_account_busy", "Another request is using this Proton account; wait and retry");
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (stopping_.load() || (cancelled && cancelled->load())) return fail("operation_cancelled", "Request cancelled before the helper started");
    const auto dataParent = paths::join(root_, "proton-data");
    const auto privateRoot = paths::join(dataParent, uuid::create());
    if (!files::makePrivateDirectory(dataParent) || !files::makePrivateDirectory(privateRoot)) return fail("proton_private_storage", "Private helper storage is unavailable");
    struct Cleanup { std::string path; ~Cleanup() { files::removeDirectoryRecursively(path); } } cleanup{privateRoot};
    params["schemaVersion"] = 1; params["method"] = method; params["destination"] = destination.toJson();
    const int timeout = method == "connect" ? 600000 : params.value("requestTimeoutMs").toInt(destination.settings.value("requestTimeoutMs").toInt(proton::defaultRequestTimeoutMs));
    std::string bytes = json::serialize(params) + "\n";
    params = {};
    if (bytes.size() > maximumRequestBytes) { std::fill(bytes.begin(), bytes.end(), '\0'); return fail("proton_protocol", "Helper request exceeded its bound"); }
    ++active_; ++started_;
    struct Release { std::atomic_int& count; ~Release() { --count; } } release{active_};
    auto child = process::startRaw(runtimePath(), {"--stdio"}, {{"PUTGD_PROTON_DATA_DIR", privateRoot}}, {"PROTON_DRIVE_", "PUTGD_PROTON_"});
    if (!child) { std::fill(bytes.begin(), bytes.end(), '\0'); ++failed_; return fail("proton_runtime_start", "The bundled Proton runtime could not start"); }
    // Once any request bytes are handed to the helper, a lost or malformed
    // completion cannot establish whether a remote mutation ran.
    size_t delivered = 0;
    while (delivered < bytes.size()) {
        DWORD written = 0;
        if (!WriteFile(child->rawStdin, bytes.data() + delivered, DWORD(bytes.size() - delivered), &written, nullptr) || written == 0) break;
        delivered += written;
    }
    out.uncertain = delivered > 0;
    std::fill(bytes.begin(), bytes.end(), '\0');
    if (delivered != bytes.size()) { child->terminate(); child->waitExit(5000); ++failed_; return fail("proton_result_unknown", "Helper request could not be delivered"); }
    CloseHandle(child->rawStdin); child->rawStdin = nullptr;
    bytes.clear();

    struct Reader { std::mutex mutex; std::condition_variable cv; std::deque<std::string> chunks; bool eof = false; };
    auto reader = std::make_shared<Reader>();
    std::thread drain([reader, h = child->rawStdout] {
        std::string buffer(64 * 1024, '\0');
        DWORD got = 0;
        while (ReadFile(h, buffer.data(), DWORD(buffer.size()), &got, nullptr) && got > 0) {
            std::lock_guard lock(reader->mutex); reader->chunks.emplace_back(buffer.data(), got); reader->cv.notify_all();
        }
        std::lock_guard lock(reader->mutex); reader->eof = true; reader->cv.notify_all();
    });
    const auto started = nowMs();
    std::string pending; json::Object final; bool gotResult = false, gotAuth = false; OutputBudget budget;
    auto consume = [&](std::string chunk) {
        // The first violation decides the outcome; output left after a kill is not parsed.
        if (out.error) return;
        pending += chunk;
        if (static_cast<long long>(pending.size()) > OutputBudget::maximumBurstBytes || !budget.consume(static_cast<long long>(chunk.size()), nowMs() - started)) { out.error = {"proton_protocol", "Helper response exceeded its bound", {}}; return; }
        size_t newline;
        while ((newline = pending.find('\n')) != std::string::npos) {
            auto line = pending.substr(0, newline); pending.erase(0, newline + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            const auto parsed = json::parseObject(line);
            if (!parsed) { out.error = {"proton_protocol", "Malformed helper response", {}}; return; }
            const auto& value = *parsed;
            if (value.value("event") == "auth") {
                if (method != "connect" || !event || gotAuth || gotResult || value.size() != 2 || !value.value("url").isString()) { out.error = {"proton_protocol", "Unexpected authentication event", {}}; return; }
                gotAuth = true; event(value);
            } else if (value.value("event") == "progress") {
                auto fields = value; fields.remove("event");
                const auto report = UploadProgress::fromJson(fields);
                if (method != "upload" || gotResult || !report) { out.error = {"proton_protocol", "Unexpected or invalid upload progress", {}}; return; }
                if (progress) progress(*report);
            } else if (gotResult || !value.value("ok").isBool() || value.size() != 2 || (value.value("ok").toBool() ? !value.value("result").isObject() : !value.value("error").isObject())) {
                out.error = {"proton_protocol", "Invalid helper completion", {}}; return;
            } else { final = value; gotResult = true; }
        }
    };
    bool killed = false;
    for (;;) {
        std::deque<std::string> chunks; bool eof;
        {
            std::unique_lock lock(reader->mutex);
            reader->cv.wait_for(lock, std::chrono::milliseconds(100), [&] { return reader->eof || !reader->chunks.empty(); });
            chunks.swap(reader->chunks); eof = reader->eof;
        }
        for (auto& chunk : chunks) consume(std::move(chunk));
        if (out.error) { child->terminate(); killed = true; break; }
        if (eof) break;
        if (stopping_.load() || (cancelled && cancelled->load())) {
            out.error = {"proton_result_unknown", "The request was cancelled after dispatch; reconcile its saved identity before retrying", {{"reason", stopping_.load() ? "shutdown" : "cancelled"}}};
            child->terminate(); killed = true; break;
        }
        if (nowMs() - started > timeout) {
            out.error = {"proton_request_timeout", "Proton exceeded the configured " + text::toString(double(timeout) / 1000.0) + "-second request deadline; the outcome is unknown. Reconcile its saved identity before retrying.",
                {{"reason", "deadline"}, {"timeoutMs", timeout}, {"method", method}}};
            child->terminate(); killed = true; break;
        }
    }
    child->waitExit(5000);
    if (!killed) { std::deque<std::string> rest; { std::lock_guard lock(reader->mutex); rest.swap(reader->chunks); } for (auto& chunk : rest) consume(std::move(chunk)); }
    if (drain.joinable()) drain.join();
    if (!out.error && !text::trim(pending).empty()) out.error = {"proton_protocol", "Incomplete helper response", {}};
    std::fill(pending.begin(), pending.end(), '\0'); pending.clear();
    const int exitCode = child->exitCode();
    if (!out.error && (!gotResult || exitCode != 0)) out.error = {"proton_result_unknown", "The helper ended without a confirmed result", {{"exitCode", exitCode}}};
    if (!out.error && !final.value("ok").toBool()) {
        out.error = parseError(final.value("error"));
        if (!out.error) out.error = {"proton_failed", "Proton operation failed", {}};
        // Only protocol-defined pre-mutation failures prove no remote side effect.
        if (preMutationFailure(out.error.code)) out.uncertain = false;
    }
    if (out.error) { ++failed_; log::warning("proton", "Helper request failed", {{"method", method}, {"code", out.error.code}, {"uncertain", out.uncertain}}); return out; }
    out.result = final.value("result").toObject();
    return out;
}

NameCheck ProtonProvider::checkName(const Destination& d, const std::string& candidate, CancelFlag cancel) {
    auto o = call("checkName", d, {{"candidate", candidate}}, cancel, {}, {});
    if (o.error) return {"inconclusive", {}, {}, o.error};
    return {o.result.value("status").toString("inconclusive"), o.result.value("objectReference").toObject(), o.result.value("concurrencyToken").toString(), parseError(o.result.value("error"))};
}
UploadResult ProtonProvider::upload(const UploadRequest& r, ProgressReport progress, CancelFlag cancel) {
    if (r.condition != "absent") return {"failed", {}, {"unsupported_capability", "Proton cannot safely replace a recorded file revision", {}}};
    json::Object params{{"requestTimeoutMs", r.requestTimeoutMs ? json::Value(*r.requestTimeoutMs) : r.destination.settings.value("requestTimeoutMs")},
        {"artifact", json::Object{{"path", r.artifact.path}, {"name", r.artifact.name}, {"mime", r.artifact.mime}, {"bytes", double(r.artifact.bytes)}, {"sha256", r.artifact.sha256}}},
        {"candidate", r.candidate}, {"publicationId", r.publicationId}, {"condition", r.condition}, {"objectReference", r.objectReference}, {"concurrencyToken", r.concurrencyToken}};
    auto o = call("upload", r.destination, std::move(params), cancel, {}, std::move(progress));
    auto error = o.error ? o.error : parseError(o.result.value("error"));
    // The plan's per-file limit is Proton's own: name the file's size beside it.
    if (error.code == "destination_file_too_large_for_plan") {
        error.message = "Proton refused this " + text::byteSize(r.artifact.bytes) + " file: it is larger than your Proton plan allows for a single file. Upload a smaller file, or upgrade the Proton plan, then Retry.";
        error.details["bytes"] = double(r.artifact.bytes);
    }
    return {o.error ? (o.uncertain ? "unknown" : "failed") : o.result.value("status").toString("unknown"), receiptFrom(o.result.value("receipt")), error};
}
StatResult ProtonProvider::stat(const StorageReceipt& receipt, CancelFlag cancel) {
    auto o = call("stat", receipt.destination, {{"receipt", receipt.toJson()}}, cancel, {}, {});
    return {o.error ? "unknown" : o.result.value("status").toString("unknown"), receiptFrom(o.result.value("receipt")), o.error ? o.error : parseError(o.result.value("error"))};
}
StatResult ProtonProvider::reconcile(const UploadRequest& r, CancelFlag cancel) {
    json::Object params{{"requestTimeoutMs", r.requestTimeoutMs ? json::Value(*r.requestTimeoutMs) : r.destination.settings.value("requestTimeoutMs")},
        {"artifact", json::Object{{"path", r.artifact.path}, {"name", r.artifact.name}, {"mime", r.artifact.mime}, {"bytes", double(r.artifact.bytes)}, {"sha256", r.artifact.sha256}}},
        {"candidate", r.candidate}, {"publicationId", r.publicationId}, {"objectReference", r.objectReference}};
    auto o = call("reconcile", r.destination, std::move(params), cancel, {}, {});
    return {o.error ? "unknown" : o.result.value("status").toString("unknown"), receiptFrom(o.result.value("receipt")), o.error ? o.error : parseError(o.result.value("error"))};
}
DeleteResult ProtonProvider::remove(const DeleteRequest&, DurableCheckpoint, CancelFlag) {
    return {"unsupported_safe_removal", {}, {"unsupported_capability", "Proton does not expose atomic revision-guarded file deletion; revoke the share link instead", {}}};
}
ShareResult ProtonProvider::share(const ShareRequest& r, DurableCheckpoint checkpoint, CancelFlag cancel) {
    const json::Object intent{{"schemaVersion", 1}, {"phase", r.action == "inspect" ? "inspect" : "intent"}, {"operationId", r.id}, {"action", r.action}};
    if (r.action != "inspect") { if (auto e = checkpoint ? checkpoint(intent) : Error{"share_journal_invalid", "No durable checkpoint is available for this request", {}}) return {"failed", r.receipt, r.progress, e, {}}; }
    auto options = r.options; options["action"] = r.action;
    auto o = call("share", r.receipt.destination, {{"receipt", r.receipt.toJson()}, {"options", options}}, cancel, {}, {});
    auto receipt = o.result.contains("receipt") ? receiptFrom(o.result.value("receipt")) : r.receipt;
    if (o.error) receipt.directUrl.clear();
    ShareResult result{o.error ? (o.uncertain ? "unknown" : "failed") : o.result.value("status").toString("unknown"), receipt, o.result.value("progress").toObject(), o.error ? o.error : parseError(o.result.value("error")), {}};
    const auto expires = files::parseIsoUtcMs(o.result.value("expiresAt").toString());
    if (!o.error && expires >= 0) result.expiresAt = files::isoUtcFromMs(expires);
    return result;
}
json::Object ProtonProvider::testDestination(const Destination& d, DurableCheckpoint, Error& e) {
    const auto root = d.settings.value("accountRootUid").toString(), folder = d.settings.value("folderUid").toString();
    if (root.empty() || folder.empty()) { e = {"proton_setup_required", "Connect your Proton account and choose an upload folder before testing", {}}; return {}; }
    auto o = call("inspectAccount", d, {{"options", json::Object{}}}, {}, {}, {});
    if (o.error) { e = o.error; return {}; }
    if (o.result.value("accountRootUid") != root.c_str()) { e = {"proton_account_changed", "Reconnect the saved Proton account before testing", {}}; return {}; }
    bool found = folder == root;
    for (const auto& value : o.result.value("folders").toArray()) if (value["uid"] == folder.c_str()) found = true;
    if (!found) { e = {"proton_folder_unavailable", "The selected Proton folder is unavailable. Refresh folders and choose an upload folder", {}}; return {}; }
    return {{"testScope", "account-folder"}};
}
json::Object ProtonProvider::setup(const std::string& kind, const Destination& d, const json::Object& input, Error& e, SetupEvent event) {
    static const std::map<std::string, std::string> methods{{"connectAccount", "connect"}, {"inspectAccount", "inspectAccount"}, {"createFolder", "createFolder"}, {"storeSharePassword", "storeSharePassword"}};
    const auto method = methods.find(kind);
    if (method == methods.end()) { e = {"setup_unavailable", "Unknown Proton setup action", {{"kind", kind}}}; return {}; }
    auto o = call(method->second, d, {{"options", input}}, {}, std::move(event), {});
    if (o.error) { e = o.error; return {}; }
    return o.result;
}
}
