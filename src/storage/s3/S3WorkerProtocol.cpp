#include "S3WorkerProtocol.h"
#include "IpAddress.h"
#include "PublicUrl.h"
#include <QDir>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QUrl>
#include <cmath>
#include <initializer_list>
#include <limits>

namespace putgd::s3wire {
namespace {
using Keys = std::initializer_list<const char*>;
constexpr qint64 maximumArtifactBytes = 1099511627776LL; // Settings' 1 TiB bound.
bool bounded(const QJsonObject& object) {
    return QJsonDocument(object).toJson(QJsonDocument::Compact).size() <= maximumFrameBytes;
}
bool keys(const QJsonObject& object, Keys required, Keys optional = {}) {
    if (object.size() < qsizetype(required.size()) ||
        object.size() > qsizetype(required.size() + optional.size())) return false;
    for (const auto* key : required) if (!object.contains(QLatin1String(key))) return false;
    for (auto it = object.begin(); it != object.end(); ++it) {
        bool known = false;
        for (const auto* key : required) known |= it.key() == QLatin1String(key);
        for (const auto* key : optional) known |= it.key() == QLatin1String(key);
        if (!known) return false;
    }
    return true;
}
bool text(const QJsonValue& value, qsizetype minimum = 0, qsizetype maximum = 4096) {
    if (!value.isString()) return false;
    const auto string = value.toString();
    const auto utf8 = string.toUtf8();
    return utf8.size() >= minimum && utf8.size() <= maximum && !string.contains(QChar(0)) &&
           QString::fromUtf8(utf8) == string;
}
bool integer(const QJsonValue& value, qint64 minimum, qint64 maximum) {
    if (!value.isDouble()) return false;
    const double number = value.toDouble();
    return std::isfinite(number) && std::floor(number) == number && number >= double(minimum) && number <= double(maximum);
}
bool choice(const QJsonValue& value, Keys choices) {
    if (!value.isString()) return false;
    for (const auto* item : choices) if (value.toString() == QLatin1String(item)) return true;
    return false;
}
bool identifier(const QJsonValue& value) {
    static const QRegularExpression pattern(QStringLiteral("\\A[A-Za-z0-9][A-Za-z0-9._-]{0,63}\\z"));
    return text(value, 1, 64) && pattern.match(value.toString()).hasMatch();
}
bool hash(const QJsonValue& value) {
    static const QRegularExpression pattern(QStringLiteral("\\A[0-9a-f]{64}\\z"));
    return value.isString() && pattern.match(value.toString()).hasMatch();
}
bool webUrl(const QJsonValue& value, bool allowHttpLoopback) {
    if (!text(value, 1, maximumFrameBytes)) return false;
    const QUrl url(value.toString());
    return url.isValid() && !url.host().isEmpty() && url.userInfo().isEmpty() && !url.hasQuery() && !url.hasFragment() &&
           (url.scheme() == QStringLiteral("https") ||
            (allowHttpLoopback && url.scheme() == QStringLiteral("http") && classifyIpAddress(url.host()).loopback));
}
bool settings(const QJsonObject& object) {
    if (!keys(object, {"bucket", "region", "endpoint", "objectPrefix", "storageClass", "aclMode", "publicUrlBase",
                       "addressing", "connectTimeoutMs", "requestTimeoutMs", "lowSpeedBytesPerSecond", "lowSpeedTimeSeconds"})) return false;
    if (!text(object["bucket"], 3, 63) || !text(object["region"], 1, maximumFrameBytes) || !text(object["objectPrefix"], 0, 1022) ||
        !webUrl(object["endpoint"], true) || (!object["publicUrlBase"].isNull() && !webUrl(object["publicUrlBase"], true)) ||
        !choice(object["storageClass"], {"STANDARD", "REDUCED_REDUNDANCY", "STANDARD_IA", "ONEZONE_IA", "INTELLIGENT_TIERING", "GLACIER", "DEEP_ARCHIVE", "GLACIER_IR"}) ||
        !choice(object["aclMode"], {"public-read", "omit"}) || !choice(object["addressing"], {"path", "virtual", "auto"})) return false;
    const auto bucket = object["bucket"].toString();
    if (bucket.contains('/') || bucket.contains(':') || bucket.contains(' ')) return false;
    for (const auto* field : {"connectTimeoutMs", "requestTimeoutMs", "lowSpeedBytesPerSecond", "lowSpeedTimeSeconds"})
        if (!integer(object[QLatin1String(field)], 1, std::numeric_limits<int>::max())) return false;
    const auto host = QUrl(object["endpoint"].toString()).host();
    return !s3VirtualAddressing(object) || (!bucket.contains('.') && !classifyIpAddress(host).numeric);
}
bool credentialReference(const QJsonObject& object) {
    return keys(object, {"mode", "reference"}) &&
           ((object["mode"] == QJsonValue("session") && object["reference"].isNull()) ||
            (object["mode"] == QJsonValue("keychain") && text(object["reference"], 1, 1024) &&
             object["reference"].toString().toUcs4().size() <= 256));
}
QString objectKey(const Destination& destination, const QString& candidate) {
    auto prefix = destination.settings["objectPrefix"].toString();
    if (!prefix.isEmpty() && !prefix.endsWith('/')) prefix += '/';
    return prefix + candidate;
}
// checkName emits a two-field absent reference or a three-field present reference.
// Receipts have exactly five fields. No generic provider object is accepted here.
bool reference(const QJsonObject& object, bool receipt) {
    if (receipt) {
        return keys(object, {"schemaVersion", "key", "versionId", "sha256", "bytes"}) &&
               integer(object["schemaVersion"], 1, 1) && text(object["key"], 1, 1024) &&
               text(object["versionId"], 0, 4096) && hash(object["sha256"]) &&
               integer(object["bytes"], 0, maximumArtifactBytes);
    }
    return keys(object, {"schemaVersion", "key"}, {"versionId"}) &&
           integer(object["schemaVersion"], 1, 1) && text(object["key"], 1, 1024) &&
           (!object.contains("versionId") || text(object["versionId"], 0, 4096));
}
bool deleteStatus(const QJsonValue& value) {
    return choice(value, {"removed", "missing", "changed", "failed", "unknown", "hidden_version_retained", "unsupported_safe_removal"});
}
QJsonValue optionalReceipt(const QString& status, const QString& success, const StorageReceipt& receipt) {
    return status == success ? QJsonValue(receipt.toJson()) : QJsonValue(QJsonValue::Null);
}
bool resultReceipt(const QJsonValue& value, bool success, StorageReceipt& receipt) {
    return success ? value.isObject() && decode(value.toObject(), receipt) : value.isNull();
}
bool errorDetails(const QJsonObject& details) {
    // Error.details is an open application-owned JSON object, including durable
    // checkpoint errors. Preserve future safe fields rather than inventing a new
    // closed schema. NetworkS3Driver sanitizes SDK errors before reaching here.
    if (details.contains("httpStatus") && !integer(details["httpStatus"], -1, 599)) return false;
    if (details.contains("serviceCode") && !choice(details["serviceCode"], {"AccessControlListNotSupported", "AccessDenied", "InvalidAccessKeyId",
            "SignatureDoesNotMatch", "ExpiredToken", "PreconditionFailed", "ConditionalRequestConflict", "InvalidRequest", "ObjectLocked",
            "SlowDown", "RequestTimeout", "InternalError", "ServiceUnavailable", "Unclassified"})) return false;
    if (details.contains("itemIndex") && !integer(details["itemIndex"], 0, 9999)) return false;
    if (details.contains("offset") && !integer(details["offset"], 0, maximumFrameBytes)) return false;
    if (details.contains("path") && !text(details["path"], 1, 4096)) return false;
    return !details.contains("observedRevision") || text(details["observedRevision"], 1, 256);
}
}

QJsonObject encode(const UploadRequest& request) {
    return {{"destination", request.destination.toJson()}, {"artifact", request.artifact.toJson()},
            {"candidate", request.candidate}, {"publicationId", request.publicationId}, {"condition", request.condition},
            {"objectReference", request.objectReference}, {"concurrencyToken", request.concurrencyToken}};
}
QJsonObject encode(const DeleteRequest& request) {
    return {{"id", request.id}, {"receipt", request.receipt.toJson()}, {"progress", request.progress}};
}
QJsonObject encode(const NameCheck& result) {
    return {{"status", result.status}, {"objectReference", result.objectReference},
            {"concurrencyToken", result.concurrencyToken}, {"error", encode(result.error)}};
}
QJsonObject encode(const UploadResult& result) {
    return {{"status", result.status}, {"receipt", optionalReceipt(result.status, "uploaded", result.receipt)}, {"error", encode(result.error)}};
}
QJsonObject encode(const StatResult& result) {
    return {{"status", result.status}, {"receipt", optionalReceipt(result.status, "own", result.receipt)}, {"error", encode(result.error)}};
}
QJsonObject encode(const DeleteResult& result) {
    return {{"status", result.status}, {"progress", result.progress}, {"error", encode(result.error)}};
}
QJsonObject encode(const Error& error) { return error.toJson(); }
QJsonObject encode(const Credentials& credentials) {
    return {{"accessKey", credentials.accessKey}, {"secretKey", credentials.secretKey},
            {"sessionToken", credentials.sessionToken}, {"generation", QString::number(credentials.generation)}};
}

bool decode(const QJsonObject& object, Destination& destination) {
    if (!bounded(object) || !keys(object, {"providerId", "profileId", "profileRevision", "settings", "credentials"}) ||
        object["providerId"] != QJsonValue("s3") || !identifier(object["profileId"]) || !text(object["profileRevision"], 1, 256) ||
        !object["settings"].isObject() || !settings(object["settings"].toObject()) ||
        !object["credentials"].isObject() || !credentialReference(object["credentials"].toObject())) return false;
    destination = Destination::fromJson(object);
    return true;
}
bool decode(const QJsonObject& object, Artifact& artifact) {
    if (!bounded(object) || !keys(object, {"id", "path", "name", "mime", "category", "bytes", "sha256", "originalFile", "previewPath", "namingPrepared"}) ||
        !text(object["id"], 1, 256) || !text(object["path"], 1, 4096) || !QDir::isAbsolutePath(object["path"].toString()) ||
        !text(object["name"], 1, 4096) || !text(object["mime"], 1, 256) ||
        !choice(object["category"], {"", "images", "videos", "text", "other"}) || !integer(object["bytes"], 0, maximumArtifactBytes) ||
        !hash(object["sha256"]) || !object["originalFile"].isBool() || !text(object["previewPath"], 0, 4096) || !object["namingPrepared"].isBool()) return false;
    const auto preview = object["previewPath"].toString();
    if (!preview.isEmpty() && !QDir::isAbsolutePath(preview)) return false;
    artifact = Artifact::fromJson(object);
    return true;
}
bool decode(const QJsonObject& object, StorageReceipt& receipt) {
    StorageReceipt value;
    if (!bounded(object) || !keys(object, {"schemaVersion", "destination", "objectReference", "concurrencyToken", "publicationId", "directUrl"}) ||
        !integer(object["schemaVersion"], 1, 1) || !object["destination"].isObject() || !decode(object["destination"].toObject(), value.destination) ||
        !object["objectReference"].isObject() || !reference(object["objectReference"].toObject(), true) ||
        !text(object["concurrencyToken"], 1, 4096) || !text(object["publicationId"], 1, 4096) || !webUrl(object["directUrl"], true)) return false;
    value.objectReference = object["objectReference"].toObject();
    value.concurrencyToken = object["concurrencyToken"].toString();
    value.publicationId = object["publicationId"].toString();
    value.directUrl = object["directUrl"].toString();
    if (value.directUrl != publicObjectUrl(value.destination.settings, value.objectReference["key"].toString()) || value.validate()) return false;
    receipt = std::move(value);
    return true;
}
bool decode(const QJsonObject& object, Error& error) {
    static const QRegularExpression codePattern(QStringLiteral("\\A[a-z][a-z0-9_]{0,127}\\z"));
    if (!bounded(object) || !keys(object, {"code", "message", "details"}) || !text(object["code"], 0, 128) ||
        !text(object["message"], 0, 4096) || !object["details"].isObject() || !errorDetails(object["details"].toObject())) return false;
    const auto code = object["code"].toString();
    if (code.isEmpty()) {
        if (!object["message"].toString().isEmpty() || !object["details"].toObject().isEmpty()) return false;
    } else if (!codePattern.match(code).hasMatch() || object["message"].toString().isEmpty()) return false;
    error = {code, object["message"].toString(), object["details"].toObject()};
    return true;
}
bool decode(const QJsonObject& object, Credentials& credentials) {
    static const QRegularExpression decimal(QStringLiteral("\\A(?:0|[1-9][0-9]{0,19})\\z"));
    if (!bounded(object) || !keys(object, {"accessKey", "secretKey", "sessionToken", "generation"}) ||
        !text(object["accessKey"], 1, 16384) || object["accessKey"].toString().size() > 4096 ||
        !text(object["secretKey"], 1, 16384) || object["secretKey"].toString().size() > 4096 ||
        !text(object["sessionToken"], 0, 65536) || object["sessionToken"].toString().size() > 16384 ||
        !object["generation"].isString() || !decimal.match(object["generation"].toString()).hasMatch()) return false;
    bool valid = false;
    const auto generation = object["generation"].toString().toULongLong(&valid);
    if (!valid) return false;
    credentials = {object["accessKey"].toString(), object["secretKey"].toString(), object["sessionToken"].toString(), generation};
    return true;
}
bool decode(const QJsonObject& object, UploadRequest& request) {
    UploadRequest value;
    if (!bounded(object) || !keys(object, {"destination", "artifact", "candidate", "publicationId", "condition", "objectReference", "concurrencyToken"}) ||
        !object["destination"].isObject() || !decode(object["destination"].toObject(), value.destination) ||
        !object["artifact"].isObject() || !decode(object["artifact"].toObject(), value.artifact) ||
        !text(object["candidate"], 1, 1024) || !text(object["publicationId"], 1, 4096) ||
        !text(object["condition"], 0, 128) || !object["objectReference"].isObject() ||
        !reference(object["objectReference"].toObject(), false) || !text(object["concurrencyToken"], 0, 4096)) return false;
    value.candidate = object["candidate"].toString();
    value.publicationId = object["publicationId"].toString();
    value.condition = object["condition"].toString();
    value.objectReference = object["objectReference"].toObject();
    value.concurrencyToken = object["concurrencyToken"].toString();
    // The driver owns the write precondition and persisted-key agreement.
    // Typed unsupported values reach its existing non-destructive rejection.
    request = std::move(value);
    return true;
}
bool decode(const QJsonObject& object, DeleteRequest& request) {
    DeleteRequest value;
    if (!bounded(object) || !keys(object, {"id", "receipt", "progress"}) || !text(object["id"], 1, 256) ||
        !object["receipt"].isObject() || !decode(object["receipt"].toObject(), value.receipt) ||
        !object["progress"].isObject()) return false;
    value.id = object["id"].toString();
    value.progress = object["progress"].toObject();
    // A different valid journal target is left for the driver's explicit changed
    // outcome. Its durable state machine compares the complete target before IO.
    request = std::move(value);
    return true;
}
bool decode(const QJsonObject& object, NameCheck& result) {
    NameCheck value;
    if (!bounded(object) || !keys(object, {"status", "objectReference", "concurrencyToken", "error"}) ||
        !choice(object["status"], {"absent", "present", "inconclusive"}) || !object["objectReference"].isObject() ||
        !text(object["concurrencyToken"], 0, 4096) || !object["error"].isObject() || !decode(object["error"].toObject(), value.error)) return false;
    value.status = object["status"].toString();
    value.objectReference = object["objectReference"].toObject();
    value.concurrencyToken = object["concurrencyToken"].toString();
    if (value.status == QStringLiteral("inconclusive")) {
        if (!value.objectReference.isEmpty() || !value.concurrencyToken.isEmpty() || !value.error) return false;
    } else {
        if (!reference(value.objectReference, false) || value.error ||
            (value.status == QStringLiteral("absent") && (!value.concurrencyToken.isEmpty() || value.objectReference.contains("versionId"))) ||
            (value.status == QStringLiteral("present") && (value.concurrencyToken.isEmpty() || !value.objectReference.contains("versionId")))) return false;
    }
    result = std::move(value);
    return true;
}
bool decode(const QJsonObject& object, UploadResult& result) {
    UploadResult value;
    if (!bounded(object) || !keys(object, {"status", "receipt", "error"}) ||
        !choice(object["status"], {"uploaded", "collision", "failed", "unknown"}) ||
        !object["error"].isObject() || !decode(object["error"].toObject(), value.error)) return false;
    value.status = object["status"].toString();
    const bool success = value.status == QStringLiteral("uploaded");
    if (!resultReceipt(object["receipt"], success, value.receipt) || (success && value.error) || (!success && !value.error)) return false;
    result = std::move(value);
    return true;
}
bool decode(const QJsonObject& object, StatResult& result) {
    StatResult value;
    if (!bounded(object) || !keys(object, {"status", "receipt", "error"}) ||
        !choice(object["status"], {"own", "foreign", "missing", "unknown"}) ||
        !object["error"].isObject() || !decode(object["error"].toObject(), value.error)) return false;
    value.status = object["status"].toString();
    if (!resultReceipt(object["receipt"], value.status == QStringLiteral("own"), value.receipt) ||
        (value.status != QStringLiteral("unknown") && value.error)) return false;
    result = std::move(value);
    return true;
}
bool decode(const QJsonObject& object, DeleteResult& result) {
    DeleteResult value;
    if (!bounded(object) || !keys(object, {"status", "progress", "error"}) || !deleteStatus(object["status"]) ||
        !object["progress"].isObject() ||
        !object["error"].isObject() || !decode(object["error"].toObject(), value.error)) return false;
    value.status = object["status"].toString();
    value.progress = object["progress"].toObject();
    if (((value.status == QStringLiteral("removed") || value.status == QStringLiteral("missing")) && value.error) ||
        ((value.status == QStringLiteral("failed") || value.status == QStringLiteral("unsupported_safe_removal")) && !value.error)) return false;
    result = std::move(value);
    return true;
}
bool validateRequest(const QString& method, const QJsonObject& object) {
    if (!bounded(object)) return false;
    if (method == QStringLiteral("upload") || method == QStringLiteral("reconcile")) {
        UploadRequest request;
        return decode(object, request);
    }
    if (method == QStringLiteral("stat")) {
        StorageReceipt receipt;
        return decode(object, receipt);
    }
    if (method == QStringLiteral("remove")) {
        DeleteRequest request;
        return decode(object, request);
    }
    Destination destination;
    if (method == QStringLiteral("testDestination"))
        return keys(object, {"destination"}) && object["destination"].isObject() && decode(object["destination"].toObject(), destination);
    if (method == QStringLiteral("checkName")) {
        if (!keys(object, {"destination", "candidate"}) || !object["destination"].isObject() ||
            !decode(object["destination"].toObject(), destination) || !text(object["candidate"], 1, 1024)) return false;
        return text(QJsonValue(objectKey(destination, object["candidate"].toString())), 1, 1024);
    }
    return false;
}
bool decodeTestDestinationResult(const QJsonObject& object) {
    if (!bounded(object)) return false;
    if (object.isEmpty()) return true;
    if (!choice(object["uploadStatus"], {"uploaded", "collision", "failed", "unknown"})) return false;
    if (object["uploadStatus"] != QJsonValue("uploaded")) return keys(object, {"uploadStatus"});
    StorageReceipt receipt;
    if (!object["receipt"].isObject() || !decode(object["receipt"].toObject(), receipt)) return false;
    if (keys(object, {"uploadStatus", "receipt"})) return true;
    return keys(object, {"uploadStatus", "receipt", "publicReachable", "cleanupStatus", "cleanupProgress"}) &&
           object["publicReachable"].isBool() && deleteStatus(object["cleanupStatus"]) &&
           object["cleanupProgress"].isObject();
}
}
