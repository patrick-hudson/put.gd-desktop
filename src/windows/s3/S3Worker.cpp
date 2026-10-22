// putgd.exe --s3-worker: exactly one S3 request per process (docs/s3.md,
// common/Protocol.h). SigV4 over WinHTTP; no SDK, no Qt. Credentials arrive only
// in the request frame and never leave this process in any frame or log line.
#include "common/Error.h"
#include "common/Files.h"
#include "common/Frames.h"
#include "common/Json.h"
#include "common/Log.h"
#include "common/Modes.h"
#include "common/Protocol.h"
#include "common/Sha256.h"
#include "common/Text.h"
#include "common/Uuid.h"
#include "core/Operation.h"
#include "core/S3Settings.h"
#include "s3/SigV4.h"
#include "s3/WinHttp.h"
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace putgd {
namespace {
using json::Object;
using json::Value;

constexpr long long maximumSinglePutBytes = 5LL * 1024 * 1024 * 1024; // S3 PutObject limit
constexpr size_t maximumKeyBytes = 1024;
constexpr int resultAcceptMs = 60000;

bool in(const std::vector<std::string>& list, const std::string& value) { return std::find(list.begin(), list.end(), value) != list.end(); }
bool asciiHeaderValue(const std::string& s) {
    for (const unsigned char c : s) if (c < 0x20 || c >= 0x7f) return false;
    return true;
}
bool lowerHex64(const std::string& s) {
    if (s.size() != 64) return false;
    for (const char c : s) if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}
Error cancelledError() { return {"cancelled", "Cancellation requested; sent requests may still have taken effect", {}}; }

// ---------------------------------------------------------------------------
// The control channel: handshake on the main thread, then a reader thread for
// cancel/acknowledgements and a ticker thread for heartbeats and progress.
class Wire {
public:
    explicit Wire(frames::Channel& channel) : channel_(channel) {}
    ~Wire() { stopThreads(); }
    bool send(const std::string& type, const Object& payload = {}) {
        std::lock_guard lock(sendMutex_);
        return channel_.send(type, payload);
    }
    void startTicker() { ticker_ = std::thread([this] { tick(); }); }
    void startReader() { reader_ = std::thread([this] { read(); }); }
    void stopThreads() {
        {
            std::lock_guard lock(stateMutex_);
            stop_ = true;
        }
        stateCv_.notify_all();
        if (ticker_.joinable()) ticker_.join();
        if (reader_.joinable()) reader_.join();
    }
    bool cancelled() const { return cancel_.load(); }
    const std::atomic<bool>& cancelFlag() const { return cancel_; }

    // Latest upload observation; frames leave at most every progressIntervalMs.
    void progress(const std::string& phase, long long transferred, long long total) {
        UploadProgress p;
        p.phase = phase;
        p.bytesTotal = total;
        p.bytesTransferred = std::min(std::max(0LL, transferred), total);
        std::lock_guard lock(progressMutex_);
        pending_ = p.toJson();
        hasPending_ = true;
        flushLocked(false);
    }
    void flushProgress(bool force) {
        std::lock_guard lock(progressMutex_);
        flushLocked(force);
    }

    // Durable checkpoint: the destructive request follows only after ok:true.
    Error checkpoint(const Object& progress) {
        paused_ = true;
        if (!send("checkpoint", {{"progress", progress}})) {
            paused_ = false;
            return {"checkpoint_failed", "The deletion journal could not be saved", {}};
        }
        auto frame = waitFor("checkpointAccepted", -1);
        paused_ = false;
        if (!frame) return {"checkpoint_failed", "The deletion journal could not be saved", {}};
        if (frame->payload.value("ok").toBool()) return {};
        auto error = Error::fromJson(frame->payload.value("error").toObject());
        if (!error) error = {"checkpoint_rejected", "The deletion journal could not be saved", {}};
        return error;
    }
    void result(const Object& payload) {
        flushProgress(true);
        if (!send("result", payload)) return;
        waitFor("resultAccepted", resultAcceptMs);
    }

private:
    void flushLocked(bool force) {
        if (!hasPending_) return;
        const ULONGLONG now = GetTickCount64();
        if (!force && sentAny_ && now - lastProgressAt_ < ULONGLONG(protocol::progressIntervalMs)) return;
        hasPending_ = false;
        if (sentAny_ && pending_ == lastProgress_) return;
        send("progress", pending_);
        lastProgress_ = pending_;
        lastProgressAt_ = now;
        sentAny_ = true;
    }
    void tick() {
        int ticks = 0;
        std::unique_lock lock(stateMutex_);
        while (!stop_) {
            stateCv_.wait_for(lock, std::chrono::milliseconds(protocol::progressIntervalMs));
            if (stop_) break;
            lock.unlock();
            flushProgress(false);
            // While a durable acknowledgement is awaited, the parent's checkpoint
            // bound replaces its silence check (docs/s3.md).
            if (++ticks * protocol::progressIntervalMs >= protocol::heartbeatMs) {
                ticks = 0;
                if (!paused_) send("heartbeat");
            }
            lock.lock();
        }
    }
    void read() {
        while (true) {
            {
                std::lock_guard lock(stateMutex_);
                if (stop_) return;
            }
            protocol::Frame frame;
            std::string error;
            if (!channel_.receive(frame, 200, &error)) {
                if (error == "timeout") continue;
                // The parent is gone or broke the protocol: exit at once. Nothing
                // can record a result any more, and an in-flight request drains or dies with us.
                TerminateProcess(GetCurrentProcess(), error == "pipe_closed" ? 0 : 1);
                return;
            }
            if (frame.type == "cancel") {
                cancel_ = true;
                continue;
            }
            if (frame.type == "checkpointAccepted" || frame.type == "resultAccepted") {
                std::lock_guard lock(stateMutex_);
                inbox_.push_back(std::move(frame));
                stateCv_.notify_all();
                continue;
            }
            TerminateProcess(GetCurrentProcess(), 1);
            return;
        }
    }
    std::optional<protocol::Frame> waitFor(const std::string& type, int timeoutMs) {
        std::unique_lock lock(stateMutex_);
        const auto ready = [&] { return stop_ || !inbox_.empty(); };
        if (timeoutMs < 0) stateCv_.wait(lock, ready);
        else if (!stateCv_.wait_for(lock, std::chrono::milliseconds(timeoutMs), ready)) return std::nullopt;
        if (inbox_.empty()) return std::nullopt;
        auto frame = std::move(inbox_.front());
        inbox_.pop_front();
        if (frame.type != type) {
            // An acknowledgement nobody asked for is a protocol violation.
            TerminateProcess(GetCurrentProcess(), 1);
            return std::nullopt;
        }
        return frame;
    }

    frames::Channel& channel_;
    std::mutex sendMutex_, stateMutex_, progressMutex_;
    std::condition_variable stateCv_;
    std::deque<protocol::Frame> inbox_;
    bool stop_ = false;
    std::atomic<bool> cancel_{false}, paused_{false};
    std::thread ticker_, reader_;
    Object pending_, lastProgress_;
    bool hasPending_ = false, sentAny_ = false;
    ULONGLONG lastProgressAt_ = 0;
};

// ---------------------------------------------------------------------------
// S3 error mapping: app-owned codes and messages; only the HTTP status, an
// allowlisted service code, the stage, timing and S3 request IDs leave.
std::string xmlElement(const std::string& body, const std::string& name) {
    const auto open = "<" + name + ">", close = "</" + name + ">";
    const auto start = body.find(open);
    if (start == std::string::npos) return {};
    const auto end = body.find(close, start + open.size());
    if (end == std::string::npos || end - start - open.size() > 128) return {};
    return text::trim(body.substr(start + open.size(), end - start - open.size()));
}
std::string requestIdentifier(const std::string& value, size_t maximum) {
    const auto t = text::trim(value);
    if (t.empty() || t.size() > maximum) return {};
    for (const char c : t)
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '+' || c == '/' || c == '=' || c == '.' || c == '_' || c == '-')) return {};
    return t;
}
const std::vector<std::string>& knownServiceCodes() {
    static const std::vector<std::string> codes{
        "AccessControlListNotSupported", "AccessDenied", "InvalidAccessKeyId", "SignatureDoesNotMatch", "ExpiredToken", "InvalidToken",
        "TokenRefreshRequired", "RequestTimeTooSkewed", "RequestExpired", "NoSuchBucket", "NoSuchKey", "NoSuchVersion", "InvalidBucketName",
        "AuthorizationHeaderMalformed", "PermanentRedirect", "TemporaryRedirect", "PreconditionFailed", "ConditionalRequestConflict",
        "InvalidRequest", "InvalidArgument", "ObjectLocked", "SlowDown", "RequestTimeout", "InternalError", "ServiceUnavailable",
        "EntityTooLarge", "BadDigest", "XAmzContentSHA256Mismatch", "NotImplemented", "MethodNotAllowed", "InvalidObjectState"};
    return codes;
}
std::string messageFor(const std::string& code, const std::string& stage) {
    if (code == "s3_invalid_access_key") return "S3 does not recognize the access key ID. Check that it is correct, active, and belongs to the selected storage service; then Store credentials again.";
    if (code == "s3_signature_mismatch") return "S3 could not verify the request signature. Check that the secret access key matches the access key ID, then check the region, endpoint and system clock.";
    if (code == "s3_token_expired") return "The temporary session token is expired or invalid. Renew and Store the complete access key, secret key and session token together.";
    if (code == "s3_clock_skew") return "S3 rejected the request time. Synchronize the system clock and try again.";
    if (code == "s3_bucket_missing") return "S3 could not find the bucket. Check the bucket name, region and endpoint; the bucket must already exist.";
    if (code == "s3_region_mismatch") return "S3 rejected the destination region or endpoint. Check that the region and endpoint match the bucket's location.";
    if (code == "s3_acl_unsupported") return "This bucket does not accept ACLs. Set Who can open files to Follow the bucket's policy (send no ACL), and ensure its public-read policy supports the intended links.";
    if (code == "s3_precondition_failed") return "The S3 object changed while the request was running. Refresh its state before retrying; put.gd will not overwrite or delete an unchecked replacement.";
    if (code == "s3_unsupported_request") return "The storage service does not support a request put.gd needs, such as conditional writes (If-None-Match and If-Match). Use an endpoint that implements S3 conditional writes.";
    if (code == "s3_object_missing") return "The S3 object no longer exists.";
    if (code == "s3_tls_failed") return "The secure connection to S3 failed. Check the endpoint, system clock and trusted certificates, including any proxy certificate.";
    if (code == "s3_timeout") return "The S3 request timed out. Check the network and endpoint, or increase the connection/request deadlines in Advanced connection settings.";
    if (code == "s3_connection_failed") return "Could not connect to the S3 endpoint. Check the endpoint, DNS, network connection, proxy and firewall.";
    if (code == "s3_service_error") return "The S3 service returned a server error. Check service availability and retry after it recovers; any uncertain upload or cleanup remains recorded.";
    if (code == "s3_slow_down") return "S3 is limiting requests. Wait before retrying and reduce concurrent uploads if this continues.";
    if (code == "s3_access_denied") {
        if (stage == "upload") return "S3 denied the upload. Check s3:PutObject in IAM and the bucket policy. Request public read also needs s3:PutObjectAcl and bucket settings that permit public ACLs.";
        if (stage == "versioning") return "S3 denied the bucket versioning check required for safe cleanup. Check s3:GetBucketVersioning in IAM and the bucket policy.";
        if (stage == "delete") return "S3 denied object cleanup. Check s3:DeleteObject in IAM and the bucket policy, and s3:DeleteObjectVersion when targeting a recorded version. Also check Object Lock, retention and MFA restrictions.";
        if (stage == "publicGet") return "The public link was refused. Check Link domain and the bucket public-read policy or ACL settings.";
        return "S3 denied the object check. Verify the key pair and s3:GetObject access in IAM and the bucket policy, plus s3:GetObjectVersion for recorded-version checks. A missing object can also return 403 without s3:ListBucket; S3 did not identify the exact cause.";
    }
    return "S3 rejected the request. Check the bucket, region, endpoint and access permissions.";
}
Error s3Error(const winhttp::Response& r, const std::string& stage) {
    Object details{{"httpStatus", r.status}, {"requestStage", stage}, {"elapsedMs", double(std::min(r.elapsedMs, 86400000LL))}};
    std::string code;
    if (r.status == 0) {
        details["systemError"] = double(r.systemError);
        switch (r.failure) {
        case winhttp::Failure::Tls: code = "s3_tls_failed"; break;
        case winhttp::Failure::Timeout: case winhttp::Failure::LowSpeed: code = "s3_timeout"; break;
        case winhttp::Failure::Cancelled: return {"cancelled", cancelledError().message, details};
        case winhttp::Failure::BodyRead: return {"artifact_changed", "Retained artifact changed or became unreadable", details};
        case winhttp::Failure::Invalid: return {"s3_request_invalid", "The S3 request could not be formed from these settings", details};
        default: code = "s3_connection_failed"; break;
        }
        if (r.failure == winhttp::Failure::LowSpeed) details["lowSpeed"] = true;
        return {code, messageFor(code, stage), details};
    }
    const auto service = xmlElement(r.body, "Code");
    details["serviceCode"] = in(knownServiceCodes(), service) ? service : std::string(service.empty() ? "" : "Unclassified");
    const auto requestId = requestIdentifier(r.header("x-amz-request-id"), 128), extended = requestIdentifier(r.header("x-amz-id-2"), 256);
    if (!requestId.empty()) details["requestId"] = requestId;
    if (!extended.empty()) details["extendedRequestId"] = extended;
    const int status = r.status;
    if (service == "AccessControlListNotSupported") code = "s3_acl_unsupported";
    else if (service == "SignatureDoesNotMatch") code = "s3_signature_mismatch";
    else if (service == "InvalidAccessKeyId") code = "s3_invalid_access_key";
    else if (service == "ExpiredToken" || service == "InvalidToken" || service == "TokenRefreshRequired") code = "s3_token_expired";
    else if (service == "RequestTimeTooSkewed" || service == "RequestExpired") code = "s3_clock_skew";
    else if (service == "NoSuchBucket") code = "s3_bucket_missing";
    else if (service == "PermanentRedirect" || service == "AuthorizationHeaderMalformed" || status == 301 ||
             (status == 400 && service.empty() && !r.header("x-amz-bucket-region").empty())) code = "s3_region_mismatch";
    else if (service == "NotImplemented" || status == 501) code = "s3_unsupported_request";
    else if (status == 412 || status == 409) code = "s3_precondition_failed";
    else if (service == "RequestTimeout") code = "s3_timeout";
    else if (service == "SlowDown" || status == 429 || status == 503) code = status >= 500 ? "s3_service_error" : "s3_slow_down";
    else if (status == 403) code = "s3_access_denied";
    else if (status == 404) code = "s3_object_missing";
    else if (status >= 500) code = "s3_service_error";
    else code = "s3_request_failed";
    return {code, messageFor(code, stage), details};
}
// The server may have acted: a reply was lost after delivery, or it failed internally.
bool mayHaveActed(const winhttp::Response& r) { return r.status == 0 ? r.delivered : (r.status >= 500 && r.status != 501); }
bool transient(const winhttp::Response& r) {
    return r.status == 0 ? (r.failure == winhttp::Failure::Connection || r.failure == winhttp::Failure::Timeout || r.failure == winhttp::Failure::LowSpeed ||
                            r.failure == winhttp::Failure::Tls)
                         : (r.status >= 500 && r.status != 501) || r.status == 429;
}

