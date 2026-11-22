// #1890: an SFTP destination keeps its Password and Key passphrase in one keychain entry.
// Storing one of them alone keeps the other: the saved entry is read first, and an entry that
// can't be read is never overwritten. The keychain is a fake here; nothing reaches the OS store.
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
    std::optional<Credentials> saved;
    int unlocks = 0;
    void unlock(const QString&, const QJsonObject&, std::function<void(Error)> reply) override { ++unlocks; reply(unlockError); }
    void save(const QString&, const QJsonObject&, Credentials value, std::function<void(Error)> reply) override { saved = std::move(value); reply({}); }
};
QJsonObject sftpConfiguration(const QString& root) {
    auto document = test::configuration(root);
    auto settings = sftp::defaults();
    settings["server"] = "files.example.com";
    document["profiles"] = QJsonArray{QJsonObject{{"id", "test"}, {"label", "Synthetic SFTP server"}, {"provider", "sftp"}, {"settings", settings},
                                                  {"credentials", QJsonObject{{"mode", "keychain"}, {"reference", "putgd/sftp/test"}}}}};
    return document;
}
}

class SftpStoreSecrets : public QObject {
    Q_OBJECT
    // Starts storeCredentials with only the password typed and gives the job once it settles.
    void storePasswordOnly(SetupService& setup, SettingsService& settings, QJsonObject& job) {
        QString id;
        setup.start("storeCredentials", "test", settings.revision(), {{"values", QJsonObject{{sftp::passwordKey(), "synthetic-new-password"}}}},
                    [&](Error e, QJsonObject result) { QVERIFY2(!e, qPrintable(e.message)); id = result["setupId"].toString(); });
        PUTGD_TRY_VERIFY((setup.inspect(id, [&](Error, QJsonObject value) { job = value; }), job["state"] != "running"));
    }
private slots:
    void aLockedEntryIsNeverOverwritten() {
        QTemporaryDir dir;
        SettingsService settings(dir.filePath("config.json"), [](auto, auto) { return Error{}; });
        QVERIFY(settings.save(sftpConfiguration(dir.path()), {}));
        FakeKeychain keychain;
        keychain.unlockError = {"credential_access_denied", "Synthetic locked keychain", {}};
        SetupService setup(settings, keychain, {});
        setup.setNamedCredentials(sftp::namedCredentials);
        QJsonObject job;
        storePasswordOnly(setup, settings, job);
        QCOMPARE(job["state"].toString(), QString("failed"));
        QCOMPARE(job["error"].toObject()["code"].toString(), QString("credential_incomplete"));
        QCOMPARE(keychain.unlocks, 1);
        // The saved key passphrase is not replaced by an empty one.
        QVERIFY(!keychain.saved);
    }
    void withNoSavedEntryTheOtherStaysEmpty() {
        QTemporaryDir dir;
        SettingsService settings(dir.filePath("config.json"), [](auto, auto) { return Error{}; });
        QVERIFY(settings.save(sftpConfiguration(dir.path()), {}));
        FakeKeychain keychain;
        keychain.unlockError = {"credential_store_unavailable", "Synthetic missing entry", {{"missing", true}}};
        SetupService setup(settings, keychain, {});
        setup.setNamedCredentials(sftp::namedCredentials);
        QJsonObject job;
        storePasswordOnly(setup, settings, job);
        QCOMPARE(job["state"].toString(), QString("completed"));
        QVERIFY(keychain.saved);
        QCOMPARE(keychain.saved->values.value(sftp::passwordKey()), QString("synthetic-new-password"));
        QCOMPARE(keychain.saved->values.value(sftp::passphraseKey()), QString());
    }
};
QTEST_GUILESS_MAIN(SftpStoreSecrets)
#include "test_sftp_store_secrets.moc"
