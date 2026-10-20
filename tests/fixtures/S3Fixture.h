#pragma once
#include <QElapsedTimer>
#include "storage/s3/S3Provider.h"
#include <QTest>
#include <QProcess>
#include <QTemporaryDir>
#include <QTcpSocket>
#include <QUrl>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTimer>
#include <QCryptographicHash>
#include <QFile>
#include <atomic>
#include <future>
using namespace putgd;
namespace putgd::test {
class S3Fixture {
public:
    QProcess process;
    QTemporaryDir directory;
    CredentialStore credentials;
    S3Provider provider;
    QString endpoint;
    Destination destination;
    explicit S3Fixture(S3UploadPolicy policy = {}) : provider(credentials, policy) {
        process.start("python3", {S3_FIXTURE_PATH});
        if (!process.waitForStarted() || !process.waitForReadyRead()) qFatal("fixture failed to start");
        endpoint = "http://127.0.0.1:" + QString::fromUtf8(process.readLine()).trimmed();
        destination = {"s3", "fixture-profile", "revision-one", Settings::developmentTemplate()["profiles"].toArray()[0].toObject()["settings"].toObject(), {{"mode", "session"}, {"reference", QJsonValue::Null}}};
        destination.settings["endpoint"] = endpoint;
        destination.settings["bucket"] = "isolated-fixture";
        destination.settings["region"] = "eu-west-3";
        destination.settings["objectPrefix"] = "prefix é";
        destination.settings["aclMode"] = "omit";
        destination.settings["publicUrlBase"] = QJsonValue::Null;
        destination.settings["requestTimeoutMs"] = 2000;
        destination.settings["connectTimeoutMs"] = 500;
        destination.settings["lowSpeedTimeSeconds"] = 1;
        credentials.setSession(destination.profileId, {"PUTGD_SYNTHETIC_ACCESS", "putgd-synthetic-secret-not-a-real-key", {}});
    }
    ~S3Fixture() { process.terminate(); process.waitForFinished(); }
    QJsonObject control(const QString& path, const QJsonObject& value = {}, bool post = false) {
        // Fixture control is plain, bounded HTTP over its private loopback socket.
        // It stays independent of QtNetwork's own HTTP worker/lifetime behavior.
        QTcpSocket socket;
        const QUrl address(endpoint);
        socket.connectToHost(address.host(), quint16(address.port()));
        if (!socket.waitForConnected(5000)) qFatal("fixture control connection failed");
        const auto body = post ? QJsonDocument(value).toJson(QJsonDocument::Compact) : QByteArray{};
        const auto request = (post ? QByteArray("POST ") : QByteArray("GET ")) + path.toUtf8() +
            " HTTP/1.1\r\nHost: " + address.authority().toUtf8() +
            "\r\nConnection: close\r\nContent-Type: application/json\r\nContent-Length: " +
            QByteArray::number(body.size()) + "\r\n\r\n" + body;
        if (socket.write(request) != request.size()) qFatal("fixture control write failed");
        QByteArray response;
        QElapsedTimer deadline;
        deadline.start();
        while (deadline.elapsed() < 5000) {
            if (!socket.bytesAvailable()) socket.waitForReadyRead(100);
            response += socket.readAll();
            if (response.size() > 16 * 1024 * 1024) qFatal("fixture control response exceeded bound");
            const auto boundary = response.indexOf("\r\n\r\n");
            if (boundary >= 0) {
                qint64 length = -1;
                for (auto header : response.left(boundary).split('\n')) {
                    header = header.trimmed();
                    if (header.toLower().startsWith("content-length:"))
                        length = header.mid(15).trimmed().toLongLong();
                }
                if (length >= 0 && response.size() - boundary - 4 >= length)
                    return QJsonDocument::fromJson(response.mid(boundary + 4, length)).object();
            }
        }
        qFatal("fixture control response timed out");
        return {};
    }
    void reset(const QJsonObject& value = {}) { control("/__reset", value, true); }
    QJsonObject state() { return control("/__state"); }
    QJsonArray events(const QString& method = {}) {
        QJsonArray result;
        for (auto event : state()["events"].toArray())
            if (method.isEmpty() || event.toObject()["method"] == method) result.append(event);
        return result;
    }
    template<class T, class F> T await(F invoke) {
        auto promise = std::make_shared<std::promise<T>>();
        auto future = promise->get_future();
        invoke([promise](T value) { promise->set_value(std::move(value)); });
        QElapsedTimer deadline;
        deadline.start();
        while (future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
            if (deadline.elapsed() > 15000) qFatal("Provider callback did not complete within fixture deadline");
            QTest::qWait(1);
        }
        return future.get();
    }
    UploadRequest request(const QString& name = "a b.txt", const QByteArray& bytes = "synthetic payload") {
        Artifact artifact;
        artifact.id = "synthetic-item";
        artifact.path = directory.filePath(name);
        artifact.name = name;
        artifact.mime = "text/plain";
        artifact.sha256 = QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
        artifact.bytes = bytes.size();
        QFile file(artifact.path);
        if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size()) qFatal("artifact fixture failed");
        file.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
        auto checked = await<NameCheck>([&](auto callback) { provider.checkName(destination, name, callback); });
        return {destination, artifact, name, "synthetic-operation/item", checked.status == "present" ? "match" : "absent", checked.objectReference, checked.concurrencyToken, {}};
    }
    UploadResult upload(const UploadRequest& request) { return await<UploadResult>([&](auto cb) { provider.upload(request, cb); }); }
    DeleteResult remove(const StorageReceipt& receipt, QJsonObject progress = {}, DurableCheckpoint save = {}) {
        if (!save) save = [](auto, auto ack) { ack({}); };
        return await<DeleteResult>([&](auto cb) { provider.remove({"synthetic-delete", receipt, progress, {}}, save, cb); });
    }
};
}
