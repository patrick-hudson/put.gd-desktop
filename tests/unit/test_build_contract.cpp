#include "storage/s3/SdkRuntime.h"
#include <qt6keychain/keychain.h>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTest>

class BuildContract final : public QObject {
    Q_OBJECT
private slots:
    void sqliteTransactions() {
        {
            auto db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("contract"));
            db.setDatabaseName(QStringLiteral(":memory:"));
            QVERIFY2(db.open(), "Install the Qt SQLite driver (libqt6sql6-sqlite on Ubuntu/Mint)");
            QSqlQuery query(db);
            QVERIFY(query.exec(QStringLiteral("CREATE TABLE history(id INTEGER PRIMARY KEY, value TEXT)")));
            QVERIFY(db.transaction());
            QVERIFY(query.exec(QStringLiteral("INSERT INTO history VALUES(1, 'synthetic')")));
            QVERIFY(db.rollback());
            QVERIFY(query.exec(QStringLiteral("SELECT COUNT(*) FROM history")));
            QVERIFY(query.next());
            QCOMPARE(query.value(0).toInt(), 0);
        }
        QSqlDatabase::removeDatabase(QStringLiteral("contract"));
    }
    void keychainAndSdkLink() {
        // No secret-store access, credentials, or network traffic in this contract test.
        QKeychain::ReadPasswordJob job(QStringLiteral("putgd-build-contract"));
        job.setInsecureFallback(false);
        QVERIFY(!job.insecureFallback());
        putgd::SdkRuntime sdk;
    }
};
QTEST_MAIN(BuildContract)
#include "test_build_contract.moc"
