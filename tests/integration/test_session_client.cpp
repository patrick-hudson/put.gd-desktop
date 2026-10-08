#include "client/SessionClient.h"
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocalServer>
#include <QLocalSocket>
#include <QPointer>
#include <QSet>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QUuid>
#include <iterator>
#include <memory>

using namespace putgd;
namespace {
QJsonObject envelope(const QJsonObject& request, const QJsonObject& result = {}) {
    return {{"schemaVersion", 1}, {"requestId", request["requestId"]}, {"ok", true},
            {"result", result}, {"error", Error{}.toJson()}};
}
QByteArray frame(const QJsonObject& object) {
    return QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n';
}
void respond(QLocalSocket* peer, const QJsonObject& request, const QJsonObject& result = {}) {
    peer->write(frame(envelope(request, result)));
    peer->disconnectFromServer();
}
class PeerServer : public QObject {
public:
    PeerServer() {
        server.setMaxPendingConnections(64);
        server.setListenBacklogSize(128);
        connect(&server, &QLocalServer::newConnection, this, [this] {
            while (auto* peer = server.nextPendingConnection()) {
                peers.append(peer);
                connect(peer, &QLocalSocket::readyRead, this, [this, peer] {
                    if (peer->property("handled").toBool()) return;
                    auto bytes = peer->property("request").toByteArray() + peer->readAll();
                    if (!bytes.contains('\n')) {
                        peer->setProperty("request", bytes);
                        return;
                    }
                    peer->setProperty("handled", true);
                    peer->setProperty("request", QByteArray{});
                    const auto request = QJsonDocument::fromJson(bytes).object();
                    requests.append(request);
                    if (onRequest) onRequest(peer, request);
                });
            }
        });
    }
    QString path() const { return directory.filePath("session.sock"); }
    bool listen() { return directory.isValid() && server.listen(path()); }

    QTemporaryDir directory;
    QLocalServer server;
    QList<QPointer<QLocalSocket>> peers;
    QList<QJsonObject> requests;
    std::function<void(QLocalSocket*, QJsonObject)> onRequest;
};
}
class SessionClientContract : public QObject {
    Q_OBJECT
private slots:
    void fragmentedReplyKeepsEventLoopResponsive() {
        PeerServer server;
        QVERIFY(server.listen());
        server.onRequest = [](QLocalSocket* peer, const QJsonObject& request) {
            const auto bytes = frame(envelope(request, {{"ready", true}}));
            peer->write(bytes.left(bytes.size() / 2));
            QTimer::singleShot(80, peer, [peer, bytes] {
                peer->write(bytes.mid(bytes.size() / 2));
                peer->disconnectFromServer();
            });
        };
        SessionClient client(server.path());
        int callbacks = 0;
        int heartbeats = 0;
        Error error;
        QJsonObject result;
        QTimer heartbeat;
        heartbeat.setInterval(5);
        connect(&heartbeat, &QTimer::timeout, this, [&] { ++heartbeats; });
        heartbeat.start();
        const auto id = client.request("capabilities", {{"fixture", true}}, this,
            [&](Error e, QJsonObject r) { ++callbacks; error = e; result = r; });
        QVERIFY(id != 0);
        QCOMPARE(callbacks, 0);
        QTRY_COMPARE_WITH_TIMEOUT(callbacks, 1, 1000);
        QVERIFY2(!error, qPrintable(error.code));
        QVERIFY(result["ready"].toBool());
        QVERIFY(heartbeats >= 3);
        QCOMPARE(server.requests.size(), 1);
        const auto received = server.requests.front();
        QCOMPARE(received["schemaVersion"].toInt(), 1);
        QCOMPARE(received["session"].toString(), server.path());
        QCOMPARE(received["method"].toString(), QString("capabilities"));
        QCOMPARE(received["params"].toObject(), QJsonObject({{"fixture", true}}));
        QVERIFY(!QUuid(received["requestId"].toString()).isNull());
        QCOMPARE(client.socketPath(), server.path());
        QTest::qWait(50);
        QCOMPARE(callbacks, 1);
    }

