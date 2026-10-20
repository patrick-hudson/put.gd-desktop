#include "fixtures/S3Fixture.h"
using namespace putgd::test;
class S3DeleteTest : public QObject {
    Q_OBJECT
    static QJsonObject rules(std::initializer_list<QJsonObject> list) {
        QJsonArray result;
        for (const auto& rule : list) result.append(rule);
        return {{"rules", result}};
    }
private slots:
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
    void deniedVersioningCheckExplainsRequiredPermission() {
        S3Fixture f;
        f.reset();
        const auto uploaded = f.upload(f.request());
        QCOMPARE(uploaded.status, QString("uploaded"));
        auto configuration = rules({{{"method", "GET"}, {"query", "versioning"}, {"status", 403}}});
        configuration["objects"] = f.state()["objects"];
        f.reset(configuration);
        const auto result = f.remove(uploaded.receipt);
        QCOMPARE(result.error.code, QString("s3_permission_denied"));
        QCOMPARE(result.error.details["requestStage"].toString(), QString("delete_versioning"));
        QVERIFY(result.error.message.contains("s3:GetBucketVersioning"));
        QVERIFY(result.error.message.contains("bucket policy"));
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
        QCOMPARE(result.status, QString("hidden_version_retained"));
        QCOMPARE(result.progress["phase"].toString(), QString("version_unknown"));
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