// ---------------------------------------------------------------------------
struct Body {
    std::string memory;       // small synthetic bodies
    std::string file;         // retained artifact
    long long bytes = 0;
    std::string sha256;       // hex; empty -> empty payload hash
};
struct Expected {
    std::string key, versionId, publicationId, sha256, etag;
    long long bytes = -1;
};
struct Found {
    std::string status; // own, foreign, missing, unknown
    StorageReceipt receipt;
    Error error;
    int httpStatus = 0;
};
struct UploadOutcome {
    std::string status; // uploaded, collision, failed, unknown
    StorageReceipt receipt;
    Error error;
};
struct RemoveOutcome {
    std::string status;
    Object progress;
    Error error;
};

class Job {
public:
    explicit Job(Wire& wire) : wire_(wire) {}
    ~Job() { credentials_.wipe(); }
    Object run(Object& request);

private:
    // Setup -----------------------------------------------------------------
    Error configure(const Object& destination) {
        destination_ = Destination::fromJson(destination);
        // The destination's credentials member is a non-secret reference; drop
        // anything that looks like key material so it can never reach a receipt.
        for (const auto* secret : {"accessKey", "secretKey", "sessionToken", "secretAccessKey", "accessKeyId"}) destination_.credentialReference.remove(secret);
        settings_ = destination_.settings;
        if (destination_.providerId.empty() || destination_.profileId.empty() || destination_.profileRevision.empty())
            return {"s3_request_invalid", "The S3 request names no complete destination", {}};
        if (auto error = s3::validateSettings(settings_)) return error;
        const auto endpoint = text::parseUrl(settings_.value("endpoint").stringRef());
        scheme_ = endpoint.scheme;
        host_ = s3::requestHost(settings_);
        region_ = settings_.value("region").toString();
        limits_.connectTimeoutMs = settings_.value("connectTimeoutMs").toInt(10000);
        limits_.requestTimeoutMs = settings_.value("requestTimeoutMs").toInt(60000);
        limits_.lowSpeedBytesPerSecond = settings_.value("lowSpeedBytesPerSecond").toInteger(1);
        limits_.lowSpeedTimeSeconds = settings_.value("lowSpeedTimeSeconds").toInt(30);
        if (!http_.open()) return {"s3_connection_failed", messageFor("s3_connection_failed", ""), {{"requestStage", "setup"}, {"systemError", double(GetLastError())}}};
        return {};
    }
    Error takeCredentials(Object& request) {
        // Copy out and overwrite the frame's members; the strings never reach a frame or log.
        auto credentials = request.take("credentials");
        if (!credentials.isObject()) return {"credential_setup_required", "Credentials are not loaded for this destination.", {}};
        const auto& c = credentials.objectRef();
        credentials_.accessKey = c.value("accessKey").toString();
        credentials_.secretKey = c.value("secretKey").toString();
        credentials_.sessionToken = c.value("sessionToken").toString();
        credentials = Value();
        if (credentials_.accessKey.empty() || credentials_.secretKey.empty() || !asciiHeaderValue(credentials_.accessKey) ||
            !asciiHeaderValue(credentials_.sessionToken) || text::containsChar(credentials_.accessKey, '/'))
            return {"credential_setup_required", "Credentials are not loaded for this destination.", {}};
        return {};
    }

