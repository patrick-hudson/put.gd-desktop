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
    // The SDK drops empty path segments, so the stored key has none (#1582).
    void storedKeysDropEmptySegments() {
        QCOMPARE(s3StoredKey("a//b/p.txt"), QString("a/b/p.txt"));
        QCOMPARE(s3StoredKey("/lead/p.txt"), QString("lead/p.txt"));
        QCOMPARE(s3StoredKey("dir//"), QString("dir"));
        QCOMPARE(s3StoredKey("p.txt"), QString("p.txt"));
        for (const auto& [prefix, key] : std::initializer_list<std::pair<const char*, const char*>>{
                 {"", "p.txt"}, {"/", "p.txt"}, {"//", "p.txt"}, {"dir", "dir/p.txt"}, {"dir/", "dir/p.txt"},
                 {"dir//", "dir/p.txt"}, {"a//b/", "a/b/p.txt"}, {"/lead", "lead/p.txt"}, {"/a//b//", "a/b/p.txt"}})
            QCOMPARE(s3ObjectKey(QJsonObject{{"objectPrefix", prefix}}, "p.txt"), QString(key));
        QVERIFY(s3KeyAddressable("a/b.c/..d/p.txt"));
        for (const auto* key : {"./p.txt", "../p.txt", "a/./b/p.txt", "a/../b/p.txt", "a/.", "a/.."})
            QVERIFY2(!s3KeyAddressable(key), key);
        const QJsonObject settings{{"endpoint", "https://storage.example"}, {"bucket", "bucket"}, {"addressing", "path"},
                                   {"publicUrlBase", QJsonValue::Null}};
        QCOMPARE(publicObjectUrl(settings, "/a//b/p.txt"), QString("https://storage.example/bucket/a/b/p.txt"));
        QCOMPARE(s3LegacyPublicObjectUrl(settings, "/a//b/p.txt"), QString("https://storage.example/bucket//a//b/p.txt"));
    }
};
QTEST_GUILESS_MAIN(PublicUrlTest)
#include "test_public_url.moc"
