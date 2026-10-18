// putgd-record: the transient screen-recording helper. The resident launches
// one per recording session and speaks UiControl frames over private pipes;
// `--capabilities` prints the media inventory and exits. This build carries the
// identity handshake, command/event framing and the probe; capture, review and
// export arrive with the next slices and until then every session command is
// refused as recording_unsupported.
#include "MediaProbe.h"
#include "app/UiControl.h"
#include "core/BuildInfo.h"
#include "ui/StdioDevice.h"
#include <QCoreApplication>
#include <QJsonDocument>
#include <QTimer>
#include <cstdio>
#include <cstring>
#include <sys/resource.h>

using namespace putgd;
namespace {
const QStringList sessionCommands{"prepare", "start", "pause", "resume", "stop", "cancel", "raise", "overlay", "preview", "export"};
}

int main(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "--version-json") == 0) { std::printf("%s\n", buildManifestJson()); return 0; }
    if (argc == 2 && (std::strcmp(argv[1], "--version") == 0 || std::strcmp(argv[1], "-v") == 0)) { std::printf("putgd-record %s\n", buildVersion()); return 0; }
    const rlimit noCore{0, 0};
    ::setrlimit(RLIMIT_CORE, &noCore);
    if (argc == 2 && std::strcmp(argv[1], "--capabilities") == 0) {
        QCoreApplication app(argc, argv);
        const auto json = QJsonDocument(record::probeMedia()).toJson(QJsonDocument::Compact);
        std::fwrite(json.constData(), 1, size_t(json.size()), stdout);
        std::fputc('\n', stdout);
        return 0;
    }
    if (argc != 1) return 2;
    UiControlPipes handles;
    if (!handles.valid()) return 2;
    QCoreApplication app(argc, argv);
    QCoreApplication::setOrganizationName("putgd");
    QCoreApplication::setApplicationName("putgd-record");
    QCoreApplication::setApplicationVersion(buildVersion());
    StdioDevice pipes(handles.input(), handles.output());
    if (!pipes.isOpen()) return 2;
    UiControl control(pipes);
    const auto build = QJsonDocument::fromJson(buildManifestJson()).object();
    QJsonObject envelope;
    bool ready = false;
    QObject::connect(&pipes, &QIODevice::readChannelFinished, &app, [&] { app.exit(5); });
    QObject::connect(&control, &UiControl::failed, &app, [&] { app.exit(3); });
    auto result = [&](const QString& requestId, const QJsonObject& value) {
        auto frame = envelope;
        frame["requestId"] = requestId;
        frame["ok"] = true;
        frame["result"] = value;
        control.send("commandResult", frame);
    };
    auto failure = [&](const QString& requestId, const QString& code, const QString& message, const QJsonObject& details = {}) {
        auto frame = envelope;
        frame["requestId"] = requestId;
        frame["ok"] = false;
        frame["code"] = code;
        frame["message"] = message;
        frame["details"] = details;
        control.send("commandResult", frame);
    };
    QObject::connect(&control, &UiControl::frame, &app, [&](const QString& type, const QString&, const QJsonObject& payload) {
        if (type == "bootstrap") {
            if (ready || payload["role"].toString() != "record" || !payload["nonce"].isString() || !payload["generation"].isString() || !payload["build"].isObject()) { app.exit(3); return; }
            envelope = {{"role", "record"}, {"nonce", payload["nonce"]}, {"generation", payload["generation"]}};
            ready = true;
            auto frame = envelope;
            frame["build"] = build;
            control.send("ready", frame);
            return;
        }
        if (!ready || payload["role"] != envelope["role"] || payload["nonce"] != envelope["nonce"] || payload["generation"] != envelope["generation"]) { app.exit(3); return; }
        if (type == "shutdown") { QTimer::singleShot(0, &app, &QCoreApplication::quit); return; }
        if (type != "command" || !payload["requestId"].isString() || !payload["command"].isString() || !payload["params"].isObject()) { app.exit(3); return; }
        const auto requestId = payload["requestId"].toString(), command = payload["command"].toString();
        if (command == "capabilities") { result(requestId, record::probeMedia()); return; }
        if (sessionCommands.contains(command)) { failure(requestId, "recording_unsupported", "Screen capture is not implemented in this build", {{"reason", "capture_not_implemented"}}); return; }
        failure(requestId, "unknown_method", "Unknown recording helper command");
    });
    return app.exec();
}