    // Transport -------------------------------------------------------------
    std::string bucketPath() const {
        return s3::virtualAddressing(settings_) ? std::string("/") : "/" + text::percentEncode(settings_.value("bucket").toString());
    }
    winhttp::Response s3(const std::string& method, const std::string& path, const sigv4::Pairs& query, const sigv4::Pairs& headers,
                         const Body& body = {}, const winhttp::Observer& observer = {}) {
        sigv4::Request request;
        request.method = method;
        request.host = host_;
        request.path = path;
        request.query = query;
        request.headers = headers;
        request.payloadHash = body.sha256.empty() ? std::string(sigv4::emptyPayloadHash) : body.sha256;
        const auto signedRequest = sigv4::sign(request, credentials_, region_, "s3", sigv4::amzDate(std::chrono::system_clock::now()));
        winhttp::Request wire;
        wire.method = method;
        wire.scheme = scheme_;
        wire.host = host_;
        wire.path = path;
        wire.query = signedRequest.query;
        wire.headers = signedRequest.headers;
        wire.body = body.memory;
        wire.bodyFile = body.file;
        wire.bodyBytes = body.file.empty() ? (long long)body.memory.size() : body.bytes;
        // Only an unfinished PUT body is cut on cancel (it cannot publish). Checks
        // always run so the truth can be learned, and DELETEs are gated by the
        // callers' explicit cancellation checks around their checkpoints.
        winhttp::Observer withCancel = observer;
        if (!withCancel.cancel && method == "PUT") withCancel.cancel = &wire_.cancelFlag();
        return http_.perform(wire, limits_, withCancel);
    }
    winhttp::Response head(const std::string& key, const std::string& versionId = {}) {
        sigv4::Pairs query;
        if (!versionId.empty()) query.emplace_back("versionId", versionId);
        return s3("HEAD", s3::requestPath(settings_, key), query, {});
    }

