// #1890: an SFTP destination keeps its Password and Key passphrase in one keychain entry.
// Storing (or clearing) one of them keeps the other: the saved entry is read first, and an
// entry that can't be read is never overwritten. The keychain is a fake here; nothing reaches
// the OS store.
#include "app/SetupService.h"
#include "fixtures/U2Fixtures.h"
#include "storage/sftp/SftpCommon.h"
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QtTest>
#include "fixtures/Waits.h"
using namespace putgd;

namespace {
class FakeKeychain final : public CredentialStore {
public:
    Error unlockError;
    std::optional<Credentials> stored;  // what a successful read finds
    bool holdUnlock = false;
    std::function<void(Error)> heldReply;
    int unlocks = 0, saves = 0;
    std::optional<Credentials> saved;
    void unlock(const QString& profile, const QJsonObject& reference, std::function<void(Error)> reply) override {
        ++unlocks;
        if (holdUnlock) { heldReply = std::move(reply); return; }
        if (stored) { rememberRead(profile, reference, *stored); reply({}); return; }
        reply(unlockError);
    }
    // As the real store does, what was written is what is loaded from then on.
    void save(const QString& profile, const QJsonObject& reference, Credentials value, std::function<void(Error)> reply) override {
        ++saves;
        saved = value;
        rememberRead(profile, reference, std::move(value));
        reply({});
    }
};
QJsonObject sftpProfile(const QString& id, const QString& reference, const QString& mode = "keychain") {
    auto settings = sftp::defaults();
    settings["server"] = "files.example.com";
    return {{"id", id}, {"label", "Synthetic SFTP server " + id}, {"provider", "sftp"}, {"settings", settings},
            {"credentials", mode == "keychain" ? QJsonObject{{"mode", "keychain"}, {"reference", reference}} : QJsonObject{{"mode", "session"}, {"reference", QJsonValue::Null}}}};
}
Credentials both(const QString& password, const QString& passphrase) {
    Credentials c;
    c.values.insert(sftp::passwordKey(), password);
    c.values.insert(sftp::passphraseKey(), passphrase);
    return c;
}
struct Rig {
    QTemporaryDir dir;
    SettingsService settings{dir.filePath("config.json"), [](auto, auto) { return Error{}; }};
    FakeKeychain keychain;
    DestinationStatus status{dir.filePath("destination-status.json"), settings};
    SetupService setup{settings, keychain, {}};
    explicit Rig(QJsonArray profiles = {sftpProfile("test", "putgd/sftp/test")}) {
        use(profiles);
        setup.setNamedCredentials(sftp::namedCredentials);
        setup.setDestinationStatus(&status);
    }
    void use(const QJsonArray& profiles) {
        auto document = test::configuration(dir.path());
        document["profiles"] = profiles;
        if (const auto saved = settings.save(document, settings.revision()); !saved) qFatal("synthetic configuration refused: %s", qPrintable(saved.error.code + " " + saved.error.message + " " + QJsonDocument(saved.error.details).toJson(QJsonDocument::Compact)));
    }
    const QJsonObject keychain_reference() const { return {{"mode", "keychain"}, {"reference", "putgd/sftp/test"}}; }
};
const Error notFound{"credential_store_unavailable", "Synthetic entry that reads as missing", {{"missing", true}}};
}

