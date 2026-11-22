// #1890: an SFTP destination keeps its Password and Key passphrase in one keychain entry.
// Storing (or clearing) one of them keeps the other: the saved entry is read first, and an
// entry that can't be read is never overwritten. The keychain is a fake here; nothing reaches
// the OS store.
#include "app/SetupService.h"
#include "fixtures/U2Fixtures.h"
#include "storage/sftp/SftpCommon.h"
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
QJsonObject sftpProfile(const QString& id, const QString& reference) {
    auto settings = sftp::defaults();
    settings["server"] = "files.example.com";
    return {{"id", id}, {"label", "Synthetic SFTP server " + id}, {"provider", "sftp"}, {"settings", settings},
            {"credentials", QJsonObject{{"mode", "keychain"}, {"reference", reference}}}};
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
        auto document = test::configuration(dir.path());
        document["profiles"] = profiles;
        if (!settings.save(document, {})) qFatal("synthetic configuration refused");
        setup.setNamedCredentials(sftp::namedCredentials);
        setup.setDestinationStatus(&status);
    }
};
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
    // A locked entry can read as "not found"; once put.gd stored credentials here, that isn't trusted.
    void notFoundAfterAStoreIsRefused() {
        Rig rig;
        rig.status.credentialsChanged("test");
        rig.keychain.unlockError = {"credential_store_unavailable", "Synthetic entry that reads as missing", {{"missing", true}}};
        QJsonObject job;
        store(rig, {{sftp::passwordKey(), "synthetic-new-password"}}, job);
        QCOMPARE(job["state"].toString(), QString("failed"));
        QCOMPARE(job["error"].toObject()["code"].toString(), QString("credential_incomplete"));
        QCOMPARE(rig.keychain.saves, 0);
        // Both secrets typed: nothing to keep, so it stores.
        store(rig, {{sftp::passwordKey(), "synthetic-new-password"}, {sftp::passphraseKey(), "synthetic-new-passphrase"}}, job);
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
