#include "fixtures/S3Fixture.h"
#include "storage/s3/PublicUrl.h"
#include "fixtures/U2Fixtures.h"
#include <QBuffer>
#include <QUrlQuery>
#include "fixtures/Waits.h"
#include "core/SettingsService.h"
using namespace putgd::test;
class S3Test : public QObject {
    Q_OBJECT
private slots:
    void presignedLinksUseEndpointAndNoPublicAcl_data() {
        QTest::addColumn<bool>("r2AuthorizationError");QTest::newRow("access-denied")<<false;QTest::newRow("r2-missing-authorization")<<true;
    }
    void presignedLinksUseEndpointAndNoPublicAcl() {
        QFETCH(bool,r2AuthorizationError);
        S3Fixture f;f.reset({{"private",true},{"versioning","Enabled"},{"unsignedAuthError",r2AuthorizationError}});
        f.destination.settings["linkMode"]="presigned";
        f.destination.settings["linkValidityDays"]=3;
        f.destination.settings["aclMode"]="public-read";
        f.destination.settings["publicUrlBase"]="https://cdn.example.invalid";
        const auto uploaded=f.upload(f.request("private é.txt"));
        QCOMPARE(uploaded.status,QString("uploaded"));
        const QUrl signedUrl(uploaded.receipt.directUrl);
        QCOMPARE(signedUrl.host(),QString("127.0.0.1"));
        QVERIFY(signedUrl.hasQuery());
        QCOMPARE(QUrlQuery(signedUrl).queryItemValue("versionId"),uploaded.receipt.objectReference["versionId"].toString());
        const auto fetch=[&](const QUrl& url){QTcpSocket peer;peer.connectToHost(url.host(),quint16(url.port()));if(!peer.waitForConnected(5000))return QByteArray{};
            const auto target=url.path(QUrl::FullyEncoded)+(url.hasQuery()?"?"+url.query(QUrl::FullyEncoded):QString{});
            peer.write("GET "+target.toUtf8()+" HTTP/1.1\r\nHost: "+url.authority().toUtf8()+"\r\nConnection: close\r\n\r\n");peer.waitForBytesWritten(5000);
            QByteArray data;while(peer.waitForReadyRead(5000))data+=peer.readAll();data+=peer.readAll();return data;};
        const auto response=fetch(signedUrl);QVERIFY(response.startsWith("HTTP/1.1 200"));QVERIFY(response.endsWith("synthetic payload"));
        auto unsignedUrl=signedUrl;unsignedUrl.setQuery(QString{});QVERIFY(fetch(unsignedUrl).startsWith(r2AuthorizationError?"HTTP/1.1 400":"HTTP/1.1 403"));
        QVERIFY(!f.events("PUT")[0].toObject()["headers"].toObject().contains("x-amz-acl"));
        const auto expiry=QDateTime::fromString(uploaded.receipt.objectReference["linkExpiresAt"].toString(),Qt::ISODateWithMs);
        QVERIFY(expiry>QDateTime::currentDateTimeUtc().addDays(2));
        const auto renewed=f.await<StatResult>([&](auto cb){f.provider.refreshLink(uploaded.receipt,cb);});
        QCOMPARE(renewed.status,QString("own"));
        QCOMPARE(renewed.receipt.destination.toJson(),uploaded.receipt.destination.toJson());
        QCOMPARE(renewed.receipt.publicationId,uploaded.receipt.publicationId);
        // The reply comes from the S3 worker thread: await hands the result and error back through a promise.
        // The test journal and result keep no signed query: it holds the access key ID and a working signature (#1680).
        auto journal=std::make_shared<QJsonArray>();
        const auto tested=f.await<std::pair<QJsonObject,Error>>([&](auto cb){
            f.provider.testDestination(f.destination,[journal](auto value,auto ack){journal->append(value);ack({});},[cb](QJsonObject value,Error e){cb({std::move(value),std::move(e)});});});
        const auto& result=tested.first;const auto& error=tested.second;
        QVERIFY2(!error,qPrintable(error.code));
        QVERIFY(result["presignedReachable"].toBool());QVERIFY(result["unsignedDenied"].toBool());
        QCOMPARE(result["cleanupStatus"].toString(),QString("removed"));
        QVERIFY(journal->size()>=3);
        for(const auto& written:{QJsonDocument(*journal).toJson(),QJsonDocument(result).toJson()})
            for(const char* secret:{"X-Amz-Signature","X-Amz-Credential","PUTGD_SYNTHETIC_ACCESS"})QVERIFY2(!written.contains(secret),secret);
    }


