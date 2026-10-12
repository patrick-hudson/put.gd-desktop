#include "storage/s3/S3WorkerProtocol.h"
#include "storage/s3/PublicUrl.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QTest>
#include <limits>

using namespace putgd;
namespace wire = putgd::s3wire;
namespace {
Destination destination() {
    return {"s3", "synthetic-profile", "synthetic-revision",
            {{"bucket", "isolated-fixture"}, {"region", "eu-west-3"}, {"endpoint", "http://127.0.0.1:12345"},
             {"objectPrefix", "prefix é"}, {"storageClass", "STANDARD"}, {"aclMode", "omit"},
             {"publicUrlBase", QJsonValue::Null}, {"addressing", "path"}, {"connectTimeoutMs", 500},
             {"requestTimeoutMs", 2000}, {"lowSpeedBytesPerSecond", 1}, {"lowSpeedTimeSeconds", 1}},
            {{"mode", "session"}, {"reference", QJsonValue::Null}}};
}
Artifact artifact() {
    return {"synthetic-item", "/tmp/synthetic-opaque-artifact", "a b.txt", "text/plain", "", QString(64, 'a'), 17, false, {}, false};
}
UploadRequest upload() {
    return {destination(), artifact(), "a b.txt", "synthetic-operation/item", "absent",
            {{"schemaVersion", 1}, {"key", "prefix é/a b.txt"}}, {}, std::make_shared<std::atomic_bool>(false)};
}
StorageReceipt receipt() {
    StorageReceipt value;
    value.destination = destination();
    value.objectReference = {{"schemaVersion", 1}, {"key", "prefix é/a b.txt"}, {"versionId", "synthetic-version"},
                             {"sha256", QString(64, 'a')}, {"bytes", 17}};
    value.concurrencyToken = "\"synthetic-etag\"";
    value.publicationId = "synthetic-operation/item";
    value.directUrl = publicObjectUrl(value.destination.settings, value.objectReference["key"].toString());
    return value;
}
Error failure() { return {"s3_request_failed", "S3 request failed; check destination access, ACL and retention settings",
                          {{"httpStatus", 403}, {"serviceCode", "AccessDenied"}}}; }
}
class S3WorkerProtocolTest final : public QObject {
    Q_OBJECT
private slots:
    void requestsRoundTripAndCancellationStaysLocal() {
        const auto original = upload();
        UploadRequest decoded;
        QVERIFY(wire::decode(wire::encode(original), decoded));
        QCOMPARE(wire::encode(decoded), wire::encode(original));
        QVERIFY(!decoded.cancellation);
        QVERIFY(!wire::encode(original).contains("cancellation"));
        QVERIFY(wire::validateRequest("upload", wire::encode(original)));
        QVERIFY(wire::validateRequest("reconcile", wire::encode(original)));
        auto replace = original;
        replace.condition = "match";
        replace.objectReference["versionId"] = "null";
        replace.concurrencyToken = "\"old-etag\"";
        QVERIFY(wire::decode(wire::encode(replace), decoded));
        QCOMPARE(decoded.concurrencyToken, replace.concurrencyToken);
        const DeleteRequest remove{"synthetic-delete", receipt(), {}, std::make_shared<std::atomic_bool>(true)};
        DeleteRequest deleted;
        QVERIFY(wire::decode(wire::encode(remove), deleted));
        QCOMPARE(wire::encode(deleted), wire::encode(remove));
        QVERIFY(!deleted.cancellation);
        QVERIFY(wire::validateRequest("remove", wire::encode(remove)));
        QVERIFY(wire::validateRequest("stat", receipt().toJson()));
        QVERIFY(wire::validateRequest("testDestination", {{"destination", destination().toJson()}}));
        QVERIFY(wire::validateRequest("checkName", {{"destination", destination().toJson()}, {"candidate", "a b.txt"}}));
        QVERIFY(!wire::validateRequest("unknownMethod", {}));
    }
    void driverOwnsTypedUnsupportedConditionsAndOpaqueProgress() {
        auto request = upload();
        request.condition = "unsupported-future-condition";
        request.objectReference["key"] = "different-persisted-key";
        request.concurrencyToken = {};
        UploadRequest decoded;
        QVERIFY(wire::decode(wire::encode(request), decoded));
        QCOMPARE(decoded.condition, request.condition);
        QCOMPARE(decoded.objectReference, request.objectReference);
        // These are current driver rejection cases. The codec must preserve them
        // so delete_progress_unsupported remains an explicit driver outcome.
        const QList<QJsonObject> journals{{{"schemaVersion", 2}}, {{"schemaVersion", "one"}},
            {{"schemaVersion", 1}, {"phase", "future-phase"}, {"target", QJsonObject{}}},
            {{"schemaVersion", 1}, {"phase", "marker_done"}}};
        for (const auto& journal : journals) {
            const DeleteRequest remove{"synthetic-delete", receipt(), journal, {}};
            DeleteRequest decodedRemove;
            QVERIFY(wire::decode(wire::encode(remove), decodedRemove));
            QCOMPARE(decodedRemove.progress, journal);
            DeleteResult result{"failed", journal, {"delete_progress_unsupported", "Unsupported deletion journal schema or phase", {}}};
            DeleteResult decodedResult;
            QVERIFY(wire::decode(wire::encode(result), decodedResult));
            QCOMPARE(decodedResult.progress, journal);
            QCOMPARE(decodedResult.error.code, result.error.code);
        }
    }
    void malformedRequestsNeverPartiallyDecode() {
        const auto valid = wire::encode(upload());
        for (const auto& key : valid.keys()) {
            auto missing = valid;
            missing.remove(key);
            auto decoded = upload();
            decoded.publicationId = "sentinel";
            QVERIFY(!wire::decode(missing, decoded));
            QCOMPARE(decoded.publicationId, QString("sentinel"));
            auto wrongType = valid;
            wrongType[key] = QJsonArray{};
            QVERIFY(!wire::decode(wrongType, decoded));
            QCOMPARE(decoded.publicationId, QString("sentinel"));
        }
        auto unexpected = valid;
        unexpected["extra"] = true;
        UploadRequest decoded;
        QVERIFY(!wire::decode(unexpected, decoded));
        unexpected = valid;
        unexpected["cancellation"] = false;
        QVERIFY(!wire::decode(unexpected, decoded));
        auto nested = valid["objectReference"].toObject();
        nested["schemaVersion"] = "1";
        unexpected = valid;
        unexpected["objectReference"] = nested;
        QVERIFY(!wire::decode(unexpected, decoded));
        const auto remove = wire::encode(DeleteRequest{"id", receipt(), {}, {}});
        auto wrongProgress = remove;
        wrongProgress["progress"] = QJsonValue::Null;
        DeleteRequest decodedRemove;
        QVERIFY(!wire::decode(wrongProgress, decodedRemove));
    }
    void keyLimitCountsUtf8AndIncludesPrefix() {
        auto target = destination();
        target.settings["objectPrefix"] = "";
        const QString boundary(512, QChar(0x00e9));
        QVERIFY(wire::validateRequest("checkName", {{"destination", target.toJson()}, {"candidate", boundary}}));
        QVERIFY(!wire::validateRequest("checkName", {{"destination", target.toJson()}, {"candidate", boundary + "a"}}));
        target.settings["objectPrefix"] = "p";
        QVERIFY(!wire::validateRequest("checkName", {{"destination", target.toJson()}, {"candidate", boundary}}));
        auto nul = QStringLiteral("a");
        nul += QChar(0);
        QVERIFY(!wire::validateRequest("checkName", {{"destination", target.toJson()}, {"candidate", nul}}));
        UploadRequest decoded;
        auto encoded = wire::encode(upload());
        auto reference = encoded["objectReference"].toObject();
        reference["key"] = boundary + "a";
        encoded["objectReference"] = reference;
        QVERIFY(!wire::decode(encoded, decoded));
    }
    void destinationSchemaAndSettingsAreStrict() {
        const auto valid = destination().toJson();
        Destination decoded;
        QVERIFY(wire::decode(valid, decoded));
        QCOMPARE(decoded.toJson(), valid);
        auto modified = valid;
        modified["providerId"] = "another-provider";
        QVERIFY(!wire::decode(modified, decoded));
        modified = valid;
        auto settings = modified["settings"].toObject();
        settings["connectTimeoutMs"] = "500";
        modified["settings"] = settings;
        QVERIFY(!wire::decode(modified, decoded));
        settings["connectTimeoutMs"] = 0;
        modified["settings"] = settings;
        QVERIFY(!wire::decode(modified, decoded));
        settings["connectTimeoutMs"] = 500;
        settings["endpoint"] = "http://example.com";
        modified["settings"] = settings;
        QVERIFY(!wire::decode(modified, decoded));
        settings["endpoint"] = "https://user:password@example.com";
        modified["settings"] = settings;
        QVERIFY(!wire::decode(modified, decoded));
        settings["endpoint"] = "https://example.com";
        settings["extra"] = true;
        modified["settings"] = settings;
        QVERIFY(!wire::decode(modified, decoded));
        modified = valid;
        modified["credentials"] = QJsonObject{{"mode", "session"}, {"reference", "unexpected-reference"}};
        QVERIFY(!wire::decode(modified, decoded));
        modified["credentials"] = QJsonObject{{"mode", "keychain"}, {"reference", "synthetic-reference"}};
        QVERIFY(wire::decode(modified, decoded));
        modified["credentials"] = QJsonObject{{"mode", "session"}, {"reference", QJsonValue::Null}, {"secretKey", "synthetic-only"}};
        QVERIFY(!wire::decode(modified, decoded));
    }
    void artifactNumbersAndRequiredMetadataAreStrict() {
        const auto valid = artifact().toJson();
        Artifact decoded;
        QVERIFY(wire::decode(valid, decoded));
        QCOMPARE(decoded.toJson(), valid);
        for (const auto& invalid : {QJsonValue("17"), QJsonValue(-1), QJsonValue(1.5), QJsonValue(1099511627777.0)}) {
            auto modified = valid;
            modified["bytes"] = invalid;
            QVERIFY(!wire::decode(modified, decoded));
        }
        auto empty = valid;
        empty["bytes"] = 0;
        QVERIFY(wire::decode(empty, decoded));
        QCOMPARE(decoded.bytes, qint64(0));
        auto modified = valid;
        modified["sha256"] = QString(64, 'z');
        QVERIFY(!wire::decode(modified, decoded));
        modified = valid;
        modified["path"] = "relative/path";
        QVERIFY(!wire::decode(modified, decoded));
        modified = valid;
        modified["originalFile"] = "false";
        QVERIFY(!wire::decode(modified, decoded));
        modified = valid;
        modified.remove("namingPrepared");
        QVERIFY(!wire::decode(modified, decoded));
    }
    void receiptsRequireRealIdentityAndCanonicalUrl() {
        const auto valid = receipt().toJson();
        StorageReceipt decoded;
        QVERIFY(wire::decode(valid, decoded));
        QCOMPARE(decoded.toJson(), valid);
        for (const auto& field : {"schemaVersion", "objectReference", "concurrencyToken", "publicationId", "directUrl"}) {
            auto modified = valid;
            modified.remove(QLatin1String(field));
            QVERIFY(!wire::decode(modified, decoded));
        }
        auto modified = valid;
        modified["schemaVersion"] = 1.5;
        QVERIFY(!wire::decode(modified, decoded));
        modified = valid;
        modified["directUrl"] = "https://example.com/wrong-object";
        QVERIFY(!wire::decode(modified, decoded));
        modified = valid;
        auto object = modified["objectReference"].toObject();
        object.remove("sha256");
        modified["objectReference"] = object;
        QVERIFY(!wire::decode(modified, decoded));
        modified = valid;
        object = modified["objectReference"].toObject();
        object["versionId"] = "null";
        modified["objectReference"] = object;
        QVERIFY(wire::decode(modified, decoded));
    }
    void resultStatusDomainsAndNullUnusedReceipts() {
        NameCheck checked;
        const NameCheck absent{"absent", {{"schemaVersion", 1}, {"key", "key"}}, {}, {}};
        QVERIFY(wire::decode(wire::encode(absent), checked));
        auto present = absent;
        present.status = "present";
        present.objectReference["versionId"] = "";
        present.concurrencyToken = "\"etag\"";
        QVERIFY(wire::decode(wire::encode(present), checked));
        QVERIFY(wire::decode(wire::encode(NameCheck{"inconclusive", {}, {}, failure()}), checked));
        auto malformed = wire::encode(absent);
        malformed["status"] = "missing";
        QVERIFY(!wire::decode(malformed, checked));
        malformed = wire::encode(present);
        malformed["error"] = wire::encode(failure());
        QVERIFY(!wire::decode(malformed, checked));
        UploadResult uploaded;
        QVERIFY(wire::decode(wire::encode(UploadResult{"uploaded", receipt(), {}}), uploaded));
        for (const auto& status : {"collision", "failed", "unknown"}) {
            const auto encoded = wire::encode(UploadResult{status, {}, failure()});
            QVERIFY(encoded["receipt"].isNull());
            QVERIFY(wire::decode(encoded, uploaded));
        }
        malformed = wire::encode(UploadResult{"uploaded", receipt(), {}});
        malformed["receipt"] = QJsonObject{};
        QVERIFY(!wire::decode(malformed, uploaded));
        malformed["receipt"] = QJsonValue::Null;
        QVERIFY(!wire::decode(malformed, uploaded));
        malformed = wire::encode(UploadResult{"failed", {}, failure()});
        malformed["receipt"] = receipt().toJson();
        QVERIFY(!wire::decode(malformed, uploaded));
        StatResult stat;
        QVERIFY(wire::decode(wire::encode(StatResult{"own", receipt(), {}}), stat));
        for (const auto& status : {"foreign", "missing", "unknown"})
            QVERIFY(wire::decode(wire::encode(StatResult{status, {}, {}}), stat));
        DeleteResult deleted;
        for (const auto& status : {"removed", "missing", "changed", "unknown"})
            QVERIFY(wire::decode(wire::encode(DeleteResult{status, {}, {}}), deleted));
        for (const auto& status : {"failed", "hidden_version_retained", "unsupported_safe_removal"})
            QVERIFY(wire::decode(wire::encode(DeleteResult{status, {}, failure()}), deleted));
        malformed = wire::encode(DeleteResult{"removed", {}, {}});
        malformed["status"] = "uploaded";
        QVERIFY(!wire::decode(malformed, deleted));
    }
    void errorDetailsCannotCarryRawSdkMessages() {
        Error decoded;
        QVERIFY(wire::decode(wire::encode(Error{}), decoded));
        QVERIFY(wire::decode(wire::encode(failure()), decoded));
        auto modified = wire::encode(failure());
        modified["details"] = QJsonObject{{"httpStatus", "403"}, {"serviceCode", "AccessDenied"}};
        QVERIFY(!wire::decode(modified, decoded));
        modified["details"] = QJsonObject{{"httpStatus", 403}, {"serviceCode", "arbitrary SDK string"}};
        QVERIFY(!wire::decode(modified, decoded));
        modified["details"] = QJsonObject{{"checkpoint", QJsonObject{{"schemaVersion", 1}, {"retained", true}}}};
        QVERIFY(wire::decode(modified, decoded));
        QCOMPARE(decoded.details, modified["details"].toObject());
        modified = wire::encode(Error{});
        modified["message"] = "nonempty message with success code";
        QVERIFY(!wire::decode(modified, decoded));
        modified = wire::encode(failure());
        modified["code"] = false;
        QVERIFY(!wire::decode(modified, decoded));
    }
    void safeTransportDetailsRoundTripButRejectUnboundedInput() {
        Error original{"s3_transport_unknown", "S3 request failed", {{"httpStatus", -1}, {"serviceCode", "Unclassified"},
            {"sdkErrorType", 99}, {"curlCode", 7}, {"sdkRetryable", true}, {"requestStage", "name_head"}, {"elapsedMs", 4}}};
        Error decoded;
        QVERIFY(wire::decode(wire::encode(original), decoded));
        QCOMPARE(decoded.details, original.details);
        const QList<QPair<QString,QJsonValue>> invalid{{"sdkErrorType", "99"}, {"sdkErrorType", 65536},
            {"curlCode", "7, private SDK message"}, {"curlCode", -2}, {"curlCode", 1000}, {"curlCode", 7.5},
            {"sdkRetryable", 1}, {"elapsedMs", -1}, {"elapsedMs", 86400001}, {"requestStage", "raw endpoint/header"}};
        for (const auto& item : invalid) {
            auto changed = original; changed.details[item.first] = item.second;
            QVERIFY(!wire::decode(wire::encode(changed), decoded));
        }
    }
    void credentialGenerationIsLosslessCanonicalDecimal() {
        const Credentials synthetic{"PUTGD_SYNTHETIC_ACCESS", "synthetic-only-secret", "synthetic-only-token", std::numeric_limits<quint64>::max()};
        Credentials decoded;
        const auto encoded = wire::encode(synthetic);
        QVERIFY(wire::decode(encoded, decoded));
        QCOMPARE(decoded.generation, synthetic.generation);
        QVERIFY(wire::encode(decoded) == encoded);
        for (const auto& invalid : {QJsonValue(1), QJsonValue("01"), QJsonValue("+1"), QJsonValue("-1"), QJsonValue("1.0"),
                                    QJsonValue("18446744073709551616"), QJsonValue(""), QJsonValue("1e3")}) {
            auto modified = encoded;
            modified["generation"] = invalid;
            decoded.generation = 7;
            QVERIFY(!wire::decode(modified, decoded));
            QCOMPARE(decoded.generation, quint64(7));
        }
        auto modified = encoded;
        modified["secretKey"] = QJsonValue::Null;
        QVERIFY(!wire::decode(modified, decoded));
        modified = encoded;
        modified["accessKey"] = QString(4097, 'a');
        QVERIFY(!wire::decode(modified, decoded));
    }
    void syntheticDestinationResultsMatchActualDriverShapes() {
        QVERIFY(wire::decodeTestDestinationResult({}));
        QVERIFY(wire::decodeTestDestinationResult({{"uploadStatus", "failed"}}));
        QVERIFY(wire::decodeTestDestinationResult({{"uploadStatus", "unknown"}}));
        QVERIFY(wire::decodeTestDestinationResult({{"uploadStatus", "uploaded"}, {"receipt", receipt().toJson()}}));
        const QJsonObject completed{{"uploadStatus", "uploaded"}, {"receipt", receipt().toJson()}, {"publicReachable", true},
                                   {"cleanupStatus", "removed"}, {"cleanupProgress", QJsonObject{}}};
        QVERIFY(wire::decodeTestDestinationResult(completed));
        auto modified = completed;
        modified["publicReachable"] = "true";
        QVERIFY(!wire::decodeTestDestinationResult(modified));
        modified = completed;
        modified["cleanupStatus"] = "uploaded";
        QVERIFY(!wire::decodeTestDestinationResult(modified));
        QVERIFY(!wire::decodeTestDestinationResult({{"uploadStatus", "uploaded"}}));
        QVERIFY(!wire::decodeTestDestinationResult({{"uploadStatus", "failed"}, {"receipt", receipt().toJson()}}));
    }
    void oversizedOpaqueObjectsAreRejected() {
        const auto oversized = QString(wire::maximumFrameBytes, 'x');
        const DeleteRequest remove{"synthetic-delete", receipt(), {{"opaque", oversized}}, {}};
        DeleteRequest decoded;
        QVERIFY(!wire::decode(wire::encode(remove), decoded));
        QVERIFY(!wire::validateRequest("remove", wire::encode(remove)));
        DeleteResult result;
        QVERIFY(!wire::decode(wire::encode(DeleteResult{"unknown", {{"opaque", oversized}}, failure()}), result));
    }
};
QTEST_GUILESS_MAIN(S3WorkerProtocolTest)
#include "test_s3_worker_protocol.moc"