    void errorReplyPreservesResidentDetails() {
        PeerServer server;
        QVERIFY(server.listen());
        const Error expected{"revision_conflict", "Settings changed", {{"revision", "synthetic-revision"}}};
        server.onRequest = [&](QLocalSocket* peer, const QJsonObject& request) {
            auto reply = envelope(request, {{"accepted", false}});
            reply["ok"] = false;
            reply["error"] = expected.toJson();
            peer->write(frame(reply));
            peer->disconnectFromServer();
        };
        SessionClient client(server.path());
        int callbacks = 0;
        Error error;
        QJsonObject result;
        client.request("settings.save", {}, this, [&](Error e, QJsonObject r) {
            ++callbacks; error = e; result = r;
        });
        QTRY_COMPARE(callbacks, 1);
        QCOMPARE(error.toJson(), expected.toJson());
        QCOMPARE(result, QJsonObject({{"accepted", false}}));
    }

    void invalidReply_data() {
        QTest::addColumn<int>("variant");
        const char* names[] = {"malformed-json", "array", "wrong-schema", "string-schema",
            "wrong-id", "numeric-id", "missing-ok", "numeric-ok", "array-result", "missing-result",
            "missing-error", "string-error", "numeric-error-code", "missing-error-message",
            "array-error-details", "success-with-error", "failure-without-error", "duplicate-frame",
            "trailing-bytes"};
        for (int index = 0; index < int(std::size(names)); ++index)
            QTest::newRow(names[index]) << index;
    }
    void invalidReply() {
        QFETCH(int, variant);
        PeerServer server;
        QVERIFY(server.listen());
        server.onRequest = [variant](QLocalSocket* peer, const QJsonObject& request) {
            auto reply = envelope(request);
            auto error = Error{}.toJson();
            QByteArray raw;
            switch (variant) {
            case 0: raw = "{malformed-DO_NOT_ECHO_RAW\n"; break;
            case 1: raw = "[]\n"; break;
            case 2: reply["schemaVersion"] = 2; break;
            case 3: reply["schemaVersion"] = "1"; break;
            case 4: reply["requestId"] = "DO_NOT_ECHO_RAW"; break;
            case 5: reply["requestId"] = 42; break;
            case 6: reply.remove("ok"); break;
            case 7: reply["ok"] = 1; break;
            case 8: reply["result"] = QJsonArray{}; break;
            case 9: reply.remove("result"); break;
            case 10: reply.remove("error"); break;
            case 11: reply["error"] = "DO_NOT_ECHO_RAW"; break;
            case 12: error["code"] = 42; reply["error"] = error; break;
            case 13: error.remove("message"); reply["error"] = error; break;
            case 14: error["details"] = QJsonArray{}; reply["error"] = error; break;
            case 15: error["code"] = "unexpected"; reply["error"] = error; break;
            case 16: reply["ok"] = false; break;
            case 17: raw = frame(reply) + frame(reply); break;
            case 18: raw = frame(reply) + "DO_NOT_ECHO_RAW"; break;
            }
            peer->write(raw.isEmpty() ? frame(reply) : raw);
            peer->disconnectFromServer();
        };
        SessionClient client(server.path());
        int callbacks = 0;
        Error error;
        client.request("settings.save", {{"marker", "DO_NOT_ECHO_RAW"}}, this,
            [&](Error e, QJsonObject result) {
                ++callbacks; error = e; QVERIFY(result.isEmpty());
            });
        QCOMPARE(callbacks, 0);
        QTRY_COMPARE(callbacks, 1);
        QCOMPARE(error.code, QString("invalid_response"));
        QVERIFY(error.details["outcomeUnknown"].toBool());
        QVERIFY(!frame(error.toJson()).contains("DO_NOT_ECHO_RAW"));
        QTest::qWait(20);
        QCOMPARE(callbacks, 1);
        QCOMPARE(server.requests.size(), 1);
    }