    void failedNameHeadRetainsSafeTransportDiagnostics() {
        S3Fixture f;
        f.process.terminate();
        QVERIFY(f.process.waitForFinished());
        const auto result = f.await<NameCheck>([&](auto cb) { f.provider.checkName(f.destination, "synthetic.txt", cb); });
        QCOMPARE(result.status, QString("inconclusive"));
        QCOMPARE(result.error.code, QString("s3_transport_unknown"));
        const auto encoded = QJsonDocument(result.error.toJson()).toJson();
        QVERIFY(!encoded.contains("putgd-synthetic-secret-not-a-real-key"));
        QVERIFY(!encoded.contains("PUTGD_SYNTHETIC_ACCESS"));
        QVERIFY(!encoded.contains(f.endpoint.toUtf8()));
        QVERIFY(!encoded.contains("synthetic.txt"));
    }
    void routingSignedBodyAndPublicUrl() {
        S3Fixture f;
        f.reset();
        auto request = f.request("a b.txt");
        auto result = f.upload(request);
        QCOMPARE(result.status, QString("uploaded"));
        QCOMPARE(result.receipt.destination.toJson(), f.destination.toJson());
        QCOMPARE(result.receipt.directUrl, f.endpoint + "/isolated-fixture/prefix%20%C3%A9/a%20b.txt");
        auto event = f.events("PUT")[0].toObject();
        QVERIFY(event["signatureValid"].toBool());
        QCOMPARE(event["bucket"].toString(), QString("isolated-fixture"));
        QCOMPARE(event["key"].toString(), QString("prefix é/a b.txt"));
        auto headers = event["headers"].toObject();
        QVERIFY(headers["authorization"].toString().contains("/eu-west-3/s3/aws4_request"));
        QVERIFY(!headers.contains("x-amz-acl"));
        QCOMPARE(headers["if-none-match"].toString(), QString("*"));
        QCOMPARE(headers["content-type"].toString(), QString("text/plain"));
        QCOMPARE(headers["x-amz-storage-class"].toString(), QString("STANDARD"));
        QCOMPARE(QByteArray::fromBase64(event["body"].toString().toUtf8()), QByteArray("synthetic payload"));
        for (const auto& event : f.events()) QVERIFY(event.toObject()["signatureValid"].toBool());
        f.destination.settings["bucket"] = "put.gd";
        f.destination.settings["region"] = "us-east-2";
        f.destination.settings["objectPrefix"] = "";
        f.destination.settings["aclMode"] = "public-read";
        f.destination.settings["storageClass"] = "STANDARD_IA";
        f.destination.settings["publicUrlBase"] = "https://put.gd/base%2Fencoded";
        QImage image(1, 1, QImage::Format_ARGB32);
        image.fill(Qt::blue);
        QByteArray png;
        QBuffer buffer(&png);
        QVERIFY(buffer.open(QIODevice::WriteOnly));
        QVERIFY(image.save(&buffer, "PNG"));
        auto pngRequest = f.request("synthetic.png", png);
        pngRequest.artifact.mime = "image/png";
        result = f.upload(pngRequest);
        QCOMPARE(result.status, QString("uploaded"));
        event = f.events("PUT")[1].toObject();
        headers = event["headers"].toObject();
        QVERIFY(event["signatureValid"].toBool());
        QCOMPARE(headers["x-amz-acl"].toString(), QString("public-read"));
        QCOMPARE(headers["x-amz-storage-class"].toString(), QString("STANDARD_IA"));
        QCOMPARE(headers["content-type"].toString(), QString("image/png"));
        QCOMPARE(QByteArray::fromBase64(event["body"].toString().toUtf8()), png);
        QCOMPARE(result.receipt.directUrl, QString("https://put.gd/base%2Fencoded/synthetic.png"));
        QVERIFY(headers["host"].toString().startsWith("127.0.0.1"));
    }
    void lostPutReplyRecoversOwnMetadata() {
        S3Fixture f;
        f.reset({{"rules", QJsonArray{QJsonObject{{"method", "PUT"}, {"drop", true}}}}});
        auto request = f.request();
        const auto result = f.upload(request);
        QVERIFY2(result.status == "uploaded", qPrintable(QJsonDocument(result.error.toJson()).toJson()));
        QCOMPARE(result.receipt.publicationId, request.publicationId);
        QCOMPARE(f.events("PUT").size(), 1);
        QVERIFY(f.events("HEAD").size() >= 2);
        QCOMPARE(f.state()["objects"].toObject()["prefix é/a b.txt"].toArray().size(), 1);
        // Explicit retry of the same conditional create gets 412 and recovers receipt.
        const auto retry = f.upload(request);
        QCOMPARE(retry.status, QString("uploaded"));
        QCOMPARE(retry.receipt.toJson(), result.receipt.toJson());
        QCOMPARE(f.events("PUT").size(), 2);
    }
    void lostReplyInconclusiveRemainsUnknown() {
        S3Fixture f;
        auto request = f.request();
        f.reset({{"rules", QJsonArray{QJsonObject{{"method", "PUT"}, {"drop", true}}, QJsonObject{{"method", "HEAD"}, {"status", 403}, {"remaining", 10}}}}});
        const auto result = f.upload(request);
        QVERIFY2(result.status == "unknown", qPrintable(QJsonDocument(result.error.toJson()).toJson()));
        QCOMPARE(f.events("PUT").size(), 1);
        auto stat = f.await<StatResult>([&](auto cb) { f.provider.reconcile(request, cb); });
        QCOMPARE(stat.status, QString("unknown"));
        QCOMPARE(f.events("PUT").size(), 1);
    }
    void boundedTransientAttemptsKeepWireIdentity() {
        S3Fixture f;
        auto request = f.request();
        f.reset({{"rules", QJsonArray{QJsonObject{{"method", "PUT"}, {"status", 500}, {"code", "InternalError"}, {"remaining", 3}}}}});
        const auto result = f.upload(request);
        QCOMPARE(result.status, QString("unknown"));
        const auto events = f.events("PUT");
        QCOMPARE(events.size(), 3);
        for (const auto& value : events) {
            const auto event = value.toObject();
            QCOMPARE(event["key"].toString(), request.objectReference["key"].toString());
            QCOMPARE(QByteArray::fromBase64(event["body"].toString().toUtf8()), QByteArray("synthetic payload"));
            QCOMPARE(event["headers"].toObject()["if-none-match"].toString(), QString("*"));
            QVERIFY(event["signatureValid"].toBool());
        }
    }
    void lostReplyWithoutOwnMetadataKeepsUnknownKey() {
        S3Fixture f;
        auto request = f.request();
        f.reset({{"rules", QJsonArray{QJsonObject{{"method", "PUT"}, {"drop", true}, {"stripMetadata", true}}}}});
        const auto result = f.upload(request);
        QCOMPARE(result.status, QString("unknown"));
        QCOMPARE(f.events("PUT").size(), 1);
        QCOMPARE(f.events("PUT")[0].toObject()["key"].toString(), request.objectReference["key"].toString());
        QCOMPARE(f.state()["objects"].toObject()["prefix é/a b.txt"].toArray().size(), 1);
    }
    void conditionalRaceNeverOverwritesCompetitor() {
        S3Fixture f;
        auto request = f.request();
        f.reset({{"rules", QJsonArray{QJsonObject{{"method", "PUT"}, {"competitor", true}}}}});
        auto result = f.upload(request);
        QCOMPARE(result.status, QString("collision"));
        QCOMPARE(QByteArray::fromBase64(f.state()["objects"].toObject()["prefix é/a b.txt"].toArray()[0].toObject()["body"].toString().toUtf8()), QByteArray("competitor"));
        QCOMPARE(f.events("PUT").size(), 1);
    }
    void checkedOverwriteDoesNotRefreshToken() {
        S3Fixture f;
        f.reset();
        QCOMPARE(f.upload(f.request()).status, QString("uploaded"));
        auto request = f.request();
        QCOMPARE(request.condition, QString("match"));
        auto objects = f.state()["objects"].toObject();
        f.reset({{"objects", objects}, {"rules", QJsonArray{QJsonObject{{"method", "PUT"}, {"competitor", true}}}}});
        auto result = f.upload(request);
        QCOMPARE(result.status, QString("collision"));
        QCOMPARE(f.events("PUT")[0].toObject()["headers"].toObject()["if-match"].toString(), request.concurrencyToken);
        QCOMPARE(f.events("PUT").size(), 1);
    }
    void headDeniedDoesNotPublishAndCredentialsNeverAmbient() {
        S3Fixture f;
        f.reset({{"rules", QJsonArray{QJsonObject{{"method", "HEAD"}, {"status", 403}}}}});
        auto checked = f.await<NameCheck>([&](auto cb) { f.provider.checkName(f.destination, "a", cb); });
        QCOMPARE(checked.status, QString("inconclusive"));
        QCOMPARE(f.events("PUT").size(), 0);
        f.credentials.forget(f.destination.profileId, f.destination.credentialReference);
        checked = f.await<NameCheck>([&](auto cb) { f.provider.checkName(f.destination, "b", cb); });
        QCOMPARE(checked.error.code, QString("credential_setup_required"));
        QCOMPARE(f.events().size(), 1);
    }
    // #1601: the hint appended to a permission or authentication error when the key, prefix
    // included, contains $ & , : = or @. The rest of the message is exactly what the same
    // failure gives for a plain key.
    static QString nameCharactersHint() {
        return QStringLiteral(" Some S3-compatible services, such as Garage, can't verify names containing $ & , : = or @; if this name or its folder in the bucket has them, try renaming it.");
    }
    // Like Garage, the oracle checks the signature against the path as it arrives, and the SDK
    // sends these characters unencoded but signs them encoded: the name check is denied.
    void nameCheckAddsTheHintForSuchKeys_data() {
        QTest::addColumn<QString>("prefix");QTest::addColumn<QString>("name");
        QTest::newRow("colon") << QString("prefix é") << QString("colon:.txt");
        QTest::newRow("at-dollar-comma") << QString("prefix é") << QString("at@dollar$comma,.txt");
        QTest::newRow("amp-equals") << QString("prefix é") << QString("amp&eq=.txt");
        QTest::newRow("in-the-folder") << QString("team=a") << QString("plain.txt");
    }
    void nameCheckAddsTheHintForSuchKeys() {
        QFETCH(QString, prefix);QFETCH(QString, name);
        S3Fixture f;
        f.reset({{"rules", QJsonArray{QJsonObject{{"method", "HEAD"}, {"key", "prefix é/plain.txt"}, {"status", 403}}}}});
        const auto plain = f.await<NameCheck>([&](auto cb) { f.provider.checkName(f.destination, "plain.txt", cb); });
        QCOMPARE(plain.error.code, QString("s3_permission_denied"));
        // A plain key never gets the hint, so the baseline below holds none.
        QVERIFY(!plain.error.message.contains(nameCharactersHint()));
        f.destination.settings["objectPrefix"] = prefix;
        f.reset();
        const auto checked = f.await<NameCheck>([&](auto cb) { f.provider.checkName(f.destination, name, cb); });
        QCOMPARE(checked.status, QString("inconclusive"));
        QCOMPARE(checked.error.code, QString("s3_permission_denied"));
        QCOMPARE(checked.error.message, plain.error.message + nameCharactersHint());
        // The hint costs no request.
        QCOMPARE(f.events().size(), 1);
    }
    // The upload's own failure, single request or multipart: both eligible codes gain the hint
    // for such a key, any other code does not, and a plain key never does. The oracle verifies
    // paths the way AWS does here, so the rule decides the answer.
    void uploadFailureAddsTheHintOnlyForEligibleCodes_data() {
        QTest::addColumn<bool>("multipart");QTest::addColumn<int>("status");QTest::addColumn<QString>("service");QTest::addColumn<QString>("code");QTest::addColumn<bool>("hint");
        for (const bool multipart : {false, true}) {
            const auto mode = QString(multipart ? "multipart-" : "single-");
            QTest::newRow(qPrintable(mode + "denied")) << multipart << 403 << QString("AccessDenied") << QString("s3_permission_denied") << true;
            QTest::newRow(qPrintable(mode + "signature")) << multipart << 403 << QString("SignatureDoesNotMatch") << QString("s3_authentication_failed") << true;
            QTest::newRow(qPrintable(mode + "other")) << multipart << 400 << QString("InvalidRequest") << QString("s3_request_failed") << false;
        }
    }
    void uploadFailureAddsTheHintOnlyForEligibleCodes() {
        QFETCH(bool, multipart);QFETCH(int, status);QFETCH(QString, service);QFETCH(QString, code);QFETCH(bool, hint);
        S3UploadPolicy policy;policy.multipartThresholdBytes = 1024 * 1024;policy.minimumPartBytes = 1024 * 1024;
        S3Fixture f(policy);
        QJsonObject rule{{"method", multipart ? "POST" : "PUT"}, {"status", status}, {"code", service}, {"remaining", 10}};
        if (multipart) rule["query"] = "uploads";
        const QByteArray bytes(multipart ? 2 * 1024 * 1024 : 64, 'x');
        auto attempt = [&](const QString& name) {
            f.reset({{"normalisePath", true}, {"rules", QJsonArray{rule}}});
            return f.upload(f.request(name, bytes));
        };
        const auto plain = attempt("plain.bin");
        const auto special = attempt("a:b@c.bin");
        QCOMPARE(plain.status, QString("failed"));QCOMPARE(special.status, QString("failed"));
        QCOMPARE(plain.error.code, code);QCOMPARE(special.error.code, code);
        QVERIFY(!plain.error.message.contains(nameCharactersHint()));
        if (!hint) QVERIFY(!special.error.message.contains(nameCharactersHint()));
        QCOMPARE(special.error.message, plain.error.message + (hint ? nameCharactersHint() : QString()));
    }
    // The SDK drops empty path segments, so the receipt, the link and the stored
    // object must all name the key the request reached (#1582).
    void prefixKeyIsTheStoredKey_data() {
        QTest::addColumn<QString>("prefix");QTest::addColumn<QString>("stored");
        QTest::newRow("doubled")<<"a//b/"<<"a/b/";QTest::newRow("trailing-doubled")<<"dir//"<<"dir/";
        QTest::newRow("leading")<<"/lead"<<"lead/";QTest::newRow("only-slashes")<<"//"<<"";
    }
    void prefixKeyIsTheStoredKey() {
        QFETCH(QString,prefix);QFETCH(QString,stored);
        S3Fixture f;f.reset();f.destination.settings["objectPrefix"]=prefix;
        const auto uploaded=f.upload(f.request("p.txt"));
        QCOMPARE(uploaded.status,QString("uploaded"));
        QCOMPARE(uploaded.receipt.objectReference["key"].toString(),stored+"p.txt");
        QCOMPARE(f.events("PUT")[0].toObject()["key"].toString(),stored+"p.txt");
        QVERIFY(f.state()["objects"].toObject().contains(stored+"p.txt"));
        QCOMPARE(uploaded.receipt.directUrl,f.endpoint+"/isolated-fixture/"+stored+"p.txt");
        QCOMPARE(f.await<StatResult>([&](auto cb){f.provider.stat(uploaded.receipt,cb);}).status,QString("own"));
        QCOMPARE(f.remove(uploaded.receipt).status,QString("removed"));
        QVERIFY(f.state()["objects"].toObject()[stored+"p.txt"].toArray().isEmpty());
    }
    // History rows and journals made before #1582 hold the key as typed. They still
    // check, delete, retry and abandon against the stored key.
    void receiptsAndJournalsBeforeNormalisation() {
        S3Fixture f;f.reset();f.destination.settings["objectPrefix"]="a//b/";
        const auto uploaded=f.upload(f.request("old.txt"));
        QCOMPARE(uploaded.status,QString("uploaded"));
        auto legacy=uploaded.receipt;legacy.objectReference["key"]="a//b/old.txt";
        legacy.directUrl=s3LegacyPublicObjectUrl(legacy.destination.settings,"a//b/old.txt");
        QCOMPARE(legacy.directUrl,f.endpoint+"/isolated-fixture/a//b/old.txt");
        const auto stat=f.await<StatResult>([&](auto cb){f.provider.stat(legacy,cb);});
        QCOMPARE(stat.status,QString("own"));
        // History compares the stat receipt with the row's own before deleting.
        QCOMPARE(stat.receipt.objectReference,legacy.objectReference);
        QCOMPARE(f.events("HEAD").last().toObject()["key"].toString(),QString("a/b/old.txt"));
        QCOMPARE(f.remove(legacy).status,QString("removed"));
        QCOMPARE(f.events("DELETE")[0].toObject()["key"].toString(),QString("a/b/old.txt"));
        QVERIFY(f.state()["objects"].toObject()["a/b/old.txt"].toArray().isEmpty());
        // Retrying an interrupted upload whose journal holds the old key.
        auto retry=f.request("retry.txt");retry.objectReference["key"]="a//b/retry.txt";
        const auto retried=f.upload(retry);
        QVERIFY2(retried.status=="uploaded",qPrintable(retried.error.code));
        QCOMPARE(retried.receipt.objectReference["key"].toString(),QString("a/b/retry.txt"));
        QVERIFY(f.state()["objects"].toObject().contains("a/b/retry.txt"));
        // Abandoning a multipart upload whose journal holds the old key.
        bool done=false;Error abandoned;
        f.provider.abandonUpload(f.destination,{{"schemaVersion",1},{"partBytes",8*1024*1024},{"uploadId","synthetic-upload-old"},{"key","a//b/big.bin"},
            {"bytes",double(64*1024*1024)},{"sha256",QString(64,'a')},{"phase","parts"}},[&](Error e){abandoned=e;done=true;});
        PUTGD_TRY_VERIFY(done);
        QVERIFY2(abandoned.code!="s3_multipart_invalid",qPrintable(abandoned.message));
        QCOMPARE(f.events("DELETE").last().toObject()["key"].toString(),QString("a/b/big.bin"));
    }
    // An upload whose reply was lost, retried from a journal that holds the key as typed:
    // reconciliation finds it and its first receipt names the stored key.
    void reconciledReceiptNamesTheStoredKey() {
        S3Fixture f;f.reset();f.destination.settings["objectPrefix"]="a//b/";
        auto request=f.request("lost.txt");
        f.reset({{"rules",QJsonArray{QJsonObject{{"method","PUT"},{"drop",true}},QJsonObject{{"method","HEAD"},{"status",503},{"code","ServiceUnavailable"},{"remaining",10}}}}});
        QCOMPARE(f.upload(request).status,QString("unknown"));
        f.reset({{"objects",f.state()["objects"]}});
        auto retry=request;retry.objectReference["key"]="a//b/lost.txt";
        const auto found=f.await<StatResult>([&](auto cb){f.provider.reconcile(retry,cb);});
        QVERIFY2(found.status=="own",qPrintable(found.error.code));
        QCOMPARE(found.receipt.objectReference["key"].toString(),QString("a/b/lost.txt"));
        QCOMPARE(found.receipt.directUrl,f.endpoint+"/isolated-fixture/a/b/lost.txt");
        QCOMPARE(f.events("HEAD").last().toObject()["key"].toString(),QString("a/b/lost.txt"));
    }
    // #1675: a destination saved with a Region that today's rules refuse still loads, and
    // other settings still save; changing that destination needs it fixed.
    void refusedSettingsSavedEarlierStillLoad() {
        S3Fixture f;ProviderRegistry registry;registry.add("s3",std::shared_ptr<S3Provider>(&f.provider,[](S3Provider*){}));
        auto doc=test::configuration(f.directory.path());auto bad=f.destination.settings;bad["region"]="Western North America (WNAM)";
        doc["profiles"]=QJsonArray{QJsonObject{{"id",f.destination.profileId},{"label","Old"},{"provider","s3"},{"settings",bad},{"credentials",f.destination.credentialReference}}};
        auto actions=doc["actions"].toObject();for(const auto& key:Settings::actionNames()){auto action=actions[key].toObject();action["profileId"]=f.destination.profileId;actions[key]=action;}doc["actions"]=actions;
        QFile file(f.directory.filePath("config.json"));QVERIFY(file.open(QIODevice::WriteOnly));file.write(QJsonDocument(doc).toJson());file.close();
        SettingsService settings(file.fileName(),[&](auto id,auto value){return registry.validate(id,value);},nullptr,{},[&](auto id,auto value){return registry.validateStored(id,value);});
        QVERIFY2(settings.metadata()["validation"].toObject()["ok"].toBool(),qPrintable(QJsonDocument(settings.metadata()).toJson()));
        QCOMPARE(settings.document()["profiles"].toArray()[0].toObject()["settings"].toObject()["region"].toString(),QString("Western North America (WNAM)"));
        auto other=settings.document();auto limits=other["retention"].toObject();limits["historyRetentionDays"]=30;other["retention"]=limits;
        const auto savedOther=settings.save(other,settings.revision());QVERIFY2(savedOther,qPrintable(savedOther.error.message));
        auto changed=settings.document();auto profiles=changed["profiles"].toArray();auto profile=profiles[0].toObject();auto s=profile["settings"].toObject();
        s["bucket"]="another-bucket";profile["settings"]=s;profiles[0]=profile;changed["profiles"]=profiles;
        const auto refused=settings.save(changed,settings.revision());
        QCOMPARE(refused.error.code,QString("s3_settings_invalid"));QCOMPARE(refused.error.details["field"].toString(),QString("region"));
        s["region"]="auto";profile["settings"]=s;profiles[0]=profile;changed["profiles"]=profiles;
        const auto fixed=settings.save(changed,settings.revision());QVERIFY2(fixed,qPrintable(fixed.error.message));
        // Cloudflare R2 signs with auto whatever Region holds (#1678): its saved dashboard location stays valid.
        auto r2=f.destination.settings;r2["region"]="Western North America (WNAM)";r2["endpoint"]="https://ACCOUNT.R2.cloudflarestorage.com";
        QVERIFY(!f.provider.validateSettings(r2));
        r2["endpoint"]="https://r2.cloudflarestorage.com.example.net";QCOMPARE(f.provider.validateSettings(r2).details["field"].toString(),QString("region"));
    }
    // A "." or ".." part is removed by libcurl after the SDK signed it, so no request
    // names it exactly: new uploads are refused and old receipts are left alone.
    void dotSegmentsAreNeverSent_data() {
        QTest::addColumn<QString>("prefix");
        for(const auto* prefix:{"a/./b/","a/../b/",".","..","/./"})QTest::newRow(prefix)<<QString(prefix);
    }
    void dotSegmentsAreNeverSent() {
        QFETCH(QString,prefix);
        S3Fixture f;f.reset();
        const auto uploaded=f.upload(f.request("p.txt"));QCOMPARE(uploaded.status,QString("uploaded"));
        f.reset({{"objects",f.state()["objects"]}});
        f.destination.settings["objectPrefix"]=prefix;
        const auto checked=f.await<NameCheck>([&](auto cb){f.provider.checkName(f.destination,"p.txt",cb);});
        QCOMPARE(checked.status,QString("inconclusive"));
        QCOMPARE(checked.error.code,QString("s3_settings_invalid"));
        auto legacy=uploaded.receipt;const auto key=prefix+"/p.txt";legacy.objectReference["key"]=key;
        legacy.directUrl=s3LegacyPublicObjectUrl(legacy.destination.settings,key);
        const auto stat=f.await<StatResult>([&](auto cb){f.provider.stat(legacy,cb);});
        QCOMPARE(stat.status,QString("unknown"));QCOMPARE(stat.error.code,QString("s3_key_invalid"));
        const auto removed=f.remove(legacy);
        QCOMPARE(removed.status,QString("failed"));QCOMPARE(removed.error.code,QString("s3_key_invalid"));
        auto retry=f.request("p.txt");retry.destination=uploaded.receipt.destination;retry.objectReference={{"schemaVersion",1},{"key",key}};
        const auto retried=f.upload(retry);
        QCOMPARE(retried.status,QString("failed"));QCOMPARE(retried.error.code,QString("s3_key_invalid"));
        // A retry under the dot folder itself names the setting to fix, not the key.
        retry.destination=f.destination;
        const auto underFolder=f.upload(retry);
        QCOMPARE(underFolder.status,QString("failed"));QCOMPARE(underFolder.error.code,QString("s3_settings_invalid"));
        // So does the reconciliation that retrying an unknown upload starts with, which stays unknown.
        const auto reconciled=f.await<StatResult>([&](auto cb){f.provider.reconcile(retry,cb);});
        QCOMPARE(reconciled.status,QString("unknown"));QCOMPARE(reconciled.error.code,QString("s3_settings_invalid"));
        QCOMPARE(f.events().size(),0);
    }
};
QTEST_GUILESS_MAIN(S3Test)
#include "test_s3.moc"
