#include "fixtures/S3Fixture.h"
#include <QRandomGenerator>
#include <algorithm>
#include <mutex>
using namespace putgd::test;
namespace {
constexpr qint64 mib = 1024 * 1024;
// Small enough for the offline fixture, still within S3's 5 MiB part minimum.
const S3UploadPolicy policy{6 * mib, 5 * mib};
QByteArray synthetic(qint64 bytes, quint32 seed = 57) {
    QByteArray data(bytes, Qt::Uninitialized);
    QRandomGenerator generator(seed);
    generator.fillRange(reinterpret_cast<quint32*>(data.data()), data.size() / 4);
    return data;
}
struct Journal {
    std::mutex mutex;
    QList<QJsonObject> saved;
    QJsonObject last() { std::lock_guard lock(mutex); return saved.isEmpty() ? QJsonObject{} : saved.last(); }
    QList<QJsonObject> all() { std::lock_guard lock(mutex); return saved; }
    void attach(UploadRequest& request) {
        request.checkpoint = [this](const QJsonObject& value, std::function<void(Error)> ack) {
            { std::lock_guard lock(mutex); saved.append(value); }
            ack({});
        };
    }
};
QJsonArray parts(S3Fixture& f, const QString& number = {}) {
    QJsonArray result;
    for (auto event : f.events("PUT")) {
        const auto path = event.toObject()["path"].toString();
        if (path.contains("uploadId=") && (number.isEmpty() || path.contains("partNumber=" + number + "&") || path.endsWith("partNumber=" + number)))
            result.append(event);
    }
    return result;
}
int count(S3Fixture& f, const QString& method, const QString& query) {
    int matches = 0;
    for (auto event : f.events(method)) matches += event.toObject()["path"].toString().contains(query);
    return matches;
}
}
class S3MultipartTest : public QObject {
    Q_OBJECT
private slots:
    void planStaysWithinS3PartRules() {
        QCOMPARE(s3PartBytes(64 * mib), qint64(0));
        QCOMPARE(s3PartBytes(64 * mib + 1), 16 * mib);
        QCOMPARE(s3PartBytes(10LL * 1024 * mib), 16 * mib);
        // Past 10,000 x 16 MiB the part grows in whole MiB.
        const qint64 huge = 5LL * 1024 * 1024 * mib;
        QCOMPARE(s3PartBytes(huge), 525 * mib);
        QVERIFY(s3PartCount(huge, s3PartBytes(huge)) <= s3MaximumParts);
        for (qint64 bytes : {64 * mib + 1, 157286400001LL, 1099511627776LL, huge}) QVERIFY(s3ValidPlan(bytes, s3PartBytes(bytes)));
        QCOMPARE(s3PartBytes(huge + 1), qint64(0));
        QVERIFY(!s3ValidPlan(10 * mib, 4 * mib));
        QVERIFY(!s3ValidPlan(10 * mib, 6LL * 1024 * mib));
        QCOMPARE(s3PartBytes(7 * mib, policy), 5 * mib);
        QCOMPARE(s3PartBytes(6 * mib, policy), qint64(0));
    }
    void smallFilesKeepOneConditionalPut() {
        S3Fixture f(policy);
        f.reset();
        const auto result = f.upload(f.request("small.bin", synthetic(6 * mib)));
        QCOMPARE(result.status, QString("uploaded"));
        QCOMPARE(count(f, "POST", "uploads"), 0);
        QCOMPARE(f.events("PUT").size(), 1);
    }
    void partsStreamWithMetadataChecksumsAndCondition() {
        S3Fixture f(policy);
        f.reset();
        const auto bytes = synthetic(11 * mib + 123);
        auto request = f.request("large file.bin", bytes);
        request.artifact.mime = "video/mp4";
        Journal journal;
        journal.attach(request);
        std::mutex mutex;
        QList<UploadProgress> progress;
        request.progress = [&](const UploadProgress& value) { std::lock_guard lock(mutex); progress.append(value); };
        const auto result = f.upload(request);
        QVERIFY2(result.status == "uploaded", qPrintable(result.error.code + ": " + result.error.message));
        QCOMPARE(result.receipt.objectReference["key"].toString(), QString("prefix é/large file.bin"));
        QCOMPARE(result.receipt.objectReference["sha256"].toString(), request.artifact.sha256);
        QVERIFY(result.receipt.concurrencyToken.endsWith("-3\""));
        QCOMPARE(result.receipt.directUrl, f.endpoint + "/isolated-fixture/prefix%20%C3%A9/large%20file.bin");
        // Create carries type, storage class, ACL mode and identity metadata.
        QCOMPARE(count(f, "POST", "uploads"), 1);
        QJsonObject create;
        for (auto event : f.events("POST")) if (event.toObject()["path"].toString().contains("uploads")) create = event.toObject();
        const auto headers = create["headers"].toObject();
        QVERIFY(create["signatureValid"].toBool());
        QCOMPARE(headers["content-type"].toString(), QString("video/mp4"));
        QCOMPARE(headers["x-amz-storage-class"].toString(), QString("STANDARD"));
        QVERIFY(!headers.contains("x-amz-acl"));
        QCOMPARE(headers["x-amz-meta-putgd-publication"].toString(), request.publicationId);
        QCOMPARE(headers["x-amz-meta-putgd-sha256"].toString(), request.artifact.sha256);
        // Three parts, each with its own Content-MD5 and signed payload hash.
        QCOMPARE(parts(f).size(), 3);
        const QList<qint64> sizes{5 * mib, 5 * mib, mib + 123};
        for (int number = 1; number <= 3; ++number) {
            const auto events = parts(f, QString::number(number));
            QCOMPARE(events.size(), 1);
            const auto part = events[0].toObject();
            QVERIFY(part["signatureValid"].toBool());
            QCOMPARE(part["bodyBytes"].toInteger(), sizes[number - 1]);
            const auto slice = bytes.mid((number - 1) * 5 * mib, sizes[number - 1]);
            QCOMPARE(part["headers"].toObject()["content-md5"].toString(), QString::fromLatin1(QCryptographicHash::hash(slice, QCryptographicHash::Md5).toBase64()));
        }
        // Completion is the only publishing request: conditional and size-checked.
        QJsonObject complete;
        for (auto event : f.events("POST")) if (event.toObject()["path"].toString().contains("uploadId=")) complete = event.toObject();
        QCOMPARE(complete["headers"].toObject()["if-none-match"].toString(), QString("*"));
        QCOMPARE(complete["headers"].toObject()["x-amz-mp-object-size"].toString(), QString::number(bytes.size()));
        const auto stored = f.state()["objects"].toObject()["prefix é/large file.bin"].toArray().last().toObject();
        QCOMPARE(stored["blob"].toString(), QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()));
        QVERIFY(f.state()["uploads"].toObject().isEmpty());
        // The upload ID is journaled before parts, then completion, then cleared.
        const auto saved = journal.all();
        QCOMPARE(saved.size(), 3);
        QCOMPARE(saved[0]["phase"].toString(), QString("parts"));
        QVERIFY(saved[0]["uploadId"].toString().startsWith("synthetic-upload-"));
        QCOMPARE(saved[0]["key"].toString(), QString("prefix é/large file.bin"));
        QCOMPARE(saved[0]["partBytes"].toInteger(), 5 * mib);
        QCOMPARE(saved[1]["phase"].toString(), QString("completing"));
        QVERIFY(saved[2].isEmpty());
        std::lock_guard lock(mutex);
        QVERIFY(!progress.isEmpty());
        qint64 most = 0;
        for (const auto& value : progress) {
            QVERIFY(value.valid());
            QCOMPARE(value.bytesTotal, std::optional<qint64>(bytes.size()));
            if (value.phase == "uploading") most = qMax(most, *value.bytesTransferred);
        }
        // The worker forwards only the latest value every 250 ms, so a fast local
        // part may be skipped; the final report still covers every byte.
        QVERIFY(most <= bytes.size());
        QCOMPARE(progress.last().phase, QString("finalizing"));
        QCOMPARE(progress.last().bytesTransferred, std::optional<qint64>(bytes.size()));
    }
    void publicReadAndOverwriteUseTheirHeaders() {
        S3Fixture f(policy);
        f.reset();
        f.destination.settings["aclMode"] = "public-read";
        f.destination.settings["storageClass"] = "STANDARD_IA";
        auto first = f.upload(f.request("public.bin", synthetic(7 * mib)));
        QCOMPARE(first.status, QString("uploaded"));
        QJsonObject create;
        for (auto event : f.events("POST")) if (event.toObject()["path"].toString().contains("uploads")) create = event.toObject();
        QCOMPARE(create["headers"].toObject()["x-amz-acl"].toString(), QString("public-read"));
        QCOMPARE(create["headers"].toObject()["x-amz-storage-class"].toString(), QString("STANDARD_IA"));
        // Explicit overwrite completes with the checked ETag, never If-None-Match.
        auto replace = f.request("public.bin", synthetic(8 * mib, 2));
        QCOMPARE(replace.condition, QString("match"));
        const auto replaced = f.upload(replace);
        QCOMPARE(replaced.status, QString("uploaded"));
        QJsonObject complete;
        for (auto event : f.events("POST")) if (event.toObject()["path"].toString().contains("uploadId=")) complete = event.toObject();
        QCOMPARE(complete["headers"].toObject()["if-match"].toString(), first.receipt.concurrencyToken);
        QVERIFY(!complete["headers"].toObject().contains("if-none-match"));
    }
    void completionPreconditionKeepsTheCompetitorAndAborts() {
        S3Fixture f(policy);
        auto request = f.request("race.bin", synthetic(7 * mib));
        f.reset({{"rules", QJsonArray{QJsonObject{{"method", "POST"}, {"query", "uploadId="}, {"competitor", true}}}}});
        Journal journal;
        journal.attach(request);
        const auto result = f.upload(request);
        QCOMPARE(result.status, QString("collision"));
        QCOMPARE(count(f, "DELETE", "uploadId="), 1);
        QVERIFY(f.state()["uploads"].toObject().isEmpty());
        QCOMPARE(f.state()["objects"].toObject()["prefix é/race.bin"].toArray().last().toObject()["etag"].toString(), QString("\"competitor\""));
        QVERIFY(journal.last().isEmpty());
    }
    void transientPartIsSentAgainUnderItsNumber() {
        S3Fixture f(policy);
        auto request = f.request("retry.bin", synthetic(11 * mib));
        f.reset({{"rules", QJsonArray{QJsonObject{{"method", "PUT"}, {"query", "partNumber=2"}, {"status", 503}, {"code", "ServiceUnavailable"}, {"remaining", 1}}}}});
        const auto result = f.upload(request);
        QCOMPARE(result.status, QString("uploaded"));
        QCOMPARE(parts(f, "2").size(), 2);
        QCOMPARE(parts(f).size(), 4);
        QCOMPARE(count(f, "DELETE", "uploadId="), 0);
    }
    void deniedPartAbortsWithGuidance() {
        S3Fixture f(policy);
        auto request = f.request("denied.bin", synthetic(11 * mib));
        f.reset({{"rules", QJsonArray{QJsonObject{{"method", "PUT"}, {"query", "partNumber=2"}, {"status", 403}}}}});
        Journal journal;
        journal.attach(request);
        const auto result = f.upload(request);
        QCOMPARE(result.status, QString("failed"));
        QCOMPARE(result.error.code, QString("s3_permission_denied"));
        QCOMPARE(result.error.details["requestStage"].toString(), QString("upload_part"));
        QCOMPARE(result.error.details["partNumber"].toInt(), 2);
        QVERIFY(result.error.details.contains("requestId"));
        QVERIFY(result.error.message.contains("s3:PutObject"));
        QVERIFY(!result.error.details.contains("incompleteUpload"));
        QCOMPARE(count(f, "DELETE", "uploadId="), 1);
        QVERIFY(f.state()["uploads"].toObject().isEmpty());
        QVERIFY(f.state()["objects"].toObject()["prefix é/denied.bin"].toArray().isEmpty());
        QVERIFY(journal.last().isEmpty());
    }
    void failedAbortReportsTheIncompleteUpload() {
        S3Fixture f(policy);
        auto request = f.request("orphan.bin", synthetic(11 * mib));
        f.reset({{"rules", QJsonArray{QJsonObject{{"method", "PUT"}, {"query", "partNumber=2"}, {"status", 403}},
                                      QJsonObject{{"method", "DELETE"}, {"query", "uploadId="}, {"status", 403}}}}});
        Journal journal;
        journal.attach(request);
        const auto result = f.upload(request);
        QCOMPARE(result.status, QString("failed"));
        QVERIFY(result.error.details["incompleteUpload"].toBool());
        QVERIFY(result.error.message.contains("keep using storage"));
        // The journal keeps the upload ID for a later retry to resume or abort.
        QCOMPARE(journal.last()["phase"].toString(), QString("parts"));
        QCOMPARE(f.state()["uploads"].toObject().size(), 1);
    }
    void cancellationAbortsTheUpload() {
        S3Fixture f(policy);
        auto request = f.request("cancel.bin", synthetic(16 * mib));
        f.reset({{"rules", QJsonArray{QJsonObject{{"method", "PUT"}, {"query", "partNumber=2"}, {"delay", 1.5}}}}});
        request.cancellation = std::make_shared<std::atomic_bool>(false);
        request.progress = [token = request.cancellation](const UploadProgress& value) {
            if (value.phase == "uploading" && value.bytesTransferred.value_or(0) > 5 * mib) token->store(true);
        };
        const auto result = f.upload(request);
        QCOMPARE(result.status, QString("failed"));
        QCOMPARE(result.error.code, QString("cancelled"));
        QCOMPARE(count(f, "DELETE", "uploadId="), 1);
        QVERIFY(parts(f, "4").isEmpty());
        QVERIFY(f.state()["uploads"].toObject().isEmpty());
    }
    void interruptedUploadResumesFromMatchingParts() {
        S3Fixture f(policy);
        auto request = f.request("resume.bin", synthetic(16 * mib));
        // An attempt that cannot abort leaves the journaled upload, like a crash would.
        f.reset({{"rules", QJsonArray{QJsonObject{{"method", "PUT"}, {"query", "partNumber=4"}, {"status", 403}},
                                      QJsonObject{{"method", "DELETE"}, {"query", "uploadId="}, {"status", 503}, {"code", "ServiceUnavailable"}, {"remaining", 3}}}}});
        Journal first;
        first.attach(request);
        QCOMPARE(f.upload(request).status, QString("failed"));
        const auto saved = first.last();
        QCOMPARE(saved["phase"].toString(), QString("parts"));
        const auto uploadId = saved["uploadId"].toString();
        // Part 2 on the server no longer matches the retained file.
        f.control("/__tamper", {{"uploadId", uploadId}, {"part", "2"}, {"etag", "\"00000000000000000000000000000000\""}}, true);
        f.control("/__clear_events", {}, true);
        auto retry = request;
        retry.resume = saved;
        Journal second;
        second.attach(retry);
        const auto result = f.upload(retry);
        QVERIFY2(result.status == "uploaded", qPrintable(result.error.code + ": " + result.error.message));
        QCOMPARE(count(f, "POST", "uploads"), 0);
        QCOMPARE(count(f, "GET", "uploadId=" + uploadId), 1);
        QCOMPARE(parts(f, "1").size(), 0);
        QCOMPARE(parts(f, "2").size(), 1);
        QCOMPARE(parts(f, "3").size(), 0);
        QCOMPARE(parts(f, "4").size(), 1);
        const auto stored = f.state()["objects"].toObject()["prefix é/resume.bin"].toArray().last().toObject();
        QCOMPARE(stored["blob"].toString(), request.artifact.sha256);
        QVERIFY(second.last().isEmpty());
    }
    void uncertainCompletionResumesWithoutResendingParts() {
        S3Fixture f(policy);
        auto request = f.request("complete.bin", synthetic(11 * mib));
        f.reset({{"rules", QJsonArray{QJsonObject{{"method", "POST"}, {"query", "uploadId="}, {"status", 500}, {"code", "InternalError"}, {"remaining", 3}}}}});
        Journal first;
        first.attach(request);
        const auto uncertain = f.upload(request);
        QCOMPARE(uncertain.status, QString("unknown"));
        QCOMPARE(count(f, "DELETE", "uploadId="), 0);
        QCOMPARE(first.last()["phase"].toString(), QString("completing"));
        f.control("/__clear_events", {}, true);
        auto retry = request;
        retry.resume = first.last();
        const auto result = f.upload(retry);
        QCOMPARE(result.status, QString("uploaded"));
        QVERIFY(parts(f).isEmpty());
        QCOMPARE(count(f, "POST", "uploadId="), 1);
    }
    void deniedListingSendsEveryPartAgain() {
        S3Fixture f(policy);
        auto request = f.request("unlisted.bin", synthetic(11 * mib));
        f.reset({{"rules", QJsonArray{QJsonObject{{"method", "PUT"}, {"query", "partNumber=3"}, {"status", 403}},
                                      QJsonObject{{"method", "DELETE"}, {"query", "uploadId="}, {"status", 503}, {"code", "ServiceUnavailable"}, {"remaining", 3}}}}});
        Journal first;
        first.attach(request);
        QCOMPARE(f.upload(request).status, QString("failed"));
        f.control("/__clear_events", {}, true);
        f.control("/__rules", {{"rules", QJsonArray{QJsonObject{{"method", "GET"}, {"query", "uploadId="}, {"status", 403}}}}}, true);
        auto retry = request;
        retry.resume = first.last();
        const auto result = f.upload(retry);
        QCOMPARE(result.status, QString("uploaded"));
        QCOMPARE(count(f, "POST", "uploads"), 0);
        QCOMPARE(parts(f).size(), 3);
    }
    void journalForAnotherKeyIsAbortedNeverResumed() {
        S3Fixture f(policy);
        auto stale = f.request("old name.bin", synthetic(11 * mib));
        f.reset({{"rules", QJsonArray{QJsonObject{{"method", "PUT"}, {"query", "partNumber=2"}, {"status", 403}},
                                      QJsonObject{{"method", "DELETE"}, {"query", "uploadId="}, {"status", 503}, {"code", "ServiceUnavailable"}, {"remaining", 3}}}}});
        Journal first;
        first.attach(stale);
        QCOMPARE(f.upload(stale).status, QString("failed"));
        f.control("/__clear_events", {}, true);
        auto renamed = f.request("new name.bin", synthetic(11 * mib));
        renamed.resume = first.last();
        const auto result = f.upload(renamed);
        QCOMPARE(result.status, QString("uploaded"));
        QJsonObject abort;
        for (auto event : f.events("DELETE")) abort = event.toObject();
        QCOMPARE(abort["key"].toString(), QString("prefix é/old name.bin"));
        QCOMPARE(count(f, "POST", "uploads"), 1);
        QVERIFY(f.state()["uploads"].toObject().isEmpty());
    }
    void testConnectionProvesMultipartPermissions_data() {
        QTest::addColumn<QString>("method");
        QTest::addColumn<QString>("permission");
        QTest::addColumn<QString>("stage");
        QTest::newRow("granted") << QString() << QString() << QString();
        QTest::newRow("abort-denied") << QString("DELETE") << QString("s3:AbortMultipartUpload") << QString("upload_abort");
        QTest::newRow("listing-denied") << QString("GET") << QString("s3:ListMultipartUploadParts") << QString("upload_list_parts");
    }
    void testConnectionProvesMultipartPermissions() {
        QFETCH(QString, method);
        QFETCH(QString, permission);
        QFETCH(QString, stage);
        S3Fixture f(policy);
        QJsonArray rules;
        if (!method.isEmpty()) rules.append(QJsonObject{{"method", method}, {"query", "uploadId="}, {"status", 403}});
        f.reset({{"rules", rules}});
        std::atomic_bool done{false};
        QJsonObject result;
        Error error;
        QList<QJsonObject> journal;
        f.provider.testDestination(f.destination, [&](auto value, auto ack) { journal.append(value); ack({}); },
                                   [&](auto value, auto e) { result = value; error = e; done = true; });
        QTRY_VERIFY_WITH_TIMEOUT(done.load(), 15000);
        const auto probe = result["multipart"].toObject();
        QCOMPARE(count(f, "POST", "uploads"), 1);
        QVERIFY(parts(f).isEmpty());
        QCOMPARE(count(f, "DELETE", "uploadId="), 1);
        QVERIFY(std::any_of(journal.begin(), journal.end(), [](const QJsonObject& value) { return value["phase"] == "synthetic_multipart_created" && value.contains("uploadId"); }));
        if (permission.isEmpty()) {
            QVERIFY2(!error, qPrintable(error.message));
            QCOMPARE(probe["status"].toString(), QString("ok"));
            QCOMPARE(probe["abort"].toString(), QString("aborted"));
            QVERIFY(f.state()["uploads"].toObject().isEmpty());
            return;
        }
        QCOMPARE(error.code, QString("s3_multipart_permission"));
        QVERIFY(error.message.contains(permission));
        QCOMPARE(probe["status"].toString(), QString("denied"));
        QCOMPARE(probe["stage"].toString(), stage);
        QCOMPARE(result["cleanupStatus"].toString(), QString("removed"));
    }
};
QTEST_GUILESS_MAIN(S3MultipartTest)
#include "test_s3_multipart.moc"