    void responseLimit_data() {
        QTest::addColumn<bool>("oversized");
        QTest::newRow("exactly-16-MiB") << false;
        QTest::newRow("over-16-MiB") << true;
    }
    void responseLimit() {
        QFETCH(bool, oversized);
        PeerServer server;
        QVERIFY(server.listen());
        server.onRequest = [oversized](QLocalSocket* peer, const QJsonObject& request) {
            auto reply = envelope(request, {{"text", ""}});
            constexpr qsizetype limit = 16 * 1024 * 1024;
            const auto length = limit - frame(reply).size() + (oversized ? 1 : 0);
            reply["result"] = QJsonObject{{"text", QString(length, QChar('x'))}};
            const auto bytes = frame(reply);
            QCOMPARE(bytes.size(), limit + (oversized ? 1 : 0));
            peer->write(bytes);
            peer->disconnectFromServer();
        };
        SessionClient client(server.path());
        Error error;
        int callbacks = 0;
        qsizetype resultLength = 0;
        client.request("history.detail", {}, this, [&](Error e, QJsonObject result) {
            ++callbacks; error = e; resultLength = result["text"].toString().size();
        });
        QTRY_COMPARE_WITH_TIMEOUT(callbacks, 1, 5000);
        if (oversized) {
            QCOMPARE(error.code, QString("invalid_response"));
            QVERIFY(error.details["outcomeUnknown"].toBool());
            QCOMPARE(resultLength, 0);
        } else {
            QVERIFY(!error);
            QVERIFY(resultLength > 15 * 1024 * 1024);
        }
        QTest::qWait(20);
        QCOMPARE(callbacks, 1);
    }

    void requestAtOneMiBLimitIsAccepted() {
        PeerServer server;
        QVERIFY(server.listen());
        SessionClient client(server.path());
        const QJsonObject sample{{"schemaVersion", 1},
            {"requestId", "00000000-0000-0000-0000-000000000000"}, {"session", server.path()},
            {"method", "settings.save"}, {"params", QJsonObject{{"data", ""}}}};
        const auto dataLength = 1024 * 1024 - frame(sample).size();
        server.onRequest = [dataLength](QLocalSocket* peer, const QJsonObject& request) {
            QCOMPARE(frame(request).size(), qsizetype(1024 * 1024));
            QCOMPARE(request["params"].toObject()["data"].toString().size(), dataLength);
            respond(peer, request);
        };
        int callbacks = 0;
        client.request("settings.save", {{"data", QString(dataLength, QChar('x'))}}, this,
            [&](Error error, QJsonObject) { QVERIFY(!error); ++callbacks; });
        QCOMPARE(callbacks, 0);
        QTRY_COMPARE(callbacks, 1);
        QCOMPARE(server.requests.size(), 1);
    }

    void oversizeRequestFailsAsynchronouslyWithoutConnecting() {
        PeerServer server;
        QVERIFY(server.listen());
        SessionClient client(server.path());
        int callbacks = 0;
        Error error;
        client.request("settings.save", {{"data", QString(1024 * 1024, QChar('x'))}}, this,
            [&](Error e, QJsonObject result) { ++callbacks; error = e; QVERIFY(result.isEmpty()); });
        QCOMPARE(callbacks, 0);
        QTRY_COMPARE(callbacks, 1);
        QCOMPARE(error.code, QString("request_too_large"));
        QVERIFY(!error.details.contains("outcomeUnknown"));
        QVERIFY(server.peers.isEmpty());
    }

    void unavailableResidentFailsOnceWithoutUncertainty() {
        QTemporaryDir directory;
        SessionClient client(directory.filePath("absent.sock"));
        Error error;
        int callbacks = 0;
        client.request("action.invoke", {{"action", "clipboard"}}, this,
            [&](Error e, QJsonObject result) { ++callbacks; error = e; QVERIFY(result.isEmpty()); });
        QCOMPARE(callbacks, 0);
        QTRY_COMPARE_WITH_TIMEOUT(callbacks, 1, 1000);
        QCOMPARE(error.code, QString("client_unavailable"));
        QVERIFY(!error.details.contains("outcomeUnknown"));
        QTest::qWait(30);
        QCOMPARE(callbacks, 1);
    }

    void lostReplyMakesMutationUnknownWithoutRetry_data() {
        QTest::addColumn<bool>("partialReply");
        QTest::newRow("no-reply") << false;
        QTest::newRow("incomplete-reply") << true;
    }
    void lostReplyMakesMutationUnknownWithoutRetry() {
        QFETCH(bool, partialReply);
        PeerServer server;
        QVERIFY(server.listen());
        server.onRequest = [partialReply](QLocalSocket* peer, const QJsonObject&) {
            if (partialReply) peer->write("{\"schemaVersion\":1");
            peer->disconnectFromServer();
        };
        SessionClient client(server.path());
        Error error;
        int callbacks = 0;
        client.request("action.invoke", {{"action", "clipboard"}}, this,
            [&](Error e, QJsonObject result) { ++callbacks; error = e; QVERIFY(result.isEmpty()); });
        QTRY_COMPARE(callbacks, 1);
        QCOMPARE(error.code, QString("client_unavailable"));
        QVERIFY(error.details["outcomeUnknown"].toBool());
        QTest::qWait(100);
        QCOMPARE(callbacks, 1);
        QCOMPARE(server.requests.size(), 1);
        QCOMPARE(server.peers.size(), 1);
    }

