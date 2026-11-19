#include "storage/s3/R2Settings.h"
#include "storage/s3/R2Provider.h"
#include "storage/s3/PublicUrl.h"
#include "secrets/CredentialStore.h"
#include <QTest>
using namespace putgd;
// Cloudflare R2 (#1678): every upload goes to the account endpoint the settings name, in their
// bucket. A wrong host or bucket here would put files somewhere else without an error.
class R2SettingsTest : public QObject {
    Q_OBJECT
    static constexpr auto account = "0123456789abcdef0123456789abcdef";
    QJsonObject settings(const QString& jurisdiction) const {
        auto value = r2::defaults();
        value["accountId"] = account;
        value["jurisdiction"] = jurisdiction;
        value["bucket"] = "lab-bucket";
        return value;
    }
private slots:
    void requestsGoToTheAccountEndpointAndBucket() {
        for (const auto& [jurisdiction, host] : std::initializer_list<std::pair<const char*, const char*>>{
                 {"default", "0123456789abcdef0123456789abcdef.r2.cloudflarestorage.com"},
                 {"eu", "0123456789abcdef0123456789abcdef.eu.r2.cloudflarestorage.com"},
                 {"fedramp", "0123456789abcdef0123456789abcdef.fedramp.r2.cloudflarestorage.com"}}) {
            const auto s3 = r2::s3Settings(settings(jurisdiction));
            QCOMPARE(s3["endpoint"].toString(), "https://" + QString(host));
            QCOMPARE(s3["bucket"].toString(), QString("lab-bucket"));
            QCOMPARE(s3["region"].toString(), QString("auto"));
            QCOMPARE(s3["addressing"].toString(), QString("path"));
            QCOMPARE(s3["aclMode"].toString(), QString("omit"));
            // #1682's versioning fallback and #1688's signing recognize it as R2.
            QVERIFY(s3CloudflareR2Endpoint(s3["endpoint"].toString()));
            QCOMPARE(publicObjectUrl(s3, "folder/p.png"), "https://" + QString(host) + "/lab-bucket/folder/p.png");
        }
        // The S3 worker takes only provider "s3"; the R2 destination reaches it converted.
        const Destination destination{r2::providerId, "r2", "revision", settings("eu"), QJsonObject{{"mode", "session"}, {"reference", QJsonValue::Null}}};
        const auto s3 = R2Provider::s3Destination(destination);
        QCOMPARE(s3.providerId, QString("s3"));
        QCOMPARE(s3.profileId, QString("r2"));
        QCOMPARE(s3.settings, r2::s3Settings(destination.settings));
        QVERIFY(!r2::validate(settings("default")));
    }
    void pastedAddressIsReadOnlyWhenEveryPartIsValid() {
        const auto read = [](const char* text) { return r2::parseS3ApiAddress(text); };
        QCOMPARE(read("https://0123456789ABCDEF0123456789ABCDEF.eu.r2.cloudflarestorage.com/lab-bucket")->accountId, QString(account));
        QCOMPARE(read("https://0123456789abcdef0123456789abcdef.eu.r2.cloudflarestorage.com/lab-bucket")->jurisdiction, QString("eu"));
        QCOMPARE(read(" https://0123456789abcdef0123456789abcdef.r2.cloudflarestorage.com/lab-bucket/ ")->bucket, QString("lab-bucket"));
        QCOMPARE(read("https://0123456789abcdef0123456789abcdef.fedramp.r2.cloudflarestorage.com")->bucket, QString());
        for (const auto* text : {"http://0123456789abcdef0123456789abcdef.r2.cloudflarestorage.com/lab-bucket",
                                 "https://0123456789abcdef0123456789abcdef.r2.cloudflarestorage.com.example/lab-bucket",
                                 "https://storage.example/0123456789abcdef0123456789abcdef.r2.cloudflarestorage.com",
                                 "https://0123456789abcdef0123456789abcde.r2.cloudflarestorage.com/lab-bucket",
                                 "https://0123456789abcdef0123456789abcdef.us.r2.cloudflarestorage.com/lab-bucket",
                                 "https://0123456789abcdef0123456789abcdef.r2.cloudflarestorage.com:443/lab-bucket",
                                 "https://user@0123456789abcdef0123456789abcdef.r2.cloudflarestorage.com/lab-bucket",
                                 "https://0123456789abcdef0123456789abcdef.r2.cloudflarestorage.com/lab%2Dbucket",
                                 "https://0123456789abcdef0123456789abcdef.r2.cloudflarestorage.com/Lab-Bucket",
                                 "https://0123456789abcdef0123456789abcdef.r2.cloudflarestorage.com/lab-bucket/folder",
                                 "https://0123456789abcdef0123456789abcdef.r2.cloudflarestorage.com/lab-bucket?x=1",
                                 "https://0123456789abcdef0123456789abcdef.r2.cloudflarestorage.com/lab-bucket#x"})
            QVERIFY2(!read(text), text);
    }
    // A malformed account, jurisdiction or bucket, saved or recorded, refuses before any request is
    // built from it: it never becomes another host or bucket (#1699 A1, A2).
    void malformedSettingsRefuseBeforeAnyRequest() {
        for (const auto& [key, value] : std::initializer_list<std::pair<const char*, const char*>>{
                 {"accountId", "0123456789abcdef0123456789abcdef\n"}, {"accountId", "evil.example/0123456789abcdef0123456"},
                 {"bucket", "lab-bucket\n"}, {"bucket", "lab-bucket/../other"}, {"jurisdiction", "eu.evil.example"}}) {
            auto bad = settings("default");
            bad[key] = value;
            QVERIFY2(r2::validate(bad), key);
            CredentialStore secrets;
            R2Provider provider(std::make_shared<S3Provider>(secrets));
            const Destination destination{r2::providerId, "r2", "revision", bad, QJsonObject{{"mode", "session"}, {"reference", QJsonValue::Null}}};
            StorageReceipt receipt;
            receipt.destination = destination;
            UploadRequest request;
            request.destination = destination;
            QString seen;
            provider.upload(request, [&](UploadResult result) { seen += result.status + ":" + result.error.code + ";"; });
            provider.reconcile(request, [&](StatResult result) { seen += result.status + ":" + result.error.code + ";"; });
            provider.stat(receipt, [&](StatResult result) { seen += result.status + ":" + result.error.code + ";"; });
            provider.refreshLink(receipt, [&](StatResult result) { seen += result.status + ":" + result.error.code + ";"; });
            provider.remove(DeleteRequest{"d", receipt, {}, {}}, [](auto, auto ack) { ack({}); }, [&](DeleteResult result) { seen += result.status + ":" + result.error.code + ";"; });
            provider.abandonUpload(destination, {}, [&](Error error) { seen += "abandon:" + error.code + ";"; });
            QCOMPARE(seen, QString("failed:s3_settings_invalid;unknown:s3_settings_invalid;unknown:s3_settings_invalid;unknown:s3_settings_invalid;"
                                   "failed:s3_settings_invalid;abandon:s3_settings_invalid;"));
        }
    }
    // A public R2 upload with no Link domain would copy an S3 API link R2 never serves: it is refused
    // before anything is sent (#1699 P3).
    void publicUploadWithoutLinkDomainRefusesFirst() {
        auto value = settings("default");
        value["linkMode"] = "public";
        CredentialStore secrets;
        R2Provider provider(std::make_shared<S3Provider>(secrets));
        UploadRequest request;
        request.destination = Destination{r2::providerId, "r2", "revision", value, QJsonObject{{"mode", "session"}, {"reference", QJsonValue::Null}}};
        QString seen;
        provider.upload(request, [&](UploadResult result) { seen = result.status + ":" + result.error.code + ":" + result.error.details["field"].toString(); });
        QCOMPARE(seen, QString("failed:s3_settings_invalid:publicUrlBase"));
    }
    // An upload through a moved Cloudflare R2 destination and one through the old S3 destination on
    // the same endpoint name the same object (#1699 A4); signed R2 requests never follow a redirect (A3).
    void movedDestinationSharesIdentityAndFollowsNoRedirect() {
        CredentialStore secrets;
        auto s3 = std::make_shared<S3Provider>(secrets);
        R2Provider r2(s3);
        const Destination moved{r2::providerId, "lab", "revision", settings("eu"), {}};
        auto old = R2Provider::s3Destination(moved);
        old.settings["endpoint"] = "HTTPS://0123456789ABCDEF0123456789ABCDEF.eu.r2.cloudflarestorage.com/";
        old.settings["addressing"] = "auto";
        QCOMPARE(r2.serializationKey(moved, {}, "shot.png"), s3->serializationKey(old, {}, "shot.png"));
        QVERIFY(!s3FollowsRedirects(R2Provider::s3Destination(moved).settings));
        QVERIFY(s3FollowsRedirects(QJsonObject{{"endpoint", "https://s3.us-east-1.amazonaws.com"}}));
        QVERIFY(!s3FollowsRedirects(QJsonObject{{"endpoint", "http://127.0.0.1:9000"}}));
        // AWS by its parsed host alone, one trailing dot ignored: never a name that merely contains it.
        QVERIFY(s3AmazonEndpoint("https://s3.us-east-1.amazonaws.com.") && s3AmazonEndpoint("https://s3.cn-north-1.amazonaws.com.cn"));
        for (const auto* endpoint : {"https://amazonaws.com.storage.example", "https://storage-amazonaws.com", "https://storage.example/s3.amazonaws.com"})
            QVERIFY2(!s3AmazonEndpoint(endpoint) && !s3FollowsRedirects(QJsonObject{{"endpoint", endpoint}}), endpoint);
        // aws-global sends a refused request on to another region; no other service gets it.
        QVERIFY(!s3RegionProblem("aws-global", "https://storage.example").isEmpty());
        QVERIFY(s3RegionProblem("aws-global", "https://s3.amazonaws.com").isEmpty());
    }
    void moveReadsOnlyAnR2AccountEndpoint() {
        QJsonObject s3{{"bucket", "lab-bucket"}, {"region", "Western North America (WNAM)"},
                       {"endpoint", "https://0123456789abcdef0123456789abcdef.eu.r2.cloudflarestorage.com"}, {"objectPrefix", "shots/"},
                       {"storageClass", "STANDARD"}, {"aclMode", "omit"}, {"publicUrlBase", "https://pub-0123.r2.dev"}, {"addressing", "auto"},
                       {"linkMode", "public"}, {"linkValidityDays", 7}, {"connectTimeoutMs", 5000}, {"requestTimeoutMs", 60000},
                       {"lowSpeedBytesPerSecond", 1}, {"lowSpeedTimeSeconds", 30}};
        const auto moved = r2::fromS3Settings(s3);
        QVERIFY(moved);
        QCOMPARE((*moved)["accountId"].toString(), QString(account));
        QCOMPARE((*moved)["jurisdiction"].toString(), QString("eu"));
        QCOMPARE(r2::s3Settings(*moved)["endpoint"], s3["endpoint"]);
        QCOMPARE((*moved)["publicUrlBase"], s3["publicUrlBase"]);
        QVERIFY(!r2::validate(*moved));
        // Expiring links never use Link domain: one R2 would refuse isn't carried over (#1699 P8).
        auto presigned = s3;
        presigned["linkMode"] = "presigned";
        presigned["publicUrlBase"] = "https://0123456789abcdef0123456789abcdef.eu.r2.cloudflarestorage.com";
        QVERIFY((*r2::fromS3Settings(presigned))["publicUrlBase"].isNull());
        QVERIFY(!r2::validate(*r2::fromS3Settings(presigned)));
        for (const auto& endpoint : {"https://s3.us-east-1.amazonaws.com", "https://0123456789abcdef0123456789abcdef.r2.cloudflarestorage.com/lab-bucket",
                                     "https://acct.r2.cloudflarestorage.com.example"}) {
            s3["endpoint"] = endpoint;
            QVERIFY2(!r2::fromS3Settings(s3), endpoint);
        }
    }
};
QTEST_GUILESS_MAIN(R2SettingsTest)
#include "test_r2_settings.moc"
