// Backblaze B2 native destination (#282) against the offline b2api v3 oracle.
#include "storage/b2/B2Provider.h"
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>
#include <QUrl>
#include <future>
#include <thread>
using namespace putgd;
namespace {
constexpr qint64 mib = 1024 * 1024;
QByteArray synthetic(qint64 bytes, quint32 seed = 282) {
    QByteArray out(bytes, Qt::Uninitialized);
    quint32 x = seed;
    for (qint64 i = 0; i < bytes; ++i) { x = x * 1664525u + 1013904223u; out[i] = char(x >> 24); }
    return out;
}
class B2Fixture {
public:
    QProcess process;
    QTemporaryDir directory;
    CredentialStore credentials;
    B2Provider provider;
    QString base;
    Destination destination;
    explicit B2Fixture(b2::UploadPolicy policy = {6 * mib, 5 * mib}) : provider(credentials, policy) {
        process.start("python3", {B2_FIXTURE_PATH});
        if (!process.waitForStarted() || !process.waitForReadyRead()) qFatal("fixture failed to start");
        base = "http://127.0.0.1:" + QString::fromUtf8(process.readLine()).trimmed();
        auto settings = b2::defaults();
        settings["apiUrl"] = base;
        settings["bucket"] = "putgd-fixture-public";
        settings["objectPrefix"] = "prefix é";
        settings["requestTimeoutMs"] = 5000;
        settings["connectTimeoutMs"] = 1000;
        destination = {"backblaze-b2", "fixture-profile", "revision-one", settings, {{"mode", "session"}, {"reference", QJsonValue::Null}}};
        credentials.setSession(destination.profileId, {"PUTGD_SYNTHETIC_B2_KEY_ID", "putgd-synthetic-b2-application-key-not-real", {}});
    }
    ~B2Fixture() { process.terminate(); process.waitForFinished(); }
    QJsonObject control(const QString& path, const QJsonObject& value = {}, bool post = false) {
        QTcpSocket socket;
        const QUrl address(base);
        socket.connectToHost(address.host(), quint16(address.port()));
        if (!socket.waitForConnected(5000)) qFatal("fixture control connection failed");
        const auto body = post ? QJsonDocument(value).toJson(QJsonDocument::Compact) : QByteArray{};
        const auto request = (post ? QByteArray("POST ") : QByteArray("GET ")) + path.toUtf8() + " HTTP/1.1\r\nHost: " + address.authority().toUtf8() +
                             "\r\nConnection: close\r\nContent-Type: application/json\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\n\r\n" + body;
        socket.write(request);
        QByteArray response;
        QElapsedTimer deadline;
        deadline.start();
        while (deadline.elapsed() < 5000) {
            if (!socket.bytesAvailable()) socket.waitForReadyRead(100);
            response += socket.readAll();
            const auto boundary = response.indexOf("\r\n\r\n");
            if (boundary >= 0) {
                qint64 length = -1;
                for (auto header : response.left(boundary).split('\n'))
                    if (header.trimmed().toLower().startsWith("content-length:")) length = header.trimmed().mid(15).trimmed().toLongLong();
                if (length >= 0 && response.size() - boundary - 4 >= length) return QJsonDocument::fromJson(response.mid(boundary + 4, length)).object();
            }
        }
        qFatal("fixture control response timed out");
        return {};
    }
    void reset(const QJsonObject& value = {}) { control("/__reset", value, true); }
    void rules(const QJsonArray& rules) { control("/__rules", {{"rules", rules}}, true); }
    QJsonObject state() { return control("/__state"); }
    QJsonArray events(const QString& call = {}) {
        QJsonArray out;
        for (const auto& e : state()["events"].toArray())
            if (call.isEmpty() || e.toObject()["call"] == call) out.append(e);
        return out;
    }
    QJsonArray versions(const QString& name = {}) {
        QJsonArray out;
        for (const auto& v : state()["versions"].toArray())
            if (name.isEmpty() || v.toObject()["fileName"] == name) out.append(v);
        return out;
    }
    template<class T, class F> T await(F start) {
        auto promise = std::make_shared<std::promise<T>>();
        auto future = promise->get_future();
        start([promise](T value) { promise->set_value(std::move(value)); });
        return future.get();
    }
    struct Journal {
        QList<QJsonObject> saved;
        void attach(UploadRequest& request) { request.checkpoint = [this](const QJsonObject& value, std::function<void(Error)> ack) { saved.append(value); ack({}); }; }
        QJsonObject last() const { return saved.isEmpty() ? QJsonObject{} : saved.last(); }
    };
    UploadRequest request(const QString& name = "a b.png", const QByteArray& bytes = "synthetic payload") {
        Artifact artifact;
        artifact.id = "synthetic-item";
        artifact.path = directory.filePath(name);
        artifact.name = name;
        artifact.mime = "image/png";
        artifact.sha256 = QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
        artifact.bytes = bytes.size();
        QFile file(artifact.path);
        if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size()) qFatal("artifact fixture failed");
        file.close();
        const auto checked = await<NameCheck>([&](auto cb) { provider.checkName(destination, name, cb); });
        UploadRequest out{destination, artifact, name, "synthetic-operation/item", "absent", checked.objectReference, checked.concurrencyToken, std::make_shared<std::atomic_bool>(false)};
        return out;
    }
    UploadResult upload(const UploadRequest& r) { return await<UploadResult>([&](auto cb) { provider.upload(r, cb); }); }
    DeleteResult remove(const StorageReceipt& receipt, QJsonObject progress = {}, DurableCheckpoint save = {}) {
        if (!save) save = [](auto, auto ack) { ack({}); };
        return await<DeleteResult>([&](auto cb) { provider.remove({"synthetic-delete", receipt, progress, {}}, save, cb); });
    }
};
QJsonObject seed(const QString& name, const QString& action = "upload", const QString& bucket = "bucket-public-id") {
    return {{"fileName", name}, {"bucketId", bucket}, {"action", action}};
}
}
class B2Test : public QObject {
    Q_OBJECT
private slots:
    void settingsValidation() {
        auto s = b2::defaults();
        s["bucket"] = "putgd-fixture";
        QVERIFY(!b2::validateSettings(s));
        auto bad = s; bad["bucket"] = "b2-reserved"; QVERIFY(b2::validateSettings(bad));
        bad = s; bad["bucket"] = "short"; QVERIFY(b2::validateSettings(bad));
        bad = s; bad["linkValidityDays"] = 8; QVERIFY(b2::validateSettings(bad));
        bad = s; bad["linkMode"] = "authorized"; bad["publicUrlBase"] = "https://cdn.example"; QVERIFY(b2::validateSettings(bad));
        bad = s; bad["apiUrl"] = "http://api.backblazeb2.com"; QVERIFY(b2::validateSettings(bad));
        bad = s; bad["extra"] = true; QVERIFY(b2::validateSettings(bad));
        bad = s; bad["objectPrefix"] = "/rooted"; QVERIFY(b2::validateSettings(bad));
        QCOMPARE(b2::partBytes(64 * mib), qint64(0));
        QCOMPARE(b2::partBytes(64 * mib + 1), 16 * mib);
        QVERIFY(b2::partBytes(b2::maximumFileBytes) * b2::maximumParts >= b2::maximumFileBytes);
        QCOMPARE(b2::fileUrl(s, "https://f001.backblazeb2.com", "a b/é.png"), QString("https://f001.backblazeb2.com/file/putgd-fixture/a%20b/%C3%A9.png"));
    }
    void singleUploadPublicLinkStatAndExactDeletion() {
        B2Fixture f;
        f.reset();
        auto request = f.request();
        QCOMPARE(request.objectReference["baseline"].toArray().size(), 0);
        const auto uploaded = f.upload(request);
        QCOMPARE(uploaded.status, QString("uploaded"));
        const auto key = QString("prefix é/a b.png");
        QCOMPARE(uploaded.receipt.objectReference["key"].toString(), key);
        const auto fileId = uploaded.receipt.objectReference["fileId"].toString();
        QVERIFY(!fileId.isEmpty());
        QCOMPARE(uploaded.receipt.concurrencyToken, fileId);
        QCOMPARE(uploaded.receipt.directUrl, f.base + "/file/putgd-fixture-public/prefix%20%C3%A9/a%20b.png");
        const auto upload = f.events("upload_file")[0].toObject();
        QCOMPARE(upload["name"].toString(), key);
        QCOMPARE(upload["info"].toObject()["putgd-publication"].toString(), QString("synthetic-operation/item"));
        QCOMPARE(upload["info"].toObject()["putgd-sha256"].toString(), request.artifact.sha256);
        QCOMPARE(upload["headers"].toObject()["content-type"].toString(), QString("image/png"));
        // Keys never reach the oracle's recorded headers, and only the authorize call carries Basic auth.
        QVERIFY(!QJsonDocument(f.state()).toJson().contains("putgd-synthetic-b2-application-key"));
        const auto stat = f.await<StatResult>([&](auto cb) { f.provider.stat(uploaded.receipt, cb); });
        QCOMPARE(stat.status, QString("own"));
        QCOMPARE(stat.receipt.objectReference, uploaded.receipt.objectReference);
        QStringList phases;
        const auto removed = f.remove(uploaded.receipt, {}, [&](auto progress, auto ack) { phases << progress["phase"].toString(); ack({}); });
        QCOMPARE(removed.status, QString("removed"));
        QCOMPARE(phases, QStringList({"delete_intent", "done"}));
        QCOMPARE(f.events("b2_delete_file_version")[0].toObject()["body"].toObject()["fileId"].toString(), fileId);
        QVERIFY(f.versions(key).isEmpty());
        QCOMPARE(f.remove(uploaded.receipt).status, QString("missing"));
    }
    void visibleNameIsPresentAndHiddenNameIsAbsent() {
        B2Fixture f;
        f.reset({{"versions", QJsonArray{seed("prefix é/taken.png")}}});
        auto taken = f.await<NameCheck>([&](auto cb) { f.provider.checkName(f.destination, "taken.png", cb); });
        QCOMPARE(taken.status, QString("present"));
        f.reset({{"versions", QJsonArray{seed("prefix é/hidden.png"), seed("prefix é/hidden.png", "hide")}}});
        auto hidden = f.await<NameCheck>([&](auto cb) { f.provider.checkName(f.destination, "hidden.png", cb); });
        QCOMPARE(hidden.status, QString("absent"));
        QCOMPARE(hidden.objectReference["baseline"].toArray().size(), 2);
        // The hidden name uploads; the earlier versions are baseline, not a collision.
        auto request = f.request("hidden.png");
        QCOMPARE(f.upload(request).status, QString("uploaded"));
    }
    void concurrentUploadOfTheSameNameIsACollisionAndOurCopyGoes() {
        B2Fixture f;
        f.reset();
        auto request = f.request("race.png");
        f.rules({QJsonObject{{"call", "upload_file"}, {"competitor", true}}});
        const auto result = f.upload(request);
        QCOMPARE(result.status, QString("collision"));
        QCOMPARE(result.error.code, QString("precondition_changed"));
        const auto left = f.versions("prefix é/race.png");
        QCOMPARE(left.size(), 1);
        QVERIFY(left[0].toObject()["fileId"].toString().startsWith("competitor-"));
    }
    // A name that holds a visible file meets the collision policy before any bytes go
    // up, even when the name check that chose it is stale.
    void existingFileIsACollisionBeforeAnyBytes() {
        B2Fixture f;
        f.reset();
        auto stale = f.request("taken.png");
        QCOMPARE(stale.objectReference["baseline"].toArray().size(), 0);
        auto first = stale;
        first.publicationId = "someone-else/item";
        QCOMPARE(f.upload(first).status, QString("uploaded"));
        const auto result = f.upload(stale);
        QCOMPARE(result.status, QString("collision"));
        QCOMPARE(result.error.code, QString("precondition_changed"));
        QCOMPARE(result.error.details["requestStage"].toString(), QString("before_upload"));
        QCOMPARE(f.events("upload_file").size(), 1);
        QCOMPARE(f.versions("prefix é/taken.png").size(), 1);
    }
    // Two uploads race for one name and each sees the other: both remove their own
    // copy and report collision, so no link points at bytes that are gone.
    void racersThatSeeEachOtherBothRemoveTheirCopies() {
        B2Fixture f;
        f.reset();
        auto a = f.request("race2.png");
        auto b = a;
        b.publicationId = "racing-operation/item";
        b.cancellation = std::make_shared<std::atomic_bool>(false);
        f.rules({QJsonObject{{"call", "upload_file"}, {"race", 2}, {"remaining", 2}}});
        UploadResult ra, rb;
        std::thread one([&] { ra = f.upload(a); });
        std::thread two([&] { rb = f.upload(b); });
        one.join();
        two.join();
        QCOMPARE(f.events("upload_file").size(), 2);
        for (const auto* result : {&ra, &rb}) {
            QCOMPARE(result->status, QString("collision"));
            QCOMPARE(result->error.code, QString("precondition_changed"));
            QVERIFY(result->error.details["exposedMs"].toDouble() >= 0);
            QVERIFY(result->receipt.directUrl.isEmpty());
        }
        QVERIFY(f.versions("prefix é/race2.png").isEmpty());
    }
    void overwriteIsRefused() {
        B2Fixture f;
        f.reset();
        auto request = f.request();
        request.condition = "match";
        const auto result = f.upload(request);
        QCOMPARE(result.status, QString("failed"));
        QCOMPARE(result.error.code, QString("unsupported_capability"));
        QVERIFY(f.events("upload_file").isEmpty());
        QVERIFY(!f.provider.capabilities(f.destination).conditionalReplace);
    }
    void lostSingleUploadReplyIsRecoveredByItsPublicationIdentity() {
        B2Fixture f;
        f.reset();
        auto request = f.request("lost.png");
        f.rules({QJsonObject{{"call", "upload_file"}, {"store", true}}});
        const auto result = f.upload(request);
        QCOMPARE(result.status, QString("uploaded"));
        QCOMPARE(f.events("upload_file").size(), 1);
        QCOMPARE(f.versions("prefix é/lost.png").size(), 1);
    }
    // Qt Network re-sends a request whose connection closes before the reply; every
    // body is read-once, so a dropped deletion is reported uncertain, never replayed.
    void droppedDeletionIsNotReplayed() {
        B2Fixture f;
        f.reset();
        const auto uploaded = f.upload(f.request("kept.png"));
        QCOMPARE(uploaded.status, QString("uploaded"));
        f.rules({QJsonObject{{"call", "b2_delete_file_version"}, {"drop", true}}});
        const auto result = f.remove(uploaded.receipt);
        QCOMPARE(result.status, QString("unknown"));
        QCOMPARE(result.progress["phase"].toString(), QString("delete_intent"));
        QCOMPARE(f.events("b2_delete_file_version").size(), 1);
        QCOMPARE(f.versions("prefix é/kept.png").size(), 1);
        // Retrying from the journal deletes it.
        QCOMPARE(f.remove(uploaded.receipt, result.progress).status, QString("removed"));
    }
    void largeFileUploadsPartsWithSha1AndJournalsFirst() {
        B2Fixture f;
        f.reset();
        const auto bytes = synthetic(11 * mib);
        auto request = f.request("large.bin", bytes);
        B2Fixture::Journal journal;
        journal.attach(request);
        QList<UploadProgress> reports;
        request.progress = [&](const UploadProgress& p) { reports << p; };
        const auto result = f.upload(request);
        QCOMPARE(result.status, QString("uploaded"));
        QVERIFY(journal.saved.size() >= 2);
        QVERIFY(journal.saved[0]["uploadId"].toString().startsWith("large-"));
        QCOMPARE(journal.saved[0]["partBytes"].toDouble(), double(5 * mib));
        QVERIFY(journal.last().isEmpty());
        const auto parts = f.events("upload_part");
        QCOMPARE(parts.size(), 3);
        for (const auto& p : parts) QVERIFY(!p.toObject()["sha1"].toString().isEmpty());
        QCOMPARE(f.events("b2_finish_large_file").size(), 1);
        const auto stored = f.versions("prefix é/large.bin");
        QCOMPARE(stored.size(), 1);
        QCOMPARE(stored[0].toObject()["action"].toString(), QString("upload"));
        QCOMPARE(qint64(stored[0].toObject()["contentLength"].toDouble()), 11 * mib);
        QVERIFY(!reports.isEmpty());
        QCOMPARE(reports.last().bytesTransferred.value_or(0), 11 * mib);
    }
    void uncertainFinishResumesWithoutResendingParts() {
        B2Fixture f;
        f.reset();
        auto request = f.request("resume.bin", synthetic(11 * mib));
        B2Fixture::Journal journal;
        journal.attach(request);
        f.rules({QJsonObject{{"call", "b2_finish_large_file"}, {"status", 503}}});
        const auto first = f.upload(request);
        QCOMPARE(first.status, QString("unknown"));
        QVERIFY(journal.last()["uploadId"].isString());
        QCOMPARE(f.events("upload_part").size(), 3);
        request.resume = journal.last();
        const auto again = f.upload(request);
        QCOMPARE(again.status, QString("uploaded"));
        QCOMPARE(f.events("upload_part").size(), 3);
        QCOMPARE(f.events("b2_list_parts").size(), 1);
    }
    void failedPartCancelsTheLargeFile() {
        B2Fixture f;
        f.reset();
        auto request = f.request("denied.bin", synthetic(11 * mib));
        B2Fixture::Journal journal;
        journal.attach(request);
        f.rules({QJsonObject{{"call", "upload_part"}, {"partNumber", 2}, {"status", 401}, {"code", "unauthorized"}}});
        const auto result = f.upload(request);
        QCOMPARE(result.status, QString("failed"));
        QCOMPARE(result.error.code, QString("b2_permission_denied"));
        QCOMPARE(result.error.details["partNumber"].toInt(), 2);
        QCOMPARE(f.events("b2_cancel_large_file").size(), 1);
        QVERIFY(f.versions("prefix é/denied.bin").isEmpty());
        QVERIFY(journal.last().isEmpty());
    }
    void cancellationCancelsTheLargeFile() {
        B2Fixture f;
        f.reset();
        auto request = f.request("cancel.bin", synthetic(16 * mib));
        B2Fixture::Journal journal;
        journal.attach(request);
        request.progress = [&](const UploadProgress& p) { if (p.bytesTransferred.value_or(0) >= 5 * mib) request.cancellation->store(true); };
        const auto result = f.upload(request);
        QCOMPARE(result.status, QString("failed"));
        QCOMPARE(result.error.code, QString("cancelled"));
        QCOMPARE(f.events("b2_cancel_large_file").size(), 1);
        QVERIFY(f.versions("prefix é/cancel.bin").isEmpty());
    }
    void abandonCancelsAJournaledLargeFile() {
        B2Fixture f;
        f.reset();
        auto request = f.request("retired.bin", synthetic(11 * mib));
        B2Fixture::Journal journal;
        journal.attach(request);
        f.rules({QJsonObject{{"call", "b2_finish_large_file"}, {"status", 503}}});
        QCOMPARE(f.upload(request).status, QString("unknown"));
        const auto retained = journal.last();
        QCOMPARE(f.versions("prefix é/retired.bin").size(), 1);
        const auto error = f.await<Error>([&](auto cb) { f.provider.abandonUpload(f.destination, retained, cb); });
        QVERIFY2(!error, qPrintable(error.message));
        QVERIFY(f.versions("prefix é/retired.bin").isEmpty());
    }
    void authorizedLinksCarryATokenForExactlyTheName() {
        B2Fixture f;
        f.reset();
        f.destination.settings["bucket"] = "putgd-fixture-private";
        f.destination.settings["linkMode"] = "authorized";
        f.destination.settings["linkValidityDays"] = 3;
        const auto uploaded = f.upload(f.request("private.png"));
        QCOMPARE(uploaded.status, QString("uploaded"));
        QVERIFY(uploaded.receipt.directUrl.endsWith("/file/putgd-fixture-private/prefix%20%C3%A9/private.png?Authorization=dl%3Aprefix%20%C3%A9%2Fprivate.png"));
        const auto expires = QDateTime::fromString(uploaded.receipt.objectReference["linkExpiresAt"].toString(), Qt::ISODateWithMs);
        QVERIFY(qAbs(QDateTime::currentDateTimeUtc().secsTo(expires) - 3 * 86400) < 120);
        const auto auth = f.events("b2_get_download_authorization")[0].toObject()["body"].toObject();
        QCOMPARE(auth["fileNamePrefix"].toString(), QString("prefix é/private.png"));
        QCOMPARE(auth["validDurationInSeconds"].toInt(), 3 * 86400);
    }
    void testConnectionPublicAndAuthorized() {
        B2Fixture f;
        f.reset();
        QStringList checkpoints;
        auto save = [&](const QJsonObject& progress, std::function<void(Error)> ack) { checkpoints << progress["phase"].toString(); ack({}); };
        auto [result, error] = f.await<std::pair<QJsonObject, Error>>([&](auto done) { f.provider.testDestination(f.destination, save, [done](QJsonObject r, Error e) { done({r, e}); }); });
        QVERIFY2(!error, qPrintable(error.code + " " + error.message));
        QCOMPARE(result["uploadStatus"].toString(), QString("uploaded"));
        QVERIFY(result["publicReachable"].toBool());
        QCOMPARE(result["cleanupStatus"].toString(), QString("removed"));
        QCOMPARE(result["largeFile"].toObject()["cancel"].toString(), QString("cancelled"));
        QCOMPARE(checkpoints, QStringList({"delete_intent", "done"}));
        QVERIFY(f.state()["versions"].toArray().isEmpty());
        // A private bucket with public links is a setup mistake; authorized links work.
        f.destination.settings["bucket"] = "putgd-fixture-private";
        std::tie(result, error) = f.await<std::pair<QJsonObject, Error>>([&](auto done) { f.provider.testDestination(f.destination, save, [done](QJsonObject r, Error e) { done({r, e}); }); });
        QCOMPARE(error.code, QString("b2_bucket_private"));
        f.destination.settings["linkMode"] = "authorized";
        std::tie(result, error) = f.await<std::pair<QJsonObject, Error>>([&](auto done) { f.provider.testDestination(f.destination, save, [done](QJsonObject r, Error e) { done({r, e}); }); });
        QVERIFY2(!error, qPrintable(error.code + " " + error.message));
        QVERIFY(result["publicReachable"].toBool());
        QVERIFY(f.events("download").last().toObject()["token"].toBool());
    }
    void testConnectionNamesMissingCapabilitiesAndForeignRestriction() {
        B2Fixture f;
        f.reset({{"capabilities", QJsonArray{"listBuckets", "listFiles", "readFiles", "writeFiles"}}});
        auto [result, error] = f.await<std::pair<QJsonObject, Error>>([&](auto done) { f.provider.testDestination(f.destination, {}, [done](QJsonObject r, Error e) { done({r, e}); }); });
        QCOMPARE(error.code, QString("b2_permission_denied"));
        QVERIFY(error.message.contains("deleteFiles"));
        QVERIFY(f.events("upload_file").isEmpty());
        f.reset({{"restricted", "bucket-private-id"}});
        std::tie(result, error) = f.await<std::pair<QJsonObject, Error>>([&](auto done) { f.provider.testDestination(f.destination, {}, [done](QJsonObject r, Error e) { done({r, e}); }); });
        QCOMPARE(error.code, QString("b2_key_restricted"));
    }
    void wrongKeyAndMissingCredentials() {
        B2Fixture f;
        f.reset();
        f.credentials.setSession(f.destination.profileId, {"PUTGD_SYNTHETIC_B2_KEY_ID", "wrong", {}});
        auto checked = f.await<NameCheck>([&](auto cb) { f.provider.checkName(f.destination, "x.png", cb); });
        QCOMPARE(checked.status, QString("inconclusive"));
        QCOMPARE(checked.error.code, QString("b2_authentication_failed"));
        auto other = f.destination;
        other.profileId = "no-credentials";
        checked = f.await<NameCheck>([&](auto cb) { f.provider.checkName(other, "x.png", cb); });
        QCOMPARE(checked.error.code, QString("credential_setup_required"));
    }
    void changedVersionIsNotDeleted() {
        B2Fixture f;
        f.reset();
        const auto uploaded = f.upload(f.request());
        QCOMPARE(uploaded.status, QString("uploaded"));
        auto altered = uploaded.receipt;
        altered.publicationId = "someone-else";
        QCOMPARE(f.remove(altered).status, QString("changed"));
        QVERIFY(f.events("b2_delete_file_version").isEmpty());
    }
};
QTEST_GUILESS_MAIN(B2Test)
#include "test_b2.moc"
