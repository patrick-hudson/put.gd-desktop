#include "app/UiControl.h"
#include "core/BuildInfo.h"
#include "storage/s3/S3WorkerProtocol.h"
#include "ui/StdioDevice.h"
#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>
#include <QSaveFile>
#include <QTimer>
#include <csignal>
#include <functional>
#ifdef Q_OS_UNIX
#include <unistd.h>
#endif

using namespace putgd;
namespace {
QByteArray frame(const QString& type, const QString& sequence, const QJsonObject& payload) {
    return QJsonDocument(QJsonObject{{"version", 1}, {"type", type}, {"sequence", sequence}, {"payload", payload}})
               .toJson(QJsonDocument::Compact) + '\n';
}
}
int main(int argc, char** argv) {
    // The fixture exercises the production private-pipe framing and has no SDK,
    // credential lookup, sockets or network requests.
    UiControlPipes handles;
    if (!handles.valid() || argc != 1) return 2;
    QCoreApplication app(argc, argv);
    StdioDevice pipes(handles.input(), handles.output());
    if (!pipes.isOpen()) return 2;
    UiControl control(pipes);
#ifdef Q_OS_UNIX
    std::signal(SIGPIPE, SIG_IGN);
#endif
    const auto mode = qEnvironmentVariable("PUTGD_S3_FAULT");
    const auto statePath = qEnvironmentVariable("PUTGD_S3_FAULT_STATE");
    if (mode.isEmpty() || statePath.isEmpty()) return 2;
    QJsonObject state{{"pid", QString::number(QCoreApplication::applicationPid())},
                      {"activated", false}, {"resultAck", false}, {"shutdown", false}};
    auto saveState = [&] {
        QSaveFile file(statePath);
        file.setDirectWriteFallback(false);
        if (!file.open(QIODevice::WriteOnly) || !file.setPermissions(QFile::ReadOwner | QFile::WriteOwner)) return false;
        const auto bytes = QJsonDocument(state).toJson(QJsonDocument::Compact);
        return file.write(bytes) == bytes.size() && file.commit();
    };
    if (!saveState()) return 2;
    QJsonObject identity;
    auto envelope = [&] {
        return QJsonObject{{"role", identity["role"]}, {"nonce", identity["nonce"]}, {"generation", identity["generation"]}};
    };
    auto valid = [&](const QJsonObject& object) {
        return object["role"] == identity["role"] && object["nonce"] == identity["nonce"] &&
               object["generation"] == identity["generation"];
    };
    QByteArray raw;
    qsizetype rawOffset = 0;
    bool exitAfterRaw = false;
    std::function<void()> flushRaw = [&] {
        while (rawOffset < raw.size()) {
            const auto size = qMin(qsizetype(16384), raw.size() - rawOffset);
            const auto written = pipes.write(raw.constData() + rawOffset, size);
            if (written < 0) { app.exit(3); return; }
            if (!written) return;
            rawOffset += written;
        }
        if (exitAfterRaw) QTimer::singleShot(0, &app, [&] { app.exit(0); });
    };
    QObject::connect(&pipes, &QIODevice::bytesWritten, &app, [&] { flushRaw(); });
    QObject::connect(&pipes, &QIODevice::readChannelFinished, &app, [&] { app.exit(3); });
    QObject::connect(&control, &UiControl::failed, &app, [&] { app.exit(3); });
    QTimer heartbeat;
    heartbeat.setInterval(50);
    QObject::connect(&heartbeat, &QTimer::timeout, &app, [&] {
        auto alive = envelope(); alive["kind"] = "alive"; alive["data"] = QJsonObject{};
        control.send("observed", alive);
    });
    QTimer::singleShot(30000, &app, [&] { app.exit(4); });
    QObject::connect(&control, &UiControl::frame, &app,
        [&](const QString& type, const QString&, const QJsonObject& object) {
            if (type == "bootstrap" && identity.isEmpty()) {
                if (object.size() != 7 || object["role"] != "s3-worker" || !object["nonce"].isString() ||
                    !object["generation"].isString() || !object["build"].isObject() || !object["contract"].isString() ||
                    !object["safeDelete"].isBool() || !object["modulePath"].isString()) { app.exit(3); return; }
                identity = object;
                auto ready = envelope();
                ready["build"] = QJsonDocument::fromJson(buildManifestJson()).object();
                ready["contract"] = object["contract"];
                ready["safeDelete"] = object["safeDelete"];
                if (mode == "wrong_build") ready["build"] = QJsonObject{{"fixture", "incompatible-build"}};
                if (mode == "wrong_nonce") ready["nonce"] = "00000000-0000-0000-0000-000000000001";
                if (mode == "duplicate_ready") {
                    // Both complete frames fit in one atomic pipe write, so the
                    // parent observes the duplicate in the same input batch.
                    raw = frame("ready", "1", ready) + frame("ready", "2", ready);
                    flushRaw();
                } else control.send("ready", ready);
                return;
            }
            if (identity.isEmpty() || !valid(object)) { app.exit(3); return; }
            if (type == "shutdown" && object["cancel"] == true) {
                state["shutdown"] = true;
                if (!saveState()) { app.exit(3); return; }
                if (mode == "cancel_ignores_alive") return;
                app.exit(0);
                return;
            }
            if (type == "activate" && !state["activated"].toBool()) {
                const auto method = object["method"].toString();
                if ((method != "checkName" && !(mode == "cancel_ignores_alive" && method == "upload")) || !object["request"].isObject() ||
                    !s3wire::validateRequest(method, object["request"].toObject())) { app.exit(3); return; }
                state["activated"] = true;
                if (!saveState()) { app.exit(3); return; }
                if (mode == "ready_ignores_activate") return;
                auto activated = envelope();
                activated["kind"] = "activated";
                activated["data"] = QJsonObject{{"method", object["method"].toString()}};
                control.send("observed", activated);
                if (mode == "cancel_ignores_alive") { heartbeat.start(); return; }
                if (mode == "activated_silent") return;
                if (mode == "ready_then_crash") { app.exit(77); return; }
                if (mode == "normal_missing_terminal") { app.exit(0); return; }
                if (mode == "oversized") {
                    raw = QByteArray(UiControl::maximumFrameBytes + 1, 'x');
                    exitAfterRaw = true;
                    flushRaw();
                    return;
                }
                if (mode == "truncated") {
                    raw = "{\"version\":1,\"type\":\"checkpoint\",\"sequence\":\"2\",\"payload\":";
                    exitAfterRaw = true;
                    flushRaw();
                    return;
                }
                const auto request = object["request"].toObject();
                const auto destination = request["destination"].toObject();
                auto prefix = destination["settings"].toObject()["objectPrefix"].toString();
                if (!prefix.isEmpty() && !prefix.endsWith('/')) prefix += '/';
                NameCheck result{"absent", {{"schemaVersion", 1}, {"key", prefix + request["candidate"].toString()}}, {}, {}};
                auto terminal = envelope();
                terminal["kind"] = "result";
                terminal["data"] = s3wire::encode(result);
                if (mode == "activated_wrong_nonce") terminal["nonce"] = "wrong-activation-nonce";
                control.send("checkpoint", terminal);
                return;
            }
            if (type == "checkpointAccepted" && state["activated"].toBool() && object["kind"] == "result" &&
                object["replyTo"] == "terminal" && object["data"].isObject() && object["data"].toObject().isEmpty()) {
                state["resultAck"] = true;
                if (!saveState()) { app.exit(3); return; }
                // A terminal frame is insufficient: the parent must also await
                // successful child exit before reporting any absence result.
                QTimer::singleShot(250, &app, [&] { app.exit(mode == "terminal_then_crash" ? 77 : 0); });
                return;
            }
            app.exit(3);
        });
    return app.exec();
}
