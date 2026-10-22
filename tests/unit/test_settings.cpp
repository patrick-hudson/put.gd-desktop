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
#include "fixtures/Waits.h"

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
    void startupUnlockAndRecentColorPreferencesAreOptionalAndBounded() {
        auto doc=putgd::Settings::developmentTemplate();auto validator=[](const QString&,const QJsonObject&){return putgd::Error{};};
        QVERIFY(!putgd::Settings::validate(doc,validator));
        const auto defaults=putgd::Settings::fromValidated(doc);QVERIFY(defaults.unlockCredentialsAtStartup);QVERIFY(defaults.editorRecentColors.isEmpty());
        auto preferences=doc["preferences"].toObject();
        preferences["unlockCredentialsAtStartup"]=false;preferences["editorRecentColors"]=QJsonArray{"#a1b2c3","#000000"};doc["preferences"]=preferences;
        QVERIFY(!putgd::Settings::validate(doc,validator));
        const auto chosen=putgd::Settings::fromValidated(doc);QVERIFY(!chosen.unlockCredentialsAtStartup);QCOMPARE(chosen.editorRecentColors,QStringList({"#a1b2c3","#000000"}));
        const QList<QJsonValue> invalid{QJsonArray{"#A1B2C3"},QJsonArray{"red"},QJsonArray{1},QJsonArray{"#1","#2"},QJsonValue("#a1b2c3")};
        for(const auto& colors:invalid){preferences["editorRecentColors"]=colors;doc["preferences"]=preferences;QCOMPARE(putgd::Settings::validate(doc,validator).code,QString("config_invalid"));}
        QJsonArray seven;for(int i=0;i<7;++i)seven.append(QString("#00000%1").arg(i));preferences["editorRecentColors"]=seven;doc["preferences"]=preferences;QVERIFY(putgd::Settings::validate(doc,validator));
        preferences.remove("editorRecentColors");preferences["unlockCredentialsAtStartup"]="yes";doc["preferences"]=preferences;QVERIFY(putgd::Settings::validate(doc,validator));
    }
    // #293: no updates object means a daily check on this build's channel.
    void updatePreferencesAreOptionalWithKnownChannels() {
        auto doc=putgd::Settings::developmentTemplate();auto validator=[](const QString&,const QJsonObject&){return putgd::Error{};};
        const auto defaults=putgd::Settings::fromValidated(doc);QVERIFY(defaults.checkForUpdates);QCOMPARE(defaults.updateChannel,QString("build"));
        auto preferences=doc["preferences"].toObject();
        for(const auto& updates:{QJsonObject{},QJsonObject{{"check",false}},QJsonObject{{"channel","nightly"}},QJsonObject{{"check",true},{"channel","stable"}}}){
            preferences["updates"]=updates;doc["preferences"]=preferences;QVERIFY(!putgd::Settings::validate(doc,validator));
        }
        preferences["updates"]=QJsonObject{{"check",false},{"channel","nightly"}};doc["preferences"]=preferences;
        const auto chosen=putgd::Settings::fromValidated(doc);QVERIFY(!chosen.checkForUpdates);QCOMPARE(chosen.updateChannel,QString("nightly"));
        for(const auto& invalid:QList<QJsonValue>{QJsonObject{{"check","yes"}},QJsonObject{{"channel","beta"}},QJsonObject{{"channel","Stable"}},QJsonObject{{"interval",1}},QJsonValue(true)}){
            preferences["updates"]=invalid;doc["preferences"]=preferences;QCOMPARE(putgd::Settings::validate(doc,validator).code,QString("config_invalid"));
        }
    }
    // #60: an older config without appearance loads as "system"; the accent and
    // overrides keep their shape but stay at the built-in values until phase 2.
    void appearanceThemeIsOptionalAndCustomColoursAreReserved() {
        auto doc=putgd::Settings::developmentTemplate();auto validator=[](const QString&,const QJsonObject&){return putgd::Error{};};
        QVERIFY(!doc["preferences"].toObject().contains("appearance"));QCOMPARE(putgd::Settings::fromValidated(doc).appearanceTheme,QString("system"));
        auto preferences=doc["preferences"].toObject();
        for(const auto* theme:{"system","dark","light"}){preferences["appearance"]=QJsonObject{{"theme",theme}};doc["preferences"]=preferences;QVERIFY(!putgd::Settings::validate(doc,validator));QCOMPARE(putgd::Settings::fromValidated(doc).appearanceTheme,QString(theme));}
        preferences["appearance"]=QJsonObject{{"theme","light"},{"accent",QJsonValue::Null},{"overrides",QJsonObject{{"dark",QJsonObject{}},{"light",QJsonObject{}}}}};doc["preferences"]=preferences;QVERIFY(!putgd::Settings::validate(doc,validator));
        const QList<QJsonObject> invalid{{},{{"theme","auto"}},{{"theme",true}},{{"theme","dark"},{"accent","#123456"}},{{"theme","dark"},{"overrides",QJsonObject{{"dark",QJsonObject{{"accent","#123456"}}},{"light",QJsonObject{}}}}},
            {{"theme","dark"},{"overrides",QJsonObject{{"dark",QJsonObject{}}}}},{{"theme","dark"},{"colour","red"}}};
        for(const auto& appearance:invalid){preferences["appearance"]=appearance;doc["preferences"]=preferences;const auto error=putgd::Settings::validate(doc,validator);QCOMPARE(error.code,QString("config_invalid"));QCOMPARE(error.details["path"].toString(),QString("/preferences/appearance"));}
        preferences["appearance"]="dark";doc["preferences"]=preferences;QVERIFY(putgd::Settings::validate(doc,validator));
    }
    void captureDefaultFollowsFutureChangesWithoutChangingExplicitExceptions() {
        auto doc=putgd::Settings::developmentTemplate();
        doc["captureDefaults"]=QJsonObject{{"destination","upload"},{"profileId","development-s3"},{"publicationMode","private"}};
        auto actions=doc["actions"].toObject();
        for(const auto& name:putgd::Settings::actionNames()){auto action=actions[name].toObject();action["destination"]="default";action["profileId"]=QJsonValue::Null;actions[name]=action;}
        auto clipboard=actions["clipboard"].toObject();clipboard["destination"]="local";actions["clipboard"]=clipboard;doc["actions"]=actions;
        auto validator=[](const QString&,const QJsonObject&){return putgd::Error{};};
        QVERIFY(!putgd::Settings::validate(doc,validator));const auto accepted=putgd::Settings::fromValidated(doc);
        QCOMPARE(accepted.actions["region"].profileId,QString("development-s3"));QCOMPARE(accepted.actions["region"].publicationMode,QString("private"));
        doc["captureDefaults"]=QJsonObject{{"destination","local"},{"profileId",QJsonValue::Null},{"publicationMode","default"}};
        QVERIFY(!putgd::Settings::validate(doc,validator));const auto changed=putgd::Settings::fromValidated(doc);
        for(const auto& name:putgd::Settings::actionNames())QCOMPARE(changed.actions[name].destination,QString("local"));
        QCOMPARE(accepted.actions["region"].destination,QString("upload"));QCOMPARE(changed.document["actions"].toObject()["clipboard"].toObject()["destination"].toString(),QString("local"));
        doc.remove("captureDefaults");QVERIFY(putgd::Settings::validate(doc,validator));
    }

    void pendingImageBudgetIsOptionalAndValidated() {
        auto document=putgd::Settings::defaults();
        auto limits=document["limits"].toObject();
        limits["pendingImageBytes"]=67108864.0;
        document["limits"]=limits;
        QVERIFY(!putgd::Settings::validate(document));
        QCOMPARE(putgd::Settings::fromValidated(document).limits.pendingImageBytes,qint64(67108864));
        for (const auto value : QJsonArray{0, -1, "67108864", 1.5, 1099511627777.0}) {
            limits["pendingImageBytes"]=value;document["limits"]=limits;
            QCOMPARE(putgd::Settings::validate(document).code,QString("config_invalid"));
        }
        limits.remove("pendingImageBytes");document["limits"]=limits;
        QVERIFY(!putgd::Settings::validate(document));
        QCOMPARE(putgd::Settings::fromValidated(document).limits.pendingImageBytes,qint64(67108864));
        QCOMPARE(putgd::Settings::fromValidated(document).document["limits"].toObject(),limits);
    }
    void artifactByteLimitsDefaultToAutoAndAcceptNoLimit() {
        constexpr qint64 gib = 1LL << 30;
        using putgd::Settings;
        using putgd::ResourceLimits;
        // "auto" sets no byte limit of put.gd's own on any computer: memory does not
        // grow with file size, and free disk and the destination bound every import.
        auto document = Settings::defaults();
        for (const auto& key : Settings::artifactByteLimitKeys()) QCOMPARE(document["limits"].toObject()[key], QJsonValue("auto"));
        QVERIFY(!Settings::validate(document));
        auto resolved = Settings::fromValidated(document);
        QCOMPARE(resolved.limits.maxFileBytes, ResourceLimits::unlimitedBytes);
        QCOMPARE(resolved.limits.maxBatchBytes, ResourceLimits::unlimitedBytes);
        QCOMPARE(resolved.limits.pendingArtifactBytes, ResourceLimits::unlimitedBytes);
        const auto effective = resolved.effectiveArtifactLimits();
        QCOMPARE(effective.keys(), QStringList({"maxBatchBytes", "maxFileBytes", "pendingArtifactBytes"}));
        for (const auto& key : Settings::artifactByteLimitKeys()) QVERIFY(effective[key].isNull());
        // Null is No limit, a byte count stays fixed, and the old 2 GiB default reads as auto.
        auto limits = document["limits"].toObject();
        limits["maxFileBytes"] = QJsonValue::Null;
        limits["maxBatchBytes"] = 3221225472.0;
        limits["pendingArtifactBytes"] = 2147483648.0;
        document["limits"] = limits;
        QVERIFY(!Settings::validate(document));
        resolved = Settings::fromValidated(document);
        QCOMPARE(resolved.limits.maxFileBytes, ResourceLimits::unlimitedBytes);
        QCOMPARE(resolved.limits.maxBatchBytes, 3 * gib);
        QCOMPARE(resolved.limits.pendingArtifactBytes, ResourceLimits::unlimitedBytes);
        QCOMPARE(resolved.effectiveArtifactLimits()["maxBatchBytes"].toInteger(), 3 * gib);
        QVERIFY(resolved.document["limits"].toObject()["maxFileBytes"].isNull());
        QCOMPARE(resolved.document["limits"].toObject()["pendingArtifactBytes"], QJsonValue("auto"));
        QCOMPARE(resolved.document["limits"].toObject()["maxBatchBytes"].toInteger(), 3 * gib);
        for (const auto& value : {QJsonValue("Auto"), QJsonValue("none"), QJsonValue(0), QJsonValue(-1), QJsonValue(1.5), QJsonValue(1099511627777.0), QJsonValue(true)}) {
            limits["maxFileBytes"] = value;
            document["limits"] = limits;
            const auto error = Settings::validate(document);
            QCOMPARE(error.code, QStringLiteral("config_invalid"));
            QCOMPARE(error.details["path"].toString(), QStringLiteral("/limits/maxFileBytes"));
        }
    }
    void existingConfigWithTheFormerDefaultLoadsAsAuto() {
        QTemporaryDir directory;
        const auto path = directory.filePath("config.json");
        auto legacy = putgd::Settings::defaults();
        auto limits = legacy["limits"].toObject();
        for (const auto& key : putgd::Settings::artifactByteLimitKeys()) limits[key] = 2147483648.0;
        legacy["limits"] = limits;
        replace(path, QJsonDocument(legacy).toJson());
        putgd::SettingsService service(path);
        QVERIFY(!service.reload().error);
        for (const auto& key : putgd::Settings::artifactByteLimitKeys()) QCOMPARE(service.document()["limits"].toObject()[key], QJsonValue("auto"));
        // The next save writes the migrated value.
        QVERIFY(!service.patch({{"retention", QJsonObject{{"recentActivityLimit", 50}}}}, service.metadata()["observedRevision"].toString()).error);
        const auto saved = QJsonDocument::fromJson(read(path)).object();
        QCOMPARE(saved["limits"].toObject()["maxFileBytes"], QJsonValue("auto"));
    }
    void filenameTemplatesAreOptionalAndLegacyChoicesRemainUnchanged() {
        auto document = putgd::Settings::defaults();
        QCOMPARE(document["naming"].toObject()["mode"].toString(), QString("original"));
        const QJsonObject legacyNaming{{"mode", "generated"}, {"collisionPolicy", "skip"}};
        document["naming"] = legacyNaming;
        QVERIFY(!putgd::Settings::validate(document));
        QCOMPARE(putgd::Settings::fromValidated(document).document["naming"].toObject(), legacyNaming);
        QCOMPARE(putgd::Settings::fromValidated(document).captureTemplate,QString("Screen-Capture-{yyyy}-{MM}-{dd}-{HH}-{mm}-{ss}.{ext}"));
        QCOMPARE(putgd::Settings::fromValidated(document).fileTemplate,QString("{name}.{ext}"));
        auto naming = legacyNaming;
        naming["mode"] = "template";
        naming["fileTemplate"] = "{name}-{yyyy}-{MM}-{dd}-{HH}-{mm}-{ss}-{uuid}.{ext}";
        naming["captureTemplate"] = "Screen-Capture-{yyyy}-{MM}-{dd}-{HH}-{mm}-{ss}.{ext}";
        naming["imageTemplate"] = "Image-{uuid}.{ext}";
        naming["textTemplate"] = "Text-{uuid}.{ext}";
        document["naming"] = naming;
        QVERIFY(!putgd::Settings::validate(document));
        QCOMPARE(putgd::Settings::fromValidated(document).document["naming"].toObject(), naming);
        for (const auto& value : QStringList{"{unknown}.png", "{yyyy.png", "../{name}.{ext}", "{name}/x", "{name}\\x", "", ".", ".."}) {
            naming["captureTemplate"] = value;
            document["naming"] = naming;
            QCOMPARE(putgd::Settings::validate(document).code, QString("config_invalid"));
        }
    }
    void asynchronousWritesAndTemplateRemainExplicit() {
        QTemporaryDir dir;putgd::SettingsService service(dir.filePath("config.json"));
        const auto templ=putgd::Settings::developmentTemplate();QVERIFY(!templ["profiles"].toArray().isEmpty());QVERIFY(service.document()["profiles"].toArray().isEmpty());
        bool saved=false;service.saveAsync(putgd::Settings::defaults(),{},[&](putgd::Result result){QVERIFY(result);saved=true;});PUTGD_TRY_VERIFY(saved);const auto revision=service.revision();QVERIFY(!revision.isEmpty());
        bool patched=false;service.patchAsync({{"preferences",QJsonObject{{"notifications",false}}}},revision,[&](putgd::Result result){QVERIFY(result);patched=true;});PUTGD_TRY_VERIFY(patched);QVERIFY(!service.snapshot().notifications);
        bool conflict=false;service.patchAsync({},revision,[&](putgd::Result result){QCOMPARE(result.error.code,QString("config_conflict"));conflict=true;});PUTGD_TRY_VERIFY(conflict);QVERIFY(!service.snapshot().notifications);
        bool rejected=false;service.saveAsync(templ,service.revision(),[&](putgd::Result result){QCOMPARE(result.error.code,QString("config_invalid"));rejected=true;});PUTGD_TRY_VERIFY(rejected);QVERIFY(service.document()["profiles"].toArray().isEmpty());
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
            PUTGD_TRY_VERIFY_WITH_TIMEOUT(service.revision() != prior, 4000);
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
        PUTGD_TRY_VERIFY_WITH_TIMEOUT(!service.revision().isEmpty(), 5000);
        QCOMPARE(service.document(), doc);
        QVERIFY(service.metadata()["watching"].toBool());
    }
    // With no configuration folder to watch, the 2 s fallback rescans; an unchanged
    // rescan must not report a status change, or every open view reloads (#234).
    void unchangedRescansStaySilent() {
        QTemporaryDir dir;
        const auto parent = dir.filePath("not-yet-created");
        putgd::SettingsService service(parent + "/config.json");
        QSignalSpy status(&service, &putgd::SettingsService::statusChanged);
        QVERIFY(!service.metadata()["watching"].toBool());
        putgd::test::settle(4500);
        PUTGD_TRY_COMPARE(status.count(), 0);
        QCOMPARE(service.reload().error.code, QStringLiteral("config_missing"));
        PUTGD_TRY_COMPARE(status.count(), 0);
        // A real change still reports: the folder and file appear.
        QVERIFY(QDir().mkpath(parent));
        replace(parent + "/config.json", QJsonDocument(putgd::SettingsService::defaults()).toJson());
        PUTGD_TRY_VERIFY_WITH_TIMEOUT(status.count() >= 1, 5000);
        const auto settled = status.count();
        QVERIFY(service.metadata()["watching"].toBool());
        putgd::test::settle(500);
        PUTGD_TRY_COMPARE(status.count(), settled);
        // So does a rejected edit, once, and its repair.
        replace(parent + "/config.json", "{");
        PUTGD_TRY_COMPARE_WITH_TIMEOUT(service.metadata()["validation"].toObject()["ok"].toBool(), false, 5000);
        QCOMPARE(status.count(), settled + 1);
        QCOMPARE(service.reload().error.code, QStringLiteral("config_invalid"));
        QCOMPARE(status.count(), settled + 1);
    }
    void ownWritesDoNotCauseRepeatedAcceptance() {
        QTemporaryDir dir;
        putgd::SettingsService service(dir.filePath("config.json"));
        QSignalSpy accepted(&service, &putgd::SettingsService::accepted);
        QVERIFY(service.save(putgd::SettingsService::defaults(), QString{}));
        QCOMPARE(accepted.count(), 1);
        putgd::test::settle(250);
        PUTGD_TRY_COMPARE(accepted.count(), 1);
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
    void absentRecordActionInheritsTheRegionRouteWithoutAShortcut() {
        auto document = putgd::Settings::defaults();
        auto actions = document["actions"].toObject();
        auto region = actions["region"].toObject();
        region["destination"] = "local";
        actions["region"] = region;
        actions.remove("edit");
        actions.remove("record");
        document["actions"] = actions;
        QVERIFY(!putgd::Settings::validate(document));
        const auto normalized = putgd::Settings::normalized(document);
        const auto edit = normalized["actions"].toObject()["edit"].toObject();
        QCOMPARE(edit["destination"].toString(), QStringLiteral("local"));
        QVERIFY(edit["shortcut"].toString().isEmpty());
        // Record stays a shortcut-only entry so it keeps following the region route.
        const auto record = normalized["actions"].toObject()["record"].toObject();
        QCOMPARE(record.keys(), QStringList{"shortcut"});
        QVERIFY(!putgd::Settings::validate(normalized));
        const auto settings = putgd::Settings::fromValidated(document);
        QCOMPARE(settings.actions.value("record").destination, QStringLiteral("local"));
        QVERIFY(settings.actions.value("record").shortcut.isEmpty());
        QCOMPARE(putgd::Settings::invocableActionNames().last(), QStringLiteral("record"));
        // A document that binds the recording key keeps it; it never inherits the region key.
        auto bound = normalized;
        auto boundActions = bound["actions"].toObject();
        auto keyed = boundActions["record"].toObject();
        keyed["shortcut"] = "Ctrl+Shift+9";
        boundActions["record"] = keyed;
        bound["actions"] = boundActions;
        QVERIFY(!putgd::Settings::validate(bound));
        QCOMPARE(putgd::Settings::fromValidated(bound).actions.value("record").shortcut, QStringLiteral("Ctrl+Shift+9"));
        QCOMPARE(putgd::Settings::fromValidated(bound).actions.value("region").shortcut, QStringLiteral("Ctrl+Shift+1"));
    }
    // validate() once read each action through a reference into a destroyed
    // temporary object, and the Debian 13 resident crashed saving this document
    // on its worker thread. Debug builds read the dead object by luck, so
    // json_ref_lifetime_contract rejects that source pattern itself.
    void localActionsAndRecordEntriesValidateOnTheWorkerThread() {
        auto document = putgd::Settings::defaults();
        auto actions = document["actions"].toObject();
        for (const auto& name : putgd::Settings::actionNames()) {
            auto action = actions[name].toObject();
            action["destination"] = "local";
            actions[name] = action;
        }
        document["actions"] = actions;
        document["captureDefaults"] = QJsonObject{{"destination", "local"}, {"profileId", QJsonValue::Null}, {"publicationMode", "default"}};
        auto archive = document["archive"].toObject();
        archive["keepUploads"] = true;
        document["archive"] = archive;
        QTemporaryDir dir;
        putgd::SettingsService service(dir.filePath("config.json"));
        bool saved = false;
        service.saveAsync(document, {}, [&](putgd::Result result) { QVERIFY(result); saved = true; });
        PUTGD_TRY_VERIFY(saved);
        QCOMPARE(service.snapshot().actions.value("record").destination, QStringLiteral("local"));
        auto withRecord = [&](const QJsonValue& record) {
            auto candidate = document;
            auto routes = candidate["actions"].toObject();
            routes["record"] = record;
            candidate["actions"] = routes;
            return putgd::Settings::validate(candidate);
        };
        QVERIFY(!withRecord(QJsonObject{{"shortcut", "Ctrl+Shift+7"}}));
        QVERIFY(!withRecord(QJsonObject{{"destination", "default"}, {"profileId", QJsonValue::Null}, {"shortcut", ""}}));
        QVERIFY(!withRecord(QJsonObject{{"destination", "upload"}, {"profileId", QJsonValue::Null}, {"shortcut", ""}, {"publicationMode", "private"}}));
        for (const auto& invalid : {QJsonValue("local"), QJsonValue(QJsonObject{{"shortcut", 7}}),
                 QJsonValue(QJsonObject{{"destination", "local"}, {"shortcut", ""}}),
                 QJsonValue(QJsonObject{{"destination", "default"}, {"profileId", "missing"}, {"shortcut", ""}})}) {
            const auto error = withRecord(invalid);
            QCOMPARE(error.code, QStringLiteral("config_invalid"));
            QCOMPARE(error.details["path"].toString(), QStringLiteral("/actions/record"));
        }
        const auto badKey = withRecord(QJsonObject{{"shortcut", "Ctrl+Shift+Nope"}});
        QCOMPARE(badKey.details["path"].toString(), QStringLiteral("/actions/record/shortcut"));
    }
};
QTEST_GUILESS_MAIN(SettingsContract)
#include "test_settings.moc"