    // Records ---------------------------------------------------------------
    StorageReceipt receipt(const std::string& key, const std::string& publicationId, const std::string& sha256, long long bytes,
                           const std::string& etag, const std::string& versionId) const {
        StorageReceipt r;
        r.schemaVersion = 1;
        r.destination = destination_;
        r.objectReference = {{"schemaVersion", 1}, {"key", key}, {"etag", etag}, {"sha256", sha256}, {"bytes", double(bytes)}};
        if (!versionId.empty()) r.objectReference["versionId"] = versionId;
        r.concurrencyToken = etag;
        r.publicationId = publicationId;
        r.directUrl = s3::publicObjectUrl(settings_, key);
        return r;
    }
    static Expected expectedFrom(const StorageReceipt& r) {
        Expected e;
        e.key = r.objectReference.value("key").toString();
        e.versionId = r.objectReference.value("versionId").toString();
        e.publicationId = r.publicationId;
        e.sha256 = r.objectReference.value("sha256").toString();
        e.bytes = r.objectReference.value("bytes").toInteger(-1);
        e.etag = r.concurrencyToken.empty() ? r.objectReference.value("etag").toString() : r.concurrencyToken;
        return e;
    }

    // HEAD the key (or the recorded version) and compare putgd identity.
    Found inspect(const Expected& expected, bool current) {
        const auto version = current ? std::string() : expected.versionId;
        const auto r = head(expected.key, version);
        Found found;
        found.httpStatus = r.status;
        if (r.status == 404) { found.status = "missing"; return found; }
        if (r.status != 200) {
            found.status = "unknown";
            found.error = s3Error(r, "head");
            return found;
        }
        if (text::lower(r.header("x-amz-delete-marker")) == "true") { found.status = "missing"; return found; }
        const auto publication = r.header("x-amz-meta-putgd-publication"), sha256 = r.header("x-amz-meta-putgd-sha256");
        const auto etag = r.header("etag"), versionId = r.header("x-amz-version-id");
        long long length = -1;
        try { length = std::stoll(r.header("content-length")); } catch (...) {}
        if (publication != expected.publicationId || sha256 != expected.sha256 || (expected.bytes >= 0 && length != expected.bytes) ||
            (!expected.etag.empty() && etag != expected.etag) || (!version.empty() && versionId != version)) {
            found.status = "foreign";
            return found;
        }
        found.status = "own";
        found.receipt = receipt(expected.key, expected.publicationId, sha256, length, etag, versionId);
        return found;
    }
    struct Versioning { bool ok = false; std::string status; Error error; };
    Versioning versioning() {
        const auto r = s3("GET", bucketPath(), {{"versioning", ""}}, {});
        Versioning v;
        if (r.status != 200 || r.failure != winhttp::Failure::None) {
            v.error = s3Error(r, "versioning");
            return v;
        }
        v.ok = true;
        v.status = xmlElement(r.body, "Status"); // "", Enabled, Suspended
        return v;
    }
    // Cloudflare R2 returns a version ID on every write but has no versioning: a
    // request naming a version gets 501 and the bucket reports no status (#267).
    bool versionless(const Expected& expected, const Found& found) {
        if (found.status != "unknown" || found.httpStatus != 501 || expected.versionId.empty() || expected.versionId == "null") return false;
        const auto v = versioning();
        return v.ok && v.status.empty();
    }

    // Requests --------------------------------------------------------------
    UploadOutcome put(const std::string& key, const std::string& publicationId, const std::string& condition, const std::string& token,
                      const Body& body, const std::string& mime, bool report);
    RemoveOutcome remove(const StorageReceipt& target, Object progress);
    Object checkName(const Object& request);
    Object upload(const Object& request);
    Object stat(const Object& request);
    Object reconcile(const Object& request);
    Object removeRequest(const Object& request);
    Object testDestination();

    Wire& wire_;
    Destination destination_;
    Object settings_;
    sigv4::Credentials credentials_;
    winhttp::Session http_;
    winhttp::Limits limits_;
    std::string scheme_, host_, region_;
};

Value errorValue(const Error& e) { return e ? Value(e.toJson()) : Value(); }

