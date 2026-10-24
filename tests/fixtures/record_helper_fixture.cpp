// A stand-in for `putgd-record`, used only by recording_helper_integration to
// prove the resident's supervision: handshake, command/result framing, events,
// timeouts, crashes and the required normal exit. It records nothing. The
// product helper never reads PUTGD_TEST_RECORD_MODE.
#include "app/UiControl.h"
#include "core/BuildInfo.h"
#include "ui/StdioDevice.h"
#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTimer>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/resource.h>

using namespace putgd;
int main(int argc, char** argv) {
    const rlimit noCore{0, 0};
    ::setrlimit(RLIMIT_CORE, &noCore);
    const auto mode = qEnvironmentVariable("PUTGD_TEST_RECORD_MODE");
    if (argc == 2 && std::strcmp(argv[1], "--capabilities") == 0) {
        if (mode == "probe-garbage") { std::printf("not json\n"); return 0; }
        if (mode == "probe-fails") return 9;
        const QJsonObject caps{{"schemaVersion", 1}, {"available", true}, {"targets", QJsonArray{"monitor", "desktop", "region", "window"}},
            {"formats", QJsonArray{"mp4", "webm", "gif", "webp"}}, {"codecs", QJsonObject{{"mp4", QJsonArray{"h264", "h265"}}, {"webm", QJsonArray{"vp9"}}}},
            {"audio", QJsonObject{{"system", true}, {"microphone", false}}}, {"overlayExclusion", QJsonObject{{"region", true}}}, {"fixture", mode}};
        const auto json = QJsonDocument(caps).toJson(QJsonDocument::Compact);
        std::fwrite(json.constData(), 1, size_t(json.size()), stdout);
        std::fputc('\n', stdout);
        return 0;
    }
    QCoreApplication app(argc, argv);
    StdioDevice pipes;
    if (!pipes.isOpen()) return 2;
    UiControl control(pipes);
    QJsonObject envelope;
    bool started = false;
    QObject::connect(&pipes, &QIODevice::readChannelFinished, &app, [&] { app.exit(5); });
    QObject::connect(&control, &UiControl::failed, &app, [&] { app.exit(3); });
    auto result = [&](const QString& requestId, const QJsonObject& value) {
        auto frame = envelope;
        frame["requestId"] = requestId;
        frame["ok"] = true;
        frame["result"] = value;
        control.send("commandResult", frame);
    };
    auto event = [&](const QString& name, QJsonObject extra) {
        auto frame = envelope;
        frame["event"] = name;
        for (auto it = extra.begin(); it != extra.end(); ++it) frame[it.key()] = it.value();
        control.send("event", frame);
    };
    QObject::connect(&control, &UiControl::frame, &app, [&](const QString& type, const QString&, const QJsonObject& payload) {
        if (type == "bootstrap") {
            envelope = {{"role", "record"}, {"nonce", payload["nonce"]}, {"generation", payload["generation"]}};
            if (mode == "silent") return;
            // #198: the resident always passes the resolved scheme.
            const auto scheme = payload["appearance"].toObject()["scheme"].toString();
            if (scheme != "dark" && scheme != "light") { app.exit(3); return; }
            auto ready = envelope;
            ready["build"] = mode == "wrong-build" ? QJsonObject{{"version", "0.0.0-other"}} : payload["build"];
            control.send("ready", ready);
            return;
        }
        if (type == "appearance") return;
        // Keep teardown alive while the resident reads late replies, as the real helper does.
        if (type == "shutdown") { QTimer::singleShot(mode.startsWith("slow-shutdown") ? 300 : 0, &app, [&] { app.exit(mode == "exit-dirty" ? 7 : 0); }); return; }
        if (type != "command") { app.exit(3); return; }
        const auto requestId = payload["requestId"].toString(), command = payload["command"].toString();
        const auto params = payload["params"].toObject();
        if (command == "prepare") { result(requestId, {{"canvas", QJsonArray{1280, 720}}, {"staging", params["staging"]}}); return; }
        if (command == "start") {
            started = true;
            if (mode == "unknown-result") { result("not-outstanding", {}); return; }
            if (mode == "malformed-result") { auto frame = envelope; frame["requestId"] = requestId; frame["ok"] = "yes"; frame["result"] = QJsonObject{}; control.send("commandResult", frame); return; }
            result(requestId, {});
            if (mode == "crash-after-start") { QTimer::singleShot(100, &app, [] { std::abort(); }); return; }
            event("progress", {{"durationUs", 1000000.0}, {"bytes", 4321.0}, {"droppedFrames", 0}});
            if (mode == "stop-event") QTimer::singleShot(150, &app, [&] { event("stopped", {{"durationUs", 9000000.0}, {"canvas", QJsonArray{1280, 720}}}); });
            return;
        }
        if (command == "stop") {
            if (mode == "stop-budget-race") {
                event("stopped", {{"reason", "budget"}, {"durationUs", 9000000.0}, {"canvas", QJsonArray{1280, 720}}});
                QTimer::singleShot(100, &app, [&, requestId] { result(requestId, {{"durationUs", 12000000.0}, {"canvas", QJsonArray{1280, 720}}}); });
            } else result(requestId, {{"durationUs", 12000000.0}, {"canvas", QJsonArray{1280, 720}}});
            return;
        }
        if (command == "cancel" && mode.startsWith("slow-shutdown")) {
            QTimer::singleShot(50, &app, [&, requestId] {
                if (mode == "slow-shutdown-foreign") envelope["nonce"] = "foreign";
                result(requestId, {});
            });
            return;
        }
        if (command == "preview") { result(requestId, {{"png", QString::fromLatin1(QByteArray("\x89PNG\r\n\x1a\n", 8).toBase64())}, {"time", params["time"]}}); return; }
        if (command == "export") {
            if (mode == "slow-export") return;
            const QByteArray bytes = QByteArray("\0\0\0\x18" "ftypisom", 12) + QByteArray(2048, 'f');
            QFile output(params["output"].toString());
            if (!output.open(QIODevice::WriteOnly | QIODevice::Truncate) || output.write(bytes) != bytes.size()) { app.exit(4); return; }
            output.close();
            result(requestId, {{"bytes", double(bytes.size())}, {"durationUs", 6000000.0}});
            return;
        }
        if (QStringList{"pause", "resume", "cancel", "raise", "overlay", "notice"}.contains(command)) { result(requestId, {}); return; }
        if (command == "review") { result(requestId, {{"action", params["action"]}, {"visible", true}}); return; }
        auto frame = envelope;
        frame["requestId"] = requestId;
        frame["ok"] = false;
        frame["code"] = "unknown_method";
        frame["message"] = "fixture does not know this command";
        frame["details"] = QJsonObject{};
        control.send("commandResult", frame);
    });
    return app.exec();
}
