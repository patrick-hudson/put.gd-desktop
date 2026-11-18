#include "fixtures/S3Fixture.h"
#include "fixtures/U2Fixtures.h"
#include <QSqlQuery>
#include "fixtures/Waits.h"
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
        const bool terminal = putgd::test::waitFor([&] {
            HistoryStore store(operations.databasePath());
            result = store.operation(id);
            return QStringList{"completed", "partial", "failed", "cancelled"}.contains(result.stateName());
        }, 15000);
        if (terminal) {
            qInfo("collision_fixture_terminal_ms=%lld",elapsed.elapsed());
            return result;
        }
        QTest::qFail("Operation did not reach a terminal state within the 15-second budget",__FILE__,__LINE__);
        return result;
    }
    // #1627: the S3 driver settles an unclear multipart completion by checking the key itself;
    // History takes its answer instead of checking again.
    struct LargeUpload {
        S3Fixture f;
        ProviderRegistry registry;
        std::unique_ptr<SettingsService> settings;
        Source source;
        std::unique_ptr<OperationService> operations;
        explicit LargeUpload(CollisionTest& test, const QString& policy = "rename") {
            constexpr qint64 mib = 1024 * 1024;
            registry.add("s3", std::make_shared<S3Provider>(f.credentials, S3UploadPolicy{6 * mib, 5 * mib}));
            settings = std::make_unique<SettingsService>(f.directory.filePath("config.json"), [this](auto id, auto value) { return registry.validate(id, value); });
            settings->save(test.configuration(f, policy), {});
            source.payload.kind = "files";
            source.payload.files = {putgd::test::fixture(f.directory.path(), "big.bin", QByteArray(11 * mib, 'x'))};
            operations = std::make_unique<OperationService>(f.directory.filePath("data"), *settings, registry, source);
        }
        // An upload whose completion stays unknown: the reply is unclear and the key check fails.
        // With published, the completion did publish before its unclear answer.
        Operation unknownCompletion(bool published) {
            const QString original = "prefix é/big.bin";
            QJsonObject completion{{"method", "POST"}, {"query", "uploadId="}, {"code", "InternalError"}};
            completion[published ? "statusAfter" : "status"] = 500;
            f.reset({{"rules", QJsonArray{completion, QJsonObject{{"method", "HEAD"}, {"key", original}, {"after", 1}, {"status", 503}, {"code", "SlowDown"}}}}});
            const auto id = start();
            putgd::test::waitFor([&] { return QStringList{"completed", "partial", "failed", "cancelled"}.contains(HistoryStore(operations->databasePath()).operation(id).stateName()); }, 15000);
            putgd::test::waitFor([&] { return operations->diagnostics()["activeOperations"].toInt() == 0; }, 5000);
            return HistoryStore(operations->databasePath()).operation(id);
        }
        Operation retry(const QString& id) {
            bool accepted = false;
            operations->retry(id, [&](auto error, auto) { accepted = !error; });
            putgd::test::waitFor([&] { return accepted; }, 5000);
            putgd::test::waitFor([&] { return operations->diagnostics()["activeOperations"].toInt() > 0; }, 1000);
            putgd::test::waitFor([&] { return operations->diagnostics()["activeOperations"].toInt() == 0; }, 15000);
            return HistoryStore(operations->databasePath()).operation(id);
        }
        QString start() {
            QString id;
            operations->invoke("clipboard", [&](auto error, auto value) { if (!error) id = value["operationId"].toString(); });
            putgd::test::waitFor([&] { return !id.isEmpty(); }, 5000);
            return id;
        }
    };
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
        PUTGD_TRY_VERIFY(operations.ready());
        QString id;
        operations.invoke("clipboard", [&](auto error, auto result) { QVERIFY(!error); id = result["operationId"].toString(); });
        PUTGD_TRY_VERIFY(!id.isEmpty());
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
            PUTGD_TRY_COMPARE(source.copies, 1);
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
    // M7: the driver's HEAD found another object, so the file is renamed; a second HEAD of the
    // key, which would fail, is never sent.
    void driverSettledCollisionIsNotCheckedAgain() {
        LargeUpload u(*this);
        PUTGD_TRY_VERIFY(u.operations->ready());
        const QString original = "prefix é/big.bin";
        u.f.reset({{"rules", QJsonArray{QJsonObject{{"method", "POST"}, {"query", "uploadId="}, {"status", 500}, {"code", "InternalError"}},
                                        QJsonObject{{"method", "HEAD"}, {"key", original}, {"after", 1}, {"competitor", true}},
                                        QJsonObject{{"method", "HEAD"}, {"key", original}, {"after", 1}, {"status", 503}, {"code", "SlowDown"}}}}});
        const auto id = u.start();
        QVERIFY(!id.isEmpty());
        const auto item = waitOperation(*u.operations, id).items[0];
        QCOMPARE(item.uploadStatus, QString("uploaded"));
        QVERIFY(item.candidate.startsWith("big-"));
        QCOMPARE(QByteArray::fromBase64(u.f.state()["objects"].toObject()[original].toArray().last().toObject()["body"].toString().toUtf8()), QByteArray("competitor"));
    }
    // M6: Retry of an item whose completion stayed unknown goes through the driver's check. Another
    // object now holds the key, so the file is renamed and uploaded instead of staying unknown.
    void retryOfSavedCompletionUsesTheDriversCheck() {
        LargeUpload u(*this);
        PUTGD_TRY_VERIFY(u.operations->ready());
        const QString original = "prefix é/big.bin";
        u.f.reset({{"rules", QJsonArray{QJsonObject{{"method", "POST"}, {"query", "uploadId="}, {"status", 500}, {"code", "InternalError"}},
                                        QJsonObject{{"method", "HEAD"}, {"key", original}, {"after", 1}, {"status", 503}, {"code", "SlowDown"}}}}});
        const auto id = u.start();
        QVERIFY(!id.isEmpty());
        const auto unknown = waitOperation(*u.operations, id).items[0];
        QCOMPARE(unknown.uploadStatus, QString("unknown"));
        QCOMPARE(unknown.uploadJournal["phase"].toString(), QString("completing"));
        u.f.control("/__replace", {{"key", original}}, true);
        PUTGD_TRY_COMPARE(u.operations->diagnostics()["activeOperations"].toInt(), 0);
        bool accepted = false;
        u.operations->retry(id, [&](auto error, auto) { QVERIFY(!error); accepted = true; });
        PUTGD_TRY_VERIFY(accepted);
        PUTGD_TRY_COMPARE(HistoryStore(u.operations->databasePath()).operation(id).items[0].uploadStatus, QString("uploaded"));
        const auto item = waitOperation(*u.operations, id).items[0];
        QVERIFY(item.candidate.startsWith("big-"));
        QVERIFY(item.uploadJournal.isEmpty());
    }
    // M6: a Retry of an unknown completion checks the saved identity first, without the local
    // file, and a local or setup problem leaves the item unknown with its journal, never failed.
    void retryOfUnknownCompletionNeverFailsOnALocalProblem_data() {
        QTest::addColumn<QString>("problem");QTest::addColumn<QString>("status");
        QTest::newRow("file-gone-object-there") << QString("file-gone-published") << QString("uploaded");
        QTest::newRow("file-gone-check-fails") << QString("file-gone-check-fails") << QString("unknown");
        QTest::newRow("credentials-locked") << QString("credentials-locked") << QString("unknown");
        // While History's identity HEAD runs, the credentials lock or the retained file changes:
        // the upload then fails before the provider settled anything, and the item stays unknown.
        QTest::newRow("credentials-lock-after-check") << QString("credentials-lock-after-check") << QString("unknown");
        QTest::newRow("file-changed-after-check") << QString("file-changed-after-check") << QString("unknown");
    }
    void retryOfUnknownCompletionNeverFailsOnALocalProblem() {
        QFETCH(QString, problem);QFETCH(QString, status);
        LargeUpload u(*this);
        PUTGD_TRY_VERIFY(u.operations->ready());
        const auto before = u.unknownCompletion(problem == "file-gone-published");
        QCOMPARE(before.items[0].uploadStatus, QString("unknown"));
        const auto journal = before.items[0].uploadJournal;
        QCOMPARE(journal["phase"].toString(), QString("completing"));
        if (problem.startsWith("file-gone")) QVERIFY(QFile::remove(before.items[0].artifact.path));
        if (problem == "file-gone-check-fails") u.f.control("/__rules", {{"rules", QJsonArray{QJsonObject{{"method", "HEAD"}, {"status", 503}, {"code", "SlowDown"}, {"remaining", 50}}}}}, true);
        else if (problem.endsWith("after-check")) u.f.control("/__rules", {{"rules", QJsonArray{QJsonObject{{"method", "HEAD"}, {"delay", 0.8}}}}}, true);
        else u.f.control("/__rules", {{"rules", QJsonArray{}}}, true);
        const auto artifact = before.items[0].artifact.path;
        if (problem == "credentials-lock-after-check")
            QTimer::singleShot(300, [&] { u.f.credentials.forget(u.f.destination.profileId, u.f.destination.credentialReference); });
        if (problem == "file-changed-after-check")
            QTimer::singleShot(300, [artifact] { QFile file(artifact); file.setPermissions(QFile::ReadOwner | QFile::WriteOwner); if (file.open(QIODevice::WriteOnly)) file.write("changed"); });
        if (problem == "credentials-locked") u.f.credentials.forget(u.f.destination.profileId, u.f.destination.credentialReference);
        const auto item = u.retry(before.id).items[0];
        QCOMPARE(item.uploadStatus, status);
        if (status == "unknown") {
            QCOMPARE(item.uploadJournal, journal);
            QVERIFY(!item.error.code.isEmpty());
        } else QVERIFY(item.uploadJournal.isEmpty());
        QCOMPARE(u.f.events("DELETE").size(), 0);
    }
    // M8: the provider finds another object at the key but S3 refuses to abort the upload. The
    // item stays unknown with its journal, instead of the skip policy recording it skipped.
    void unabortableCollisionStaysUnknownBeforeThePolicy() {
        LargeUpload u(*this, "skip");
        PUTGD_TRY_VERIFY(u.operations->ready());
        const auto before = u.unknownCompletion(false);
        QCOMPARE(before.items[0].uploadStatus, QString("unknown"));
        const auto journal = before.items[0].uploadJournal;
        u.f.control("/__replace", {{"key", "prefix é/big.bin"}}, true);
        u.f.control("/__rules", {{"rules", QJsonArray{QJsonObject{{"method", "DELETE"}, {"query", "uploadId="}, {"status", 403}, {"code", "AccessDenied"}, {"remaining", 20}}}}}, true);
        const auto item = u.retry(before.id).items[0];
        QCOMPARE(item.uploadStatus, QString("unknown"));
        QCOMPARE(item.uploadJournal, journal);
        QVERIFY(item.error.details["incompleteUpload"].toBool());
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
        PUTGD_TRY_VERIFY(operations.ready());
        QString id;
        operations.invoke("clipboard", [&](auto error, auto result) { QVERIFY(!error); id = result["operationId"].toString(); });
        PUTGD_TRY_VERIFY(!id.isEmpty());
        auto operation = waitOperation(operations, id);
        QCOMPARE(operation.stateName(), QString("partial"));
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
        PUTGD_TRY_COMPARE(source.copies, 1);
        QCOMPARE(source.copied.text, operation.items[0].receipt.directUrl);
        PUTGD_TRY_COMPARE(operations.diagnostics()["activeOperations"].toInt(), 0);
        naming["mode"]="generated";naming["fileTemplate"]="Changed-{uuid}.{ext}";document["naming"]=naming;
        QVERIFY(settings.save(document,settings.revision()));
        bool retryAccepted = false;
        operations.retry(id, [&](auto error, auto) { QVERIFY(!error); retryAccepted = true; });
        PUTGD_TRY_VERIFY(retryAccepted);
        PUTGD_TRY_COMPARE(HistoryStore(operations.databasePath()).operation(id).items[1].uploadStatus, QString("uploaded"));
        PUTGD_TRY_COMPARE(operations.diagnostics()["activeOperations"].toInt(), 0);
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
        PUTGD_TRY_VERIFY(operations.ready());
        QString operationId;
        operations.invoke("clipboard", [&](auto error, auto value) { QVERIFY(!error); operationId = value["operationId"].toString(); });
        PUTGD_TRY_VERIFY(!operationId.isEmpty());
        const auto uploaded = waitOperation(operations, operationId).items[0];
        QCOMPARE(uploaded.uploadStatus, QString("uploaded"));
        QVERIFY(QFile::exists(uploaded.local.path));
        HistoryStore store(operations.databasePath());
        const auto originalReceipt = store.detail(uploaded.artifact.id)["receipt"].toObject();
        // Make the profile local-only after upload; history must retain its origin.
        QVERIFY(settings.save(putgd::test::configuration(f.directory.path(), true), settings.revision()));
        QJsonObject preview;
        history.prepareDelete({uploaded.artifact.id}, [&](auto error, auto value) { QVERIFY(!error); preview = value; });
        PUTGD_TRY_VERIFY(!preview.isEmpty());
        // Delay the marker's response so its durable intent can be observed while
        // a real SDK destructive request is already on the fixture wire.
        f.reset({{"objects", f.state()["objects"]}, {"versioning", "Enabled"},
                 {"rules", QJsonArray{QJsonObject{{"method", "DELETE"}, {"barrier", "history-marker"}}}}});
        QString deletionId;
        history.deleteConfirmed({uploaded.artifact.id}, preview["acknowledgement"].toString(), [&](auto error, auto value) {
            QVERIFY(!error);
            deletionId = value["deleteOperationId"].toString();
        });
        PUTGD_TRY_VERIFY(!deletionId.isEmpty());
        PUTGD_TRY_COMPARE(f.events("DELETE").size(), 1);
        auto journal = store.deletion(deletionId);
        const auto persistedIntent = journal["items"].toArray()[0].toObject()["progress"].toObject();
        QCOMPARE(persistedIntent["phase"].toString(), QString("marker_intent"));
        QCOMPARE(persistedIntent["target"].toObject(), originalReceipt);
        f.control("/__release", {{"barrier", "history-marker"}}, true);
        PUTGD_TRY_COMPARE(store.detail(uploaded.artifact.id)["remoteStatus"].toString(), QString("removed"));
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
    void failedDeletionCheckpointRemainsRetryableAfterReopen_data() {
        QTest::addColumn<QString>("checkpointPhase");
        QTest::addColumn<int>("initialDeletes");
        QTest::newRow("before-marker") << QString("marker_intent") << 0;
        QTest::newRow("after-marker") << QString("marker_done") << 1;
        QTest::newRow("before-version") << QString("version_intent") << 1;
    }
    void failedDeletionCheckpointRemainsRetryableAfterReopen() {
        QFETCH(QString, checkpointPhase);
        QFETCH(int, initialDeletes);
        S3Fixture f;
        f.reset({{"versioning", "Enabled"}});
        ProviderRegistry registry;
        registry.add("s3", std::make_shared<S3Provider>(f.credentials));
        SettingsService settings(f.directory.filePath("config.json"), [&](auto id, auto value) { return registry.validate(id, value); });
        QVERIFY(settings.save(configuration(f, "rename", true), {}));
        Source source;
        source.payload.kind = "files";
        source.payload.files = {putgd::test::fixture(f.directory.path(), "retry.txt", "synthetic deletion retry")};
        const auto data = f.directory.filePath("data");
        QString deletionId, historyId, localPath, databasePath;
        QJsonObject originalReceipt, failedProgress;
        const auto executeSql = [&](const QString& sql) {
            const auto connection = QString("delete-failure-fixture");
            bool ok = false;
            {
                auto db = QSqlDatabase::addDatabase("QSQLITE", connection);
                db.setDatabaseName(databasePath);
                if (db.open()) {
                    QSqlQuery query(db);
                    ok = query.exec(sql);
                }
            }
            QSqlDatabase::removeDatabase(connection);
            return ok;
        };
        {
            OperationService operations(data, settings, registry, source);
            HistoryService history(operations);
            PUTGD_TRY_VERIFY(operations.ready());
            databasePath = operations.databasePath();
            QString operationId;
            operations.invoke("clipboard", [&](auto error, auto value) { QVERIFY(!error); operationId = value["operationId"].toString(); });
            PUTGD_TRY_VERIFY(!operationId.isEmpty());
            const auto uploaded = waitOperation(operations, operationId).items[0];
            QCOMPARE(uploaded.uploadStatus, QString("uploaded"));
            historyId = uploaded.artifact.id;
            localPath = uploaded.local.path;
            originalReceipt = uploaded.receipt.toJson();
            QVERIFY(executeSql(QString("CREATE TRIGGER fail_marker_checkpoint BEFORE INSERT ON deletions "
                "WHEN json_extract(NEW.data,'$.items[0].status')='deleting' "
                "AND json_extract(NEW.data,'$.items[0].progress.phase')='%1' "
                "BEGIN SELECT RAISE(ABORT,'synthetic journal failure'); END").arg(checkpointPhase)));
            QJsonObject preview;
            history.prepareDelete({historyId}, [&](auto error, auto value) { QVERIFY(!error); preview = value; });
            PUTGD_TRY_VERIFY(!preview.isEmpty());
            history.deleteConfirmed({historyId}, preview["acknowledgement"].toString(), [&](auto error, auto value) {
                QVERIFY(!error); deletionId = value["deleteOperationId"].toString();
            });
            PUTGD_TRY_VERIFY(!deletionId.isEmpty());
            HistoryStore store(databasePath);
            PUTGD_TRY_COMPARE(store.deletion(deletionId)["state"].toString(), QString("completed"));
            const auto item = store.deletion(deletionId)["items"].toArray()[0].toObject();
            QCOMPARE(item["status"].toString(), initialDeletes ? QString("hidden_version_retained") : QString("failed"));
            QVERIFY(!item["error"].toObject()["code"].toString().isEmpty());
            failedProgress = item["progress"].toObject();
            QCOMPARE(f.events("DELETE").size(), initialDeletes);
            if (initialDeletes) QVERIFY(!failedProgress["markerVersionId"].toString().isEmpty());
            QCOMPARE(store.detail(historyId)["receipt"].toObject(), originalReceipt);
            QVERIFY(QFile::exists(localPath));
            qInfo("failed_checkpoint_phase=%s delete_requests=%d", qPrintable(failedProgress.value("phase").toString()), initialDeletes);
            QVERIFY(executeSql("DROP TRIGGER fail_marker_checkpoint"));
        }
        {
            OperationService operations(data, settings, registry, source);
            HistoryService history(operations);
            PUTGD_TRY_VERIFY(operations.ready());
            HistoryStore reopened(operations.databasePath());
            QCOMPARE(reopened.deletion(deletionId)["items"].toArray()[0].toObject()["progress"].toObject(), failedProgress);
            bool accepted = false;
            history.retryDelete(deletionId, [&](auto error, auto value) { QVERIFY(!error); accepted = value["retryAccepted"].toBool(); });
            PUTGD_TRY_VERIFY(accepted);
            PUTGD_TRY_COMPARE(reopened.detail(historyId)["remoteStatus"].toString(), QString("removed"));
            PUTGD_TRY_COMPARE(reopened.deletion(deletionId)["state"].toString(), QString("completed"));
            qInfo("reopened_retry_status=%s delete_requests=%lld", qPrintable(reopened.detail(historyId)["remoteStatus"].toString()), qint64(f.events("DELETE").size()));
            QVERIFY(failedProgress.value("phase").toString() != "marker_intent");
            const auto finalProgress = reopened.deletion(deletionId)["items"].toArray()[0].toObject()["progress"].toObject();
            if (initialDeletes) QCOMPARE(finalProgress["markerVersionId"], failedProgress["markerVersionId"]);
            QCOMPARE(f.events("DELETE").size(), 2);
            QCOMPARE(reopened.detail(historyId)["receipt"].toObject(), originalReceipt);
            QVERIFY(QFile::exists(localPath));
        }
    }
    void cancellationRetainsCommittedArchivesAndSuppressesCopy() {
        S3Fixture f;
        f.reset({{"rules", QJsonArray{QJsonObject{{"method", "PUT"}, {"delay", 0.4}}}}});
        ProviderRegistry registry;
        registry.add("s3", std::make_shared<S3Provider>(f.credentials));
        SettingsService settings(f.directory.filePath("config.json"), [&](auto id, auto value) { return registry.validate(id, value); });
        // One upload at a time: cancelling stops the file still waiting for its slot.
        auto document = configuration(f, "rename", true);
        auto limits = document["limits"].toObject();
        limits["concurrentUploads"] = 1;
        document["limits"] = limits;
        QVERIFY(settings.save(document, {}));
        Source source;
        source.payload.kind = "files";
        source.payload.files = {putgd::test::fixture(f.directory.path(), "a.txt", "a synthetic"), putgd::test::fixture(f.directory.path(), "b.txt", "b synthetic")};
        OperationService operations(f.directory.filePath("data"), settings, registry, source);
        PUTGD_TRY_VERIFY(operations.ready());
        QString id;
        operations.invoke("clipboard", [&](auto error, auto result) { QVERIFY(!error); id = result["operationId"].toString(); });
        PUTGD_TRY_VERIFY(!id.isEmpty());
        PUTGD_TRY_COMPARE(f.events("PUT").size(), 1);
        operations.cancel(id, [](auto error, auto) { QVERIFY(!error); });
        const auto operation = waitOperation(operations, id);
        QCOMPARE(operation.stateName(), QString("cancelled"));
        QCOMPARE(f.events("PUT").size(), 1);
        QCOMPARE(source.copies, 0);
        for (const auto& item : operation.items) {
            QCOMPARE(item.local.status, QString("saved"));
            QVERIFY(QFile::exists(item.local.path));
        }
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
        PUTGD_TRY_VERIFY(operations.ready());
        QString id;
        operations.invoke("clipboard", [&](auto error, auto result) { QVERIFY(!error); id = result["operationId"].toString(); });
        PUTGD_TRY_VERIFY(!id.isEmpty());
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