UploadOutcome Job::put(const std::string& key, const std::string& publicationId, const std::string& condition, const std::string& token,
                       const Body& body, const std::string& mime, bool report) {
    const long long total = body.file.empty() ? (long long)body.memory.size() : body.bytes;
    Expected expected{key, {}, publicationId, body.sha256, {}, total};
    sigv4::Pairs headers{{"content-type", mime}, {"x-amz-meta-putgd-publication", publicationId}, {"x-amz-meta-putgd-sha256", body.sha256}};
    const auto storageClass = settings_.value("storageClass").toString();
    if (!storageClass.empty() && storageClass != "STANDARD") headers.emplace_back("x-amz-storage-class", storageClass);
    if (settings_.value("aclMode") == "public-read") headers.emplace_back("x-amz-acl", "public-read");
    if (condition == "absent") headers.emplace_back("if-none-match", "*");
    else headers.emplace_back("if-match", token);

    bool uncertain = false;
    Error last;
    for (int attempt = 0; attempt < 3; ++attempt) {
        if (wire_.cancelled()) return {uncertain ? "unknown" : "failed", {}, cancelledError()};
        winhttp::Observer observer;
        if (report) {
            wire_.progress("uploading", 0, total);
            observer.progress = [&](long long sent, long long all) { wire_.progress("uploading", sent, all); };
            observer.bodySent = [&] { wire_.progress("finalizing", total, total); };
        }
        const auto r = s3("PUT", s3::requestPath(settings_, key), {}, headers, body, observer);
        if (r.status == 200)
            return {"uploaded", receipt(key, publicationId, body.sha256, total, r.header("etag"), r.header("x-amz-version-id")), {}};
        if (r.status == 0 && r.failure == winhttp::Failure::Cancelled && !r.delivered) return {uncertain ? "unknown" : "failed", {}, cancelledError()};
        last = s3Error(r, "upload");
        if (r.status == 0 && r.failure == winhttp::Failure::BodyRead) return {uncertain ? "unknown" : "failed", {}, last};
        const bool conflict = r.status == 412 || r.status == 409;
        if (!transient(r) && !conflict && !mayHaveActed(r)) return {uncertain ? "unknown" : "failed", {}, last};
        uncertain = uncertain || mayHaveActed(r);
        // Reconcile before deciding or retrying: a lost success reply recovers only
        // when HEAD shows this publication, hash and byte count.
        const auto found = inspect(expected, true);
        if (found.status == "own") return {"uploaded", found.receipt, {}};
        if (found.status == "foreign") return {uncertain ? "unknown" : "collision", {}, uncertain ? Error{"publication_unknown", "Publication remains uncertain at the saved object key", last.details} : last};
        if (found.status != "missing") return {uncertain ? "unknown" : "failed", {}, uncertain ? Error{"publication_unknown", "Publication remains uncertain at the saved object key", last.details} : last};
        if (conflict) return {uncertain ? "unknown" : "collision", {}, last};
    }
    if (uncertain) return {"unknown", {}, {"publication_unknown", "Publication remains uncertain at the saved object key", last.details}};
    return {"failed", {}, last};
}

Object Job::checkName(const Object& request) {
    const auto candidate = request.value("candidate").toString();
    const auto key = s3::objectKey(settings_, candidate);
    if (candidate.empty() || key.size() > maximumKeyBytes || text::containsChar(key, '\0') || !text::validUtf8(key))
        return {{"status", "inconclusive"}, {"error", Error{"s3_key_invalid", "S3 key exceeds its byte limit", {}}.toJson()}};
    const auto r = head(key);
    Object reference{{"schemaVersion", 1}, {"key", key}};
    if (r.status == 404) return {{"status", "absent"}, {"objectReference", reference}, {"concurrencyToken", ""}, {"error", Value()}};
    if (r.status == 200) {
        const auto etag = r.header("etag");
        reference["etag"] = etag;
        if (!r.header("x-amz-version-id").empty()) reference["versionId"] = r.header("x-amz-version-id");
        return {{"status", "present"}, {"objectReference", reference}, {"concurrencyToken", etag}, {"error", Value()}};
    }
    return {{"status", "inconclusive"}, {"objectReference", reference}, {"concurrencyToken", ""}, {"error", s3Error(r, "checkName").toJson()}};
}

Object Job::upload(const Object& request) {
    const auto artifact = request.value("artifact").toObject();
    const long long total = artifact.value("bytes").toInteger(-1);
    wire_.progress("preparing", 0, std::max(0LL, total));
    auto failed = [](const std::string& status, const Error& e) { return Object{{"status", status}, {"receipt", Value()}, {"error", e.toJson()}}; };
    const auto key = request.value("objectReference")["key"].toString();
    const auto candidate = request.value("candidate").toString();
    const auto condition = request.value("condition").toString(), token = request.value("concurrencyToken").toString();
    const auto publicationId = request.value("publicationId").toString();
    const auto sha256 = artifact.value("sha256").toString(), mime = artifact.value("mime").toString(), path = artifact.value("path").toString();
    if (key.empty() || key != s3::objectKey(settings_, candidate) || key.size() > maximumKeyBytes || !text::validUtf8(key) ||
        (condition != "absent" && (condition != "match" || token.empty() || !asciiHeaderValue(token))))
        return failed("failed", {"s3_precondition_invalid", "Persist an exact object key and write precondition before upload", {}});
    if (publicationId.empty() || !asciiHeaderValue(publicationId) || mime.empty() || !asciiHeaderValue(mime) || !lowerHex64(sha256) || path.empty() || total < 0)
        return failed("failed", {"s3_request_invalid", "The upload request is incomplete", {}});
    if (total > maximumSinglePutBytes) return failed("failed", {"s3_object_too_large", "The file is larger than one S3 request can upload", {}});
    long long measured = -1;
    const auto digest = files::sha256(path, &measured);
    if (digest.empty() || digest != sha256 || measured != total) return failed("failed", {"artifact_changed", "Retained artifact changed or became unreadable", {}});
    if (wire_.cancelled()) return failed("failed", cancelledError());
    if (const int timeout = request.value("timeoutMs").toInt(0); timeout >= 1000) limits_.requestTimeoutMs = timeout;
    Body body;
    body.file = path;
    body.bytes = total;
    body.sha256 = sha256;
    const auto outcome = put(key, publicationId, condition, token, body, mime, true);
    return {{"status", outcome.status}, {"receipt", outcome.status == "uploaded" ? Value(outcome.receipt.toJson()) : Value()}, {"error", errorValue(outcome.error)}};
}

Object Job::stat(const Object& request) {
    const auto target = StorageReceipt::fromJson(request.value("receipt").toObject());
    auto expected = expectedFrom(target);
    if (expected.key.empty() || expected.publicationId.empty())
        return {{"status", "unknown"}, {"receipt", Value()}, {"error", Error{"s3_receipt_invalid", "Recorded identity is required", {}}.toJson()}};
    auto found = inspect(expected, false);
    if (versionless(expected, found)) {
        auto current = expected;
        current.versionId.clear();
        found = inspect(current, true);
        // The recorded ID names the only object the key can hold; keep it as recorded.
        if (found.status == "own") found.receipt.objectReference["versionId"] = expected.versionId;
    }
    return {{"status", found.status}, {"receipt", found.status == "own" ? Value(found.receipt.toJson()) : Value()}, {"error", errorValue(found.error)}};
}

