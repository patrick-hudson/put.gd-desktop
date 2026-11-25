#include "fixtures/S3Fixture.h"
#include <QStandardPaths>
#include <QUrlQuery>
#include <stdexcept>
#include <thread>
using namespace putgd::test;
namespace {
// HTTPS in front of the plain fixture (tests/fixtures/tls_front.py), with a certificate made for
// this run: it can reset the TLS handshakes of the next new connections, as the lab network did
// to put.gd's S3 calls on 2026-11-23 and 2026-11-25 (#2051).
class TlsFront {
public:
    QTemporaryDir directory;
    QProcess process;
    QString endpoint;
    explicit TlsFront(S3Fixture& fixture) {
        const auto openssl = QStandardPaths::findExecutable("openssl");
        if (openssl.isEmpty()) qFatal("the TLS front needs the openssl command to make its certificate");
        QProcess make;
        make.start(openssl, {"req", "-x509", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:prime256v1", "-nodes",
            "-days", "2", "-subj", "/CN=putgd-tls-front", "-addext", "subjectAltName=IP:127.0.0.1",
            "-keyout", directory.filePath("key.pem"), "-out", certificate()});
        if (!make.waitForFinished(30000) || make.exitCode() != 0) qFatal("the TLS front's certificate couldn't be made");
        arm(0, 0);
        process.start("python3", {TLS_FRONT_PATH, certificate(), directory.filePath("key.pem"),
            QString::number(QUrl(fixture.endpoint).port()), directory.filePath("plan")});
        if (!process.waitForStarted() || !process.waitForReadyRead()) qFatal("TLS front failed to start");
        endpoint = "https://127.0.0.1:" + QString::fromUtf8(process.readLine()).trimmed();
        // libcurl trusts this run's certificate alone (src/core/CaBundle.h).
        qputenv("PUTGD_CA_BUNDLE", certificate().toLocal8Bit());
        fixture.destination.settings["endpoint"] = endpoint;
    }
    ~TlsFront() { qunsetenv("PUTGD_CA_BUNDLE"); process.terminate(); process.waitForFinished(); }
    QString certificate() const { return directory.filePath("cert.pem"); }
    // Let PASS new connections through, then reset the handshakes of the next RESET.
    void arm(int pass, int reset) {
        QFile plan(directory.filePath("plan"));
        if (!plan.open(QIODevice::WriteOnly | QIODevice::Truncate) || plan.write(QByteArray::number(pass) + " " + QByteArray::number(reset) + "\n") < 0)
            qFatal("TLS front plan write failed");
        QFile::remove(directory.filePath("plan.log"));
    }
    // "pass" or "reset" for each new connection since the last arm().
    QStringList connections() const {
        QFile log(directory.filePath("plan.log"));
        if (!log.open(QIODevice::ReadOnly)) return {};
        return QString::fromUtf8(log.readAll()).split('\n', Qt::SkipEmptyParts);
    }
};
}
class S3DeleteTest : public QObject {
    Q_OBJECT
    static QJsonObject rules(std::initializer_list<QJsonObject> list) {
        QJsonArray result;
        for (const auto& rule : list) result.append(rule);
        return {{"rules", result}};
    }
private slots:
    // A deletion whose journal write throws, before or after acknowledging, is never sent.
    void throwingCheckpointNeverDeletes() {
        S3Fixture fixture;
        auto uploaded = fixture.upload(fixture.request("checkpoint-exception.txt"));
        QCOMPARE(uploaded.status, QString("uploaded"));
        auto removed = fixture.remove(uploaded.receipt, {}, [](auto, auto) {
            throw std::runtime_error("synthetic checkpoint exception");
        });
        QCOMPARE(removed.status, QString("unknown"));
        QCOMPARE(removed.error.code, QString("s3_transport_unknown"));
        QVERIFY(removed.progress.isEmpty());
        QCOMPARE(fixture.events("DELETE").size(), 0);
        auto acknowledgedThenThrown = fixture.remove(uploaded.receipt, {}, [](auto, auto ack) {
            ack({});
            throw std::runtime_error("synthetic exception after checkpoint acknowledgement");
        });
        QCOMPARE(acknowledgedThenThrown.status, QString("unknown"));
        QCOMPARE(acknowledgedThenThrown.error.code, QString("s3_transport_unknown"));
        QVERIFY(acknowledgedThenThrown.progress.isEmpty());
        QCOMPARE(fixture.events("DELETE").size(), 0);
    }
    void guardedUnversionedAndOriginalDestination() {
        S3Fixture f;
        f.reset();
        auto uploaded = f.upload(f.request());
        QCOMPARE(uploaded.status, QString("uploaded"));
        f.destination.settings["bucket"] = "edited-bucket";
        f.destination.settings["endpoint"] = "https://unrelated.example";
        QStringList checkpoints;
        auto removed = f.remove(uploaded.receipt, {}, [&](auto progress, auto ack) {
            checkpoints.append(progress["phase"].toString());
            // A destructive request must not yet exist when intent is acknowledged.
            if (progress["phase"] == "object_intent") {
                QCOMPARE(f.events("DELETE").size(), 0);
            }
            ack({});
        });
        QCOMPARE(removed.status, QString("removed"));
        QCOMPARE(checkpoints, QStringList({"object_intent", "done"}));
        const auto event = f.events("DELETE")[0].toObject();
        QVERIFY(event["signatureValid"].toBool());
        QCOMPARE(event["bucket"].toString(), QString("isolated-fixture"));
        QCOMPARE(event["headers"].toObject()["if-match"].toString(), uploaded.receipt.concurrencyToken);
        QCOMPARE(f.remove(uploaded.receipt).status, QString("missing"));
        QCOMPARE(f.events("DELETE").size(), 1);
    }
    void deniedIdentityAndCheckpointBlockDelete() {
        S3Fixture f;
        f.reset();
        auto uploaded = f.upload(f.request());
        auto objects = f.state()["objects"].toObject();
        auto configuration = rules({{{"method", "HEAD"}, {"status", 403}}});
        configuration["objects"] = objects;
        f.reset(configuration);
        QCOMPARE(f.remove(uploaded.receipt).status, QString("unknown"));
        QCOMPARE(f.events("DELETE").size(), 0);
        auto versions = objects["prefix é/a b.txt"].toArray();
        auto replacement = versions[0].toObject();
        replacement["metadata"] = QJsonObject{};
        versions[0] = replacement;
        objects["prefix é/a b.txt"] = versions;
        f.reset({{"objects", objects}});
        QCOMPARE(f.remove(uploaded.receipt).status, QString("changed"));
        QCOMPARE(f.events("DELETE").size(), 0);
        replacement["metadata"] = QJsonObject{{"putgd-publication", uploaded.receipt.publicationId}, {"putgd-sha256", uploaded.receipt.objectReference["sha256"]}};
        versions[0] = replacement;
        objects["prefix é/a b.txt"] = versions;
        f.reset({{"objects", objects}});
        auto result = f.remove(uploaded.receipt, {}, [](auto, auto ack) { ack({"disk_full", "synthetic checkpoint failure", {}}); });
        QCOMPARE(result.status, QString("failed"));
        QCOMPARE(result.error.code, QString("disk_full"));
        QCOMPARE(f.events("DELETE").size(), 0);
    }
    void unsupportedJournalNeverDeletes_data() {
        QTest::addColumn<QJsonObject>("progress");
        QTest::newRow("future-schema") << QJsonObject{{"schemaVersion", 2}};
        QTest::newRow("invalid-schema") << QJsonObject{{"schemaVersion", "one"}};
        QTest::newRow("unknown-phase") << QJsonObject{{"schemaVersion", 1}, {"phase", "future-phase"}, {"target", QJsonObject{}}};
        QTest::newRow("missing-phase-target") << QJsonObject{{"schemaVersion", 1}, {"phase", "marker_done"}};
    }
    void unsupportedJournalNeverDeletes() {
        QFETCH(QJsonObject, progress);
        S3Fixture f;
        f.reset({{"versioning", "Enabled"}});
        auto uploaded = f.upload(f.request());
        const auto before = f.events().size();
        auto result = f.remove(uploaded.receipt, progress);
        QCOMPARE(result.status, QString("failed"));
        QCOMPARE(result.error.code, QString("delete_progress_unsupported"));
        QCOMPARE(f.events().size(), before);
        QCOMPARE(f.events("DELETE").size(), 0);
    }
    void retentionAndConflict_data() {
        QTest::addColumn<int>("http");
        QTest::addColumn<QString>("service");
        QTest::addColumn<QString>("status");
        QTest::newRow("access") << 403 << QString("AccessDenied") << QString("failed");
        QTest::newRow("retention") << 403 << QString("ObjectLocked") << QString("failed");
        QTest::newRow("mfa") << 400 << QString("InvalidRequest") << QString("failed");
        QTest::newRow("conflict") << 409 << QString("ConditionalRequestConflict") << QString("changed");
        QTest::newRow("precondition") << 412 << QString("PreconditionFailed") << QString("changed");
    }
    void retentionAndConflict() {
        QFETCH(int, http); QFETCH(QString, service); QFETCH(QString, status);
        S3Fixture f;
        f.reset();
        auto uploaded = f.upload(f.request());
        auto configuration = rules({{{"method", "DELETE"}, {"status", http}, {"code", service}}});
        configuration["objects"] = f.state()["objects"];
        f.reset(configuration);
        QCOMPARE(f.remove(uploaded.receipt).status, status);
        QCOMPARE(f.events("DELETE").size(), 1);
        QVERIFY(!f.state()["objects"].toObject()["prefix é/a b.txt"].toArray().isEmpty());
    }
    void enabledCurrentHidesBeforeRemovingExactVersion() {
        S3Fixture f;
        f.reset({{"versioning", "Enabled"}});
        auto first = f.upload(f.request("a b.txt", "old synthetic"));
        auto current = f.upload(f.request("a b.txt", "new synthetic"));
        QCOMPARE(current.status, QString("uploaded"));
        QStringList phases;
        auto result = f.remove(current.receipt, {}, [&](auto progress, auto ack) { phases.append(progress["phase"].toString()); ack({}); });
        QCOMPARE(result.status, QString("removed"));
        QCOMPARE(phases, QStringList({"marker_intent", "marker_done", "version_intent", "done"}));
        auto deletes = f.events("DELETE");
        QCOMPARE(deletes.size(), 2);
        QVERIFY(!deletes[0].toObject()["path"].toString().contains("versionId"));
        QCOMPARE(deletes[0].toObject()["headers"].toObject()["if-match"].toString(), current.receipt.concurrencyToken);
        QVERIFY(deletes[1].toObject()["path"].toString().contains("versionId=" + current.receipt.objectReference["versionId"].toString()));
        auto versions = f.state()["objects"].toObject()["prefix é/a b.txt"].toArray();
        QCOMPARE(versions.size(), 2);
        QCOMPARE(versions[0].toObject()["version"], first.receipt.objectReference["versionId"]);
        QVERIFY(versions[1].toObject()["marker"].toBool());
        QCOMPARE(versions[1].toObject()["version"], result.progress["markerVersionId"]);
        for (auto event : deletes) QVERIFY(event.toObject()["signatureValid"].toBool());
    }
    void enabledNoncurrentKeepsReplacement() {
        S3Fixture f;
        f.reset({{"versioning", "Enabled"}});
        auto old = f.upload(f.request());
        auto replacement = f.upload(f.request("a b.txt", "replacement synthetic"));
        auto result = f.remove(old.receipt);
        QCOMPARE(result.status, QString("removed"));
        QCOMPARE(f.events("DELETE").size(), 1);
        auto versions = f.state()["objects"].toObject()["prefix é/a b.txt"].toArray();
        QCOMPARE(versions.size(), 1);
        QCOMPARE(versions[0].toObject()["version"], replacement.receipt.objectReference["versionId"]);
        QVERIFY(!versions[0].toObject()["marker"].toBool());
    }
    void missingRecordedVersionAfterVersioningChangeNeverDeletes_data() {
        QTest::addColumn<QString>("state");
        QTest::newRow("enabled") << QString("Enabled");
        QTest::newRow("suspended") << QString("Suspended");
    }
    void missingRecordedVersionAfterVersioningChangeNeverDeletes() {
        QFETCH(QString, state);
        S3Fixture f;
        f.reset();
        auto uploaded = f.upload(f.request());
        QVERIFY(uploaded.receipt.objectReference["versionId"].toString().isEmpty());
        auto objects = f.state()["objects"].toObject();
        f.reset({{"objects", objects}, {"versioning", state}});
        auto result = f.remove(uploaded.receipt);
        QCOMPARE(result.status, QString("unsupported_safe_removal"));
        QCOMPARE(f.events("DELETE").size(), 0);
        QCOMPARE(f.state()["objects"].toObject()["prefix é/a b.txt"], objects["prefix é/a b.txt"]);
    }
    void suspendedNullSafety_data() {
        QTest::addColumn<QString>("nullState");
        QTest::newRow("unselected-null-object") << QString("object");
        QTest::newRow("unselected-null-marker") << QString("marker");
        QTest::newRow("denied-null-head") << QString("denied");
        QTest::newRow("inconclusive-null-head") << QString("timeout");
    }
    void suspendedNullSafety() {
        QFETCH(QString, nullState);
        S3Fixture f;
        f.reset({{"versioning", "Enabled"}});
        auto uploaded = f.upload(f.request());
        auto objects = f.state()["objects"].toObject();
        auto versions = objects["prefix é/a b.txt"].toArray();
        if (nullState == "object" || nullState == "marker") {
            versions.prepend(QJsonObject{{"version", "null"}, {"body", ""}, {"metadata", QJsonObject{}}, {"etag", "\"null\""}, {"marker", nullState == "marker"}});
            objects["prefix é/a b.txt"] = versions;
        }
        QJsonArray wireRules;
        if (nullState == "denied" || nullState == "timeout")
            wireRules.append(QJsonObject{{"method", "HEAD"}, {"query", "versionId=null"}, {"status", nullState == "denied" ? 403 : 500}});
        f.reset({{"objects", objects}, {"versioning", "Suspended"}, {"rules", wireRules}});
        const auto result = f.remove(uploaded.receipt);
        QCOMPARE(result.status, QString("unsupported_safe_removal"));
        QCOMPARE(f.events("DELETE").size(), 0);
        QCOMPARE(f.state()["objects"].toObject(), objects);
    }
    void suspendedAbsentNullAndMutableNull() {
        S3Fixture f;
        f.reset({{"versioning", "Enabled"}});
        auto uploaded = f.upload(f.request());
        auto objects = f.state()["objects"].toObject();
        f.reset({{"objects", objects}, {"versioning", "Suspended"}});
        auto result = f.remove(uploaded.receipt);
        QCOMPARE(result.status, QString("removed"));
        QCOMPARE(result.progress["markerVersionId"].toString(), QString("null"));
        auto versions = f.state()["objects"].toObject()["prefix é/a b.txt"].toArray();
        QCOMPARE(versions.size(), 1);
        QVERIFY(versions[0].toObject()["marker"].toBool());
        f.reset({{"versioning", "Suspended"}});
        uploaded = f.upload(f.request());
        QCOMPARE(uploaded.receipt.objectReference["versionId"].toString(), QString("null"));
        result = f.remove(uploaded.receipt);
        QCOMPARE(result.status, QString("removed"));
        QCOMPARE(f.events("DELETE").size(), 1);
        QVERIFY(f.events("DELETE")[0].toObject()["path"].toString().contains("versionId=null"));
        QCOMPARE(f.events("DELETE")[0].toObject()["headers"].toObject()["if-match"].toString(), uploaded.receipt.concurrencyToken);
    }
    // #1690 review: deleting a suspended bucket's null version can be answered with a delete marker
    // whose version is "null". The null version itself is gone, so the object is removed, and Test
    // upload and link's cleanup doesn't fail on such buckets.
    void suspendedNullVersionAnsweredWithANullMarkerIsRemoved() {
        S3Fixture f;
        f.reset({{"versioning", "Suspended"}});
        const auto uploaded = f.upload(f.request());
        QCOMPARE(uploaded.receipt.objectReference["versionId"].toString(), QString("null"));
        f.reset({{"objects", f.state()["objects"]}, {"versioning", "Suspended"}, {"rules", QJsonArray{QJsonObject{{"method", "DELETE"}, {"markerReply", true}}}}});
        const auto result = f.remove(uploaded.receipt);
        QCOMPARE(result.status, QString("removed"));
        QVERIFY(f.state()["objects"].toObject()["prefix é/a b.txt"].toArray().isEmpty());
    }
    void partialVersionFailureRetainsMarkerAndResumesOnlyExactVersion() {
        S3Fixture f;
        f.reset({{"versioning", "Enabled"}});
        auto uploaded = f.upload(f.request());
        auto configuration = rules({{{"method", "DELETE"}, {"query", "versionId="}, {"status", 403}}});
        configuration["objects"] = f.state()["objects"];
        configuration["versioning"] = "Enabled";
        f.reset(configuration);
        auto result = f.remove(uploaded.receipt);
        QCOMPARE(result.status, QString("hidden_version_retained"));
        QCOMPARE(result.error.code, QString("s3_permission_denied"));
        QCOMPARE(result.error.details["requestStage"].toString(), QString("delete_version"));
        QVERIFY(result.error.message.contains("s3:DeleteObjectVersion"));
        QVERIFY(result.error.message.contains("bucket policy"));
        QVERIFY(result.progress.contains("markerVersionId"));
        QCOMPARE(f.events("DELETE").size(), 2);
        const QJsonValue objects = f.state().value("objects");
        auto marker = result.progress["markerVersionId"];
        f.reset({{"objects", objects}, {"versioning", "Enabled"}});
        auto resumed = f.remove(uploaded.receipt, result.progress);
        QCOMPARE(resumed.status, QString("removed"));
        QCOMPARE(f.events("DELETE").size(), 1);
        QVERIFY(f.events("DELETE")[0].toObject()["path"].toString().contains("versionId=" + uploaded.receipt.objectReference["versionId"].toString()));
        auto versions = f.state()["objects"].toObject()["prefix é/a b.txt"].toArray();
        QCOMPARE(versions.size(), 1);
        QCOMPARE(versions[0].toObject()["version"], marker);
    }
    // #1771: an unversioned delete answered with a delete marker leaves the object's bytes as a
    // version. Retry only reads whether that version is still there, so once it is removed in the
    // bucket the deletion ends removed instead of staying hidden_version_retained.
    void retryAfterMarkerSettlesOnceTheKeptVersionIsRemoved() {
        S3Fixture f;
        f.reset();
        const auto uploaded = f.upload(f.request());
        QVERIFY(uploaded.receipt.objectReference["versionId"].toString().isEmpty());
        // Versioning was turned on later, and the bucket still reports it off.
        f.reset({{"objects", f.state()["objects"]}, {"versioning", "Enabled"}, {"versioningReported", ""}});
        const auto marked = f.remove(uploaded.receipt);
        QCOMPARE(marked.status, QString("hidden_version_retained"));
        QCOMPARE(f.events("DELETE").size(), 1);
        auto versions = f.state()["objects"].toObject()["prefix é/a b.txt"].toArray();
        QCOMPARE(versions.size(), 2);
        const auto stillThere = f.remove(uploaded.receipt, marked.progress);
        QCOMPARE(stillThere.status, QString("hidden_version_retained"));
        QCOMPARE(stillThere.error.code, QString("delete_marker_created"));
        QVERIFY(!stillThere.error.message.contains("Couldn't check"));
        QVERIFY(f.events("HEAD").last().toObject()["path"].toString().contains("versionId=null"));
        QCOMPARE(f.events("DELETE").size(), 1);
        // The kept version is removed in the bucket; the marker stays.
        versions.removeAt(0);
        f.reset({{"objects", QJsonObject{{"prefix é/a b.txt", versions}}}, {"versioning", "Enabled"}, {"versioningReported", ""}});
        const auto settled = f.remove(uploaded.receipt, stillThere.progress);
        QCOMPARE(settled.status, QString("removed"));
        QCOMPARE(settled.progress["phase"].toString(), QString("done"));
        QCOMPARE(f.events("DELETE").size(), 0);
    }
    void cancellationAfterMarkerRetainsExactHiddenVersion() {
        S3Fixture f;
        f.reset({{"versioning", "Enabled"}});
        auto uploaded = f.upload(f.request());
        auto cancellation = std::make_shared<std::atomic_bool>(false);
        auto result = f.await<DeleteResult>([&](auto cb) {
            f.provider.remove({"cancelled-delete", uploaded.receipt, {}, cancellation},
                [cancellation](auto progress, auto ack) {
                    if (progress["phase"] == "marker_done") cancellation->store(true);
                    ack({});
                }, cb);
        });
        QCOMPARE(result.status, QString("hidden_version_retained"));
        QCOMPARE(f.events("DELETE").size(), 1);
        QVERIFY(result.progress.contains("markerVersionId"));
        QCOMPARE(f.state()["objects"].toObject()["prefix é/a b.txt"].toArray().size(), 2);
        auto retry = f.remove(uploaded.receipt, result.progress);
        QCOMPARE(retry.status, QString("removed"));
        QCOMPARE(f.events("DELETE").size(), 2);
    }
    void lostMarkerReplyNeverCreatesAnotherMarker() {
        S3Fixture f;
        f.reset({{"versioning", "Enabled"}});
        auto uploaded = f.upload(f.request());
        auto configuration = rules({{{"method", "DELETE"}, {"drop", true}}});
        configuration["objects"] = f.state()["objects"];
        configuration["versioning"] = "Enabled";
        f.reset(configuration);
        auto result = f.remove(uploaded.receipt);
        QCOMPARE(result.status, QString("unknown"));
        QCOMPARE(result.progress["phase"].toString(), QString("marker_unknown"));
        QCOMPARE(f.events("DELETE").size(), 1);
        // Restore a new provider instance as a restart with only the persisted journal.
        S3Provider restarted(f.credentials);
        auto retry = f.await<DeleteResult>([&](auto cb) { restarted.remove({"same-delete", uploaded.receipt, result.progress, {}}, [](auto, auto ack) { ack({}); }, cb); });
        QCOMPARE(retry.status, QString("unknown"));
        QCOMPARE(f.events("DELETE").size(), 1);
        QCOMPARE(f.state()["objects"].toObject()["prefix é/a b.txt"].toArray().size(), 2);
    }
    void lostVersionReplyReconcilesExactRemovedVersion() {
        S3Fixture f;
        f.reset({{"versioning", "Enabled"}});
        auto uploaded = f.upload(f.request());
        auto configuration = rules({{{"method", "DELETE"}, {"query", "versionId="}, {"drop", true}}});
        configuration["objects"] = f.state()["objects"];
        configuration["versioning"] = "Enabled";
        f.reset(configuration);
        auto result = f.remove(uploaded.receipt);
        QCOMPARE(result.status, QString("unknown"));
        QCOMPARE(result.progress["phase"].toString(), QString("version_unknown"));
        QVERIFY(result.progress.contains("markerVersionId"));
        auto retry = f.remove(uploaded.receipt, result.progress);
        QCOMPARE(retry.status, QString("removed"));
        QCOMPARE(f.events("DELETE").size(), 2);
        QCOMPARE(f.state()["objects"].toObject()["prefix é/a b.txt"].toArray().size(), 1);
    }
    void lostUnversionedReplyReconcilesAndReplacementSurvives() {
        S3Fixture f;
        f.reset();
        auto uploaded = f.upload(f.request());
        auto configuration = rules({{{"method", "DELETE"}, {"drop", true}}});
        configuration["objects"] = f.state()["objects"];
        f.reset(configuration);
        auto result = f.remove(uploaded.receipt);
        QCOMPARE(result.status, QString("unknown"));
        QCOMPARE(f.remove(uploaded.receipt, result.progress).status, QString("removed"));
        QCOMPARE(f.events("DELETE").size(), 1);
        auto replacement = f.upload(f.request("a b.txt", "replacement"));
        QCOMPARE(replacement.status, QString("uploaded"));
        QCOMPARE(f.remove(uploaded.receipt, result.progress).status, QString("changed"));
        QCOMPARE(f.events("DELETE").size(), 1);
    }
    // Cloudflare R2 (#145): a version ID on every write, no versioning, 501 for versionId.
    void versionlessBucketChecksAndRemovesLikeUnversioned() {
        S3Fixture f;
        f.reset({{"versionless", true}});
        const auto uploaded = f.upload(f.request());
        QCOMPARE(uploaded.status, QString("uploaded"));
        QVERIFY(!uploaded.receipt.objectReference["versionId"].toString().isEmpty());
        // History checks the recorded identity before deleting; the receipt stays as recorded.
        const auto stat = f.await<StatResult>([&](auto cb) { f.provider.stat(uploaded.receipt, cb); });
        QCOMPARE(stat.status, QString("own"));
        QCOMPARE(stat.receipt.objectReference, uploaded.receipt.objectReference);
        QCOMPARE(stat.receipt.concurrencyToken, uploaded.receipt.concurrencyToken);
        QStringList phases;
        QJsonObject journal;
        const auto removed = f.remove(uploaded.receipt, {}, [&](auto progress, auto ack) {
            phases.append(progress["phase"].toString());
            journal = progress;
            ack({});
        });
        QCOMPARE(removed.status, QString("removed"));
        QCOMPARE(phases, QStringList({"object_intent", "done"}));
        QVERIFY(journal["versionless"].toBool());
        QCOMPARE(journal["target"].toObject(), uploaded.receipt.toJson());
        const auto deletes = f.events("DELETE");
        QCOMPARE(deletes.size(), 1);
        QVERIFY(!deletes[0].toObject()["path"].toString().contains("versionId"));
        QCOMPARE(deletes[0].toObject()["headers"].toObject()["if-match"].toString(), uploaded.receipt.concurrencyToken);
        QVERIFY(f.state()["objects"].toObject()["prefix é/a b.txt"].toArray().isEmpty());
        // A retry from that journal stays unversioned and finds the object gone.
        QCOMPARE(f.remove(uploaded.receipt, journal).status, QString("removed"));
        QCOMPARE(f.events("DELETE").size(), 1);
    }
    // Garage (#1581): a version ID on every write, no versioning, a request naming a version is
    // answered with the current object and no version ID, and DELETE ignores If-Match. put.gd took
    // that for another object ("changed") and never deleted what it had uploaded. It now checks the
    // current object without the version and removes it with the conditional DELETE, as on R2.
    void versionIgnoringBucketChecksAndRemovesLikeUnversioned() {
        S3Fixture f;
        f.reset({{"versionIgnored", true}, {"ignoreConditions", true}});
        const auto uploaded = f.upload(f.request());
        QCOMPARE(uploaded.status, QString("uploaded"));
        QVERIFY(!uploaded.receipt.objectReference["versionId"].toString().isEmpty());
        const auto stat = f.await<StatResult>([&](auto cb) { f.provider.stat(uploaded.receipt, cb); });
        QCOMPARE(stat.status, QString("own"));
        QCOMPARE(stat.receipt.objectReference, uploaded.receipt.objectReference);
        QJsonObject journal;
        const auto removed = f.remove(uploaded.receipt, {}, [&](auto progress, auto ack) {
            journal = progress;
            ack({});
        });
        QCOMPARE(removed.status, QString("removed"));
        QVERIFY(journal["versionless"].toBool());
        const auto deletes = f.events("DELETE");
        QCOMPARE(deletes.size(), 1);
        QVERIFY(!deletes[0].toObject()["path"].toString().contains("versionId"));
        QCOMPARE(deletes[0].toObject()["headers"].toObject()["if-match"].toString(), uploaded.receipt.concurrencyToken);
        QVERIFY(f.state()["objects"].toObject()["prefix é/a b.txt"].toArray().isEmpty());
    }
    // A read that fails while telling Garage's version-only mismatch from another object ends
    // unknown, not "changed" (which History never retries). A later Retry removes the file.
    void versionIgnoringBucketTransientReadStaysRetryable_data() {
        QTest::addColumn<QString>("method");QTest::addColumn<QString>("query");QTest::addColumn<int>("after");
        // The second HEAD, of the current object without the version.
        QTest::newRow("current-head") << QString("HEAD") << QString() << 1;
        QTest::newRow("bucket-versioning") << QString("GET") << QString("versioning") << 0;
    }
    void versionIgnoringBucketTransientReadStaysRetryable() {
        QFETCH(QString, method);QFETCH(QString, query);QFETCH(int, after);
        S3Fixture f;
        f.reset({{"versionIgnored", true}, {"ignoreConditions", true}});
        const auto uploaded = f.upload(f.request());
        QCOMPARE(uploaded.status, QString("uploaded"));
        const auto objects = f.state()["objects"].toObject();
        QJsonObject rule{{"method", method}, {"status", 503}, {"code", "SlowDown"}, {"after", after}};
        if (!query.isEmpty()) rule["query"] = query;
        f.reset({{"objects", objects}, {"versionIgnored", true}, {"ignoreConditions", true}, {"rules", QJsonArray{rule}}});
        const auto checked = f.await<StatResult>([&](auto cb) { f.provider.stat(uploaded.receipt, cb); });
        QCOMPARE(checked.status, QString("unknown"));
        QCOMPARE(checked.error.details["httpStatus"].toInt(), 503);
        rule["remaining"] = 1;
        f.reset({{"objects", objects}, {"versionIgnored", true}, {"ignoreConditions", true}, {"rules", QJsonArray{rule}}});
        const auto first = f.remove(uploaded.receipt);
        QCOMPARE(first.status, QString("unknown"));
        QCOMPARE(first.error.details["httpStatus"].toInt(), 503);
        QCOMPARE(f.events("DELETE").size(), 0);
        const auto retried = f.remove(uploaded.receipt, first.progress);
        QCOMPARE(retried.status, QString("removed"));
        QCOMPARE(f.events("DELETE").size(), 1);
        QVERIFY(f.state()["objects"].toObject()["prefix é/a b.txt"].toArray().isEmpty());
    }
    // The same answer from an object someone else wrote (other bytes) is still "changed", and nothing is sent.
    void versionIgnoringBucketStillRefusesAReplacedObject() {
        S3Fixture f;
        f.reset({{"versionIgnored", true}, {"ignoreConditions", true}});
        const auto uploaded = f.upload(f.request());
        QCOMPARE(uploaded.status, QString("uploaded"));
        auto objects = f.state()["objects"].toObject();
        auto versions = objects["prefix é/a b.txt"].toArray();
        auto replaced = versions.last().toObject();
        replaced["etag"] = "\"someone-else\"";
        versions.replace(versions.size() - 1, replaced);
        objects["prefix é/a b.txt"] = versions;
        QJsonObject configuration{{"objects", objects}, {"versionIgnored", true}, {"ignoreConditions", true}};
        f.reset(configuration);
        const auto result = f.remove(uploaded.receipt);
        QCOMPARE(result.status, QString("changed"));
        QCOMPARE(f.events("DELETE").size(), 0);
    }
    // A denied versioning check stops deletion with the versioning guidance (#1677). Off Cloudflare
    // R2's endpoint that holds even when a front answers 501 to requests naming a version: the
    // bucket behind it may keep versions, and an unversioned delete would leave the recorded
    // version behind a marker (Enabled) or remove a null version for good (Suspended).
    void deniedVersioningCheckNeverDeletesWithoutProof_data() {
        QTest::addColumn<QString>("versioning");QTest::addColumn<bool>("refusesVersions");QTest::addColumn<bool>("recordedVersion");
        QTest::newRow("versioned-bucket") << QString("Enabled") << false << true;
        QTest::newRow("unversioned-bucket") << QString() << false << true;
        QTest::newRow("versioned-bucket-behind-501") << QString("Enabled") << true << true;
        QTest::newRow("suspended-bucket-behind-501") << QString("Suspended") << true << false;
    }
    void deniedVersioningCheckNeverDeletesWithoutProof() {
        QFETCH(QString, versioning);QFETCH(bool, refusesVersions);QFETCH(bool, recordedVersion);
        S3Fixture f;
        f.reset({{"versioning", versioning}});
        auto uploaded = f.upload(f.request());
        QCOMPARE(uploaded.status, QString("uploaded"));
        if (!recordedVersion) uploaded.receipt.objectReference["versionId"] = "";
        QJsonArray denied{QJsonObject{{"method", "GET"}, {"query", "versioning"}, {"status", 403}, {"remaining", 1000}}};
        if (refusesVersions) denied.append(QJsonObject{{"method", "HEAD"}, {"query", "versionId"}, {"status", 501}, {"code", "NotImplemented"}, {"remaining", 1000}});
        f.reset({{"objects", f.state()["objects"]}, {"versioning", versioning}, {"rules", denied}});
        const auto result = f.remove(uploaded.receipt);
        QCOMPARE(result.status, QString("unknown"));
        QCOMPARE(result.error.code, QString("s3_permission_denied"));
        QVERIFY(result.error.message.contains("s3:GetBucketVersioning"));
        QVERIFY(!result.progress.contains("versionless"));
        QCOMPARE(f.events("DELETE").size(), 0);
    }
    // #2051: an expiring-link upload deleted while the network resets a TLS handshake. Nothing of
    // that request reached the server, so it is sent again, and the deletion removes the file. A
    // deletion's lookups share its first new connection; its DELETE always opens another one
    // (SdkRuntime.cpp).
    void resetHandshakeIsSentAgain_data() {
        QTest::addColumn<int>("pass");
        QTest::addColumn<QStringList>("connections");
        QTest::newRow("lookup") << 0 << QStringList({"reset", "pass", "pass"});
        QTest::newRow("delete") << 1 << QStringList({"pass", "reset", "pass"});
    }
    void resetHandshakeIsSentAgain() {
        QFETCH(int, pass);
        QFETCH(QStringList, connections);
        S3Fixture f;
        f.reset();
        TlsFront front(f);
        f.destination.settings["linkMode"] = "presigned";
        f.destination.settings["linkValidityDays"] = 1;
        const auto uploaded = f.upload(f.request("expiring link.png"));
        QVERIFY2(uploaded.status == "uploaded", qPrintable(uploaded.error.code + ": " + uploaded.error.message));
        QVERIFY(QUrlQuery(QUrl(uploaded.receipt.directUrl)).hasQueryItem("X-Amz-Signature"));
        front.arm(pass, 1);
        QStringList phases;
        const auto removed = f.remove(uploaded.receipt, {}, [&](auto progress, auto ack) { phases.append(progress["phase"].toString()); ack({}); });
        QCOMPARE(removed.status, QString("removed"));
        QCOMPARE(phases, QStringList({"object_intent", "done"}));
        QCOMPARE(front.connections(), connections);
        QCOMPARE(f.events("DELETE").size(), 1);
        QVERIFY(f.state()["objects"].toObject()["prefix é/expiring link.png"].toArray().isEmpty());
    }
    // A DELETE whose every attempt is reset was never sent: the file is still there, so the
    // deletion failed, rather than ending unknown as if S3 might have removed it.
    void unsentDeleteFailsWithFileKept() {
        S3Fixture f;
        f.reset();
        TlsFront front(f);
        const auto uploaded = f.upload(f.request());
        QVERIFY2(uploaded.status == "uploaded", qPrintable(uploaded.error.code + ": " + uploaded.error.message));
        front.arm(1, 3);
        QStringList phases;
        const auto result = f.remove(uploaded.receipt, {}, [&](auto progress, auto ack) { phases.append(progress["phase"].toString()); ack({}); });
        QCOMPARE(front.connections(), QStringList({"pass", "reset", "reset", "reset"}));
        QCOMPARE(result.status, QString("failed"));
        QCOMPARE(result.error.details["requestStage"].toString(), QString("delete_object"));
        QCOMPARE(result.error.details["curlCode"].toInt(), 35);
        QCOMPARE(phases, QStringList({"object_intent", "object_failed"}));
        QCOMPARE(f.events("DELETE").size(), 0);
        QVERIFY(!f.state()["objects"].toObject()["prefix é/a b.txt"].toArray().isEmpty());
        // Retry finds the file and deletes it.
        front.arm(0, 0);
        QCOMPARE(f.remove(uploaded.receipt, result.progress).status, QString("removed"));
        QCOMPARE(f.events("DELETE").size(), 1);
    }
    // Only a read, or a request on a fresh never-reused connection, is sent again by the SDK:
    // libcurl may already have resent a PutObject whose reused connection lost its reply, so its
    // failed reconnect proves nothing (#2051). The adapter's own three attempts decide the upload.
    void unconnectedPutIsLeftToTheAdapter() {
        S3Fixture f;
        f.reset();
        TlsFront front(f);
        const auto request = f.request();
        front.arm(0, 3);
        const auto uploaded = f.upload(request);
        QCOMPARE(front.connections(), QStringList({"reset", "reset", "reset"}));
        QCOMPARE(uploaded.status, QString("failed"));
        QCOMPARE(uploaded.error.code, QString("s3_upload_interrupted"));
        QCOMPARE(f.events("PUT").size(), 0);
    }
    // A deletion cancelled while its DELETE couldn't connect sends no further attempt (#2051).
    void cancelledDeleteIsNotSentAgain() {
        S3Fixture f;
        f.reset();
        TlsFront front(f);
        const auto uploaded = f.upload(f.request());
        QVERIFY2(uploaded.status == "uploaded", qPrintable(uploaded.error.code + ": " + uploaded.error.message));
        front.arm(1, 3);
        auto cancellation = std::make_shared<std::atomic_bool>(false);
        // Cancel as soon as the DELETE's first connection is reset: inside the 0.25 s before the next attempt.
        std::atomic_bool done{false};
        std::thread watcher([&] {
            while (!done.load() && !front.connections().contains("reset")) std::this_thread::sleep_for(std::chrono::milliseconds(5));
            cancellation->store(true);
        });
        QStringList phases;
        const auto result = f.await<DeleteResult>([&](auto cb) {
            f.provider.remove({"synthetic-delete", uploaded.receipt, {}, cancellation}, [&](auto progress, auto ack) {
                phases.append(progress["phase"].toString());
                ack({});
            }, cb);
        });
        done.store(true);
        watcher.join();
        QCOMPARE(front.connections(), QStringList({"pass", "reset"}));
        QCOMPARE(result.status, QString("failed"));
        QCOMPARE(result.error.code, QString("cancelled"));
        QCOMPARE(phases, QStringList({"object_intent", "object_cancelled"}));
        QCOMPARE(f.events("DELETE").size(), 0);
        QVERIFY(!f.state()["objects"].toObject()["prefix é/a b.txt"].toArray().isEmpty());
    }
    void notImplementedVersionOnVersionedBucketNeverDeletes() {
        S3Fixture f;
        f.reset({{"versioning", "Enabled"}});
        const auto uploaded = f.upload(f.request());
        auto configuration = rules({{{"method", "HEAD"}, {"query", "versionId"}, {"status", 501}, {"code", "NotImplemented"}}});
        configuration["objects"] = f.state()["objects"];
        configuration["versioning"] = "Enabled";
        f.reset(configuration);
        const auto result = f.remove(uploaded.receipt);
        QCOMPARE(result.status, QString("unknown"));
        QCOMPARE(result.error.code, QString("s3_unsupported_request"));
        QVERIFY(!result.progress.contains("versionless"));
        QCOMPARE(f.events("DELETE").size(), 0);
    }
};
QTEST_GUILESS_MAIN(S3DeleteTest)
#include "test_s3_delete.moc"
