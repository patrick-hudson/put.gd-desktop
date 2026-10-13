#include "S3Driver.h"
#include "S3SdkContext.h"
#include "PublicUrl.h"
#include "SdkRuntime.h"
#include "S3ErrorDiagnostics.h"
#include <QElapsedTimer>
#include <aws/core/auth/AWSCredentials.h>
#include <aws/core/http/HttpClientFactory.h>
#include <aws/core/http/HttpClient.h>
#include <aws/core/http/HttpResponse.h>
#include <array>
#include <aws/core/client/DefaultRetryStrategy.h>
#include <aws/core/utils/memory/stl/AWSStreamFwd.h>
#include <aws/core/utils/threading/PooledThreadExecutor.h>
#include <aws/s3/S3Client.h>
#include <aws/s3/model/HeadObjectRequest.h>
#include <aws/s3/model/PutObjectRequest.h>
#include <aws/s3/model/DeleteObjectRequest.h>
#include <aws/s3/model/GetBucketVersioningRequest.h>
#include <aws/s3/model/ObjectCannedACL.h>
#include <aws/s3/model/StorageClass.h>
#include <QCryptographicHash>
#include <QFile>
#include <QHostAddress>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QUuid>
#include <condition_variable>
#include <fstream>
#include <mutex>
namespace putgd {
namespace {
Aws::String aws(const QString& value) { return value.toStdString(); }
QString qt(const Aws::String& value) { return QString::fromUtf8(value.data(), int(value.size())); }
QString objectKey(const Destination& destination, const QString& candidate) {
    auto prefix = destination.settings["objectPrefix"].toString();
    if (!prefix.isEmpty() && !prefix.endsWith('/')) prefix += '/';
    return prefix + candidate;
}
bool stopped(const std::shared_ptr<std::atomic_bool>& token) { return token && token->load(); }
Error cancelled() {
    return {"cancelled", "Cancellation requested; sent requests may still have taken effect", {}};
}
Error accessError() { return {"credential_setup_required", "Restore explicit credentials for this original destination", {}}; }
template<class E> Error wireError(const E& error, const char* stage, qint64 elapsedMs) {
    const auto name = qt(error.GetExceptionName());
    const int status = int(error.GetResponseCode());
    QString code = "s3_request_failed";
    if (name == "AccessControlListNotSupported") code = "s3_acl_incompatible";
    else if (name == "InvalidAccessKeyId" || name == "SignatureDoesNotMatch" || name == "ExpiredToken") code = "s3_authentication_failed";
    else if (name == "ObjectLocked" || qt(error.GetMessage()).contains("retention", Qt::CaseInsensitive) ||
             qt(error.GetMessage()).contains("legal hold", Qt::CaseInsensitive)) code = "s3_retention_blocked";
    else if (qt(error.GetMessage()).contains("MFA", Qt::CaseInsensitive)) code = "s3_mfa_required";
    else if (status == 403) code = "s3_permission_denied";
    else if (status == 409 || status == 412) code = "precondition_changed";
    else if (status < 200 || status >= 500) code = "s3_transport_unknown";
    return {code, "S3 request failed; check destination access, ACL and retention settings",
            {{"httpStatus", status}, {"sdkErrorType", int(error.GetErrorType())},
             {"curlCode", sdkCurlCode({error.GetMessage().data(), error.GetMessage().size()})},
             {"sdkRetryable", error.ShouldRetry()}, {"requestStage", QString::fromLatin1(stage)},
             {"elapsedMs", qMin(elapsedMs, qint64(86400000))}, {"serviceCode", QStringList{"AccessControlListNotSupported", "AccessDenied", "InvalidAccessKeyId", "SignatureDoesNotMatch", "ExpiredToken", "PreconditionFailed", "ConditionalRequestConflict", "InvalidRequest", "ObjectLocked", "SlowDown", "RequestTimeout", "InternalError", "ServiceUnavailable"}.contains(name) ? name : QString("Unclassified")}}};
}
template<class E> bool absent(const E& error) { return int(error.GetResponseCode()) == 404; }
template<class E> bool ambiguous(const E& error) {
    const int status = int(error.GetResponseCode());
    return status < 200 || status >= 500;
}
// Only the explicit synthetic destination test uses this unsigned GET. Keep
// its response bounded even when a public endpoint returns unexpected content.
class SyntheticResponseStream final : public Aws::IOStream {
    class Buffer final : public std::streambuf {
        std::array<char, 128> bytes_{};
    protected:
        pos_type seekoff(off_type offset, std::ios_base::seekdir direction,
                         std::ios_base::openmode mode) override {
            // The SDK queries tellp() before each write. Allow position queries
            // without permitting writes to seek beyond the fixed response bound.
            if (offset == 0 && direction == std::ios_base::cur && mode == std::ios_base::out)
                return pos_type(pptr() - pbase());
            return pos_type(off_type(-1));
        }
    public:
        explicit Buffer(qsizetype limit) { setp(bytes_.data(), bytes_.data() + qMin(limit, qsizetype(bytes_.size()))); }
        QByteArray bytes() const { return QByteArray(pbase(), pptr() - pbase()); }
    } buffer_;
public:
    explicit SyntheticResponseStream(qsizetype limit) : Aws::IOStream(nullptr), buffer_(limit) { rdbuf(&buffer_); }
    QByteArray bytes() const { return buffer_.bytes(); }
};
bool publicFixtureMatches(const StorageReceipt& published, const QByteArray& expected) {
    Aws::Client::ClientConfiguration config(false, "legacy", true);
    config.verifySSL = true;
    config.followRedirects = Aws::Client::FollowRedirectsPolicy::NEVER;
    config.connectTimeoutMs = published.destination.settings["connectTimeoutMs"].toInt();
    config.httpRequestTimeoutMs = published.destination.settings["requestTimeoutMs"].toInt();
    config.requestTimeoutMs = published.destination.settings["lowSpeedTimeSeconds"].toInt() * 1000L;
    config.lowSpeedLimit = published.destination.settings["lowSpeedBytesPerSecond"].toInt();
    auto client = Aws::Http::CreateHttpClient(config);
    const QUrl publicUrl(published.directUrl);
    Aws::Http::URI uri(aws(publicUrl.toString(QUrl::FullyEncoded)));
    // SDK URI takes decoded path segments and performs the wire encoding itself.
    // Passing the already encoded public path would escape each '%' twice.
    uri.SetPath(aws(publicUrl.path(QUrl::FullyDecoded)));
    auto request = Aws::Http::CreateHttpRequest(uri, Aws::Http::HttpMethod::HTTP_GET,
        [limit = expected.size() + 1] { return Aws::New<SyntheticResponseStream>("putgd-public-fixture", limit); });
    // CreateHttpRequest/MakeRequest do not apply an AWS signer or credentials.
    auto response = client->MakeRequest(request);
    if (!response || response->GetResponseCode() != Aws::Http::HttpResponseCode::OK) return false;
    auto* body = dynamic_cast<SyntheticResponseStream*>(&response->GetResponseBody());
    return body && body->bytes() == expected;
}
using Client = Aws::S3::S3Client;
using Head = Aws::S3::Model::HeadObjectOutcome;
Head head(Client& client, const Destination& destination, const QString& key, const QString& version = {}) {
    Aws::S3::Model::HeadObjectRequest request;
    request.SetBucket(aws(destination.settings["bucket"].toString()));
    request.SetKey(aws(key));
    if (!version.isEmpty()) request.SetVersionId(aws(version));
    return client.HeadObject(request);
}
QString metadata(const Aws::S3::Model::HeadObjectResult& result, const char* name) {
    const auto it = result.GetMetadata().find(name);
    return it == result.GetMetadata().end() ? QString{} : qt(it->second);
}
StorageReceipt receipt(const Destination& destination, const QString& key, const QString& identity,
                       const QString& hash, qint64 bytes, const QString& etag, const QString& version) {
    StorageReceipt result;
    result.destination = destination;
    result.objectReference = {{"schemaVersion", 1}, {"key", key}, {"versionId", version},
                              {"sha256", hash}, {"bytes", double(bytes)}};
    result.publicationId = identity;
    result.concurrencyToken = etag;
    result.directUrl = publicObjectUrl(destination.settings, key);
    return result;
}
StatResult inspect(Client& client, const StorageReceipt& expected, bool current = false) {
    const auto key = expected.objectReference["key"].toString();
    const auto version = current ? QString{} : expected.objectReference["versionId"].toString();
    QElapsedTimer requestTimer;
    requestTimer.start();
    const auto outcome = head(client, expected.destination, key, version);
    if (!outcome.IsSuccess()) {
        if (absent(outcome.GetError())) return {"missing", {}, {}};
        return {"unknown", {}, wireError(outcome.GetError(), current ? "current_head" : "object_head", requestTimer.elapsed())};
    }
    const auto& result = outcome.GetResult();
    if (result.GetDeleteMarker()) return {"missing", {}, {}};
    if (metadata(result, "putgd-publication") != expected.publicationId ||
        metadata(result, "putgd-sha256") != expected.objectReference["sha256"].toString() ||
        result.GetContentLength() != qint64(expected.objectReference["bytes"].toDouble()) ||
        (!expected.concurrencyToken.isEmpty() && qt(result.GetETag()) != expected.concurrencyToken) ||
        (!version.isEmpty() && qt(result.GetVersionId()) != version)) return {"foreign", {}, {}};
    return {"own", receipt(expected.destination, key, expected.publicationId,
                            metadata(result, "putgd-sha256"), result.GetContentLength(),
                            qt(result.GetETag()), qt(result.GetVersionId())), {}};
}
Error checkpoint(const DurableCheckpoint& save, const QJsonObject& progress) {
    std::mutex mutex;
    std::condition_variable condition;
    bool done = false;
    Error result;
    save(progress, [&](Error error) {
        std::lock_guard lock(mutex);
        result = error;
        done = true;
        condition.notify_one();
    });
    std::unique_lock lock(mutex);
    condition.wait(lock, [&] { return done; });
    return result;
}
}
struct NetworkS3Driver::State {
    CredentialLookup credentials;
    ReadinessCheck readiness;
    std::shared_ptr<S3SdkContext> runtime;
    QHash<QByteArray, std::shared_ptr<Client>> clients;
    State(CredentialLookup lookup, ReadinessCheck check)
        : credentials(std::move(lookup)), readiness(std::move(check)), runtime(S3SdkContext::acquire()) {}
    std::shared_ptr<Client> client(const Destination& destination) {
        const auto credential = credentials(destination.profileId, destination.credentialReference);
        if (!credential) return {};
        const auto key = QJsonDocument(destination.settings).toJson(QJsonDocument::Compact) + QByteArray::number(credential->generation);
        if (clients.contains(key)) return clients[key];
        // Validation and credential lookup above are Qt-only. Initialize before
        // client configuration touches SDK endpoint, crypto and HTTP globals.
        SdkRuntime::ensureInitialized();
        Aws::S3::S3ClientConfiguration config(false, "legacy", true);
        config.region = aws(destination.settings["region"].toString());
        config.endpointOverride = aws(destination.settings["endpoint"].toString());
        config.scheme = QUrl(destination.settings["endpoint"].toString()).scheme() == "http" ? Aws::Http::Scheme::HTTP : Aws::Http::Scheme::HTTPS;
        config.verifySSL = true;
        config.connectTimeoutMs = destination.settings["connectTimeoutMs"].toInt();
        config.httpRequestTimeoutMs = destination.settings["requestTimeoutMs"].toInt();
        config.requestTimeoutMs = destination.settings["lowSpeedTimeSeconds"].toInt() * 1000L;
        config.lowSpeedLimit = destination.settings["lowSpeedBytesPerSecond"].toInt();
        config.maxConnections = 1;
        config.retryStrategy = Aws::MakeShared<Aws::Client::DefaultRetryStrategy>("putgd", 0);
        config.executor = Aws::MakeShared<Aws::Utils::Threading::PooledThreadExecutor>("putgd", 1);
        config.useVirtualAddressing = s3VirtualAddressing(destination.settings);
        config.disableS3ExpressAuth = true;
        config.disableMultiRegionAccessPoints = true;
        config.payloadSigningPolicy = Aws::Client::AWSAuthV4Signer::PayloadSigningPolicy::Always;
        config.checksumConfig.requestChecksumCalculation = Aws::Client::RequestChecksumCalculation::WHEN_REQUIRED;
        auto value = std::make_shared<Client>(Aws::Auth::AWSCredentials(aws(credential->accessKey), aws(credential->secretKey), aws(credential->sessionToken)), nullptr, config);
        if (clients.size() >= 8) clients.erase(clients.begin());
        clients.insert(key, value);
        return value;
    }
};
NetworkS3Driver::NetworkS3Driver(CredentialLookup lookup, ReadinessCheck readiness)
    : state_(std::make_unique<State>(std::move(lookup), std::move(readiness))) {}
NetworkS3Driver::~NetworkS3Driver() = default;
void NetworkS3Driver::checkName(const Destination& destination, const QString& candidate, std::function<void(NameCheck)> reply) {
    if (auto error = state_->readiness(destination)) {
        reply({"inconclusive", {}, {}, error});
        return;
    }
    auto client = state_->client(destination);
    if (!client) {
        reply({"inconclusive", {}, {}, accessError()});
        return;
    }
    const auto key = objectKey(destination, candidate);
    if (key.toUtf8().size() > 1024 || key.contains(QChar(0))) {
        reply({"inconclusive", {}, {}, {"s3_key_invalid", "S3 key exceeds its byte limit", {}}});
        return;
    }
    QElapsedTimer requestTimer;
    requestTimer.start();
    const auto outcome = head(*client, destination, key);
    QJsonObject reference{{"schemaVersion", 1}, {"key", key}};
    if (!outcome.IsSuccess()) {
        if (absent(outcome.GetError())) reply({"absent", reference, {}, {}});
        else reply({"inconclusive", {}, {}, wireError(outcome.GetError(), "name_head", requestTimer.elapsed())});
    } else {
        reference["versionId"] = qt(outcome.GetResult().GetVersionId());
        reply({"present", reference, qt(outcome.GetResult().GetETag()), {}});
    }
}
void NetworkS3Driver::stat(const StorageReceipt& expected, std::function<void(StatResult)> reply) {
    if (auto error = state_->readiness(expected.destination)) {
        reply({"unknown", {}, error});
        return;
    }
    auto client = state_->client(expected.destination);
    if (!client) {
        reply({"unknown", {}, accessError()});
        return;
    }
    reply(inspect(*client, expected));
}
void NetworkS3Driver::reconcile(const UploadRequest& request, std::function<void(StatResult)> reply) {
    if (auto error = state_->readiness(request.destination)) {
        reply({"unknown", {}, error});
        return;
    }
    const auto expected = receipt(request.destination, request.objectReference["key"].toString(), request.publicationId,
                                  request.artifact.sha256, request.artifact.bytes, {}, {});
    auto client = state_->client(request.destination);
    if (!client) {
        reply({"unknown", {}, accessError()});
        return;
    }
    reply(inspect(*client, expected, true));
}
void NetworkS3Driver::upload(const UploadRequest& request, std::function<void(UploadResult)> reply) {
    if (auto error = state_->readiness(request.destination)) {
        reply({"failed", {}, error});
        return;
    }
    if (stopped(request.cancellation)) {
        reply({"failed", {}, cancelled()});
        return;
    }
    const auto key = request.objectReference["key"].toString();
    if (key != objectKey(request.destination, request.candidate) || key.toUtf8().size() > 1024 ||
        (request.condition != "absent" && (request.condition != "match" || request.concurrencyToken.isEmpty()))) {
        reply({"failed", {}, {"s3_precondition_invalid", "Persist an exact object key and write precondition before upload", {}}});
        return;
    }
    QFile source(request.artifact.path);
    if (!source.open(QIODevice::ReadOnly) || source.size() != request.artifact.bytes) {
        reply({"failed", {}, {"artifact_invalid", "Retained artifact is unavailable", {}}});
        return;
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&source) || QString::fromLatin1(hash.result().toHex()) != request.artifact.sha256) {
        reply({"failed", {}, {"artifact_invalid", "Retained artifact changed", {}}});
        return;
    }
    source.close();
    auto client = state_->client(request.destination);
    if (!client) {
        reply({"failed", {}, accessError()});
        return;
    }
    const auto expected = receipt(request.destination, key, request.publicationId, request.artifact.sha256, request.artifact.bytes, {}, {});
    bool uncertain = false;
    for (int attempt = 0; attempt < 3; ++attempt) {
        if (stopped(request.cancellation)) {
            reply({uncertain ? "unknown" : "failed", {}, cancelled()});
            return;
        }
        Aws::S3::Model::PutObjectRequest put;
        put.SetBucket(aws(request.destination.settings["bucket"].toString()));
        put.SetKey(aws(key));
        put.SetContentLength(request.artifact.bytes);
        put.SetContentType(aws(request.artifact.mime));
        put.SetStorageClass(Aws::S3::Model::StorageClassMapper::GetStorageClassForName(aws(request.destination.settings["storageClass"].toString())));
        if (request.destination.settings["aclMode"] == "public-read") put.SetACL(Aws::S3::Model::ObjectCannedACL::public_read);
        if (request.condition == "absent") put.SetIfNoneMatch("*");
        else put.SetIfMatch(aws(request.concurrencyToken));
        put.AddMetadata("putgd-publication", aws(request.publicationId));
        put.AddMetadata("putgd-sha256", aws(request.artifact.sha256));
        auto stream = Aws::MakeShared<Aws::FStream>("putgd-artifact", request.artifact.path.toStdString().c_str(), std::ios::in | std::ios::binary);
        if (!stream->is_open()) {
            reply({"failed", {}, {"artifact_invalid", "Retained artifact is unavailable", {}}});
            return;
        }
        put.SetBody(stream);
        QElapsedTimer requestTimer;
        requestTimer.start();
        const auto outcome = client->PutObject(put);
        if (outcome.IsSuccess()) {
            reply({"uploaded", receipt(request.destination, key, request.publicationId, request.artifact.sha256, request.artifact.bytes,
                                         qt(outcome.GetResult().GetETag()), qt(outcome.GetResult().GetVersionId())), {}});
                                         return;
        }
        const auto error = wireError(outcome.GetError(), "upload_put", requestTimer.elapsed());
        const bool conflict = int(outcome.GetError().GetResponseCode()) == 412 || int(outcome.GetError().GetResponseCode()) == 409;
        if (!ambiguous(outcome.GetError()) && !conflict) {
            reply({uncertain ? "unknown" : "failed", {}, error});
            return;
        }
        uncertain = uncertain || ambiguous(outcome.GetError());
        auto found = inspect(*client, expected, true);
        if (found.status == "own") {
            reply({"uploaded", found.receipt, {}});
            return;
        }
        if (found.status == "foreign") {
            reply({uncertain ? "unknown" : "collision", {}, error});
            return;
        }
        if (found.status != "missing") {
            reply({"unknown", {}, error});
            return;
        }
        if (conflict) {
            reply({uncertain ? "unknown" : "collision", {}, error});
            return;
        }
        uncertain = true;
    }
    reply({"unknown", {}, {"publication_unknown", "Publication remains uncertain at the saved object key", {}}});
}
void NetworkS3Driver::remove(const DeleteRequest& request, DurableCheckpoint save, std::function<void(DeleteResult)> reply) {
    QJsonObject progress = request.progress;
    const QStringList phases{"", "marker_intent", "marker_unknown", "marker_failed", "marker_cancelled",
                             "marker_done", "version_ready", "version_intent", "version_unknown", "version_failed",
                             "version_cancelled", "object_intent", "object_unknown", "object_failed", "object_cancelled", "done"};
    if ((!progress.isEmpty() && progress.value("schemaVersion") != QJsonValue(1)) ||
        (!progress.value("phase").isUndefined() && !progress.value("phase").isString()) ||
        !phases.contains(progress.value("phase").toString()) ||
        (progress.contains("phase") && !progress["target"].isObject())) {
        reply({"failed", progress, {"delete_progress_unsupported", "Unsupported deletion journal schema or phase", {}}});
        return;
    }
    progress["schemaVersion"] = 1;
    auto finish = [&](const QString& status, Error error = {}) { reply({status, progress, error}); };
    auto persist = [&](const QString& phase) {
        progress["phase"] = phase;
        return checkpoint(save, progress);
    };
    auto persistIntent = [&](const QString& phase) {
        // A rejected pre-effect checkpoint must not masquerade as a sent DELETE.
        auto proposed = progress;
        proposed["phase"] = phase;
        auto error = checkpoint(save, proposed);
        if (!error) progress = std::move(proposed);
        return error;
    };
    if (!SdkRuntime::safeDeleteTransport()) {
        finish("failed", {"unsupported_capability", "Safe destructive HTTP transport is unavailable on this SDK build", {}});
        return;
    }
    if (auto error = state_->readiness(request.receipt.destination)) {
        finish("failed", error);
        return;
    }
    const auto& expected = request.receipt;
    const auto& destination = expected.destination;
    const auto key = expected.objectReference["key"].toString();
    const auto version = expected.objectReference["versionId"].toString();
    if (key.isEmpty() || expected.publicationId.isEmpty() || expected.concurrencyToken.isEmpty() ||
        expected.objectReference["schemaVersion"].toInt() != 1) {
        finish("failed", {"s3_receipt_invalid", "Recorded identity and object token are required", {}});
        return;
    }
    const auto target = expected.toJson();
    if (progress.contains("target") && progress["target"].toObject() != target) {
        finish("changed", {"delete_target_changed", "Deletion journal does not match the original target", {}});
        return;
    }
    progress["target"] = target;
    auto client = state_->client(destination);
    if (!client) {
        finish("failed", accessError());
        return;
    }
    auto phase = progress.value("phase").toString();
    const bool immutable = !version.isEmpty() && version != "null";
    const bool objectPhase = phase.startsWith("object_");
    const bool immutablePhase = phase.startsWith("marker_") || phase.startsWith("version_");
    if ((objectPhase && immutable) || (immutablePhase && !immutable) ||
        (phase == "marker_done" && progress["markerVersionId"].toString().isEmpty()) ||
        (phase.startsWith("version_") && !progress.contains("markerVersionId") && !progress["noncurrent"].toBool())) {
        finish("failed", {"delete_progress_unsupported", "Deletion phase does not match the recorded version", {}});
        return;
    }
    if (phase == "done") {
        finish("removed");
        return;
    }
    if (phase == "marker_intent" || phase == "marker_unknown") {
        // A marker has no publication metadata. A missing current object cannot
        // attribute its marker to this operation rather than an external delete.
        // Retain the exact intent and never issue another marker-creating DELETE.
        head(*client, destination, key);
        finish("unknown", {"delete_unknown", "Marker creation may have completed; its identity is unresolved", {}});
        return;
    }
    auto identity = inspect(*client, expected);
    if (identity.status == "foreign") {
        finish("changed");
        return;
    }
    if (identity.status == "unknown") {
        finish("unknown", identity.error);
        return;
    }
    if (identity.status == "missing") {
        const bool removalStarted = phase == "version_intent" || phase == "version_unknown" || phase == "object_intent" || phase == "object_unknown";
        if (auto error = persist("done")) {
            finish("unknown", error);
            return;
        }
        finish(removalStarted ? "removed" : "missing");
        return;
    }
    if (stopped(request.cancellation)) {
        finish("failed", cancelled());
        return;
    }
    Aws::S3::Model::GetBucketVersioningRequest versioning;
    versioning.SetBucket(aws(destination.settings["bucket"].toString()));
    QElapsedTimer versioningTimer;
    versioningTimer.start();
    auto bucket = client->GetBucketVersioning(versioning);
    if (!bucket.IsSuccess()) {
        finish("unknown", wireError(bucket.GetError(), "delete_versioning", versioningTimer.elapsed()));
        return;
    }
    auto state = bucket.GetResult().GetStatus();
    const bool versioned = state == Aws::S3::Model::BucketVersioningStatus::Enabled || state == Aws::S3::Model::BucketVersioningStatus::Suspended;
    if (version.isEmpty() && versioned) {
        finish("unsupported_safe_removal", {"unsupported_safe_removal", "A recorded version identity is required after bucket versioning changes", {}});
        return;
    }
    auto erase = [&](const QString& exactVersion, const QString& etag) {
        Aws::S3::Model::DeleteObjectRequest remove;
        remove.SetBucket(aws(destination.settings["bucket"].toString()));
        remove.SetKey(aws(key));
        if (!exactVersion.isEmpty()) remove.SetVersionId(aws(exactVersion));
        if (!etag.isEmpty()) remove.SetIfMatch(aws(etag));
        return client->DeleteObject(remove);
    };
    if (immutable) {
        if (!versioned) {
            finish("unknown", {"versioning_inconclusive", "Recorded immutable version requires confirmed bucket versioning", {}});
            return;
        }
        if (phase != "marker_done" && phase != "version_intent" && phase != "version_unknown") {
            QElapsedTimer currentTimer;
            currentTimer.start();
            const auto current = head(*client, destination, key);
            if (!current.IsSuccess() && !absent(current.GetError())) {
                finish("unknown", wireError(current.GetError(), "delete_current_head", currentTimer.elapsed()));
                return;
            }
            const bool isCurrent = current.IsSuccess() && qt(current.GetResult().GetVersionId()) == version;
            if (isCurrent) {
                if (qt(current.GetResult().GetETag()) != expected.concurrencyToken ||
                    metadata(current.GetResult(), "putgd-publication") != expected.publicationId) {
                        finish("changed");
                        return;
                    }
                if (state == Aws::S3::Model::BucketVersioningStatus::Suspended) {
                    const auto nullObject = head(*client, destination, key, "null");
                    if (nullObject.IsSuccess() || !absent(nullObject.GetError()) ||
                        nullObject.GetError().GetResponseHeaders().count("x-amz-delete-marker")) {
                        finish("unsupported_safe_removal", {"unsupported_safe_removal", "Suspended bucket has an unselected or unresolved null version", {}});
                        return;
                    }
                }
                // External versioning changes are outside the object ETag guard.
                const auto rechecked = client->GetBucketVersioning(versioning);
                if (!rechecked.IsSuccess() || rechecked.GetResult().GetStatus() != state) {
                    finish("unknown", {"versioning_changed", "Bucket versioning changed before marker creation", {}});
                    return;
                }
                if (stopped(request.cancellation)) {
                    finish("failed", cancelled());
                    return;
                }
                if (auto error = persistIntent("marker_intent")) {
                    finish("failed", error);
                    return;
                }
                if (stopped(request.cancellation)) {
                    persist("marker_cancelled");
                    finish("failed", cancelled());
                    return;
                }
                QElapsedTimer markerTimer;
                markerTimer.start();
                const auto marked = erase({}, expected.concurrencyToken);
                const auto markerElapsedMs = markerTimer.elapsed();
                if (!marked.IsSuccess()) {
                    const bool unknown = ambiguous(marked.GetError());
                    if (auto error = persist(unknown ? "marker_unknown" : "marker_failed")) {
                        finish("unknown", error);
                        return;
                    }
                    finish(unknown ? "unknown" : (int(marked.GetError().GetResponseCode()) == 412 || int(marked.GetError().GetResponseCode()) == 409 ? "changed" : "failed"), wireError(marked.GetError(), "delete_marker", markerElapsedMs));
                    return;
                }
                if (!marked.GetResult().GetDeleteMarker() || qt(marked.GetResult().GetVersionId()).isEmpty()) {
                    persist("marker_unknown");
                    finish("unknown", {"delete_unknown", "S3 did not return a protective marker identity", {}});
                    return;
                }
                progress["markerVersionId"] = qt(marked.GetResult().GetVersionId());
                if (auto error = persist("marker_done")) {
                    finish("hidden_version_retained", error);
                    return;
                }
            } else {
                progress["noncurrent"] = true;
                if (auto error = persist("version_ready")) {
                    finish("failed", error);
                    return;
                }
            }
        }
        // Verify the exact immutable version again; never adopt a replacement.
        identity = inspect(*client, expected);
        if (identity.status != "own") {
            finish(identity.status == "missing" ? "removed" : (identity.status == "foreign" ? "changed" : "unknown"), identity.error);
            return;
        }
        if (stopped(request.cancellation)) {
            finish(progress.contains("markerVersionId") ? "hidden_version_retained" : "failed", cancelled());
            return;
        }
        if (auto error = persistIntent("version_intent")) {
            finish(progress.contains("markerVersionId") ? "hidden_version_retained" : "failed", error);
            return;
        }
        if (stopped(request.cancellation)) {
            persist("version_cancelled");
            finish(progress.contains("markerVersionId") ? "hidden_version_retained" : "failed", cancelled());
            return;
        }
        QElapsedTimer removeTimer;
        removeTimer.start();
        const auto removed = erase(version, {});
        const auto removeElapsedMs = removeTimer.elapsed();
        if (!removed.IsSuccess()) {
            const bool unknown = ambiguous(removed.GetError());
            persist(unknown ? "version_unknown" : "version_failed");
            finish(progress.contains("markerVersionId") ? "hidden_version_retained" : (unknown ? "unknown" : "failed"), wireError(removed.GetError(), "delete_version", removeElapsedMs));
            return;
        }
    } else {
        // Unversioned delete rechecks the recorded identity and original ETag.
        identity = inspect(*client, expected, true);
        if (identity.status != "own") {
            finish(identity.status == "foreign" ? "changed" : "unknown", identity.error);
            return;
        }
        if (stopped(request.cancellation)) {
            finish("failed", cancelled());
            return;
        }
        if (auto error = persistIntent("object_intent")) {
            finish("failed", error);
            return;
        }
        if (stopped(request.cancellation)) {
            persist("object_cancelled");
            finish("failed", cancelled());
            return;
        }
        QElapsedTimer removeTimer;
        removeTimer.start();
        const auto removed = erase(version, expected.concurrencyToken);
        const auto removeElapsedMs = removeTimer.elapsed();
        if (!removed.IsSuccess()) {
            const bool unknown = ambiguous(removed.GetError());
            persist(unknown ? "object_unknown" : "object_failed");
            finish(unknown ? "unknown" : (int(removed.GetError().GetResponseCode()) == 412 || int(removed.GetError().GetResponseCode()) == 409 ? "changed" : "failed"), wireError(removed.GetError(), "delete_object", removeElapsedMs));
            return;
        }
    }
    if (auto error = persist("done")) {
        finish("unknown", error);
        return;
    }
    finish("removed");
}
void NetworkS3Driver::testDestination(const Destination& destination, DurableCheckpoint save, std::function<void(QJsonObject, Error)> reply) {
    // The explicit setup caller owns this asynchronous operation. No captured input.
    auto temporary = std::make_shared<QTemporaryDir>();
    const QByteArray bytes("put.gd synthetic destination test\n");
    const QString identity = QUuid::createUuid().toString(QUuid::WithoutBraces);
    Artifact artifact;
    artifact.path = temporary->filePath("synthetic.txt");
    artifact.bytes = bytes.size();
    artifact.sha256 = QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
    artifact.mime = "text/plain";
    QFile file(artifact.path);
    if (!temporary->isValid() || !file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size()) {
        reply({}, {"synthetic_artifact_failed", "Could not create private synthetic fixture", {}});
        return;
    }
    file.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
    file.close();
    const auto name = "putgd-test-" + identity + ".txt";
    checkName(destination, name, [this, destination, name, artifact, identity, temporary, bytes, save, reply](NameCheck checked) {
        if (checked.status != "absent" || checked.error) {
            reply({}, checked.error ? checked.error : Error{"synthetic_collision", "Synthetic fixture key is unavailable", {}});
            return;
        }
        UploadRequest request{destination, artifact, name, identity, "absent", checked.objectReference, {}, {}};
        const QJsonObject intent{{"schemaVersion", 1}, {"phase", "synthetic_upload_intent"},
                                 {"destination", destination.toJson()}, {"objectReference", checked.objectReference},
                                 {"publicationId", identity}, {"sha256", artifact.sha256}, {"bytes", double(artifact.bytes)}};
        // The setup caller must durably journal the synthetic target too.
        const auto error = checkpoint(save, intent);
        if (error) {
            reply({}, error);
            return;
        }
        upload(request, [this, temporary, bytes, save, reply](UploadResult uploaded) {
            if (uploaded.status != "uploaded") {
                reply({{"uploadStatus", uploaded.status}}, uploaded.error);
                return;
            }
            const QJsonObject confirmed{{"schemaVersion", 1}, {"phase", "synthetic_uploaded"}, {"receipt", uploaded.receipt.toJson()}};
            if (auto error = checkpoint(save, confirmed)) {
                reply({{"uploadStatus", "uploaded"}, {"receipt", uploaded.receipt.toJson()}}, error);
                return;
            }
            const bool reachable = publicFixtureMatches(uploaded.receipt, bytes);
            QJsonObject result{{"uploadStatus", "uploaded"}, {"publicReachable", reachable}, {"receipt", uploaded.receipt.toJson()}};
            DeleteRequest removeRequest{QUuid::createUuid().toString(QUuid::WithoutBraces), uploaded.receipt, {}, {}};
            remove(removeRequest, save, [result, reachable, reply](DeleteResult removed) mutable {
                result["cleanupStatus"] = removed.status;
                result["cleanupProgress"] = removed.progress;
                reply(result, reachable ? removed.error : Error{"public_url_unreachable", "Synthetic upload succeeded but unauthenticated public GET did not return its bytes", {}});
            });
        });
    });
}
}
