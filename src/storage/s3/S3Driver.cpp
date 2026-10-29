#include "core/DisplaySize.h"
#include "S3Driver.h"
#include "S3SdkContext.h"
#include "PublicUrl.h"
#include "SdkRuntime.h"
#include "S3ErrorDiagnostics.h"
#include "S3Multipart.h"
#include "core/CaBundle.h"
#include <QElapsedTimer>
#include <atomic>
#include <aws/core/auth/AWSCredentials.h>
#include <aws/core/http/HttpClientFactory.h>
#include <aws/core/http/HttpClient.h>
#include <aws/core/http/HttpResponse.h>
#include <algorithm>
#include <array>
#include <aws/core/client/DefaultRetryStrategy.h>
#include <aws/core/utils/memory/stl/AWSStreamFwd.h>
#include <aws/core/utils/threading/PooledThreadExecutor.h>
#include <aws/s3/S3Client.h>
#include <aws/s3/model/HeadObjectRequest.h>
#include <aws/s3/model/PutObjectRequest.h>
#include <aws/s3/model/AbortMultipartUploadRequest.h>
#include <aws/s3/model/CompleteMultipartUploadRequest.h>
#include <aws/s3/model/CompletedMultipartUpload.h>
#include <aws/s3/model/CreateMultipartUploadRequest.h>
#include <aws/s3/model/ListPartsRequest.h>
#include <aws/s3/model/UploadPartRequest.h>
#include <aws/s3/model/DeleteObjectRequest.h>
#include <aws/s3/model/GetBucketVersioningRequest.h>
#include <aws/s3/model/ObjectCannedACL.h>
#include <aws/s3/model/StorageClass.h>
#include <QCryptographicHash>
#include <QFile>
#include <QHostAddress>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QUuid>
#include <condition_variable>
#include <fstream>
#include <map>
#include <mutex>
#include <thread>
namespace putgd {
namespace {
Aws::String aws(const QString& value) { return value.toStdString(); }
// The AppImage's bundled libcurl looks only for Debian's bundle file; AppRun names
// the host's own (#329). Other builds keep libcurl's default.
void useHostCaFile(Aws::Client::ClientConfiguration& config) {
    const auto bundle = hostCaBundle();
    if (!bundle.isEmpty()) config.caFile = aws(bundle);
}
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
Error accessError() { return {"credential_setup_required", "Credentials are not loaded for this destination. Unlock saved credentials, or enter and Store credentials for this session.", {}}; }
QString requestGuidance(const QString& name, int status, const QString& stage, int curlCode) {
    if (name == "InvalidAccessKeyId") return "S3 does not recognize the access key ID. Check that it is correct, active, and belongs to the selected storage service; then Store credentials again.";
    if (name == "SignatureDoesNotMatch") return "S3 could not verify the request signature. Check that the secret access key matches the access key ID, then check the region, endpoint and system clock.";
    if (name == "ExpiredToken" || name == "InvalidToken" || name == "TokenRefreshRequired") return "The temporary session token is expired or invalid. Renew and Store the complete access key, secret key and session token together.";
    if (name == "RequestTimeTooSkewed" || name == "RequestExpired") return "S3 rejected the request time. Synchronize the system clock and try again.";
    if (name == "NoSuchBucket") return "S3 could not find the bucket. Check the bucket name, region and endpoint; the bucket must already exist.";
    if (name == "InvalidBucketName") return "S3 rejected the bucket name. Enter only the existing bucket name, without a URL or folder path.";
    if (name == "AuthorizationHeaderMalformed" || name == "PermanentRedirect" || status == 301) return "S3 rejected the destination region or endpoint. Check that the region and endpoint match the bucket's location.";
    if (name == "AccessControlListNotSupported") return "This bucket does not accept ACLs. Set Upload access request to Use bucket policy, and ensure its public-read policy supports the intended links.";
    if (name == "NoSuchUpload") return "S3 no longer has this multipart upload; it was completed, aborted or expired by a bucket lifecycle rule. Retry to upload the file again.";
    if (name == "EntityTooLarge") return DisplaySize::labels("The storage service rejected the file size. Check its object and part size limits; Amazon S3 accepts objects up to 5 TiB.");
    if (name == "EntityTooSmall" || name == "InvalidPart" || name == "InvalidPartOrder") return DisplaySize::labels("The storage service rejected the uploaded parts. It may not support S3 multipart uploads of 5 MiB or larger parts; check its multipart limits.");
    if (name == "BadDigest" || name == "XAmzContentSHA256Mismatch") return "The storage service received different bytes than put.gd sent. Retry the upload; if it continues, check proxies or network equipment that change request bodies.";
    if ((name == "NotImplemented" || status == 501) && (stage == "upload_put" || stage == "upload_complete"))
        return "The storage service does not support conditional writes (If-None-Match and If-Match), which put.gd uses so an upload never replaces another file. Use a storage service or endpoint that implements S3 conditional writes.";
    if (name == "NotImplemented" || status == 501) return "The storage service does not support a request put.gd needs, such as conditional multipart completion. Check that the endpoint implements S3 conditional writes for multipart uploads.";
    if (status == 403) {
        if (stage == "upload_put") return "S3 denied the upload. Check s3:PutObject in IAM and the bucket policy. Request public read also needs s3:PutObjectAcl and bucket settings that permit public ACLs.";
        if (stage == "upload_create" || stage == "upload_part" || stage == "upload_complete") return DisplaySize::labels("S3 denied the multipart upload that files over 64 MiB use. Check s3:PutObject in IAM and the bucket policy; it covers every part. Request public read also needs s3:PutObjectAcl and bucket settings that permit public ACLs.");
        if (stage == "upload_abort") return "S3 denied removing an incomplete multipart upload, so its uploaded parts keep using storage. Check s3:AbortMultipartUpload in IAM and the bucket policy, or add a bucket lifecycle rule that aborts incomplete multipart uploads.";
        if (stage == "upload_list_parts") return "S3 denied listing the parts of an interrupted multipart upload, so put.gd sends every part again. Check s3:ListMultipartUploadParts in IAM and the bucket policy.";
        if (stage == "delete_versioning") return "S3 denied the bucket versioning check required for safe cleanup. Check s3:GetBucketVersioning in IAM and the bucket policy.";
        if (stage == "delete_version") return "S3 denied deletion of the recorded object version. Check s3:DeleteObjectVersion in IAM and the bucket policy, plus Object Lock, retention and MFA restrictions.";
        if (stage == "delete_object" || stage == "delete_marker") return "S3 denied object cleanup. Check s3:DeleteObject in IAM and the bucket policy, and s3:DeleteObjectVersion when targeting a recorded version (including the null version). Also check Object Lock, retention and MFA restrictions.";
        return "S3 denied the object check. Verify the key pair and s3:GetObject access in IAM and the bucket policy, plus s3:GetObjectVersion for recorded-version checks. A missing object can also return 403 without s3:ListBucket; S3 did not identify the exact cause.";
    }
    if (curlCode == 6) return "The S3 endpoint hostname could not be resolved. Check the endpoint, DNS and network connection.";
    if (curlCode == 7) return "Could not connect to the S3 endpoint. Check the endpoint, network connection, proxy and firewall.";
    if (curlCode == 28 || name == "RequestTimeout") return "The S3 request timed out. Check the network and endpoint, or increase the connection/request deadlines in Advanced connection settings.";
    if (curlCode == 35 || curlCode == 60) return "The secure connection to S3 failed. Check the endpoint, system clock and trusted certificates, including any proxy certificate.";
    if (name == "SlowDown" || status == 429) return "S3 is limiting requests. Wait before retrying and reduce concurrent uploads if this continues.";
    if (status >= 500) return "The S3 service returned a server error. Check service availability and retry after it recovers; any uncertain upload or cleanup remains recorded.";
    if (status < 200) return "S3 could not be reached reliably. Check the endpoint, network, proxy and TLS settings; any uncertain upload or cleanup remains recorded.";
    return QString("S3 rejected the request (HTTP %1). Check the bucket, region, endpoint and access permissions.").arg(status);
}
// S3's x-amz-request-id and x-amz-id-2 identify a failed request to AWS support.
// They carry no secret; anything outside their alphabet is dropped.
QString requestIdentifier(const QString& value, qsizetype maximum) {
    static const QRegularExpression pattern(QStringLiteral("\\A[A-Za-z0-9+/=._-]+\\z"));
    const auto text = value.trimmed();
    return !text.isEmpty() && text.size() <= maximum && pattern.match(text).hasMatch() ? text : QString();
}
template<class E> QString responseHeader(const E& error, const char* name) {
    for (const auto& header : error.GetResponseHeaders())
        if (qt(header.first).compare(QLatin1String(name), Qt::CaseInsensitive) == 0) return qt(header.second);
    return {};
}
template<class E> void addRequestIds(const E& error, QJsonObject& details) {
    auto requestId = requestIdentifier(qt(error.GetRequestId()), 128);
    if (requestId.isEmpty()) requestId = requestIdentifier(responseHeader(error, "x-amz-request-id"), 128);
    const auto extended = requestIdentifier(responseHeader(error, "x-amz-id-2"), 256);
    if (!requestId.isEmpty()) details["requestId"] = requestId;
    if (!extended.isEmpty()) details["extendedRequestId"] = extended;
}
// A successful response's IDs (#148): the SDK keeps only x-amz-request-id, so both
// are read from the response headers once they have all arrived.
struct ResponseIds { QString request, extended; };
template<class R> void watchIds(R& request, ResponseIds& ids) {
    request.SetHeadersReceivedEventHandler([&ids](const Aws::Http::HttpRequest*, Aws::Http::HttpResponse* response) {
        if (!response) return;
        if (response->HasHeader("x-amz-request-id")) ids.request = requestIdentifier(qt(response->GetHeader("x-amz-request-id")), 128);
        if (response->HasHeader("x-amz-id-2")) ids.extended = requestIdentifier(qt(response->GetHeader("x-amz-id-2")), 256);
    });
}
QJsonObject withIds(QJsonObject record, const ResponseIds& ids, const Aws::String& resultId) {
    auto requestId = requestIdentifier(qt(resultId), 128);
    if (requestId.isEmpty()) requestId = ids.request;
    if (!requestId.isEmpty()) record["requestId"] = requestId;
    if (!ids.extended.isEmpty()) record["extendedRequestId"] = ids.extended;
    return record;
}
template<class E> Error wireError(const E& error, const char* stage, qint64 elapsedMs) {
    const auto name = qt(error.GetExceptionName());
    const int status = int(error.GetResponseCode());
    const int curlCode = sdkCurlCode({error.GetMessage().data(), error.GetMessage().size()});
    QString code = "s3_request_failed";
    if (name == "AccessControlListNotSupported") code = "s3_acl_incompatible";
    else if (QStringList{"InvalidAccessKeyId", "SignatureDoesNotMatch", "ExpiredToken", "InvalidToken", "TokenRefreshRequired", "RequestTimeTooSkewed", "RequestExpired"}.contains(name)) code = "s3_authentication_failed";
    else if (name == "NoSuchBucket") code = "s3_bucket_not_found";
    else if (name == "NoSuchUpload") code = "s3_multipart_missing";
    else if (name == "EntityTooLarge") code = "s3_object_too_large";
    else if (name == "NotImplemented" || status == 501) code = "s3_unsupported_request";
    else if (name == "InvalidBucketName") code = "s3_bucket_invalid";
    else if (name == "AuthorizationHeaderMalformed" || name == "PermanentRedirect" || status == 301) code = "s3_region_mismatch";
    else if (name == "ObjectLocked" || qt(error.GetMessage()).contains("retention", Qt::CaseInsensitive) ||
             qt(error.GetMessage()).contains("legal hold", Qt::CaseInsensitive)) code = "s3_retention_blocked";
    else if (qt(error.GetMessage()).contains("MFA", Qt::CaseInsensitive)) code = "s3_mfa_required";
    else if (status == 403) code = "s3_permission_denied";
    else if (status == 409 || status == 412) code = "precondition_changed";
    else if (status < 200 || status >= 500) code = "s3_transport_unknown";
    QString message=requestGuidance(name,status,QString::fromLatin1(stage),curlCode);
    if(code=="s3_retention_blocked")message="S3 blocked deletion because of Object Lock, retention or a legal hold. Review the object's protection settings with the bucket administrator; put.gd does not bypass them.";
    else if(code=="s3_mfa_required")message="S3 requires MFA for this deletion. Ask the bucket administrator to use the required MFA-authorized workflow; put.gd does not bypass it.";
    else if(code=="precondition_changed")message="The S3 object changed while the request was running. Refresh its state before retrying; put.gd will not overwrite or delete an unchecked replacement.";
    QJsonObject details{{"httpStatus", status}, {"sdkErrorType", int(error.GetErrorType())},
             {"curlCode", curlCode},
             {"sdkRetryable", error.ShouldRetry()}, {"requestStage", QString::fromLatin1(stage)},
             {"elapsedMs", qMin(elapsedMs, qint64(86400000))}, {"serviceCode", QStringList{"AccessControlListNotSupported", "AccessDenied", "InvalidAccessKeyId", "SignatureDoesNotMatch", "ExpiredToken", "PreconditionFailed", "ConditionalRequestConflict", "InvalidRequest", "ObjectLocked", "SlowDown", "RequestTimeout", "InternalError", "ServiceUnavailable", "InvalidToken", "TokenRefreshRequired", "RequestTimeTooSkewed", "RequestExpired", "NoSuchBucket", "InvalidBucketName", "AuthorizationHeaderMalformed", "PermanentRedirect", "NoSuchUpload", "EntityTooLarge", "EntityTooSmall", "InvalidPart", "InvalidPartOrder", "BadDigest", "XAmzContentSHA256Mismatch", "NotImplemented"}.contains(name) ? name : QString("Unclassified")}};
    addRequestIds(error, details);
    return {code, message, details};
}
template<class E> bool absent(const E& error) { return int(error.GetResponseCode()) == 404; }
template<class E> bool ambiguous(const E& error) {
    const int status = int(error.GetResponseCode());
    // 501 Not Implemented is a definite refusal of an unsupported request.
    return status < 200 || (status >= 500 && status != 501);
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
    useHostCaFile(config);
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
Head head(Client& client, const Destination& destination, const QString& key, const QString& version = {}, const std::shared_ptr<std::atomic_bool>& cancellation = {}) {
    Aws::S3::Model::HeadObjectRequest request;
    request.SetBucket(aws(destination.settings["bucket"].toString()));
    request.SetKey(aws(key));
    if (!version.isEmpty()) request.SetVersionId(aws(version));
    // SdkRuntime installs the SDK continuation handler as Curl progress, even
    // while a HEAD is waiting for response headers and transfers no body.
    if (cancellation) request.SetContinueRequestHandler([cancellation](const Aws::Http::HttpRequest*) { return !stopped(cancellation); });
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
// Cloudflare R2 (#145) returns a version ID on every write but implements no versioning:
// a request naming a version gets 501, and the bucket reports no versioning status. A
// bucket whose versioning was never enabled holds one object per key, so a receipt from
// it is checked and removed like an unversioned one.
bool versionless(Client& client, const StorageReceipt& expected, const StatResult& found) {
    const auto version = expected.objectReference["versionId"].toString();
    if (found.status != "unknown" || found.error.details["httpStatus"].toInt() != 501 || version.isEmpty() || version == "null") return false;
    Aws::S3::Model::GetBucketVersioningRequest request;
    request.SetBucket(aws(expected.destination.settings["bucket"].toString()));
    const auto outcome = client.GetBucketVersioning(request);
    return outcome.IsSuccess() && outcome.GetResult().GetStatus() == Aws::S3::Model::BucketVersioningStatus::NOT_SET;
}
StorageReceipt withoutVersion(StorageReceipt receipt) {
    receipt.objectReference.remove("versionId");
    return receipt;
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
// One part of the retained artifact as a seekable request body. Positions are
// relative to the part; reads never pass its end. The SDK reads it once for the
// signed payload hash and again for the request, so only this buffer is held.
class PartStream final : public Aws::IOStream {
    class Buffer final : public std::streambuf {
        std::filebuf file_;
        qint64 offset_ = 0, length_ = 0, end_ = 0; // end_: part position after the buffered bytes
        std::array<char, 64 * 1024> bytes_{};
    protected:
        int_type underflow() override {
            if (gptr() < egptr()) return traits_type::to_int_type(*gptr());
            const auto count = qMin<qint64>(qint64(bytes_.size()), length_ - end_);
            if (count <= 0 || file_.pubseekpos(offset_ + end_, std::ios::in) != pos_type(offset_ + end_)) return traits_type::eof();
            const auto read = file_.sgetn(bytes_.data(), count);
            if (read <= 0) return traits_type::eof();
            end_ += read;
            setg(bytes_.data(), bytes_.data(), bytes_.data() + read);
            return traits_type::to_int_type(*gptr());
        }
        pos_type seekoff(off_type delta, std::ios_base::seekdir direction, std::ios_base::openmode mode) override {
            if (!(mode & std::ios_base::in)) return pos_type(off_type(-1));
            const qint64 current = end_ - (egptr() - gptr());
            const qint64 target = (direction == std::ios_base::beg ? 0 : direction == std::ios_base::cur ? current : length_) + delta;
            if (target < 0 || target > length_) return pos_type(off_type(-1));
            if (target != current) {
                end_ = target;
                setg(bytes_.data(), bytes_.data(), bytes_.data());
            }
            return pos_type(target);
        }
        pos_type seekpos(pos_type position, std::ios_base::openmode mode) override {
            return seekoff(off_type(position), std::ios_base::beg, mode);
        }
    public:
        bool open(const QString& path, qint64 offset, qint64 length) {
            offset_ = offset;
            length_ = length;
            return file_.open(path.toStdString(), std::ios::in | std::ios::binary);
        }
    } buffer_;
    bool open_ = false;
public:
    PartStream(const QString& path, qint64 offset, qint64 length) : Aws::IOStream(nullptr) {
        open_ = buffer_.open(path, offset, length);
        rdbuf(&buffer_);
    }
    bool isOpen() const { return open_; }
};
QByteArray partDigest(const QString& path, qint64 offset, qint64 length) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || !file.seek(offset)) return {};
    QCryptographicHash md5(QCryptographicHash::Md5);
    for (qint64 left = length; left > 0;) {
        const auto chunk = file.read(qMin<qint64>(left, 1024 * 1024));
        if (chunk.isEmpty()) return {};
        md5.addData(chunk);
        left -= chunk.size();
    }
    return md5.result();
}
QString plainEtag(const QString& etag) {
    auto value = etag;
    if (value.startsWith('"') && value.endsWith('"') && value.size() >= 2) value = value.mid(1, value.size() - 2);
    return value.toLower();
}
template<class E> Error partError(const E& error, const char* stage, qint64 elapsedMs, int part) {
    auto result = wireError(error, stage, elapsedMs);
    if (part > 0) result.details["partNumber"] = part;
    return result;
}
// Aborts a multipart upload so S3 keeps none of its parts. An upload S3 no longer
// has (completed, aborted, or expired by a lifecycle rule) counts as aborted.
Error abortMultipart(Client& client, const Aws::String& bucket, const QString& key, const QString& uploadId) {
    Aws::S3::Model::AbortMultipartUploadRequest cancel;
    cancel.SetBucket(bucket);
    cancel.SetKey(aws(key));
    cancel.SetUploadId(aws(uploadId));
    Error last;
    for (int attempt = 0; attempt < 3; ++attempt) {
        QElapsedTimer timer;
        timer.start();
        const auto outcome = client.AbortMultipartUpload(cancel);
        if (outcome.IsSuccess() || int(outcome.GetError().GetResponseCode()) == 404) return {};
        last = wireError(outcome.GetError(), "upload_abort", timer.elapsed());
        if (!ambiguous(outcome.GetError())) break;
    }
    return last;
}
// Files above the multipart threshold upload in parts streamed from the retained
// artifact, so memory stays bounded by one read buffer per lane whatever the file size.
// Part requests never publish: an uncertain part is simply sent again under the
// same number. Only CompleteMultipartUpload, sent with the item's create or replace
// precondition, can publish, and only its uncertainty makes the item unknown.
// Cancellation and every failure that ends this attempt abort the upload so no
// parts keep using storage. A durable journal of the upload ID lets an attempt that
// could not abort (a crash, or a lost worker) resume on retry from the parts S3 lists.
UploadResult multipartUpload(Client& client, const UploadRequest& request, const QString& key,
                             const std::function<void(const QString&, qint64)>& report) {
    const auto& settings = request.destination.settings;
    const auto bucket = aws(settings["bucket"].toString());
    const qint64 bytes = request.artifact.bytes;
    const qint64 partBytes = request.resume["partBytes"].toInteger();
    if (!s3ValidPlan(bytes, partBytes))
        return {"failed", {}, {"s3_multipart_invalid", "The multipart upload plan does not fit S3's part limits", {}}};
    const int parts = int(s3PartCount(bytes, partBytes));
    const auto expected = receipt(request.destination, key, request.publicationId, request.artifact.sha256, bytes, {}, {});
    auto save = [&](const QJsonObject& journal) { return request.checkpoint ? checkpoint(request.checkpoint, journal) : Error{}; };
    QJsonObject journal{{"schemaVersion", 1}, {"partBytes", double(partBytes)}};
    QString uploadId;
    // Abort reports whether S3 still holds parts of this upload.
    auto abort = [&](const QString& id, const QString& target) {
        if (id.isEmpty()) return true;
        if (abortMultipart(client, bucket, target, id)) return false;
        save({});
        return true;
    };
    // Ends the attempt without publication: S3 keeps no parts unless abort failed.
    auto fail = [&](Error error, const QString& status = QStringLiteral("failed")) -> UploadResult {
        if (!abort(uploadId, key)) {
            error.details["incompleteUpload"] = true;
            error.message += " The incomplete multipart upload could not be removed, so its uploaded parts keep using storage until a bucket lifecycle rule or a later retry removes them.";
        }
        return {status, {}, error};
    };
    std::map<int, Aws::String> done;
    qint64 completed = 0;
    const auto previous = request.resume["uploadId"].toString();
    const bool interruptedCompletion = request.resume["phase"] == "completing";
    if (!previous.isEmpty() &&
        (request.resume["key"].toString() != key || request.resume["bytes"].toInteger() != bytes ||
         request.resume["sha256"].toString() != request.artifact.sha256 || request.resume["partBytes"].toInteger() != partBytes)) {
        // A journal for another target or another artifact is never resumed.
        abort(previous, request.resume["key"].toString());
    } else if (!previous.isEmpty()) {
        uploadId = previous;
        Aws::S3::Model::ListPartsRequest list;
        list.SetBucket(bucket);
        list.SetKey(aws(key));
        list.SetUploadId(aws(uploadId));
        list.SetMaxParts(1000);
        for (int page = 0; page < 11; ++page) {
            QElapsedTimer timer;
            timer.start();
            const auto outcome = client.ListParts(list);
            if (!outcome.IsSuccess()) {
                const int status = int(outcome.GetError().GetResponseCode());
                if (status == 404) {
                    // Completed or aborted: our object is either published or absent.
                    const auto found = inspect(client, expected, true);
                    if (found.status == "own") {
                        save({});
                        return {"uploaded", found.receipt, {}};
                    }
                    if (found.status == "unknown") return {"unknown", {}, found.error};
                    if (found.status == "foreign")
                        return {interruptedCompletion ? "unknown" : "collision", {}, {"precondition_changed", "Another object now uses the saved object key", {}}};
                    save({});
                    uploadId.clear();
                } else if (status == 403) {
                    // Without the listing, every part is sent again under the same upload.
                    done.clear();
                } else {
                    const auto error = partError(outcome.GetError(), "upload_list_parts", timer.elapsed(), 0);
                    return {interruptedCompletion ? "unknown" : "failed", {}, error};
                }
                break;
            }
            for (const auto& part : outcome.GetResult().GetParts()) {
                const int number = part.GetPartNumber();
                if (number < 1 || number > parts) continue;
                const qint64 offset = qint64(number - 1) * partBytes;
                const qint64 length = qMin(partBytes, bytes - offset);
                // Keep a listed part only if S3 holds exactly these bytes.
                if (part.GetSize() == length && plainEtag(qt(part.GetETag())) == QString::fromLatin1(partDigest(request.artifact.path, offset, length).toHex()))
                    done[number] = part.GetETag();
            }
            if (!outcome.GetResult().GetIsTruncated()) break;
            list.SetPartNumberMarker(outcome.GetResult().GetNextPartNumberMarker());
        }
        for (const auto& [number, etag] : done) completed += qMin(partBytes, bytes - qint64(number - 1) * partBytes);
    }
    if (uploadId.isEmpty()) {
        done.clear();
        completed = 0;
        Aws::S3::Model::CreateMultipartUploadRequest create;
        create.SetBucket(bucket);
        create.SetKey(aws(key));
        create.SetContentType(aws(request.artifact.mime));
        create.SetStorageClass(Aws::S3::Model::StorageClassMapper::GetStorageClassForName(aws(settings["storageClass"].toString())));
        if (settings["aclMode"] == "public-read") create.SetACL(Aws::S3::Model::ObjectCannedACL::public_read);
        create.AddMetadata("putgd-publication", aws(request.publicationId));
        create.AddMetadata("putgd-sha256", aws(request.artifact.sha256));
        for (int attempt = 0; attempt < 3 && uploadId.isEmpty(); ++attempt) {
            if (stopped(request.cancellation)) return {"failed", {}, cancelled()};
            QElapsedTimer timer;
            timer.start();
            const auto outcome = client.CreateMultipartUpload(create);
            if (outcome.IsSuccess()) uploadId = qt(outcome.GetResult().GetUploadId());
            else if (!ambiguous(outcome.GetError()) || attempt == 2) return {"failed", {}, partError(outcome.GetError(), "upload_create", timer.elapsed(), 0)};
        }
        if (uploadId.isEmpty()) return {"failed", {}, {"s3_multipart_invalid", "S3 returned no multipart upload ID", {}}};
    }
    journal["uploadId"] = uploadId;
    journal["key"] = key;
    journal["bytes"] = double(bytes);
    journal["sha256"] = request.artifact.sha256;
    journal["phase"] = "parts";
    // The upload ID is durable before any part relies on it.
    if (auto error = save(journal)) return fail(error);
    report("uploading", completed);
    // Parts go out on one lane, plus one per idle upload slot the resident lends,
    // up to four (#174). Any order is safe: a part is retried under its own number,
    // the journal names only the upload, and completion lists parts in number order.
    // A lane past the lent width finishes its part in flight and starts no other.
    struct Lanes {
        std::mutex mutex;
        std::condition_variable changed;
        int next = 1, width = 1;
        std::array<bool, s3MaximumPartLanes> busy{};
        std::array<qint64, s3MaximumPartLanes> sending{}; // bytes read of each lane's part in flight
        Error error;           // the first error that ends this attempt
        bool vanished = false; // S3 no longer has the upload
        std::atomic_bool ended{false}; // error is set: parts in flight stop (#203)
    } lanes;
    auto progress = [&] { // lanes.mutex held
        qint64 total = completed;
        for (const auto value : lanes.sending) total += value;
        report("uploading", qMin(total, bytes));
    };
    struct Sent { bool ok = false; Aws::String etag; Error error; bool vanished = false; };
    auto sendPart = [&](int number, int lane) -> Sent {
        const qint64 offset = qint64(number - 1) * partBytes;
        const qint64 length = qMin(partBytes, bytes - offset);
        const auto digest = partDigest(request.artifact.path, offset, length);
        if (digest.isEmpty()) return {false, {}, {"artifact_invalid", "Retained artifact is unavailable", {}}};
        Error last;
        for (int attempt = 1; attempt <= 3; ++attempt) {
            if (stopped(request.cancellation)) return {false, {}, cancelled()};
            {
                std::lock_guard lock(lanes.mutex);
                if (lanes.error) return {};
            }
            auto body = Aws::MakeShared<PartStream>("putgd-part", request.artifact.path, offset, length);
            if (!body->isOpen()) return {false, {}, {"artifact_invalid", "Retained artifact is unavailable", {}}};
            Aws::S3::Model::UploadPartRequest put;
            put.SetBucket(bucket);
            put.SetKey(aws(key));
            put.SetUploadId(aws(uploadId));
            put.SetPartNumber(number);
            put.SetContentLength(length);
            // S3 rejects a part whose bytes differ from this digest (BadDigest).
            put.SetContentMD5(aws(QString::fromLatin1(digest.toBase64())));
            put.SetBody(body);
            // Cancel, or another lane's error, cuts a part in flight instead of
            // letting it run to the end of its transfer (#203). A cut part never
            // published anything: it is simply sent again on Retry.
            put.SetContinueRequestHandler([&](const Aws::Http::HttpRequest*) {
                return !stopped(request.cancellation) && !lanes.ended.load();
            });
            put.SetRequestSignedHandler([&](const Aws::Http::HttpRequest&) {
                std::lock_guard lock(lanes.mutex);
                lanes.sending[lane] = 0;
                progress();
            });
            put.SetDataSentEventHandler([&](const Aws::Http::HttpRequest*, long long amount) {
                if (amount <= 0) return;
                const auto position = body->rdbuf()->pubseekoff(0, std::ios::cur, std::ios::in);
                if (position < 0 || position > length) return;
                std::lock_guard lock(lanes.mutex);
                lanes.sending[lane] = qint64(position);
                progress();
            });
            ResponseIds ids;
            watchIds(put, ids);
            QElapsedTimer timer;
            timer.start();
            const auto outcome = client.UploadPart(put);
            if (outcome.IsSuccess()) {
                if (request.trace) {
                    // Diagnostics only (#148): numbers and S3's request IDs, never the key or upload ID.
                    int width = 1;
                    { std::lock_guard lock(lanes.mutex); width = lanes.width; }
                    try {
                        request.trace(withIds({{"event", "part"}, {"partNumber", number}, {"bytes", double(length)}, {"attempt", attempt},
                                               {"elapsedMs", timer.elapsed()}, {"lane", lane}, {"lanes", width}}, ids, outcome.GetResult().GetRequestId()));
                    } catch (...) {}
                }
                return {true, outcome.GetResult().GetETag(), {}};
            }
            // A part cut on cancel or by another lane's error reports that, not its transport error.
            if (stopped(request.cancellation)) return {false, {}, cancelled()};
            if (lanes.ended.load()) return {};
            last = partError(outcome.GetError(), "upload_part", timer.elapsed(), number);
            // The upload vanished (aborted elsewhere, or a lifecycle rule): start again on retry.
            if (int(outcome.GetError().GetResponseCode()) == 404) return {false, {}, last, true};
            if (!ambiguous(outcome.GetError())) return {false, {}, last};
        }
        return {false, {}, last};
    };
    auto sendParts = [&](int lane) {
        std::unique_lock lock(lanes.mutex);
        for (;;) {
            while (lanes.next <= parts && done.count(lanes.next)) ++lanes.next;
            if (lanes.error || lanes.next > parts || lane >= lanes.width) break;
            const int number = lanes.next++;
            lock.unlock();
            const auto sent = sendPart(number, lane);
            lock.lock();
            lanes.sending[lane] = 0;
            if (sent.ok) {
                done[number] = sent.etag;
                completed += qMin(partBytes, bytes - qint64(number - 1) * partBytes);
                progress();
            } else if (!lanes.error) {
                // Another lane's error ends the attempt; this part is sent again on retry.
                lanes.error = sent.error ? sent.error : Error{"s3_multipart_invalid", "A multipart part ended without a result", {}};
                lanes.vanished = sent.vanished;
                lanes.ended = true;
            }
        }
        lanes.busy[lane] = false;
        lanes.changed.notify_all();
    };
    std::array<std::thread, s3MaximumPartLanes> threads;
    {
        std::unique_lock lock(lanes.mutex);
        for (;;) {
            // The lent width comes from the resident without this lock held.
            lock.unlock();
            const int width = 1 + (request.extraLanes ? qBound(0, request.extraLanes(qMin(s3MaximumPartLanes - 1, parts - 1)), s3MaximumPartLanes - 1) : 0);
            lock.lock();
            lanes.width = width;
            while (lanes.next <= parts && done.count(lanes.next)) ++lanes.next;
            const bool more = !lanes.error && lanes.next <= parts;
            for (int lane = 0; more && lane < width; ++lane) {
                if (lanes.busy[lane]) continue;
                // An idle lane's thread has already given up the lock: join it here.
                if (threads[lane].joinable()) threads[lane].join();
                try {
                    threads[lane] = std::thread(sendParts, lane);
                    lanes.busy[lane] = true;
                } catch (...) {
                    if (lane == 0 && !lanes.error) {
                        lanes.error = {"s3_multipart_invalid", "The multipart upload could not start sending parts", {}};
                        lanes.ended = true;
                    }
                }
            }
            if (std::none_of(lanes.busy.begin(), lanes.busy.end(), [](bool busy) { return busy; })) break;
            lanes.changed.wait_for(lock, std::chrono::milliseconds(100));
        }
    }
    for (auto& thread : threads)
        if (thread.joinable()) thread.join();
    if (lanes.error) {
        if (!lanes.vanished) return fail(lanes.error);
        save({});
        return {"failed", {}, lanes.error};
    }
    if (int(done.size()) != parts) return fail({"s3_multipart_invalid", "A multipart part ended without a result", {}});
    report("uploading", completed);
    journal["phase"] = "completing";
    if (auto error = save(journal)) return fail(error);
    report("finalizing", bytes);
    Aws::S3::Model::CompletedMultipartUpload manifest;
    QCryptographicHash composite(QCryptographicHash::Md5);
    bool digestEtags = true;
    for (const auto& [number, etag] : done) {
        Aws::S3::Model::CompletedPart part;
        part.SetPartNumber(number);
        part.SetETag(etag);
        manifest.AddParts(part);
        const auto plain = plainEtag(qt(etag));
        const auto raw = QByteArray::fromHex(plain.toLatin1());
        digestEtags = digestEtags && plain.size() == 32 && raw.size() == 16;
        composite.addData(raw);
    }
    Aws::S3::Model::CompleteMultipartUploadRequest finish;
    finish.SetBucket(bucket);
    finish.SetKey(aws(key));
    finish.SetUploadId(aws(uploadId));
    finish.SetMultipartUpload(manifest);
    finish.SetMpuObjectSize(bytes);
    if (request.condition == "absent") finish.SetIfNoneMatch("*");
    else finish.SetIfMatch(aws(request.concurrencyToken));
    bool uncertain = interruptedCompletion;
    for (int attempt = 0; attempt < 3; ++attempt) {
        ResponseIds ids;
        watchIds(finish, ids);
        QElapsedTimer timer;
        timer.start();
        const auto outcome = client.CompleteMultipartUpload(finish);
        if (outcome.IsSuccess()) {
            save({});
            if (request.trace) {
                try {
                    request.trace(withIds({{"event", "complete"}, {"parts", int(done.size())}, {"bytes", double(bytes)}, {"attempt", attempt + 1},
                                           {"elapsedMs", timer.elapsed()}}, ids, outcome.GetResult().GetRequestId()));
                } catch (...) {}
            }
            const auto etag = qt(outcome.GetResult().GetETag());
            // Plain-MD5 part ETags (no SSE-KMS) fix the object's ETag: MD5 of the part MD5s, a dash, the count.
            const auto expectedEtag = QString::fromLatin1(composite.result().toHex()) + "-" + QString::number(done.size());
            if (digestEtags && plainEtag(etag).contains('-') && plainEtag(etag) != expectedEtag)
                return {"unknown", {}, {"s3_multipart_checksum_mismatch", "S3 completed the upload with a different part checksum than put.gd sent; inspect the object before relying on it", {}}};
            return {"uploaded", receipt(request.destination, key, request.publicationId, request.artifact.sha256, bytes, etag,
                                        qt(outcome.GetResult().GetVersionId())), {}};
        }
        const auto error = partError(outcome.GetError(), "upload_complete", timer.elapsed(), 0);
        const int status = int(outcome.GetError().GetResponseCode());
        const bool conflict = status == 412 || status == 409;
        uncertain = uncertain || ambiguous(outcome.GetError());
        if (!ambiguous(outcome.GetError()) && !conflict && status != 404) return fail(error);
        // A lost reply, a precondition failure or a vanished upload: check what the key holds.
        const auto found = inspect(client, expected, true);
        if (found.status == "own") {
            save({});
            return {"uploaded", found.receipt, {}};
        }
        if (found.status == "unknown") return {"unknown", {}, error};
        if (found.status == "foreign") return fail(error, uncertain ? "unknown" : "collision");
        if (conflict) return fail(error, uncertain ? "unknown" : "collision");
        if (status == 404) {
            save({});
            return {uncertain ? "unknown" : "failed", {}, error};
        }
        // Missing after an uncertain completion: the upload still exists, so complete it again.
    }
    return {"unknown", {}, {"publication_unknown", "Publication remains uncertain at the saved object key", {}}};
}
// Test connection proves the extra multipart permissions on a synthetic key,
// without sending a part: create (s3:PutObject), list (s3:ListMultipartUploadParts),
// abort (s3:AbortMultipartUpload). The journal names the upload before any later step.
QJsonObject multipartProbe(Client& client, const Destination& destination, const DurableCheckpoint& save, Error& error) {
    const auto bucket = aws(destination.settings["bucket"].toString());
    const auto key = objectKey(destination, "putgd-test-" + QUuid::createUuid().toString(QUuid::WithoutBraces) + "-multipart.txt");
    QJsonObject journal{{"schemaVersion", 1}, {"phase", "synthetic_multipart_intent"}, {"destination", destination.toJson()}, {"key", key}};
    if ((error = checkpoint(save, journal))) return {{"status", "failed"}, {"stage", "upload_create"}};
    Aws::S3::Model::CreateMultipartUploadRequest create;
    create.SetBucket(bucket);
    create.SetKey(aws(key));
    create.SetContentType("text/plain");
    create.SetStorageClass(Aws::S3::Model::StorageClassMapper::GetStorageClassForName(aws(destination.settings["storageClass"].toString())));
    if (destination.settings["aclMode"] == "public-read") create.SetACL(Aws::S3::Model::ObjectCannedACL::public_read);
    create.AddMetadata("putgd-publication", aws(QUuid::createUuid().toString(QUuid::WithoutBraces)));
    QElapsedTimer timer;
    timer.start();
    const auto created = client.CreateMultipartUpload(create);
    if (!created.IsSuccess()) {
        error = wireError(created.GetError(), "upload_create", timer.elapsed());
        const int status = int(created.GetError().GetResponseCode());
        return {{"status", status == 403 ? "denied" : ambiguous(created.GetError()) ? "unknown" : "failed"}, {"stage", "upload_create"}};
    }
    const auto uploadId = qt(created.GetResult().GetUploadId());
    journal["phase"] = "synthetic_multipart_created";
    journal["uploadId"] = uploadId;
    QJsonObject result{{"status", "ok"}};
    if ((error = checkpoint(save, journal))) result = {{"status", "failed"}, {"stage", "upload_create"}};
    Aws::S3::Model::ListPartsRequest list;
    list.SetBucket(bucket);
    list.SetKey(aws(key));
    list.SetUploadId(aws(uploadId));
    timer.restart();
    const auto listed = client.ListParts(list);
    if (!listed.IsSuccess() && !error) {
        error = wireError(listed.GetError(), "upload_list_parts", timer.elapsed());
        result = {{"status", int(listed.GetError().GetResponseCode()) == 403 ? "denied" : "failed"}, {"stage", "upload_list_parts"}};
    }
    Aws::S3::Model::AbortMultipartUploadRequest cancel;
    cancel.SetBucket(bucket);
    cancel.SetKey(aws(key));
    cancel.SetUploadId(aws(uploadId));
    timer.restart();
    const auto aborted = client.AbortMultipartUpload(cancel);
    result["abort"] = aborted.IsSuccess() ? "aborted" : ambiguous(aborted.GetError()) ? "unknown" : "failed";
    if (!aborted.IsSuccess() && (!error || result["status"] == "ok")) {
        error = wireError(aborted.GetError(), "upload_abort", timer.elapsed());
        result["status"] = int(aborted.GetError().GetResponseCode()) == 403 ? "denied" : "failed";
        result["stage"] = "upload_abort";
    }
    journal["phase"] = "synthetic_multipart_finished";
    journal["abort"] = result["abort"];
    if (auto saved = checkpoint(save, journal); saved && !error) error = saved;
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
        useHostCaFile(config);
        config.connectTimeoutMs = destination.settings["connectTimeoutMs"].toInt();
        config.httpRequestTimeoutMs = destination.settings["requestTimeoutMs"].toInt();
        config.requestTimeoutMs = destination.settings["lowSpeedTimeSeconds"].toInt() * 1000L;
        config.lowSpeedLimit = destination.settings["lowSpeedBytesPerSecond"].toInt();
        // A multipart upload sends up to four parts at once (#174); other calls use one.
        config.maxConnections = s3MaximumPartLanes;
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
void NetworkS3Driver::checkName(const Destination& destination, const QString& candidate, std::function<void(NameCheck)> reply, std::shared_ptr<std::atomic_bool> cancellation) {
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
    if (stopped(cancellation)) { reply({"inconclusive", {}, {}, cancelled()}); return; }
    const auto outcome = head(*client, destination, key, {}, cancellation);
    if (stopped(cancellation)) { reply({"inconclusive", {}, {}, cancelled()}); return; }
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
    auto found = inspect(*client, expected);
    if (versionless(*client, expected, found)) {
        found = inspect(*client, withoutVersion(expected), true);
        // The recorded ID names the only object the key can hold; keep the receipt as recorded.
        if (found.status == "own") found.receipt.objectReference["versionId"] = expected.objectReference["versionId"];
    }
    reply(found);
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
    auto report = [&](const QString& phase, qint64 transferred) {
        // Informational callbacks cannot affect the publication result. Curl's
        // data-sent handler counts request-body bytes read, not HTTP framing or
        // acknowledgement by the remote service.
        if (request.progress) {
            try { request.progress({phase, transferred, request.artifact.bytes}); }
            catch (...) {}
        }
    };
    report("preparing", 0);
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
    if (!request.resume.isEmpty()) {
        reply(multipartUpload(*client, request, key, report));
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
        qint64 transferred = 0;
        report("uploading", transferred);
        // A new signed SDK request or our bounded outer retry sends the body
        // afresh. Keep each attempt's total equal to the retained artifact.
        put.SetRequestSignedHandler([&](const Aws::Http::HttpRequest&) {
            transferred = 0;
            report("uploading", transferred);
        });
        put.SetDataSentEventHandler([&](const Aws::Http::HttpRequest*, long long amount) {
            if (amount <= 0) return;
            // Curl can rewind this seekable body on a redirect without signing
            // a new request. Query the buffer position rather than accumulating
            // retransmissions; pubseekoff also works after a short EOF read sets
            // the stream's failbit, when tellg() would incorrectly return -1.
            const auto position = stream->rdbuf()->pubseekoff(0, std::ios::cur, std::ios::in);
            if (position < 0 || position > request.artifact.bytes) return;
            transferred = qint64(position);
            report("uploading", transferred);
        });
        QElapsedTimer requestTimer;
        requestTimer.start();
        const auto outcome = client->PutObject(put);
        report("finalizing", transferred);
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
    // A journal that found a versionless bucket (see versionless()) continues unversioned.
    auto expected = progress.value("versionless").toBool() ? withoutVersion(request.receipt) : request.receipt;
    const auto& destination = expected.destination;
    const auto key = expected.objectReference["key"].toString();
    auto version = expected.objectReference["versionId"].toString();
    if (key.isEmpty() || expected.publicationId.isEmpty() || expected.concurrencyToken.isEmpty() ||
        expected.objectReference["schemaVersion"].toInt() != 1) {
        finish("failed", {"s3_receipt_invalid", "Recorded identity and object token are required", {}});
        return;
    }
    const auto target = request.receipt.toJson();
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
    bool immutable = !version.isEmpty() && version != "null";
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
    if (phase.isEmpty() && immutable && versionless(*client, expected, identity)) {
        progress["versionless"] = true;
        expected = withoutVersion(expected);
        version.clear();
        immutable = false;
        identity = inspect(*client, expected);
    }
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
void NetworkS3Driver::abandon(const Destination& destination, const QJsonObject& journal, std::function<void(Error)> reply) {
    const auto uploadId = journal["uploadId"].toString(), key = journal["key"].toString();
    if (uploadId.isEmpty()) {
        reply({});
        return;
    }
    if (auto error = state_->readiness(destination)) {
        reply(error);
        return;
    }
    if (key.isEmpty() || key.toUtf8().size() > 1024 || !key.startsWith(objectKey(destination, {}))) {
        reply({"s3_multipart_invalid", "The saved multipart upload does not belong to this destination", {}});
        return;
    }
    auto client = state_->client(destination);
    if (!client) {
        reply(accessError());
        return;
    }
    reply(abortMultipart(*client, aws(destination.settings["bucket"].toString()), key, uploadId));
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
            remove(removeRequest, save, [this, destination = uploaded.receipt.destination, save, result, reachable, reply](DeleteResult removed) mutable {
                result["cleanupStatus"] = removed.status;
                result["cleanupProgress"] = removed.progress;
                Error multipart;
                if (auto client = state_->client(destination)) result["multipart"] = multipartProbe(*client, destination, save, multipart);
                else multipart = accessError();
                if (reachable && !removed.error && multipart) {
                    // Small uploads work; say what larger (multipart) uploads lose.
                    const auto stage = result["multipart"].toObject()["stage"].toString();
                    const auto abort = result["multipart"].toObject()["abort"].toString();
                    QString message = multipart.message;
                    if (stage == "upload_list_parts" && multipart.details["httpStatus"].toInt() == 403)
                        message = DisplaySize::labels("Uploads work, but S3 denied s3:ListMultipartUploadParts. Files over 64 MiB still upload in parts, but an interrupted upload sends every part again instead of resuming. Grant s3:ListMultipartUploadParts in IAM and the bucket policy.");
                    else if (stage == "upload_abort" && multipart.details["httpStatus"].toInt() == 403)
                        message = DisplaySize::labels("Uploads work, but S3 denied s3:AbortMultipartUpload. A cancelled or failed upload of a file over 64 MiB can leave incomplete parts that keep using storage. Grant s3:AbortMultipartUpload, or add a bucket lifecycle rule that aborts incomplete multipart uploads.");
                    else if (stage == "upload_create" && multipart.details["httpStatus"].toInt() == 403)
                        message = DisplaySize::labels("Uploads work, but S3 denied starting a multipart upload, which files over 64 MiB use. Check s3:PutObject in IAM and the bucket policy, and s3:PutObjectAcl when requesting public read.");
                    if (abort != "aborted" && abort != "")
                        message += " The test's incomplete multipart upload could not be removed; its recorded target is retained.";
                    reply(result, {"s3_multipart_permission", message, multipart.details});
                    return;
                }
                if (reachable) { reply(result, removed.error); return; }
                Error error{"public_url_unreachable", "The test upload succeeded, but its public link did not return the test file. Check Link domain and the bucket public-read policy or ACL settings.", {}};
                if (removed.error) error.message += " Cleanup also failed: " + removed.error.message;
                else if (removed.status != "removed" && removed.status != "missing") error.message += " Test cleanup is incomplete; inspect the recorded test target before retrying.";
                reply(result, error);
            });
        });
    });
}
}