class SftpStoreSecrets : public QObject {
    Q_OBJECT
    // Stores `values` for `profile` and gives the job once it settles.
    void store(Rig& rig, const QJsonObject& values, QJsonObject& job, const QString& profile = "test") {
        QString id;
        rig.setup.start("storeCredentials", profile, rig.settings.revision(), {{"values", values}},
                        [&](Error e, QJsonObject result) { QVERIFY2(!e, qPrintable(e.message)); id = result["setupId"].toString(); });
        QVERIFY(!id.isEmpty());
        PUTGD_TRY_VERIFY((rig.setup.inspect(id, [&](Error, QJsonObject value) { job = value; }), job["state"] != "running"));
    }
private slots:
    void aLockedEntryIsNeverOverwritten() {
        Rig rig;
        rig.keychain.unlockError = {"credential_access_denied", "Synthetic locked keychain", {}};
        QJsonObject job;
        store(rig, {{sftp::passwordKey(), "synthetic-new-password"}}, job);
        QCOMPARE(job["state"].toString(), QString("failed"));
        QCOMPARE(job["error"].toObject()["code"].toString(), QString("credential_incomplete"));
        QCOMPARE(rig.keychain.unlocks, 1);
        QCOMPARE(rig.keychain.saves, 0);
    }
    // Store and Clear each change only the field given; the other keeps its saved value.
    void storeAndClearChangeOnlyTheirField() {
        Rig rig;
        rig.keychain.stored = both("synthetic-saved-password", "synthetic-saved-passphrase");
        QJsonObject job;
        store(rig, {{sftp::passwordKey(), "synthetic-new-password"}}, job);
        QCOMPARE(job["state"].toString(), QString("completed"));
        QCOMPARE(rig.keychain.saved->values, both("synthetic-new-password", "synthetic-saved-passphrase").values);
        store(rig, {{sftp::passphraseKey(), QString()}}, job);
        QCOMPARE(job["state"].toString(), QString("completed"));
        QCOMPARE(rig.keychain.saved->values, both("synthetic-new-password", QString()).values);
    }
    void aNewDestinationStoresOneSecret() {
        Rig rig;
        rig.keychain.unlockError = {"credential_store_unavailable", "Synthetic missing entry", {{"missing", true}}};
        QJsonObject job;
        store(rig, {{sftp::passwordKey(), "synthetic-new-password"}}, job);
        QCOMPARE(job["state"].toString(), QString("completed"));
        QCOMPARE(rig.keychain.saved->values, both("synthetic-new-password", QString()).values);
    }
    // A locked entry can read as "not found"; once put.gd saved to this entry, that isn't trusted.
    void notFoundAfterASaveIsRefused() {
        Rig rig;
        rig.keychain.unlockError = notFound;
        QJsonObject job;
        store(rig, {{sftp::passwordKey(), "synthetic-first-password"}}, job);
        QCOMPARE(job["state"].toString(), QString("completed"));
        rig.keychain.forget("test", rig.keychain_reference());  // as after a restart: nothing loaded
        store(rig, {{sftp::passwordKey(), "synthetic-new-password"}}, job);
        QCOMPARE(job["state"].toString(), QString("failed"));
        QCOMPARE(job["error"].toObject()["code"].toString(), QString("credential_incomplete"));
        QCOMPARE(rig.keychain.saves, 1);
        // Both secrets typed: nothing to keep, so it stores.
        store(rig, {{sftp::passwordKey(), "synthetic-new-password"}, {sftp::passphraseKey(), "synthetic-new-passphrase"}}, job);
        QCOMPARE(job["state"].toString(), QString("completed"));
    }
    // A status file from before #1890 has no saved-entry list: its keychain entries count as
    // saved, so a locked one that reads as "not found" is refused, and its records are kept.
    void aPrePatchStatusFileCountsEntriesAsSaved() {
        Rig rig;
        {
            QFile file(rig.dir.filePath("destination-status.json"));
            QVERIFY(file.open(QIODevice::WriteOnly));
            const QJsonObject record{{"state", "passed"}, {"source", "test"}, {"fingerprint", QString(64, 'a')}, {"credentialRevision", 3}};
            file.write(QJsonDocument(QJsonObject{{"schemaVersion", 1}, {"destinations", QJsonObject{{"test", record}}}, {"credentialRevisions", QJsonObject{{"test", 3}}}})
                           .toJson(QJsonDocument::Compact));
        }
        rig.status.load();
        QVERIFY(rig.status.keychainSaved("putgd/sftp/test"));
        rig.keychain.unlockError = notFound;
        QJsonObject job;
        store(rig, {{sftp::passwordKey(), "synthetic-new-password"}}, job);
        QCOMPARE(job["state"].toString(), QString("failed"));
        QCOMPARE(job["error"].toObject()["code"].toString(), QString("credential_incomplete"));
        QCOMPARE(rig.keychain.saves, 0);
        QFile file(rig.dir.filePath("destination-status.json"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const auto saved = QJsonDocument::fromJson(file.readAll()).object();
        QVERIFY(saved["keychainEntries"].toArray().contains(QJsonValue("putgd/sftp/test")));
        QVERIFY(saved["destinations"].toObject().contains("test"));
    }
    // An install from before #1890 that saved to putgd/<id> and then switched to session mode
    // (a null reference) still has that entry marked after the upgrade.
    void anUpgradeMarksEntriesOfSessionDestinations() {
        Rig rig({sftpProfile("test", {}, "session")});
        {
            QFile file(rig.dir.filePath("destination-status.json"));
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write(QJsonDocument(QJsonObject{{"schemaVersion", 1}, {"destinations", QJsonObject{}}, {"credentialRevisions", QJsonObject{{"test", 2}}}}).toJson(QJsonDocument::Compact));
        }
        rig.status.load();
        QVERIFY(rig.status.keychainSaved("putgd/test"));
        rig.use({sftpProfile("test", "putgd/test")});
        rig.keychain.unlockError = notFound;
        QJsonObject job;
        store(rig, {{sftp::passwordKey(), "synthetic-new-password"}}, job);
        QCOMPARE(job["state"].toString(), QString("failed"));
        QCOMPARE(rig.keychain.saves, 0);
    }
    // A destination that never stored credentials (revision 0) isn't marked by the upgrade, so its
    // first password-only store after switching to the keychain goes through.
    void anUpgradeLeavesNeverStoredDestinationsUnmarked() {
        Rig rig({sftpProfile("test", {}, "session")});
        {
            QFile file(rig.dir.filePath("destination-status.json"));
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write(QJsonDocument(QJsonObject{{"schemaVersion", 1}, {"destinations", QJsonObject{}}, {"credentialRevisions", QJsonObject{}}}).toJson(QJsonDocument::Compact));
        }
        rig.status.load();
        QVERIFY(!rig.status.keychainSaved("putgd/test"));
        rig.use({sftpProfile("test", "putgd/test")});
        rig.keychain.unlockError = notFound;
        QJsonObject job;
        store(rig, {{sftp::passwordKey(), "synthetic-first-password"}}, job);
        QCOMPARE(job["state"].toString(), QString("completed"));
        QCOMPARE(rig.keychain.saved->values, both("synthetic-first-password", QString()).values);
    }
    // An unlock holds the entry like a store, so a read begun before a store can't finish after it.
    void anUnlockHoldsTheEntry() {
        Rig rig;
        rig.keychain.holdUnlock = true;
        QString unlock;
        rig.setup.start("unlockCredentials", "test", rig.settings.revision(), {}, [&](Error e, QJsonObject result) { QVERIFY(!e); unlock = result["setupId"].toString(); });
        QVERIFY(rig.keychain.heldReply);
        Error busy;
        rig.setup.start("storeCredentials", "test", rig.settings.revision(), {{"values", QJsonObject{{sftp::passwordKey(), "synthetic-a"}}}}, [&](Error e, QJsonObject) { busy = e; });
        QCOMPARE(busy.code, QString("setup_busy"));
        auto reply = std::move(rig.keychain.heldReply);
        reply({"credential_access_denied", "Synthetic locked keychain", {}});
        QJsonObject job;
        PUTGD_TRY_VERIFY((rig.setup.inspect(unlock, [&](Error, QJsonObject value) { job = value; }), job["state"] == "failed"));
        rig.keychain.holdUnlock = false;
        rig.keychain.unlockError = notFound;
        store(rig, {{sftp::passwordKey(), "synthetic-a"}}, job);
        QCOMPARE(job["state"].toString(), QString("completed"));
    }
    // Keychain, then session, then keychain again keeps the entry marked as saved. Settings
    // writes putgd/<id> when it switches a destination to the keychain.
    void aModeRoundTripKeepsTheMark() {
        Rig rig({sftpProfile("test", "putgd/test")});
        rig.keychain.unlockError = notFound;
        QJsonObject job;
        store(rig, {{sftp::passwordKey(), "synthetic-first-password"}}, job);
        QCOMPARE(job["state"].toString(), QString("completed"));
        rig.use({sftpProfile("test", {}, "session")});
        rig.status.prune();
        rig.use({sftpProfile("test", "putgd/test")});
        rig.status.prune();
        QVERIFY(rig.status.keychainSaved("putgd/test"));
        rig.keychain.forget("test", QJsonObject{{"mode", "keychain"}, {"reference", "putgd/test"}});
        store(rig, {{sftp::passwordKey(), "synthetic-new-password"}}, job);
        QCOMPARE(job["state"].toString(), QString("failed"));
        QCOMPARE(rig.keychain.saves, 1);
    }
    // Only a successful keychain save counts: a session store, or a failed first store, doesn't.
    void sessionStoresAndFailedStoresDontCount() {
        Rig rig({sftpProfile("test", {}, "session")});
        QJsonObject job;
        store(rig, {{sftp::passwordKey(), "synthetic-session-password"}}, job);
        QCOMPARE(job["state"].toString(), QString("completed"));
        rig.use({sftpProfile("test", "putgd/sftp/test")});
        rig.keychain.unlockError = {"credential_access_denied", "Synthetic locked keychain", {}};
        store(rig, {{sftp::passwordKey(), "synthetic-new-password"}}, job);
        QCOMPARE(job["state"].toString(), QString("failed"));
        QVERIFY(rig.status.credentialRevision("test") > 0);
        rig.keychain.unlockError = notFound;
        store(rig, {{sftp::passwordKey(), "synthetic-new-password"}}, job);
        QCOMPARE(job["state"].toString(), QString("completed"));
        QCOMPARE(rig.keychain.saved->values, both("synthetic-new-password", QString()).values);
    }
    // setup.cancel while the entry is being read: nothing is written, and the entry is free again.
    void aCancelWhileReadingWritesNothing() {
        Rig rig;
        rig.keychain.holdUnlock = true;
        QString id;
        rig.setup.start("storeCredentials", "test", rig.settings.revision(), {{"values", QJsonObject{{sftp::passwordKey(), "synthetic-a"}}}},
                        [&](Error e, QJsonObject result) { QVERIFY(!e); id = result["setupId"].toString(); });
        QVERIFY(rig.keychain.heldReply);
        bool requested = false;
        rig.setup.cancel(id, [&](Error e, QJsonObject result) { QVERIFY(!e); requested = result["cancelRequested"].toBool(); });
        QVERIFY(requested);
        auto reply = std::move(rig.keychain.heldReply);
        reply(notFound);
        QJsonObject job;
        PUTGD_TRY_VERIFY((rig.setup.inspect(id, [&](Error, QJsonObject value) { job = value; }), job["state"] != "running"));
        QCOMPARE(job["error"].toObject()["code"].toString(), QString("cancelled"));
        QCOMPARE(rig.keychain.saves, 0);
        rig.keychain.holdUnlock = false;
        rig.keychain.unlockError = notFound;
        store(rig, {{sftp::passwordKey(), "synthetic-b"}}, job);
        QCOMPARE(job["state"].toString(), QString("completed"));
    }
    // Two destinations sharing one keychain entry store one at a time.
    void oneStorePerSharedEntry() {
        Rig rig({sftpProfile("test", "putgd/sftp/shared"), sftpProfile("other", "putgd/sftp/shared")});
        rig.keychain.holdUnlock = true;
        QString first;
        rig.setup.start("storeCredentials", "test", rig.settings.revision(), {{"values", QJsonObject{{sftp::passwordKey(), "synthetic-a"}}}},
                        [&](Error e, QJsonObject result) { QVERIFY(!e); first = result["setupId"].toString(); });
        QVERIFY(rig.keychain.heldReply);
        Error busy;
        const auto other = [&] {
            busy = {};
            rig.setup.start("storeCredentials", "other", rig.settings.revision(), {{"values", QJsonObject{{sftp::passwordKey(), "synthetic-b"}}}}, [&](Error e, QJsonObject) { busy = e; });
        };
        other();
        QCOMPARE(busy.code, QString("setup_busy"));
        auto reply = std::move(rig.keychain.heldReply);
        reply({"credential_access_denied", "Synthetic locked keychain", {}});
        QJsonObject job;
        PUTGD_TRY_VERIFY((rig.setup.inspect(first, [&](Error, QJsonObject value) { job = value; }), job["state"] == "failed"));
        // Once the first store is over, the entry is free again.
        other();
        QVERIFY2(busy.code.isEmpty(), qPrintable(busy.code + ": " + busy.message));
    }
};
QTEST_GUILESS_MAIN(SftpStoreSecrets)
#include "test_sftp_store_secrets.moc"
