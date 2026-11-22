// Nextcloud and WebDAV address and settings rules (#1872). A wrong answer here sends files to
// the wrong place or a password over plain HTTP, and nothing else would notice.
#include "storage/dav/DavCommon.h"
#include <QTest>
using namespace putgd;
class DavRulesTest final : public QObject {
    Q_OBJECT
private slots:
    void pastedAddressesReduceToTheServer() {
        QCOMPARE(dav::nextcloudServer("cloud.example.com"), QStringLiteral("https://cloud.example.com"));
        QCOMPARE(dav::nextcloudServer(" https://Cloud.Example.com/ "), QStringLiteral("https://cloud.example.com"));
        QCOMPARE(dav::nextcloudServer("https://cloud.example.com/nextcloud/index.php/apps/files/?dir=/Photos&fileid=4"), QStringLiteral("https://cloud.example.com/nextcloud"));
        QCOMPARE(dav::nextcloudServer("https://cloud.example.com:8443/s/AbCdEf1234"), QStringLiteral("https://cloud.example.com:8443"));
        QCOMPARE(dav::nextcloudServer("https://u:p@cloud.example.com/remote.php/dav/files/me/"), QStringLiteral("https://cloud.example.com"));
        QCOMPARE(dav::nextcloudServer("http://192.168.1.5/nc/index.php/login"), QStringLiteral("http://192.168.1.5/nc"));
        QCOMPARE(dav::nextcloudServer("ftp://cloud.example.com"), QString());
        QCOMPARE(dav::collectionAddress("dav.example.com/uploads"), QStringLiteral("https://dav.example.com/uploads/"));
    }
    void plainHttpOnlyWhereAllowed() {
        auto s = dav::nextcloudDefaults();
        QVERIFY(!dav::validateSettings(dav::nextcloudId, s));
        s["server"] = "http://cloud.example.com";
        QCOMPARE(dav::validateSettings(dav::nextcloudId, s).code, QStringLiteral("webdav_settings_invalid"));
        s["allowInsecureHttp"] = true;
        QVERIFY(!dav::validateSettings(dav::nextcloudId, s));
        s["allowInsecureHttp"] = false;
        s["server"] = "http://127.0.0.1:8080";
        QVERIFY(!dav::validateSettings(dav::nextcloudId, s));
        s["server"] = "https://cloud.example.com/index.php";
        QVERIFY(dav::validateSettings(dav::nextcloudId, s));
        s["server"] = "https://cloud.example.com";
        s["folder"] = "/put.gd/";
        QVERIFY(dav::validateSettings(dav::nextcloudId, s));
        auto w = dav::webdavDefaults();
        w["address"] = "https://dav.example.com/up/";
        w["linkBase"] = "http://files.example.com/up/";
        QVERIFY(dav::validateSettings(dav::webdavId, w));
        w["linkBase"] = "https://files.example.com/up/";
        QVERIFY(!dav::validateSettings(dav::webdavId, w));
    }
    void urlsEncodeEachSegment() {
        auto s = dav::nextcloudDefaults();
        s["server"] = "https://cloud.example.com/nc";
        s["folder"] = "Screen shots/put.gd";
        QCOMPARE(dav::fileUrl(s, dav::nextcloudId, "a@b.example", "Shot #1 ?.png").toString(QUrl::FullyEncoded),
                 QStringLiteral("https://cloud.example.com/nc/remote.php/dav/files/a%40b.example/Screen%20shots/put.gd/Shot%20%231%20%3F.png"));
        auto w = dav::webdavDefaults();
        w["address"] = "https://dav.example.com/up/";
        QCOMPARE(dav::webdavLink(w, "a b.png"), QStringLiteral("https://dav.example.com/up/a%20b.png"));
        w["linkBase"] = "https://files.example.com/p/";
        QCOMPARE(dav::webdavLink(w, "a b.png"), QStringLiteral("https://files.example.com/p/a%20b.png"));
    }
    void shareTokensAreRedacted() {
        QCOMPARE(dav::shareToken("https://cloud.example.com/nc/s/AbC123xyz"), QStringLiteral("AbC123xyz"));
        QCOMPARE(dav::redactShareTokens("link https://cloud.example.com/s/AbC123xyz/download ok"), QStringLiteral("link https://cloud.example.com/s/…/download ok"));
    }
};
QTEST_GUILESS_MAIN(DavRulesTest)
#include "test_dav_rules.moc"
