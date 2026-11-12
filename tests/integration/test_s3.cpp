#include "fixtures/S3Fixture.h"
#include "fixtures/U2Fixtures.h"
#include <QBuffer>
#include <QUrlQuery>
#include "fixtures/Waits.h"
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
        bool done=false;Error error;QJsonObject result;
        f.provider.testDestination(f.destination,[](auto,auto ack){ack({});},[&](auto value,auto e){result=value;error=e;done=true;});
        PUTGD_TRY_VERIFY(done);QVERIFY2(!error,qPrintable(error.code));
        QVERIFY(result["presignedReachable"].toBool());QVERIFY(result["unsignedDenied"].toBool());
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
};
QTEST_GUILESS_MAIN(S3Test)
#include "test_s3.moc"
