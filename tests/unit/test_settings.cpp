#include "core/SettingsService.h"
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLockFile>
#include <QSaveFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

class SettingsContract final : public QObject {
    Q_OBJECT
    static void replace(const QString& path, const QByteArray& bytes) {
        QSaveFile file(path);
        file.setDirectWriteFallback(false);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(bytes), bytes.size());
        QVERIFY(file.commit());
    }
    static QByteArray read(const QString& path) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) return {};
        return file.readAll();
    }
private slots:
    void asynchronousWritesAndTemplateRemainExplicit() {
        QTemporaryDir dir;putgd::SettingsService service(dir.filePath("config.json"));
        const auto templ=putgd::Settings::developmentTemplate();QVERIFY(!templ["profiles"].toArray().isEmpty());QVERIFY(service.document()["profiles"].toArray().isEmpty());
        bool saved=false;service.saveAsync(putgd::Settings::defaults(),{},[&](putgd::Result result){QVERIFY(result);saved=true;});QTRY_VERIFY(saved);const auto revision=service.revision();QVERIFY(!revision.isEmpty());
        bool patched=false;service.patchAsync({{"preferences",QJsonObject{{"notifications",false}}}},revision,[&](putgd::Result result){QVERIFY(result);patched=true;});QTRY_VERIFY(patched);QVERIFY(!service.snapshot().notifications);
        bool conflict=false;service.patchAsync({},revision,[&](putgd::Result result){QCOMPARE(result.error.code,QString("config_conflict"));conflict=true;});QTRY_VERIFY(conflict);QVERIFY(!service.snapshot().notifications);
        bool rejected=false;service.saveAsync(templ,service.revision(),[&](putgd::Result result){QCOMPARE(result.error.code,QString("config_invalid"));rejected=true;});QTRY_VERIFY(rejected);QVERIFY(service.document()["profiles"].toArray().isEmpty());
    }
    void freshStartupDoesNotSelectDevelopmentDestination() {
        QTemporaryDir dir;
        const auto path = dir.filePath(QStringLiteral("config.json"));
        putgd::SettingsService service(path);
        QVERIFY(service.revision().isEmpty());
        QVERIFY(!QFile::exists(path));
        QVERIFY(service.document()[QStringLiteral("profiles")].toArray().isEmpty());
        QVERIFY(service.document()[QStringLiteral("actions")].toObject()[QStringLiteral("region")]
                    .toObject()[QStringLiteral("profileId")].isNull());
    }
    void atomicSaveAndConflict() {
        QTemporaryDir dir;
        const auto path = dir.filePath(QStringLiteral("config.json"));
        putgd::SettingsService service(path);
        auto doc = putgd::SettingsService::defaults();
        QVERIFY(service.save(doc, QString{}));
        const auto first = service.revision();
        QVERIFY(!first.isEmpty());
        auto preferences = doc[QStringLiteral("preferences")].toObject();
        preferences[QStringLiteral("notifications")] = false;
        doc[QStringLiteral("preferences")] = preferences;
        QVERIFY(service.save(doc, first));
        QVERIFY(service.revision() != first);
        const auto bytes = read(path);
        const auto result = service.save(putgd::SettingsService::defaults(), first);
        QCOMPARE(result.error.code, QStringLiteral("config_conflict"));
        QCOMPARE(read(path), bytes);
#ifdef Q_OS_UNIX
        const auto permissions = QFile::permissions(path);
        QVERIFY(!(permissions & (QFile::ReadGroup | QFile::WriteGroup | QFile::ReadOther | QFile::WriteOther)));
#endif
    }
    void invalidEditRetainsAcceptedSettingsAndBadFile() {
        QTemporaryDir dir;
        const auto path = dir.filePath(QStringLiteral("config.json"));
        putgd::SettingsService service(path);
        QVERIFY(service.save(putgd::SettingsService::defaults(), QString{}));
        const auto revision = service.revision();
        const auto accepted = service.document();
        const QByteArray invalid("{broken");
        replace(path, invalid);
        QVERIFY(!service.reload());
        QCOMPARE(service.revision(), revision);
        QCOMPARE(service.document(), accepted);
        QCOMPARE(read(path), invalid);
        QVERIFY(!service.metadata()[QStringLiteral("validation")].toObject()[QStringLiteral("ok")].toBool());
    }
    void repeatedExternalReplacementsAreAccepted() {
        QTemporaryDir dir;
        const auto path = dir.filePath(QStringLiteral("config.json"));
        putgd::SettingsService service(path);
        auto doc = putgd::SettingsService::defaults();
        QVERIFY(service.save(doc, QString{}));
        for (int i = 0; i < 3; ++i) {
            const auto prior = service.revision();
            auto preferences = doc[QStringLiteral("preferences")].toObject();
            preferences[QStringLiteral("notifications")] = (i % 2 == 1);
            doc[QStringLiteral("preferences")] = preferences;
            replace(path, QJsonDocument(doc).toJson());
            QTRY_VERIFY_WITH_TIMEOUT(service.revision() != prior, 4000);
            QCOMPARE(service.document(), doc);
        }
    }
    void strictValidation() {
        QTemporaryDir dir;
        putgd::SettingsService service(dir.filePath(QStringLiteral("config.json")));
        auto doc = putgd::SettingsService::defaults();
        doc[QStringLiteral("schemaVersion")] = 99;
        QCOMPARE(service.save(doc, QString{}).error.code, QStringLiteral("config_invalid"));
        doc = putgd::SettingsService::defaults();
        doc[QStringLiteral("accessKey")] = QStringLiteral("synthetic-secret");
        auto result = service.save(doc, QString{});
        QCOMPARE(result.error.code, QStringLiteral("config_invalid"));
        QVERIFY(!result.error.message.contains(QStringLiteral("synthetic-secret")));
        doc = putgd::SettingsService::defaults();
        auto actions = doc[QStringLiteral("actions")].toObject();
        auto region = actions[QStringLiteral("region")].toObject();
        region[QStringLiteral("profileId")] = QStringLiteral("missing-profile");
        actions[QStringLiteral("region")] = region;
        doc[QStringLiteral("actions")] = actions;
        QCOMPARE(service.save(doc, QString{}).error.code, QStringLiteral("config_invalid"));
        QVERIFY(!QFile::exists(service.path()));
    }
    void patchCanClearOptionalFolderOverride() {
        QTemporaryDir dir;
        putgd::SettingsService service(dir.filePath(QStringLiteral("config.json")));
        auto doc = putgd::SettingsService::defaults();
        auto archive = doc[QStringLiteral("archive")].toObject();
        auto folders = archive[QStringLiteral("folders")].toObject();
        folders[QStringLiteral("images")] = dir.filePath(QStringLiteral("image-copies"));
        archive[QStringLiteral("folders")] = folders;
        doc[QStringLiteral("archive")] = archive;
        QVERIFY(service.save(doc, QString{}));
        QVERIFY(service.patch({{"archive", QJsonObject{{"folders", QJsonObject{{"images", QJsonValue::Null}}}}}}, service.revision()));
        QVERIFY(service.document()["archive"].toObject()["folders"].toObject()["images"].isNull());
        QCOMPARE(service.document()["archive"].toObject()["root"], archive["root"]);
    }
    void lockContentionDoesNotOverwrite() {
        QTemporaryDir dir;
        const auto path = dir.filePath("config.json");
        putgd::SettingsService service(path);
        QVERIFY(service.save(putgd::SettingsService::defaults(), QString{}));
        const auto bytes = read(path);
        QLockFile lock(path + ".lock");
        QVERIFY(lock.tryLock());
        QCOMPARE(service.save(putgd::SettingsService::defaults(), service.revision()).error.code, QStringLiteral("config_busy"));
        QCOMPARE(read(path), bytes);
    }
    void missingAndOversizedFilesPreserveAcceptedState() {
        QTemporaryDir dir;
        const auto path = dir.filePath("config.json");
        putgd::SettingsService service(path);
        QVERIFY(service.save(putgd::SettingsService::defaults(), QString{}));
        const auto revision = service.revision();
        const auto accepted = service.document();
        QVERIFY(QFile::remove(path));
        QCOMPARE(service.reload().error.code, QStringLiteral("config_missing"));
        QCOMPARE(service.revision(), revision);
        QCOMPARE(service.document(), accepted);
        const QByteArray excessive(1024 * 1024 + 1, ' ');
        replace(path, excessive);
        QCOMPARE(service.reload().error.code, QStringLiteral("config_too_large"));
        QCOMPARE(service.document(), accepted);
        QCOMPARE(read(path), excessive);
    }
    void fallbackDiscoversNewParentDirectory() {
        QTemporaryDir dir;
        const auto parent = dir.filePath("new-parent");
        const auto path = parent + "/config.json";
        putgd::SettingsService service(path);
        QVERIFY(!service.metadata()["watching"].toBool());
        QVERIFY(QDir().mkpath(parent));
        const auto doc = putgd::SettingsService::defaults();
        replace(path, QJsonDocument(doc).toJson());
        QTRY_VERIFY_WITH_TIMEOUT(!service.revision().isEmpty(), 5000);
        QCOMPARE(service.document(), doc);
        QVERIFY(service.metadata()["watching"].toBool());
    }
    void ownWritesDoNotCauseRepeatedAcceptance() {
        QTemporaryDir dir;
        putgd::SettingsService service(dir.filePath("config.json"));
        QSignalSpy accepted(&service, &putgd::SettingsService::accepted);
        QVERIFY(service.save(putgd::SettingsService::defaults(), QString{}));
        QCOMPARE(accepted.count(), 1);
        QTest::qWait(250);
        QCOMPARE(accepted.count(), 1);
    }
    void malformedTypesAndIncompleteActionsAreRejected() {
        QTemporaryDir dir;
        putgd::SettingsService service(dir.filePath("config.json"));
        auto doc = putgd::SettingsService::defaults();
        auto limits = doc["limits"].toObject();
        limits["maxFiles"] = QStringLiteral("100");
        doc["limits"] = limits;
        QCOMPARE(service.save(doc, QString{}).error.code, QStringLiteral("config_invalid"));
        doc = putgd::SettingsService::defaults();
        auto actions = doc["actions"].toObject();
        actions.remove("clipboard");
        doc["actions"] = actions;
        QCOMPARE(service.save(doc, QString{}).error.code, QStringLiteral("config_invalid"));
    }
#ifdef Q_OS_UNIX
    void unwritableParentIsReportedWithoutChangingFile() {
        QTemporaryDir dir;
        const auto path = dir.filePath("config.json");
        putgd::SettingsService service(path);
        QVERIFY(service.save(putgd::SettingsService::defaults(), QString{}));
        const auto bytes = read(path);
        const auto originalPermissions = QFile::permissions(dir.path());
        QVERIFY(QFile::setPermissions(dir.path(), QFile::ReadOwner | QFile::ExeOwner));
        const auto result = service.save(putgd::SettingsService::defaults(), service.revision());
        const auto unchanged = read(path);
        QVERIFY(QFile::setPermissions(dir.path(), originalPermissions));
        QCOMPARE(result.error.code, QStringLiteral("config_unwritable"));
        QCOMPARE(unchanged, bytes);
    }
#endif
};
QTEST_GUILESS_MAIN(SettingsContract)
#include "test_settings.moc"
