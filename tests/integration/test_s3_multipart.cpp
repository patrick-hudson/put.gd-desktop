#include "fixtures/S3Fixture.h"
#include "fixtures/U2Fixtures.h"
#include "core/Log.h"
#include <QDir>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QScopeGuard>
#include <mutex>
#include "fixtures/Waits.h"
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
QByteArray readLogs(const QString& directory) {
    QByteArray bytes;
    for (const auto& name : QDir(directory).entryList({"putgd-*.jsonl"}, QDir::Files, QDir::Name)) {
        QFile file(directory + "/" + name);
        if (file.open(QIODevice::ReadOnly)) bytes += file.readAll();
    }
    return bytes;
}
}
class S3MultipartTest : public QObject {
    Q_OBJECT
private slots:
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
    // A journal written before #1582 holds the key with its doubled slash. It names the
    // same stored key, so the upload resumes with its upload ID and matching parts.
    void oldFormJournalKeyResumesTheSameUpload() {
        S3Fixture f(policy);
        f.destination.settings["objectPrefix"] = "a//b/";
        auto request = f.request("resume.bin", synthetic(16 * mib));
        QCOMPARE(request.objectReference["key"].toString(), QString("a/b/resume.bin"));
        f.reset({{"rules", QJsonArray{QJsonObject{{"method", "PUT"}, {"query", "partNumber=4"}, {"status", 403}},
                                      QJsonObject{{"method", "DELETE"}, {"query", "uploadId="}, {"status", 503}, {"code", "ServiceUnavailable"}, {"remaining", 3}}}}});
        Journal first;
        first.attach(request);
        QCOMPARE(f.upload(request).status, QString("failed"));
        auto saved = first.last();
        QCOMPARE(saved["phase"].toString(), QString("parts"));
        const auto uploadId = saved["uploadId"].toString();
        saved["key"] = "a//b/resume.bin";
        f.control("/__clear_events", {}, true);
        auto retry = request;
        retry.objectReference["key"] = "a//b/resume.bin";
        retry.resume = saved;
        const auto result = f.upload(retry);
        QVERIFY2(result.status == "uploaded", qPrintable(result.error.code + ": " + result.error.message));
        QCOMPARE(result.receipt.objectReference["key"].toString(), QString("a/b/resume.bin"));
        QCOMPARE(count(f, "POST", "uploads"), 0);
        QCOMPARE(count(f, "DELETE", "uploadId="), 0);
        QCOMPARE(count(f, "GET", "uploadId=" + uploadId), 1);
        for (const auto* number : {"1", "2", "3"}) QCOMPARE(parts(f, number).size(), 0);
        QCOMPARE(parts(f, "4").size(), 1);
        QCOMPARE(f.state()["objects"].toObject()["a/b/resume.bin"].toArray().last().toObject()["blob"].toString(), request.artifact.sha256);
    }
    // A journal for another key is aborted, but one whose key has a "." or ".." part
    // names nothing a request can reach, so no abort is sent for it.
    void journalWithADotKeyIsNeverAborted() {
        S3Fixture f(policy);
        auto request = f.request("fresh.bin", synthetic(11 * mib));
        f.control("/__clear_events", {}, true);
        request.resume = {{"schemaVersion", 1}, {"partBytes", double(5 * mib)}, {"uploadId", "synthetic-upload-dot"},
                          {"key", "prefix é/../fresh.bin"}, {"bytes", double(11 * mib)}, {"sha256", request.artifact.sha256}, {"phase", "parts"}};
        const auto result = f.upload(request);
        QVERIFY2(result.status == "uploaded", qPrintable(result.error.code + ": " + result.error.message));
        QCOMPARE(count(f, "DELETE", "uploadId="), 0);
        QCOMPARE(count(f, "POST", "uploads"), 1);
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
    // #1627: a completion that may already have published stays unknown when its resend is
    // refused outright, and the upload is not aborted. Two ways to get there: the first reply is
    // lost after publishing and the follow-up HEAD doesn't see the object yet; or put.gd resumes
    // a completion an earlier run left uncertain.
    void refusedCompletionResendStaysUnknown_data() {
        QTest::addColumn<QString>("path");QTest::addColumn<int>("status");QTest::addColumn<QString>("service");
        for (const auto& [status, service] : QList<QPair<int, QString>>{{501, "NotImplemented"}, {400, "InvalidRequest"}, {403, "AccessDenied"}}) {
            QTest::newRow(qPrintable("lost-reply-" + QString::number(status))) << QString("lost-reply") << status << service;
            QTest::newRow(qPrintable("resumed-" + QString::number(status))) << QString("resumed") << status << service;
        }
    }
    void refusedCompletionResendStaysUnknown() {
        QFETCH(QString, path);QFETCH(int, status);QFETCH(QString, service);
        S3Fixture f(policy);
        auto request = f.request("complete.bin", synthetic(11 * mib));
        const QJsonObject refuse{{"method", "POST"}, {"query", "uploadId="}, {"status", status}, {"code", service}, {"remaining", 1}};
        Journal journal;
        journal.attach(request);
        if (path == "resumed") {
            f.reset({{"rules", QJsonArray{QJsonObject{{"method", "POST"}, {"query", "uploadId="}, {"status", 500}, {"code", "InternalError"}, {"remaining", 3}}}}});
            QCOMPARE(f.upload(request).status, QString("unknown"));
            QCOMPARE(journal.last()["phase"].toString(), QString("completing"));
            request.resume = journal.last();
            f.control("/__rules", {{"rules", QJsonArray{refuse}}}, true);
        } else {
            // The first completion outlasts the request timeout, then publishes; the check right
            // after doesn't see the object yet.
            f.reset({{"rules", QJsonArray{QJsonObject{{"method", "POST"}, {"query", "uploadId="}, {"delay", 3}, {"remaining", 1}},
                                          QJsonObject{{"method", "HEAD"}, {"status", 404}, {"code", "NoSuchKey"}, {"remaining", 1}}, refuse}}});
        }
        const auto result = f.upload(request);
        QCOMPARE(result.status, QString("unknown"));
        QCOMPARE(result.error.details["httpStatus"].toInt(), status);
        QCOMPARE(count(f, "DELETE", "uploadId="), 0);
        if (path == "lost-reply") PUTGD_TRY_COMPARE(f.state()["objects"].toObject()["prefix é/complete.bin"].toArray().size(), 1);
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
    void debugLogRecordsPartsWithoutSecretsOrNames() {
        QTemporaryDir dir;
        logging::open(dir.filePath("logs"));
        logging::setDebug(true);
        auto closeLog = qScopeGuard([] { logging::setDebug(false); logging::close(); });
        S3Fixture f(policy);
        f.reset();
        ProviderRegistry registry;
        registry.add("s3", std::shared_ptr<StorageProvider>(&f.provider, [](StorageProvider*) {}));
        auto document = configuration(dir.path());
        document["profiles"] = QJsonArray{QJsonObject{{"id", f.destination.profileId}, {"label", "Synthetic fixture"}, {"provider", "s3"},
            {"settings", f.destination.settings}, {"credentials", QJsonObject{{"mode", "session"}, {"reference", QJsonValue::Null}}}}};
        auto actions = document["actions"].toObject();
        for (const auto& action : Settings::actionNames()) {
            auto route = actions[action].toObject();
            route["profileId"] = f.destination.profileId;
            actions[action] = route;
        }
        document["actions"] = actions;
        SettingsService settings(dir.filePath("config.json"), [&registry](const auto& id, const auto& value) { return registry.validate(id, value); });
        QVERIFY(settings.save(document, {}));
        Source source;
        source.payload.kind = "files";
        source.payload.files = {fixture(dir.path(), "private-name-canary.bin", synthetic(11 * mib, 148))};
        OperationService operations(dir.filePath("data"), settings, registry, source);
        PUTGD_TRY_VERIFY(operations.ready());
        QString id;
        operations.invoke("clipboard", [&](Error error, QJsonObject result) { QVERIFY(!error); id = result["operationId"].toString(); });
        PUTGD_TRY_VERIFY_WITH_TIMEOUT(!id.isEmpty() && HistoryStore(operations.databasePath()).operation(id).stateName() == "completed", 30000);
        operations.shutdown();
        logging::flush();
        const auto bytes = readLogs(dir.filePath("logs"));
        QList<QJsonObject> partRecords, completions;
        for (const auto& line : bytes.split('\n')) {
            const auto record = QJsonDocument::fromJson(line).object();
            if (record["operationId"] != id) continue;
            if (record["message"] == "Multipart part uploaded") partRecords.append(record);
            if (record["message"] == "Multipart upload completed") completions.append(record);
        }
        QCOMPARE(partRecords.size(), 3);
        for (const auto& record : partRecords) {
            QCOMPARE(record["level"].toString(), QString("debug"));
            const auto fields = record["fields"].toObject();
            QVERIFY(fields["partNumber"].toInt() >= 1);
            QVERIFY(fields["bytes"].toInteger() > 0);
            QCOMPARE(fields["attempt"].toInt(), 1);
            QVERIFY(fields["requestId"].toString().startsWith("SYNTHPART"));
            QVERIFY(fields["extendedRequestId"].toString().startsWith("synthetic+Part/"));
        }
        QCOMPARE(completions.size(), 1);
        QCOMPARE(completions[0]["level"].toString(), QString("info"));
        QCOMPARE(completions[0]["fields"].toObject()["requestId"].toString(), QString("SYNTHDONE0001"));
        // Part records never name the file, its key, the upload or the endpoint.
        for (const auto& record : partRecords + completions) {
            const auto text = QJsonDocument(record).toJson(QJsonDocument::Compact);
            for (const auto& value : {QByteArray("private-name-canary"), QByteArray("prefix"), QByteArray("synthetic-upload-"), QByteArray("isolated-fixture"), f.endpoint.toUtf8()})
                QVERIFY2(!text.contains(value), text.constData());
        }
        // No secret, signature or signed URL reaches the log at all.
        QVERIFY(!bytes.contains("putgd-synthetic-secret-not-a-real-key"));
        QVERIFY(!bytes.contains("Credential=PUTGD_SYNTHETIC_ACCESS"));
        QVERIFY(!QRegularExpression("(?i)signature=[0-9a-f]{16}").match(QString::fromUtf8(bytes)).hasMatch());
        QVERIFY(!QRegularExpression("(?i)x-amz-(signature|credential|security-token)=[^&\\s\"<]+").match(QString::fromUtf8(bytes)).hasMatch());
    }
};
QTEST_GUILESS_MAIN(S3MultipartTest)
#include "test_s3_multipart.moc"
