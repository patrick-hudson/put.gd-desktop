#include "fixtures/S3Fixture.h"
#include <thread>
#include <QBuffer>
#include <mutex>
using namespace putgd::test;
class S3Test : public QObject {
    Q_OBJECT
private slots:
    void failedNameHeadRetainsSafeTransportDiagnostics() {
        S3Fixture f;
        f.process.terminate();
        QVERIFY(f.process.waitForFinished());
        const auto result = f.await<NameCheck>([&](auto cb) { f.provider.checkName(f.destination, "synthetic.txt", cb); });
        QCOMPARE(result.status, QString("inconclusive"));
        QCOMPARE(result.error.code, QString("s3_transport_unknown"));
        QCOMPARE(result.error.details["httpStatus"].toInt(), -1);
        QVERIFY(result.error.details.contains("sdkErrorType"));
        QVERIFY(result.error.details["curlCode"].isDouble());
        const auto curlCode = result.error.details["curlCode"].toInt();
        // SDK builds with a different HTTP backend have no Curl message prefix.
        QVERIFY(curlCode == -1 || curlCode == 7);
        QCOMPARE(result.error.details["requestStage"].toString(), QString("name_head"));
        QVERIFY(result.error.details["elapsedMs"].toInteger() >= 0);
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
    void uploadProgressUsesBodyBytesAndFinishesBeforeReply() {
        S3Fixture f;
        auto request = f.request("progress.txt", QByteArray(1024 * 1024, 'p'));
        // Keep the real SDK call active after it sends the body, allowing the
        // production child timer to forward byte counters before completion.
        f.reset({{"rules", QJsonArray{QJsonObject{{"method", "PUT"}, {"delay", 0.7}}}}});
        std::mutex mutex;
        QList<UploadProgress> progress;
        request.progress = [&](const UploadProgress& value) {
            std::lock_guard lock(mutex);
            progress.append(value);
        };
        const auto result = f.upload(request);
        QCOMPARE(result.status, QString("uploaded"));
        qsizetype count;
        {
            std::lock_guard lock(mutex);
            QVERIFY(!progress.isEmpty());
            bool sentBody = false;
            for (const auto& value : progress) {
                QVERIFY(value.valid());
                QCOMPARE(value.bytesTotal, std::optional<qint64>(request.artifact.bytes));
                QVERIFY(value.bytesTransferred.has_value());
                if (value.phase == "uploading" && value.bytesTransferred == request.artifact.bytes) sentBody = true;
            }
            QVERIFY(sentBody);
            QCOMPARE(progress.last().phase, QString("finalizing"));
            QCOMPARE(progress.last().bytesTransferred, std::optional<qint64>(request.artifact.bytes));
            count = progress.size();
        }
        QTest::qWait(350);
        std::lock_guard lock(mutex);
        QCOMPARE(progress.size(), count);
    }
    void transientRetryRestartsProgressAtSameBodyTotal() {
        S3Fixture f;
        auto request = f.request("retry-progress.txt", QByteArray(128 * 1024, 'p'));
        f.reset({{"rules", QJsonArray{QJsonObject{{"method", "PUT"}, {"delay", 0.7}, {"status", 500},
                                                {"code", "InternalError"}, {"remaining", 3}}}}});
        std::mutex mutex;
        QList<UploadProgress> progress;
        request.progress = [&](const UploadProgress& value) {
            std::lock_guard lock(mutex);
            progress.append(value);
        };
        QCOMPARE(f.upload(request).status, QString("unknown"));
        QCOMPARE(f.events("PUT").size(), 3);
        std::lock_guard lock(mutex);
        QVERIFY(!progress.isEmpty());
        int completeBodies = 0;
        for (const auto& value : progress) {
            QVERIFY(value.valid());
            QCOMPARE(value.bytesTotal, std::optional<qint64>(request.artifact.bytes));
            QVERIFY(*value.bytesTransferred <= request.artifact.bytes);
            if (value.phase == "uploading" && value.bytesTransferred == request.artifact.bytes) ++completeBodies;
        }
        QVERIFY(completeBodies >= 3);
    }
    void redirectedBodyKeepsTheCurrentArtifactTotal() {
        S3Fixture f;
        auto request = f.request("redirect-progress.txt", QByteArray(128 * 1024 + 7, 'p'));
        f.reset({{"rules", QJsonArray{QJsonObject{{"method", "PUT"}, {"delay", 0.7}, {"redirect", true}}}}});
        std::mutex mutex;
        QList<UploadProgress> progress;
        request.progress = [&](const UploadProgress& value) {
            std::lock_guard lock(mutex);
            progress.append(value);
        };
        QCOMPARE(f.upload(request).status, QString("uploaded"));
        const auto events = f.events("PUT");
        QCOMPARE(events.size(), 2);
        for (const auto& event : events)
            QCOMPARE(QByteArray::fromBase64(event.toObject()["body"].toString().toUtf8()).size(), request.artifact.bytes);
        std::lock_guard lock(mutex);
        QVERIFY(!progress.isEmpty());
        for (const auto& value : progress) {
            QVERIFY(value.valid());
            QCOMPARE(value.bytesTotal, std::optional<qint64>(request.artifact.bytes));
            QVERIFY(*value.bytesTransferred <= request.artifact.bytes);
        }
        QCOMPARE(progress.last().phase, QString("finalizing"));
        QCOMPARE(progress.last().bytesTransferred, std::optional<qint64>(request.artifact.bytes));
    }
    void terminalErrors_data() {
        QTest::addColumn<int>("http");
        QTest::addColumn<QString>("service");
        QTest::addColumn<QString>("expected");
        QTest::addColumn<QString>("guidance");
        QTest::newRow("ownership") << 400 << QString("AccessControlListNotSupported") << QString("s3_acl_incompatible") << QString("Follow the bucket's policy");
        QTest::newRow("public-access-block") << 403 << QString("AccessDenied") << QString("s3_permission_denied") << QString("s3:PutObject");
        QTest::newRow("authentication") << 403 << QString("InvalidAccessKeyId") << QString("s3_authentication_failed") << QString("access key ID");
        QTest::newRow("wrong-secret") << 403 << QString("SignatureDoesNotMatch") << QString("s3_authentication_failed") << QString("secret access key");
        QTest::newRow("expired-token") << 400 << QString("ExpiredToken") << QString("s3_authentication_failed") << QString("session token");
        QTest::newRow("missing-bucket") << 404 << QString("NoSuchBucket") << QString("s3_bucket_not_found") << QString("bucket name");
        QTest::newRow("wrong-region") << 400 << QString("AuthorizationHeaderMalformed") << QString("s3_region_mismatch") << QString("region and endpoint");
        QTest::newRow("clock-skew") << 403 << QString("RequestTimeTooSkewed") << QString("s3_authentication_failed") << QString("system clock");
    }
    void terminalErrors() {
        QFETCH(int, http); QFETCH(QString, service); QFETCH(QString, expected);QFETCH(QString, guidance);
        S3Fixture f;
        f.destination.settings["aclMode"] = "public-read";
        f.reset({{"rules", QJsonArray{QJsonObject{{"method", "PUT"}, {"status", http}, {"code", service}}}}});
        auto result = f.upload(f.request());
        QCOMPARE(result.status, QString("failed"));
        QCOMPARE(result.error.code, expected);
        QVERIFY2(result.error.message.contains(guidance),qPrintable(result.error.message));
        QVERIFY(!QJsonDocument(result.error.toJson()).toJson().contains("synthetic fixture"));
        QCOMPARE(result.error.details["serviceCode"].toString(),service);
        // #74: the request IDs AWS support asks for survive the worker protocol.
        QCOMPARE(result.error.details["requestId"].toString(),QString("SYNTHREQ0123ABCD"));
        QCOMPARE(result.error.details["extendedRequestId"].toString(),QString("synthetic+Extended/Id2="));
        QCOMPARE(f.events("PUT").size(), 1);
        QCOMPARE(f.events("DELETE").size(), 0);
        QCOMPARE(f.events("PUT")[0].toObject()["headers"].toObject()["x-amz-acl"].toString(), QString("public-read"));
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
    void queuedCancellationAndSlowEventLoop() {
        // One lane: the second call queues behind the first.
        S3Fixture f({}, 1);
        auto first = f.request("slow.txt");
        auto second = f.request("cancelled.txt");
        second.cancellation = std::make_shared<std::atomic_bool>(false);
        f.reset({{"rules", QJsonArray{QJsonObject{{"method", "PUT"}, {"delay", 0.2}}}}});
        std::atomic_int callbacks{0};
        QString secondStatus;
        f.provider.upload(first, [&](auto) { ++callbacks; });
        f.provider.upload(second, [&](auto result) { secondStatus = result.error.code; ++callbacks; });
        second.cancellation->store(true);
        int ticks = 0;
        QTimer timer;
        connect(&timer, &QTimer::timeout, this, [&] { ++ticks; });
        timer.start(5);
        QTRY_COMPARE(callbacks.load(), 2);
        QVERIFY(ticks > 10);
        QCOMPARE(secondStatus, QString("cancelled"));
        QCOMPARE(f.events("PUT").size(), 1);
    }
    void queuedCredentialRevocationPreservesInFlightClient() {
        // One lane: the second call queues behind the first.
        S3Fixture f({}, 1);
        auto first = f.request("in-flight.txt");
        auto queued = f.request("queued.txt");
        f.reset({{"rules", QJsonArray{QJsonObject{{"method", "PUT"}, {"barrier", "credential-change"}}}}});
        std::atomic_int callbacks{0};
        QString firstStatus, secondCode;
        f.provider.upload(first, [&](auto result) { firstStatus = result.status; ++callbacks; });
        QTRY_COMPARE(f.events("PUT").size(), 1);
        QCOMPARE(callbacks.load(), 0);
        f.provider.upload(queued, [&](auto result) { secondCode = result.error.code; ++callbacks; });
        f.credentials.forget(f.destination.profileId, f.destination.credentialReference);
        f.control("/__release", {{"barrier", "credential-change"}}, true);
        QTRY_COMPARE(callbacks.load(), 2);
        QCOMPARE(firstStatus, QString("uploaded"));
        QCOMPARE(secondCode, QString("credential_setup_required"));
        QCOMPARE(f.events("PUT").size(), 1);
        QVERIFY(f.events("PUT")[0].toObject()["signatureValid"].toBool());
    }
    void lanesRunUploadsInParallel() {
        S3Fixture f;
        QVERIFY(f.provider.maximumConcurrentRequests(f.destination) > 1);
        auto blocked = f.request("blocked.txt");
        auto free = f.request("free.txt");
        f.reset({{"rules", QJsonArray{QJsonObject{{"method", "PUT"}, {"key", "prefix é/blocked.txt"}, {"barrier", "lane"}}}}});
        std::atomic_bool blockedDone{false}, freeDone{false};
        QString blockedStatus, freeStatus;
        f.provider.upload(blocked, [&](auto result) { blockedStatus = result.status; blockedDone = true; });
        QTRY_COMPARE(f.events("PUT").size(), 1);
        // A second upload completes on another lane while the first is still in flight.
        f.provider.upload(free, [&](auto result) { freeStatus = result.status; freeDone = true; });
        QTRY_VERIFY_WITH_TIMEOUT(freeDone.load(), 15000);
        QCOMPARE(freeStatus, QString("uploaded"));
        QVERIFY(!blockedDone.load());
        QCOMPARE(f.provider.diagnostics()["lanes"].toInt(), 2);
        f.control("/__release", {{"barrier", "lane"}}, true);
        QTRY_VERIFY_WITH_TIMEOUT(blockedDone.load(), 15000);
        QCOMPARE(blockedStatus, QString("uploaded"));
        // An idle provider reuses its lanes; their drivers are released once idle.
        QTRY_COMPARE(f.provider.diagnostics()["queuedRequests"].toInt(), 0);
        QCOMPARE(f.provider.diagnostics()["retainedDrivers"].toInt(), 0);
    }
    void providerDestructionDrainsSentRequestBeforeRuntime() {
        S3Fixture f;
        auto request = f.request();
        f.reset({{"rules", QJsonArray{QJsonObject{{"method", "PUT"}, {"barrier", "shutdown"}}}}});
        auto provider = std::make_unique<S3Provider>(f.credentials);
        std::atomic_bool completed{false};
        QString status;
        provider->upload(request, [&](auto result) { status = result.status; completed.store(true); });
        QTRY_COMPARE(f.events("PUT").size(), 1);
        QVERIFY(!completed.load());
        std::thread release([&] { f.control("/__release", {{"barrier", "shutdown"}}, true); });
        provider.reset();
        release.join();
        QVERIFY(completed.load());
        QCOMPARE(status, QString("uploaded"));
        QCOMPARE(f.events("PUT").size(), 1);
        QVERIFY(f.events("PUT")[0].toObject()["signatureValid"].toBool());
    }
    void explicitSyntheticDestinationTest_data() {
        QTest::addColumn<QString>("publicBody");
        QTest::addColumn<bool>("reachable");
        QTest::addColumn<bool>("cleanupDenied");
        QTest::newRow("matching-public-bytes") << QString{} << true << false;
        QTest::newRow("wrong-public-bytes") << QString("unexpected public content") << false << false;
        QTest::newRow("oversized-public-response") << QString(1024 * 1024, 'x') << false << false;
        QTest::newRow("public-and-cleanup-failed") << QString("unexpected public content") << false << true;
    }
    void explicitSyntheticDestinationTest() {
        QFETCH(QString, publicBody);
        QFETCH(bool, reachable);
        QFETCH(bool, cleanupDenied);
        S3Fixture f;
        QJsonArray rules;
        if (!publicBody.isEmpty()) rules.append(QJsonObject{{"method", "GET"}, {"responseBody", publicBody}, {"remaining", 1}});
        if (cleanupDenied) rules.append(QJsonObject{{"method", "DELETE"}, {"status", 403}, {"code", "AccessDenied"}});
        f.reset({{"rules", rules}});
        std::atomic_bool done{false};
        QJsonObject result;
        Error error;
        f.provider.testDestination(f.destination, [](auto, auto ack) { ack({}); }, [&](auto value, auto e) { result = value; error = e; done = true; });
        QTRY_VERIFY(done.load());
        if (reachable) QVERIFY2(!error, qPrintable(error.code));
        else {QCOMPARE(error.code, QString("public_url_unreachable"));QVERIFY(error.message.contains("Link domain"));QVERIFY(error.message.contains("public-read"));}
        QCOMPARE(result["publicReachable"].toBool(), reachable);
        QCOMPARE(result["cleanupStatus"].toString(), cleanupDenied ? QString("failed") : QString("removed"));
        if (cleanupDenied) {QVERIFY(error.message.contains("Cleanup also failed"));QVERIFY(error.message.contains("s3:DeleteObject"));}
        bool unauthenticated = false;
        for (auto event : f.events("GET"))
            if (!event.toObject()["headers"].toObject().contains("authorization")) unauthenticated = true;
        QVERIFY(unauthenticated);
    }
};
QTEST_GUILESS_MAIN(S3Test)
#include "test_s3.moc"