Object Job::reconcile(const Object& request) {
    const auto artifact = request.value("artifact").toObject();
    Expected expected;
    expected.key = request.value("objectReference")["key"].toString();
    expected.publicationId = request.value("publicationId").toString();
    expected.sha256 = artifact.value("sha256").toString();
    expected.bytes = artifact.value("bytes").toInteger(-1);
    if (expected.key.empty() || expected.publicationId.empty() || expected.sha256.empty())
        return {{"status", "unknown"}, {"receipt", Value()}, {"error", Error{"s3_request_invalid", "Reconciliation needs the saved key, publication and hash", {}}.toJson()}};
    const auto found = inspect(expected, true);
    return {{"status", found.status}, {"receipt", found.status == "own" ? Value(found.receipt.toJson()) : Value()}, {"error", errorValue(found.error)}};
}

// The docs/s3.md deletion rules, phase for phase with the Linux journal so a
// retry (on either platform) continues from the last durable checkpoint.
RemoveOutcome Job::remove(const StorageReceipt& target, Object progress) {
    static const std::vector<std::string> phases{"", "marker_intent", "marker_unknown", "marker_failed", "marker_cancelled",
                                                 "marker_done", "version_ready", "version_intent", "version_unknown", "version_failed",
                                                 "version_cancelled", "object_intent", "object_unknown", "object_failed", "object_cancelled", "done"};
    RemoveOutcome out;
    auto finish = [&](const std::string& status, Error error = {}) {
        out.status = status;
        out.progress = progress;
        out.error = std::move(error);
        return out;
    };
    if ((!progress.empty() && !json::integer(progress.value("schemaVersion"), 1, 1)) ||
        (!progress.value("phase").isNull() && !progress.value("phase").isString()) || !in(phases, progress.value("phase").toString()) ||
        (progress.contains("phase") && !progress.value("target").isObject()))
        return finish("failed", {"delete_progress_unsupported", "Unsupported deletion journal schema or phase", {}});
    progress["schemaVersion"] = 1;
    auto persist = [&](const std::string& phase) {
        progress["phase"] = phase;
        return wire_.checkpoint(progress);
    };
    auto persistIntent = [&](const std::string& phase) {
        // A rejected pre-effect checkpoint must not masquerade as a sent DELETE.
        auto proposed = progress;
        proposed["phase"] = phase;
        auto error = wire_.checkpoint(proposed);
        if (!error) progress = std::move(proposed);
        return error;
    };
    auto expected = expectedFrom(target);
    if (progress.value("versionless").toBool()) expected.versionId.clear();
    if (expected.key.empty() || expected.publicationId.empty() || expected.etag.empty() || target.objectReference.value("schemaVersion").toInt() != 1)
        return finish("failed", {"s3_receipt_invalid", "Recorded identity and object token are required", {}});
    const auto targetJson = target.toJson();
    if (progress.contains("target") && progress.value("target").toObject() != targetJson)
        return finish("changed", {"delete_target_changed", "Deletion journal does not match the original target", {}});
    progress["target"] = targetJson;

    auto phase = progress.value("phase").toString();
    bool immutable = !expected.versionId.empty() && expected.versionId != "null";
    const bool objectPhase = text::startsWith(phase, "object_");
    const bool immutablePhase = text::startsWith(phase, "marker_") || text::startsWith(phase, "version_");
    if ((objectPhase && immutable) || (immutablePhase && !immutable) || (phase == "marker_done" && progress.value("markerVersionId").toString().empty()) ||
        (text::startsWith(phase, "version_") && !progress.contains("markerVersionId") && !progress.value("noncurrent").toBool()))
        return finish("failed", {"delete_progress_unsupported", "Deletion phase does not match the recorded version", {}});
    if (phase == "done") return finish("removed");
    if (phase == "marker_intent" || phase == "marker_unknown") {
        // A marker has no publication metadata: current absence cannot attribute it
        // to this operation. Never issue another marker-creating DELETE.
        return finish("unknown", {"delete_unknown", "Marker creation may have completed; its identity is unresolved", {}});
    }
    auto identity = inspect(expected, false);
    if (phase.empty() && immutable && versionless(expected, identity)) {
        progress["versionless"] = true;
        expected.versionId.clear();
        immutable = false;
        identity = inspect(expected, false);
    }
    if (identity.status == "foreign") return finish("changed");
    if (identity.status == "unknown") return finish("unknown", identity.error);
    if (identity.status == "missing") {
        const bool removalStarted = in({"version_intent", "version_unknown", "object_intent", "object_unknown"}, phase);
        if (auto error = persist("done")) return finish("unknown", error);
        return finish(removalStarted ? "removed" : "missing");
    }
    if (wire_.cancelled()) return finish("failed", cancelledError());

    const auto bucket = versioning();
    if (!bucket.ok) return finish("unknown", bucket.error);
    const bool versioned = bucket.status == "Enabled" || bucket.status == "Suspended";
    if (expected.versionId.empty() && versioned)
        return finish("unsupported_safe_removal", {"unsupported_safe_removal", "A recorded version identity is required after bucket versioning changes", {}});

    auto erase = [&](const std::string& exactVersion, const std::string& etag) {
        sigv4::Pairs query, headers;
        if (!exactVersion.empty()) query.emplace_back("versionId", exactVersion);
        if (!etag.empty()) headers.emplace_back("if-match", etag);
        return s3("DELETE", s3::requestPath(settings_, expected.key), query, headers);
    };
    auto deleted = [](const winhttp::Response& r) { return r.status == 200 || r.status == 204; };
    auto conflicted = [](const winhttp::Response& r) { return r.status == 412 || r.status == 409; };

    if (immutable) {
        if (!versioned) return finish("unknown", {"versioning_inconclusive", "Recorded immutable version requires confirmed bucket versioning", {}});
        if (phase != "marker_done" && phase != "version_intent" && phase != "version_unknown") {
            const auto current = head(expected.key);
            if (current.status != 200 && current.status != 404) return finish("unknown", s3Error(current, "head"));
            const bool isCurrent = current.status == 200 && current.header("x-amz-version-id") == expected.versionId;
            if (isCurrent) {
                if (current.header("etag") != expected.etag || current.header("x-amz-meta-putgd-publication") != expected.publicationId) return finish("changed");
                if (bucket.status == "Suspended") {
                    const auto nullObject = head(expected.key, "null");
                    if (nullObject.status != 404 || !nullObject.header("x-amz-delete-marker").empty())
                        return finish("unsupported_safe_removal", {"unsupported_safe_removal", "Suspended bucket has an unselected or unresolved null version", {}});
                }
                // External versioning changes are outside the object ETag guard.
                const auto rechecked = versioning();
                if (!rechecked.ok || rechecked.status != bucket.status)
                    return finish("unknown", {"versioning_changed", "Bucket versioning changed before marker creation", {}});
                if (wire_.cancelled()) return finish("failed", cancelledError());
                if (auto error = persistIntent("marker_intent")) return finish("failed", error);
                if (wire_.cancelled()) {
                    persist("marker_cancelled");
                    return finish("failed", cancelledError());
                }
                const auto marked = erase({}, expected.etag);
                if (!deleted(marked)) {
                    const bool unknown = mayHaveActed(marked);
                    if (auto error = persist(unknown ? "marker_unknown" : "marker_failed")) return finish("unknown", error);
                    return finish(unknown ? "unknown" : (conflicted(marked) ? "changed" : "failed"), s3Error(marked, "delete"));
                }
                const auto markerVersion = marked.header("x-amz-version-id");
                if (text::lower(marked.header("x-amz-delete-marker")) != "true" || markerVersion.empty()) {
                    persist("marker_unknown");
                    return finish("unknown", {"delete_unknown", "S3 did not return a protective marker identity", {}});
                }
                progress["markerVersionId"] = markerVersion;
                if (auto error = persist("marker_done")) return finish("hidden_version_retained", error);
            } else {
                progress["noncurrent"] = true;
                if (auto error = persist("version_ready")) return finish("failed", error);
            }
        }
        const bool marker = progress.contains("markerVersionId");
        // Verify the exact immutable version again; never adopt a replacement.
        identity = inspect(expected, false);
        if (identity.status != "own")
            return finish(identity.status == "missing" ? "removed" : (identity.status == "foreign" ? "changed" : "unknown"), identity.error);
        if (wire_.cancelled()) return finish(marker ? "hidden_version_retained" : "failed", cancelledError());
        if (auto error = persistIntent("version_intent")) return finish(marker ? "hidden_version_retained" : "failed", error);
        if (wire_.cancelled()) {
            persist("version_cancelled");
            return finish(marker ? "hidden_version_retained" : "failed", cancelledError());
        }
        const auto removed = erase(expected.versionId, {});
        if (!deleted(removed)) {
            const bool unknown = mayHaveActed(removed);
            persist(unknown ? "version_unknown" : "version_failed");
            return finish(marker ? "hidden_version_retained" : (unknown ? "unknown" : "failed"), s3Error(removed, "delete"));
        }
    } else {
        // Unversioned (or null-version) removal rechecks the identity and original ETag.
        identity = inspect(expected, true);
        if (identity.status != "own") return finish(identity.status == "foreign" ? "changed" : (identity.status == "missing" ? "missing" : "unknown"), identity.error);
        if (wire_.cancelled()) return finish("failed", cancelledError());
        if (auto error = persistIntent("object_intent")) return finish("failed", error);
        if (wire_.cancelled()) {
            persist("object_cancelled");
            return finish("failed", cancelledError());
        }
        const auto removed = erase(expected.versionId, expected.etag);
        if (removed.status == 404) {
            if (auto error = persist("done")) return finish("unknown", error);
            return finish("missing");
        }
        if (!deleted(removed)) {
            const bool unknown = mayHaveActed(removed);
            persist(unknown ? "object_unknown" : "object_failed");
            return finish(unknown ? "unknown" : (conflicted(removed) ? "changed" : "failed"), s3Error(removed, "delete"));
        }
    }
    if (auto error = persist("done")) return finish("unknown", error);
    return finish("removed");
}

