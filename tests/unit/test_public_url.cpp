#include "storage/s3/PublicUrl.h"
#include <QTest>
using namespace putgd;
class PublicUrlTest : public QObject {
    Q_OBJECT
private slots:
    void encodedBaseAndKey() {
        QJsonObject settings{{"endpoint", "https://storage.example"}, {"bucket", "bucket"},
                             {"addressing", "path"}, {"publicUrlBase", "https://share.example/base%2Fkeep"}};
        QCOMPARE(publicObjectUrl(settings, "folder/a b/é%?#.png"),
                 QString("https://share.example/base%2Fkeep/folder/a%20b/%C3%A9%25%3F%23.png"));
        settings["publicUrlBase"] = QJsonValue::Null;
        QCOMPARE(publicObjectUrl(settings, "folder/a b"), QString("https://storage.example/bucket/folder/a%20b"));
        settings["addressing"] = "virtual";
        QCOMPARE(publicObjectUrl(settings, "a"), QString("https://bucket.storage.example/a"));
    }
};
QTEST_GUILESS_MAIN(PublicUrlTest)
#include "test_public_url.moc"