    void deadlineIsBoundedAndKeepsEventLoopResponsive() {
        PeerServer server;
        QVERIFY(server.listen());
        SessionClient client(server.path());
        int callbacks = 0;
        int heartbeats = 0;
        Error error;
        QTimer heartbeat;
        heartbeat.setInterval(100);
        connect(&heartbeat, &QTimer::timeout, this, [&] { ++heartbeats; });
        heartbeat.start();
        QElapsedTimer elapsed;
        elapsed.start();
        client.request("settings.save", {}, this,
            [&](Error e, QJsonObject) { ++callbacks; error = e; });
        QTRY_COMPARE_WITH_TIMEOUT(callbacks, 1, 18000);
        QVERIFY(elapsed.elapsed() >= 14900);
        QVERIFY(elapsed.elapsed() < 18000);
        QVERIFY(heartbeats >= 100);
        QCOMPARE(error.code, QString("client_timeout"));
        QVERIFY(error.details["outcomeUnknown"].toBool());
        QTest::qWait(100);
        QCOMPARE(callbacks, 1);
        QCOMPARE(server.requests.size(), 1);
        QCOMPARE(server.peers.size(), 1);
    }

    void concurrentRepliesStayCorrelated() {
        PeerServer server;
        QVERIFY(server.listen());
        server.onRequest = [](QLocalSocket* peer, const QJsonObject& request) {
            const auto number = request["params"].toObject()["number"].toInt();
            QTimer::singleShot((12 - number) * 8, peer,
                [peer, request, number] { respond(peer, request, {{"number", number}}); });
        };
        SessionClient client(server.path());
        QSet<SessionClient::RequestId> ids;
        QList<int> completionOrder;
        for (int number = 0; number < 12; ++number) {
            const auto id = client.request("operation.inspect", {{"number", number}}, this,
                [&, number](Error error, QJsonObject result) {
                    QVERIFY(!error);
                    QCOMPARE(result["number"].toInt(), number);
                    completionOrder.append(number);
                });
            QVERIFY(!ids.contains(id));
            ids.insert(id);
        }
        QVERIFY(completionOrder.isEmpty());
        QTRY_COMPARE(completionOrder.size(), 12);
        QVERIFY(completionOrder.first() > completionOrder.last());
        QSet<QString> wireIds;
        for (const auto& request : server.requests) wireIds.insert(request["requestId"].toString());
        QCOMPARE(wireIds.size(), 12);
    }

    void concurrencyLimitAndAbandonReleaseCapacity() {
        PeerServer server;
        QVERIFY(server.listen());
        SessionClient client(server.path());
        QList<SessionClient::RequestId> ids;
        int acceptedCallbacks = 0;
        for (int index = 0; index < 64; ++index)
            ids.append(client.request("operation.inspect", {}, this,
                [&](Error, QJsonObject) { ++acceptedCallbacks; }));
        int busyCallbacks = 0;
        Error busy;
        client.request("operation.inspect", {}, this,
            [&](Error error, QJsonObject) { ++busyCallbacks; busy = error; });
        QCOMPARE(busyCallbacks, 0);
        QTRY_COMPARE(busyCallbacks, 1);
        QCOMPARE(busy.code, QString("client_busy"));
        QVERIFY(!busy.details.contains("outcomeUnknown"));
        QTRY_COMPARE_WITH_TIMEOUT(server.requests.size(), 64, 4000);
        QCOMPARE(server.peers.size(), 64);
        client.abandon(ids.takeFirst());
        server.onRequest = [](QLocalSocket* peer, const QJsonObject& request) { respond(peer, request); };
        int healthyCallbacks = 0;
        client.request("capabilities", {}, this, [&](Error error, QJsonObject) {
            QVERIFY(!error); ++healthyCallbacks;
        });
        QTRY_COMPARE(healthyCallbacks, 1);
        QCOMPARE(acceptedCallbacks, 0);
        QCOMPARE(server.requests.size(), 65);
        for (const auto id : ids) client.abandon(id);
    }

