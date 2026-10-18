// putgd-record: the transient screen-recording helper. The resident launches
// one per recording session and speaks UiControl frames over private pipes;
// `--capabilities` prints the media inventory and exits. The helper owns the
// live GStreamer capture, the private working clip, frame previews and the
// FFmpeg export; only control metadata crosses the pipes.
#include "MediaProbe.h"
#include "ClipExporter.h"
#include "DebugLog.h"
#include "RecordSession.h"
#include "app/UiControl.h"
#include "core/BuildInfo.h"
#include "ui/StdioDevice.h"
#include <QApplication>
#include <QImage>
#include <QCoreApplication>
#include <QGuiApplication>
#include <gst/gst.h>
#include <QJsonDocument>
#include <QTimer>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/resource.h>

using namespace putgd;

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
    if (argc == 5 && std::strcmp(argv[1], "--preview") == 0) {
        // Lab diagnostic: render one preview frame of a working clip to a PNG.
        QCoreApplication app(argc, argv);
        QImage image;
        qint64 actual = 0;
        RecordingEdit edit;
        RecordingConfig config;
        const auto error = record::previewFrame(QString::fromLocal8Bit(argv[2]), edit, config, QString::fromLocal8Bit(argv[3]).toLongLong(), 1280, {}, image, &actual);
        if (error) { std::fprintf(stderr, "%s: %s\n", qPrintable(error.code), qPrintable(error.message)); return 1; }
        std::printf("frame %dx%d at %lld us\n", image.width(), image.height(), static_cast<long long>(actual));
        return image.save(QString::fromLocal8Bit(argv[4]), "PNG") ? 0 : 3;
    }
    if (argc != 1) return 2;
    // Qt diagnostics go to the lab debug log when enabled; stderr is discarded by the resident.
    if (!qEnvironmentVariableIsEmpty("PUTGD_RECORD_DEBUG_LOG")) qInstallMessageHandler([](QtMsgType type, const QMessageLogContext&, const QString& message) { record::debugLog(QStringLiteral("qt%1: %2").arg(int(type)).arg(message.left(300))); if (type == QtFatalMsg) std::abort(); });
    record::installCrashLog();
    UiControlPipes handles;
    if (!handles.valid()) return 2;
    // Raster overlays and frame previews only: no desktop theme plugins.
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM") && qEnvironmentVariableIsEmpty("DISPLAY") && qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY")) qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication::setDesktopSettingsAware(false);
    qunsetenv("QT_QPA_PLATFORMTHEME");
    qunsetenv("QT_STYLE_OVERRIDE");
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    QCoreApplication::setOrganizationName("putgd");
    QCoreApplication::setApplicationName("putgd-record");
    QCoreApplication::setApplicationVersion(buildVersion());
    GError* gstError = nullptr;
    if (!gst_init_check(nullptr, nullptr, &gstError)) { if (gstError) g_error_free(gstError); return 6; }
    StdioDevice pipes(handles.input(), handles.output());
    if (!pipes.isOpen()) return 2;
    UiControl control(pipes);
    const auto build = QJsonDocument::fromJson(buildManifestJson()).object();
    QJsonObject envelope;
    bool ready = false;
    record::RecordSession session;
    QObject::connect(&pipes, &QIODevice::readChannelFinished, &app, [&] { app.exit(5); });
    QObject::connect(&control, &UiControl::failed, &app, [&] { app.exit(3); });
    auto result = [&](const QString& requestId, const QJsonObject& value) {
        auto frame = envelope;
        frame["requestId"] = requestId;
        frame["ok"] = true;
        frame["result"] = value;
        control.send("commandResult", frame);
    };
    auto failure = [&](const QString& requestId, const Error& error) {
        auto frame = envelope;
        frame["requestId"] = requestId;
        frame["ok"] = false;
        frame["code"] = error.code;
        frame["message"] = error.message;
        frame["details"] = error.details;
        control.send("commandResult", frame);
    };
    QObject::connect(&session, &record::RecordSession::event, &app, [&](const QJsonObject& payload) {
        if (!ready) return;
        auto frame = envelope;
        for (auto it = payload.begin(); it != payload.end(); ++it) frame[it.key()] = it.value();
        control.send("event", frame);
    });
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
        record::debugLog("command " + command);
        session.command(command, payload["params"].toObject(), [requestId, command, &result, &failure](Error error, QJsonObject value) { record::debugLog("result " + command + (error ? " error " + error.code + " " + error.message : QString(" ok"))); if (error) failure(requestId, error); else result(requestId, value); });
    });
    return app.exec();
}