Object Job::removeRequest(const Object& request) {
    const auto target = StorageReceipt::fromJson(request.value("receipt").toObject());
    const auto outcome = remove(target, request.value("progress").toObject());
    return {{"status", outcome.status}, {"progress", outcome.progress}, {"error", errorValue(outcome.error)}};
}

Object Job::testDestination() {
    const std::string bytes = "put.gd synthetic destination test\n";
    const auto identity = uuid::create();
    const auto key = s3::objectKey(settings_, "putgd-connection-test-" + identity + ".txt");
    Body body;
    body.memory = bytes;
    body.sha256 = crypto::Sha256::hexDigest(bytes);
    Object result{{"status", "failed"}};
    auto section = [](bool ok, const Error& e = {}) {
        Object o{{"ok", ok}};
        if (e) o["error"] = e.toJson();
        return o;
    };
    Object intent{{"schemaVersion", 1}, {"phase", "synthetic_upload_intent"}, {"destination", destination_.toJson()},
                  {"objectReference", Object{{"schemaVersion", 1}, {"key", key}}}, {"publicationId", identity},
                  {"sha256", body.sha256}, {"bytes", double(bytes.size())}};
    const Error skipped{"test_step_skipped", "This step did not run because an earlier step failed", {}};
    if (key.size() > maximumKeyBytes) {
        const Error e{"s3_key_invalid", "S3 key exceeds its byte limit", {}};
        result["upload"] = section(false, e); result["publicGet"] = section(false, skipped); result["cleanup"] = section(true);
        result["error"] = e.toJson();
        return result;
    }
    // The setup caller journals the synthetic target before anything is sent.
    if (auto error = wire_.checkpoint(intent)) {
        result["upload"] = section(false, error); result["publicGet"] = section(false, skipped); result["cleanup"] = section(true);
        result["error"] = error.toJson();
        return result;
    }
    const auto uploaded = put(key, identity, "absent", {}, body, "text/plain; charset=utf-8", false);
    if (uploaded.status != "uploaded") {
        result["upload"] = section(false, uploaded.error);
        result["publicGet"] = section(false, skipped);
        if (uploaded.status == "unknown") {
            // The object may exist; keep the journaled target for reconciliation.
            result["cleanup"] = section(false, {"cleanup_uncertain", "The synthetic test object may exist; its recorded target is retained", {}});
            result["progress"] = intent;
        } else {
            result["cleanup"] = section(true);
        }
        result["error"] = uploaded.error.toJson();
        return result;
    }
    result["upload"] = section(true);
    const Object confirmed{{"schemaVersion", 1}, {"phase", "synthetic_uploaded"}, {"receipt", uploaded.receipt.toJson()}};
    if (auto error = wire_.checkpoint(confirmed)) {
        result["publicGet"] = section(false, skipped);
        result["cleanup"] = section(false, error);
        result["receipt"] = uploaded.receipt.toJson();
        result["progress"] = confirmed;
        result["error"] = error.toJson();
        return result;
    }

    // Unauthenticated GET of the direct URL; exact bytes or it fails.
    Object publicGet{{"ok", false}};
    Error publicError;
    {
        const auto url = text::parseUrl(uploaded.receipt.directUrl);
        winhttp::Request get;
        get.method = "GET";
        get.scheme = url.scheme;
        get.host = url.authority();
        get.path = url.path.empty() ? std::string("/") : url.path;
        get.maximumResponseBytes = bytes.size() + 1;
        winhttp::Observer observer;
        observer.cancel = &wire_.cancelFlag();
        winhttp::Response r;
        if (!url.valid || (url.scheme != "https" && !(url.scheme == "http" && s3::loopbackHost(url.host)))) r.failure = winhttp::Failure::Invalid;
        else r = http_.perform(get, limits_, observer);
        if (r.status != 0) publicGet["status"] = r.status;
        if (r.status == 200 && r.failure == winhttp::Failure::None && r.body == bytes) {
            publicGet["ok"] = true;
        } else if (r.status == 0) {
            publicError = s3Error(r, "publicGet");
        } else if (r.status == 200) {
            publicError = {"public_url_mismatch", "The public link returned different bytes than the test upload", {{"httpStatus", r.status}, {"requestStage", "publicGet"}, {"elapsedMs", double(r.elapsedMs)}}};
        } else {
            publicError = {"public_url_unreachable", "The test upload succeeded, but its public link did not return the test file. Check Link domain and the bucket public-read policy or ACL settings.",
                           {{"httpStatus", r.status}, {"requestStage", "publicGet"}, {"elapsedMs", double(r.elapsedMs)}}};
        }
        if (publicError) publicGet["error"] = publicError.toJson();
    }
    result["publicGet"] = publicGet;

    const auto removed = remove(uploaded.receipt, {});
    const bool cleaned = removed.status == "removed" || removed.status == "missing";
    Object cleanup{{"ok", cleaned}, {"status", removed.status}};
    Error cleanupError = removed.error;
    if (!cleaned && !cleanupError) cleanupError = {"cleanup_incomplete", "Test cleanup is incomplete; inspect the recorded test target before retrying", {}};
    if (cleanupError) cleanup["error"] = cleanupError.toJson();
    result["cleanup"] = cleanup;
    result["progress"] = removed.progress;
    if (!cleaned) result["receipt"] = uploaded.receipt.toJson();
    const bool ok = publicGet.value("ok").toBool() && cleaned;
    result["status"] = ok ? "ok" : "failed";
    if (publicError) result["error"] = publicError.toJson();
    else if (!cleaned) result["error"] = cleanupError.toJson();
    else result["error"] = Value();
    return result;
}

