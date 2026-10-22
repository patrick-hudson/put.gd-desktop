#include "secrets/CredentialStore.h"
#include <QTest>
#include <QDBusConnection>
#include <QDBusContext>
#include <QTemporaryDir>
#include <QDirIterator>
#include <QFile>
#include <QJsonDocument>
#include "fixtures/Waits.h"
using namespace putgd;
// A real QtKeychain job speaks its KWallet DBus protocol on an isolated bus.
class WalletFixture : public QObject, protected QDBusContext {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.kde.KWallet")
public:
    QString mode;
    QByteArray stored;
    int opens = 0;
    int writes = 0;
public slots:
    QString networkWallet() { return "isolated-synthetic-wallet"; }
    int open(const QString&, qlonglong, const QString&) {
        ++opens;
        if (mode == "denied") {
            sendErrorReply("org.freedesktop.DBus.Error.AccessDenied", "synthetic-secret must not appear in diagnostics");
            return -1;
        }
        return mode == "locked" ? -1 : 7;
    }
    int writeEntry(int, const QString&, const QString&, const QByteArray& value, const QString&) {
        ++writes;
        stored = value;
        return 0;
    }
    int entryType(int, const QString&, const QString&, const QString&) { return 2; }
    QByteArray readEntry(int, const QString&, const QString&, const QString&) { return stored; }
};
class KeychainBackendTest : public QObject {
    Q_OBJECT
    WalletFixture wallet;
    QTemporaryDir directory;
private slots:
    void initTestCase() {
        qputenv("XDG_CURRENT_DESKTOP", "KDE");
        qputenv("KDE_SESSION_VERSION", "4");
        qputenv("XDG_CONFIG_HOME", directory.path().toUtf8());
        qputenv("XDG_DATA_HOME", directory.path().toUtf8());
        QVERIFY(QDBusConnection::sessionBus().registerService("org.kde.kwalletd"));
        QVERIFY(QDBusConnection::sessionBus().registerObject("/modules/kwalletd", &wallet, QDBusConnection::ExportAllSlots));
    }
    void lockedOrDeniedDoesNotPersist_data() {
        QTest::addColumn<QString>("mode");
        QTest::newRow("locked-unlock-refused") << QString("locked");
        QTest::newRow("dbus-access-denied") << QString("denied");
    }
    void lockedOrDeniedDoesNotPersist() {
        QFETCH(QString, mode);
        wallet.mode = mode;
        wallet.stored.clear();
        const auto writesBefore = wallet.writes;
        const auto opensBefore = wallet.opens;
        CredentialStore store;
        QJsonObject reference{{"mode", "keychain"}, {"reference", "synthetic-reference"}};
        bool done = false;
        Error error;
        store.save("profile", reference, {"synthetic-key", "synthetic-secret", {}}, [&](auto result) { error = result; done = true; });
        PUTGD_TRY_VERIFY(done);
        QVERIFY(error);
        QVERIFY(!store.cached("profile", reference));
        QVERIFY(!QJsonDocument(error.toJson()).toJson().contains("synthetic-secret"));
        QCOMPARE(wallet.writes, writesBefore);
        done = false;
        store.unlock("profile", reference, [&](auto result) { error = result; done = true; });
        PUTGD_TRY_VERIFY(done);
        QVERIFY(error);
        QVERIFY(!store.cached("profile", reference));
        QVERIFY(wallet.opens >= opensBefore + 2);
        QDirIterator files(directory.path(), QDir::Files, QDirIterator::Subdirectories);
        while (files.hasNext()) {
            QFile file(files.next());
            QVERIFY(file.open(QIODevice::ReadOnly));
            QVERIFY(!file.readAll().contains("synthetic-secret"));
        }
    }
    void explicitUnlockedRoundtrip() {
        wallet.mode = "ready";
        CredentialStore store;
        QJsonObject reference{{"mode", "keychain"}, {"reference", "synthetic-reference"}};
        bool done = false;
        Error error;
        store.save("profile", reference, {"synthetic-key", "synthetic-secret", "synthetic-token"}, [&](auto result) { error = result; done = true; });
        PUTGD_TRY_VERIFY(done);
        QVERIFY2(!error, qPrintable(error.code));
        QVERIFY(store.cached("profile", reference));
        store.forget("profile", reference);
        QVERIFY(!store.cached("profile", reference));
        done = false;
        store.unlock("profile", reference, [&](auto result) { error = result; done = true; });
        PUTGD_TRY_VERIFY(done);
        QVERIFY2(!error, qPrintable(error.code));
        QCOMPARE(store.cached("profile", reference)->sessionToken, QString("synthetic-token"));
    }
};
QTEST_GUILESS_MAIN(KeychainBackendTest)
#include "test_keychain_backend.moc"