    void receiverDeathAndAbandonSuppressDelivery_data() {
        QTest::addColumn<bool>("destroyReceiver");
        QTest::newRow("receiver-destruction") << true;
        QTest::newRow("explicit-abandon") << false;
    }
    void receiverDeathAndAbandonSuppressDelivery() {
        QFETCH(bool, destroyReceiver);
        PeerServer server;
        QVERIFY(server.listen());
        SessionClient client(server.path());
        auto receiver = std::make_unique<QObject>();
        int callbacks = 0;
        const auto id = client.request("action.invoke", {{"action", "clipboard"}}, receiver.get(),
            [&](Error, QJsonObject) { ++callbacks; });
        QTRY_COMPARE(server.requests.size(), 1);
        if (destroyReceiver) receiver.reset();
        else client.abandon(id);
        QTRY_COMPARE(server.peers.front()->state(), QLocalSocket::UnconnectedState);
        QTest::qWait(30);
        QCOMPARE(callbacks, 0);
        QCOMPARE(server.requests.size(), 1); // No operation.cancel or other follow-up was sent.
        server.onRequest = [](QLocalSocket* peer, const QJsonObject& request) { respond(peer, request); };
        client.request("capabilities", {}, this, [&](Error error, QJsonObject) {
            QVERIFY(!error); ++callbacks;
        });
        QTRY_COMPARE(callbacks, 1);
    }

    void queuedFailuresCanBeAbandonedAndReceiverDestroyed() {
        PeerServer server;
        QVERIFY(server.listen());
        SessionClient client(server.path());
        int callbacks = 0;
        const auto beforeConnection = client.request("capabilities", {}, this,
            [&](Error, QJsonObject) { ++callbacks; });
        client.abandon(beforeConnection);
        const QJsonObject large{{"data", QString(1024 * 1024, QChar('x'))}};
        const auto id = client.request("settings.save", large, this,
            [&](Error, QJsonObject) { ++callbacks; });
        client.abandon(id);
        auto receiver = std::make_unique<QObject>();
        client.request("settings.save", large, receiver.get(),
            [&](Error, QJsonObject) { ++callbacks; });
        receiver.reset();
        QTest::qWait(30);
        QCOMPARE(callbacks, 0);
        QVERIFY(server.peers.isEmpty());
    }

    void clientDestructionStopsPendingDelivery() {
        PeerServer server;
        QVERIFY(server.listen());
        auto client = std::make_unique<SessionClient>(server.path());
        int callbacks = 0;
        client->request("operation.inspect", {}, this, [&](Error, QJsonObject) { ++callbacks; });
        QTRY_COMPARE(server.requests.size(), 1);
        client.reset();
        QTRY_COMPARE(server.peers.front()->state(), QLocalSocket::UnconnectedState);
        QTest::qWait(30);
        QCOMPARE(callbacks, 0);
    }

    void clientDestructionWithOwnedReceiver_data() {
        QTest::addColumn<bool>("useChild");
        QTest::newRow("client-is-receiver") << false;
        QTest::newRow("child-is-receiver") << true;
    }
    void clientDestructionWithOwnedReceiver() {
        QFETCH(bool, useChild);
        PeerServer server;
        QVERIFY(server.listen());
        auto client = std::make_unique<SessionClient>(server.path());
        QObject* receiver = useChild ? new QObject(client.get()) : client.get();
        int callbacks = 0;
        client->request("operation.inspect", {}, receiver, [&](Error, QJsonObject) { ++callbacks; });
        QTRY_COMPARE(server.requests.size(), 1);
        client.reset();
        QTRY_COMPARE(server.peers.front()->state(), QLocalSocket::UnconnectedState);
        QTest::qWait(30);
        QCOMPARE(callbacks, 0);
    }

    void callbackMayDeleteClient() {
        PeerServer server;
        QVERIFY(server.listen());
        server.onRequest = [](QLocalSocket* peer, const QJsonObject& request) { respond(peer, request); };
        auto client = std::make_unique<SessionClient>(server.path());
        int callbacks = 0;
        client->request("capabilities", {}, client.get(), [&](Error error, QJsonObject) {
            QVERIFY(!error); ++callbacks; client.reset();
        });
        QTRY_COMPARE(callbacks, 1);
        QVERIFY(!client);
        QTest::qWait(30);
        QCOMPARE(callbacks, 1);
    }
};
QTEST_GUILESS_MAIN(SessionClientContract)
#include "test_session_client.moc"