Object invalidResult(const std::string& kind, const Error& error, const Object& request) {
    Object result{{"error", error.toJson()}};
    if (kind == "checkName") result["status"] = "inconclusive";
    else if (kind == "stat" || kind == "reconcile") result["status"] = "unknown";
    else result["status"] = "failed";
    if (kind == "remove") result["progress"] = request.value("progress").toObject();
    if (kind == "testDestination") {
        result["upload"] = Object{{"ok", false}, {"error", error.toJson()}};
        result["publicGet"] = Object{{"ok", false}};
        result["cleanup"] = Object{{"ok", true}};
    }
    return result;
}

Object Job::run(Object& request) {
    const auto kind = request.value("kind").toString();
    auto credentialError = takeCredentials(request);
    if (!in({"checkName", "upload", "stat", "reconcile", "remove", "testDestination"}, kind))
        return invalidResult(kind, {"s3_request_invalid", "Unsupported S3 request", {}}, request);
    // Stat and removal address the object through the receipt's saved destination,
    // never an edited profile.
    const bool byReceipt = kind == "stat" || kind == "remove";
    const auto destination = byReceipt ? request.value("receipt")["destination"].toObject() : request.value("destination").toObject();
    if (auto error = configure(destination)) return invalidResult(kind, error, request);
    if (credentialError) return invalidResult(kind, credentialError, request);
    if (kind == "checkName") return checkName(request);
    if (kind == "upload") return upload(request);
    if (kind == "stat") return stat(request);
    if (kind == "reconcile") return reconcile(request);
    if (kind == "remove") return removeRequest(request);
    return testDestination();
}
}

int s3WorkerMain(const std::vector<std::string>&) {
    auto& channel = frames::helperChannel();
    Wire wire(channel);
    protocol::Frame frame;
    std::string error;
    if (!channel.receive(frame, protocol::startupMs, &error)) return error == "pipe_closed" ? 0 : 1;
    if (frame.type != "bootstrap" || frame.payload.value("role") != "s3" || !frame.payload.value("nonce").isString()) return 1;
    if (!wire.send("ready", {{"nonce", frame.payload.value("nonce")}, {"build", protocol::buildIdentity()}})) return 0;
    if (!channel.receive(frame, protocol::startupMs, &error)) return error == "pipe_closed" ? 0 : 1;
    if (frame.type != "activate") return 1;
    wire.startTicker();
    if (!channel.receive(frame, protocol::startupMs, &error)) return error == "pipe_closed" ? 0 : 1;
    if (frame.type != "request") return 1;
    wire.startReader();

    const auto started = GetTickCount64();
    const auto kind = frame.payload.value("kind").toString();
    Object result;
    {
        Job job(wire);
        result = job.run(frame.payload);
        frame.payload = Object();
    }
    const auto& resultError = result.value("error");
    log::info("s3-worker", "request finished", {{"kind", kind}, {"status", result.value("status")},
              {"code", resultError.isObject() ? resultError["code"] : Value("")}, {"elapsedMs", double(GetTickCount64() - started)}});
    wire.result(result);
    wire.stopThreads();
    return 0;
}
}
