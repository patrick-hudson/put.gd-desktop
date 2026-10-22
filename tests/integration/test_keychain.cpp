#include "secrets/CredentialStore.h"
#include <QTest>
#include <QTemporaryDir>
#include <QDirIterator>
#include <QFile>
#include <QProcess>
#include <QJsonDocument>
#include "fixtures/Waits.h"
using namespace putgd;
class KeychainTest : public QObject {
    Q_OBJECT
private slots:
    void sessionReadinessIsExplicitAndForgetRevokesCache() {
        CredentialStore store;
        QJsonObject session{{"mode", "session"}, {"reference", QJsonValue::Null}};
        QVERIFY(!store.cached("profile", session));
        store.setSession("profile", {"synthetic-key", "synthetic-secret", "synthetic-token"});
        auto value = store.cached("profile", session);
        QVERIFY(value.has_value());
        QCOMPARE(value->accessKey, QString("synthetic-key"));
        const auto generation = value->generation;
        store.setSession("profile", {"replacement-key", "replacement-secret", {}});
        QVERIFY(store.cached("profile", session)->generation > generation);
        QVERIFY(!store.cached("other-profile", session));
        QVERIFY(!store.cached("profile", {}));
        QVERIFY(!store.cached("profile", {{"mode", "unknown"}, {"reference", QJsonValue::Null}}));
        store.forget("profile", session);
        QVERIFY(!store.cached("profile", session));
    }
    void unavailableStoreDoesNotFallBackOrCache() {
        QTemporaryDir root;
        qputenv("XDG_CONFIG_HOME", root.path().toUtf8());
        qputenv("XDG_DATA_HOME", root.path().toUtf8());
        qputenv("DBUS_SESSION_BUS_ADDRESS", "unix:path=/nonexistent-putgd-synthetic-bus");
        CredentialStore store;
        QJsonObject reference{{"mode", "keychain"}, {"reference", "putgd-synthetic-no-store"}};
        bool done = false;
        Error error;
        store.save("profile", reference, {"synthetic-key", "synthetic-secret", {}}, [&](auto value) { error = value; done = true; });
        PUTGD_TRY_VERIFY(done);
        QVERIFY(error);
        QVERIFY(!store.cached("profile", reference));
        QVERIFY(!QJsonDocument(error.toJson()).toJson().contains("synthetic-secret"));
        done = false;
        store.unlock("profile", reference, [&](auto value) { error = value; done = true; });
        PUTGD_TRY_VERIFY(done);
        QVERIFY(error);
        QVERIFY(!store.cached("profile", reference));
        QDirIterator files(root.path(), QDir::Files, QDirIterator::Subdirectories);
        while (files.hasNext()) {
            QFile file(files.next());
            QVERIFY(file.open(QIODevice::ReadOnly));
            QVERIFY(!file.readAll().contains("synthetic-secret"));
        }
    }
};
QTEST_GUILESS_MAIN(KeychainTest)
#include "test_keychain.moc"
