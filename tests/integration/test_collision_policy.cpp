#include "fixtures/S3Fixture.h"
#include "fixtures/U2Fixtures.h"
using namespace putgd::test;
class CollisionTest : public QObject {
    Q_OBJECT
    QJsonObject configuration(S3Fixture& fixture, const QString& policy, bool archive = false) {
        auto doc = putgd::test::configuration(fixture.directory.path());
        auto naming = doc["naming"].toObject();
        naming["mode"] = "original";
        naming["collisionPolicy"] = policy;
        doc["naming"] = naming;
        doc["profiles"] = QJsonArray{QJsonObject{{"id", fixture.destination.profileId}, {"label", "Synthetic S3"}, {"provider", "s3"}, {"settings", fixture.destination.settings}, {"credentials", fixture.destination.credentialReference}}};
        auto actions = doc["actions"].toObject();
        for (const auto& key : Settings::actionNames()) {
            auto action = actions[key].toObject();
            action["profileId"] = fixture.destination.profileId;
            actions[key] = action;
        }
        doc["actions"] = actions;
        auto archiveSettings = doc["archive"].toObject();
        archiveSettings["keepUploads"] = archive;
        doc["archive"] = archiveSettings;
        return doc;
    }
    static Operation waitOperation(OperationService& operations, const QString& id) {
        Operation result;
        QElapsedTimer elapsed;elapsed.start();
        // Thirty-two guarded HEAD probes include durable checkpoints and worker IPC.
        while (elapsed.elapsed() < 15000) {
            QTest::qWait(25);
            HistoryStore store(operations.databasePath());
            result = store.operation(id);
            if (QStringList{"completed", "partial", "failed", "cancelled"}.contains(result.state)) {
                qInfo("collision_fixture_terminal_ms=%lld",elapsed.elapsed());
                return result;
            }
        }
        QTest::qFail("Operation did not reach a terminal state within 15 seconds",__FILE__,__LINE__);
        return result;
    }
private slots:
    void existingKeyPolicies_data() {
        QTest::addColumn<QString>("policy");
        QTest::newRow("rename") << QString("rename");
        QTest::newRow("skip") << QString("skip");
        QTest::newRow("overwrite") << QString("overwrite");
    }
    void existingKeyPolicies() {
        QFETCH(QString, policy);
        S3Fixture f;
        f.reset();
        auto original = f.upload(f.request("report.pdf", "original fixture"));
        QCOMPARE(original.status, QString("uploaded"));
        ProviderRegistry registry;
        auto provider = std::make_shared<S3Provider>(f.credentials);
        registry.add("s3", provider);
        SettingsService settings(f.directory.filePath("config.json"), [&](auto id, auto value) { return registry.validate(id, value); });
        const auto saved = settings.save(configuration(f, policy), {});
        QVERIFY2(saved, qPrintable(saved.error.code));
        Source source;
        source.payload.kind = "files";
        source.payload.files = {putgd::test::fixture(f.directory.path(), "report.pdf", "new fixture")};
        OperationService operations(f.directory.filePath("data"), settings, registry, source);
        QTRY_VERIFY(operations.ready());
        QString id;
        operations.invoke("clipboard", [&](auto error, auto result) { QVERIFY(!error); id = result["operationId"].toString(); });
        QTRY_VERIFY(!id.isEmpty());
        auto operation = waitOperation(operations, id);
        QCOMPARE(operation.items.size(), 1);
        const auto item = operation.items[0];
        if (policy == "skip") {
            QCOMPARE(item.uploadStatus, QString("skipped"));
            QCOMPARE(f.events("PUT").size(), 1);
            QCOMPARE(source.copies, 0);
            QVERIFY(item.receipt.directUrl.isEmpty());
        } else {
            QCOMPARE(item.uploadStatus, QString("uploaded"));
            QTRY_COMPARE(source.copies, 1);
            QCOMPARE(source.copied.text, item.receipt.directUrl);
            if (policy == "rename") {
                QVERIFY(item.candidate.startsWith("report-"));
                QVERIFY(item.candidate.endsWith(".pdf"));
                QVERIFY(item.candidate != "report.pdf");
            } else {
                QCOMPARE(item.candidate, QString("report.pdf"));
                QCOMPARE(f.events("PUT")[1].toObject()["headers"].toObject()["if-match"].toString(), original.receipt.concurrencyToken);
            }
            const auto objects = f.state()["objects"].toObject();
            const auto key = item.receipt.objectReference["key"].toString();
            QCOMPARE(QByteArray::fromBase64(objects[key].toArray().last().toObject()["body"].toString().toUtf8()), QByteArray("new fixture"));
            if (policy == "rename") QCOMPARE(QByteArray::fromBase64(objects["prefix é/report.pdf"].toArray().last().toObject()["body"].toString().toUtf8()), QByteArray("original fixture"));
        }
    }
    void partialBatchRetriesOnlyFailedAndRetainsArchives() {
        S3Fixture f;
        f.reset({{"rules", QJsonArray{QJsonObject{{"method", "PUT"}, {"key", "prefix é/Upload-b.txt"}, {"status", 403}}}}});
        ProviderRegistry registry;
        registry.add("s3", std::make_shared<S3Provider>(f.credentials));
        SettingsService settings(f.directory.filePath("config.json"), [&](auto id, auto value) { return registry.validate(id, value); });
        auto document=configuration(f, "rename", true);
        auto naming=document["naming"].toObject();naming["mode"]="template";naming["fileTemplate"]="Upload-{name}.{ext}";document["naming"]=naming;
        QVERIFY(settings.save(document, {}));
        Source source;
        source.payload.kind = "files";
        source.payload.files = {putgd::test::fixture(f.directory.path(), "a.txt", "a synthetic"), putgd::test::fixture(f.directory.path(), "b.txt", "b synthetic")};
        OperationService operations(f.directory.filePath("data"), settings, registry, source);
        QTRY_VERIFY(operations.ready());
        QString id;
        operations.invoke("clipboard", [&](auto error, auto result) { QVERIFY(!error); id = result["operationId"].toString(); });
        QTRY_VERIFY(!id.isEmpty());
        auto operation = waitOperation(operations, id);
        QCOMPARE(operation.state, QString("partial"));
        QCOMPARE(operation.items[0].uploadStatus, QString("uploaded"));
        QCOMPARE(operation.items[1].uploadStatus, QString("failed"));
        QCOMPARE(operation.items[0].candidate,QString("Upload-a.txt"));
        QCOMPARE(operation.items[1].candidate,QString("Upload-b.txt"));
        QCOMPARE(operation.items[1].artifact.name,QString("Upload-b.txt"));
        QCOMPARE(f.events("PUT").size(), 2);
        const auto savedReceipt = operation.items[0].receipt.toJson();
        for (const auto& item : operation.items) {
            QCOMPARE(item.local.status, QString("saved"));
            QVERIFY(QFile::exists(item.local.path));
            QCOMPARE(QFileInfo(item.local.path).fileName(),item.artifact.name);
        }
        QTRY_COMPARE(source.copies, 1);
        QCOMPARE(source.copied.text, operation.items[0].receipt.directUrl);
        QTRY_COMPARE(operations.diagnostics()["activeOperations"].toInt(), 0);
        naming["mode"]="generated";naming["fileTemplate"]="Changed-{uuid}.{ext}";document["naming"]=naming;
        QVERIFY(settings.save(document,settings.revision()));
        bool retryAccepted = false;
        operations.retry(id, [&](auto error, auto) { QVERIFY(!error); retryAccepted = true; });
        QTRY_VERIFY(retryAccepted);
        QTRY_COMPARE(HistoryStore(operations.databasePath()).operation(id).items[1].uploadStatus, QString("uploaded"));
        QTRY_COMPARE(operations.diagnostics()["activeOperations"].toInt(), 0);
        operation = waitOperation(operations, id);
        QCOMPARE(operation.items[0].receipt.toJson(), savedReceipt);
        QCOMPARE(operation.items[1].uploadStatus, QString("uploaded"));
        QCOMPARE(f.events("PUT").size(), 3);
        QCOMPARE(f.events("PUT")[2].toObject()["key"].toString(), QString("prefix é/Upload-b.txt"));
        QCOMPARE(operation.items[1].artifact.name,QString("Upload-b.txt"));
        QCOMPARE(operation.settings.namingMode,QString("template"));
        QCOMPARE(operation.settings.fileTemplate,QString("Upload-{name}.{ext}"));
        for (const auto& item : operation.items) QVERIFY(QFile::exists(item.local.path));
    }
    void realHistoryDeletionPersistsSdkPhasesAndPreservesLocalCopy() {
        S3Fixture f;
        f.reset({{"versioning", "Enabled"}});
        ProviderRegistry registry;
        registry.add("s3", std::make_shared<S3Provider>(f.credentials));
        SettingsService settings(f.directory.filePath("config.json"), [&](auto id, auto value) { return registry.validate(id, value); });
        QVERIFY(settings.save(configuration(f, "rename", true), {}));
        Source source;
        source.payload.kind = "files";
        source.payload.files = {putgd::test::fixture(f.directory.path(), "history.txt", "history synthetic bytes")};
        OperationService operations(f.directory.filePath("data"), settings, registry, source);
        HistoryService history(operations);
        QTRY_VERIFY(operations.ready());
        QString operationId;
        operations.invoke("clipboard", [&](auto error, auto value) { QVERIFY(!error); operationId = value["operationId"].toString(); });
        QTRY_VERIFY(!operationId.isEmpty());
        const auto uploaded = waitOperation(operations, operationId).items[0];
        QCOMPARE(uploaded.uploadStatus, QString("uploaded"));
        QVERIFY(QFile::exists(uploaded.local.path));
        HistoryStore store(operations.databasePath());
        const auto originalReceipt = store.detail(uploaded.artifact.id)["receipt"].toObject();
        // Make the profile local-only after upload; history must retain its origin.
        QVERIFY(settings.save(putgd::test::configuration(f.directory.path(), true), settings.revision()));
        QJsonObject preview;
        history.prepareDelete({uploaded.artifact.id}, [&](auto error, auto value) { QVERIFY(!error); preview = value; });
        QTRY_VERIFY(!preview.isEmpty());
        // Delay the marker's response so its durable intent can be observed while
        // a real SDK destructive request is already on the fixture wire.
        f.reset({{"objects", f.state()["objects"]}, {"versioning", "Enabled"},
                 {"rules", QJsonArray{QJsonObject{{"method", "DELETE"}, {"barrier", "history-marker"}}}}});
        QString deletionId;
        history.deleteConfirmed({uploaded.artifact.id}, preview["acknowledgement"].toString(), [&](auto error, auto value) {
            QVERIFY(!error);
            deletionId = value["deleteOperationId"].toString();
        });
        QTRY_VERIFY(!deletionId.isEmpty());
        QTRY_COMPARE(f.events("DELETE").size(), 1);
        auto journal = store.deletion(deletionId);
        const auto persistedIntent = journal["items"].toArray()[0].toObject()["progress"].toObject();
        QCOMPARE(persistedIntent["phase"].toString(), QString("marker_intent"));
        QCOMPARE(persistedIntent["target"].toObject(), originalReceipt);
        f.control("/__release", {{"barrier", "history-marker"}}, true);
        QTRY_COMPARE(store.detail(uploaded.artifact.id)["remoteStatus"].toString(), QString("removed"));
        journal = store.deletion(deletionId);
        const auto finalProgress = journal["items"].toArray()[0].toObject()["progress"].toObject();
        QCOMPARE(finalProgress["phase"].toString(), QString("done"));
        QVERIFY(!finalProgress["markerVersionId"].toString().isEmpty());
        QCOMPARE(finalProgress["target"].toObject(), originalReceipt);
        QCOMPARE(store.detail(uploaded.artifact.id)["receipt"].toObject(), originalReceipt);
        QVERIFY(QFile::exists(uploaded.local.path));
        const auto events = f.events("DELETE");
        QCOMPARE(events.size(), 2);
        QVERIFY(!events[0].toObject()["path"].toString().contains("versionId"));
        QVERIFY(events[1].toObject()["path"].toString().contains("versionId=" + uploaded.receipt.objectReference["versionId"].toString()));
        for (auto event : events) {
            QVERIFY(event.toObject()["signatureValid"].toBool());
            QCOMPARE(event.toObject()["bucket"].toString(), QString("isolated-fixture"));
        }
    }
    void cancellationRetainsCommittedArchivesAndSuppressesCopy() {
        S3Fixture f;
        f.reset({{"rules", QJsonArray{QJsonObject{{"method", "PUT"}, {"delay", 0.4}}}}});
        ProviderRegistry registry;
        registry.add("s3", std::make_shared<S3Provider>(f.credentials));
        SettingsService settings(f.directory.filePath("config.json"), [&](auto id, auto value) { return registry.validate(id, value); });
        QVERIFY(settings.save(configuration(f, "rename", true), {}));
        Source source;
        source.payload.kind = "files";
        source.payload.files = {putgd::test::fixture(f.directory.path(), "a.txt", "a synthetic"), putgd::test::fixture(f.directory.path(), "b.txt", "b synthetic")};
        OperationService operations(f.directory.filePath("data"), settings, registry, source);
        QTRY_VERIFY(operations.ready());
        QString id;
        operations.invoke("clipboard", [&](auto error, auto result) { QVERIFY(!error); id = result["operationId"].toString(); });
        QTRY_VERIFY(!id.isEmpty());
        QTRY_COMPARE(f.events("PUT").size(), 1);
        operations.cancel(id, [](auto error, auto) { QVERIFY(!error); });
        const auto operation = waitOperation(operations, id);
        QCOMPARE(operation.state, QString("cancelled"));
        QCOMPARE(f.events("PUT").size(), 1);
        QCOMPARE(source.copies, 0);
        for (const auto& item : operation.items) {
            QCOMPARE(item.local.status, QString("saved"));
            QVERIFY(QFile::exists(item.local.path));
        }
    }
    void archiveFailureDoesNotBlockUpload() {
        S3Fixture f;
        f.reset();
        ProviderRegistry registry;
        registry.add("s3", std::make_shared<S3Provider>(f.credentials));
        SettingsService settings(f.directory.filePath("config.json"), [&](auto id, auto value) { return registry.validate(id, value); });
        auto doc = configuration(f, "rename", true);
        auto archive = doc["archive"].toObject();
        archive["root"] = putgd::test::fixture(f.directory.path(), "not-directory", "synthetic file");
        doc["archive"] = archive;
        QVERIFY(settings.save(doc, {}));
        Source source;
        source.payload = textPayload();
        OperationService operations(f.directory.filePath("data"), settings, registry, source);
        QTRY_VERIFY(operations.ready());
        QString id;
        operations.invoke("clipboard", [&](auto error, auto result) { QVERIFY(!error); id = result["operationId"].toString(); });
        QTRY_VERIFY(!id.isEmpty());
        auto operation = waitOperation(operations, id);
        QCOMPARE(operation.items[0].local.status, QString("failed"));
        QCOMPARE(operation.items[0].uploadStatus, QString("uploaded"));
        QTRY_COMPARE(source.copies, 1);
    }
    void exhaustedRenameAndDeniedHeadsNeverPut_data() {
        QTest::addColumn<QString>("failure");
        QTest::newRow("exhaustion") << QString("exhaustion");
        QTest::newRow("denied") << QString("denied");
        QTest::newRow("timeout") << QString("timeout");
    }
    void exhaustedRenameAndDeniedHeadsNeverPut() {
        QFETCH(QString, failure);
        S3Fixture f;
        QJsonObject rule{{"method", "HEAD"}, {"remaining", 100}};
        if (failure == "exhaustion") rule["competitor"] = true;
        if (failure == "denied") rule["status"] = 403;
        if (failure == "timeout") { rule["delay"] = 0.3; f.destination.settings["requestTimeoutMs"] = 100; }
        f.reset({{"rules", QJsonArray{rule}}});
        ProviderRegistry registry;
        registry.add("s3", std::make_shared<S3Provider>(f.credentials));
        SettingsService settings(f.directory.filePath("config.json"), [&](auto id, auto value) { return registry.validate(id, value); });
        QVERIFY(settings.save(configuration(f, "rename"), {}));
        Source source;
        source.payload.kind = "files";
        source.payload.files = {putgd::test::fixture(f.directory.path(), "report.pdf", "synthetic")};
        OperationService operations(f.directory.filePath("data"), settings, registry, source);
        QTRY_VERIFY(operations.ready());
        QString id;
        operations.invoke("clipboard", [&](auto error, auto result) { QVERIFY(!error); id = result["operationId"].toString(); });
        QTRY_VERIFY(!id.isEmpty());
        auto operation = waitOperation(operations, id);
        QCOMPARE(operation.items[0].uploadStatus, QString("failed"));
        QCOMPARE(f.events("PUT").size(), 0);
        QCOMPARE(source.copies, 0);
        if (failure == "exhaustion") {
            QCOMPARE(operation.items[0].error.code, QString("collision_exhausted"));
            QCOMPARE(f.events("HEAD").size(), 32);
        }
    }
};
QTEST_GUILESS_MAIN(CollisionTest)
#include "test_collision_policy.moc"
