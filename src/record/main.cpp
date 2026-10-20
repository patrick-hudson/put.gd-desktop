// putgd-record: the transient screen-recording helper. The resident launches
// one per recording session and speaks UiControl frames over private pipes;
// `--capabilities` prints the media inventory and exits. The helper owns the
// live GStreamer capture, the private working clip, frame previews and the
// FFmpeg export; only control metadata crosses the pipes.
#include "MediaProbe.h"
#include "BundledRuntime.h"
#include "Poster.h"
#include "ClipExporter.h"
#include "DebugLog.h"
#include "AvSync.h"
#include "RecordSession.h"
#include "app/UiControl.h"
#include "core/BuildInfo.h"
#include "ui/StdioDevice.h"
#include <QApplication>
#include <QBuffer>
#include <QImage>
#include <QCoreApplication>
#include <atomic>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <gst/gst.h>
#include <QJsonDocument>
#include <QTimer>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <sys/resource.h>
extern "C" {
#include <libavutil/log.h>
}

using namespace putgd;

int main(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "--version-json") == 0) { std::printf("%s\n", buildManifestJson()); return 0; }
    if (argc == 2 && (std::strcmp(argv[1], "--version") == 0 || std::strcmp(argv[1], "-v") == 0)) { std::printf("putgd-record %s\n", buildVersion()); return 0; }
    const rlimit noCore{0, 0};
    ::setrlimit(RLIMIT_CORE, &noCore);
    record::useBundledGStreamer();
    if (argc == 2 && std::strcmp(argv[1], "--capabilities") == 0) {
        QCoreApplication app(argc, argv);
        const auto json = QJsonDocument(record::probeMedia()).toJson(QJsonDocument::Compact);
        std::fwrite(json.constData(), 1, size_t(json.size()), stdout);
        std::fputc('\n', stdout);
        return 0;
    }
    if (argc == 3 && std::strcmp(argv[1], "--poster") == 0) {
        // History's poster frame for a finished video (#112). The resident opens
        // the stored file and passes it as standard input; the PNG (long edge at
        // most 256 px, duration in a text chunk) goes to standard output. The
        // resident bounds time, memory and output size and kills a late job.
        QCoreApplication app(argc, argv);
        av_log_set_level(AV_LOG_QUIET);
        bool ok = false;
        const qint64 maxPixels = QByteArray(argv[2]).toLongLong(&ok);
        if (!ok || maxPixels < 1) return 2;
        record::Poster poster;
        if (const auto error = record::renderPoster(QStringLiteral("/proc/self/fd/0"), 256, maxPixels, poster)) return error.code == "input_excessive" ? 3 : 1;
        poster.image.setText(QStringLiteral("putgd-duration-ms"), QString::number(poster.durationUs / 1000));
        QByteArray png;
        QBuffer buffer(&png);
        buffer.open(QIODevice::WriteOnly);
        if (!poster.image.save(&buffer, "PNG")) return 4;
        return std::fwrite(png.constData(), 1, size_t(png.size()), stdout) == size_t(png.size()) && std::fflush(stdout) == 0 ? 0 : 4;
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
    if (argc >= 3 && std::strcmp(argv[1], "--motion") == 0) {
        // Lab diagnostic: does the clip contain movement? Samples frames evenly
        // across the clip and prints the mean absolute pixel difference between
        // consecutive samples (0 for a still picture) so a test can require motion.
        QCoreApplication app(argc, argv);
        const QString clip = QString::fromLocal8Bit(argv[2]);
        const int samples = argc >= 4 ? std::max(2, std::atoi(argv[3])) : 8;
        qint64 durationUs = 0;
        QSize canvas;
        if (auto error = record::probeClip(clip, durationUs, canvas)) { std::fprintf(stderr, "%s: %s\n", qPrintable(error.code), qPrintable(error.message)); return 1; }
        QImage previous;
        double total = 0;
        int compared = 0;
        qint64 previousUs = -1;
        for (int i = 0; i < samples; ++i) {
            const qint64 at = durationUs * (2 * i + 1) / (2 * samples);
            QImage image;
            qint64 actual = 0;
            if (auto error = record::previewFrame(clip, {}, {}, at, 320, canvas, image, &actual)) { std::fprintf(stderr, "%s: %s\n", qPrintable(error.code), qPrintable(error.message)); return 1; }
            if (!previous.isNull() && actual != previousUs && image.size() == previous.size()) {
                double sum = 0;
                for (int y = 0; y < image.height(); ++y) {
                    const auto* a = reinterpret_cast<const QRgb*>(image.constScanLine(y));
                    const auto* b = reinterpret_cast<const QRgb*>(previous.constScanLine(y));
                    for (int x = 0; x < image.width(); ++x) sum += std::abs(qGray(a[x]) - qGray(b[x]));
                }
                total += sum / (double(image.width()) * image.height());
                ++compared;
            }
            previous = image;
            previousUs = actual;
        }
        std::printf("motion=%.3f compared=%d samples=%d durationMs=%lld size=%dx%d\n", compared ? total / compared : 0.0, compared, samples, static_cast<long long>(durationUs / 1000), canvas.width(), canvas.height());
        return 0;
    }
    if (argc == 4 && std::strcmp(argv[1], "--av-marker") == 0) {
        // Lab diagnostic: write the synthetic A/V marker clip to play while recording.
        QCoreApplication app(argc, argv);
        if (auto error = record::writeAvMarker(QString::fromLocal8Bit(argv[2]), std::atoi(argv[3]))) { std::fprintf(stderr, "%s: %s\n", qPrintable(error.code), qPrintable(error.message)); return 1; }
        return 0;
    }
    if (argc == 3 && std::strcmp(argv[1], "--av-sync") == 0) {
        // Lab diagnostic: A/V sync of a recording of the synthetic marker clip
        // (flashes with simultaneous tone bursts); prints one JSON object.
        QCoreApplication app(argc, argv);
        QJsonObject result;
        if (auto error = record::measureAvSync(QString::fromLocal8Bit(argv[2]), result)) { std::fprintf(stderr, "%s: %s\n", qPrintable(error.code), qPrintable(error.message)); return 1; }
        std::printf("%s\n", QJsonDocument(result).toJson(QJsonDocument::Compact).constData());
        return 0;
    }
    if (argc >= 4 && std::strcmp(argv[1], "--export") == 0) {
        // Lab diagnostic: export a working clip the way the review does and
        // report the wall time, so encoder settings can be compared per machine.
        QCoreApplication app(argc, argv);
        record::ExportRequest request;
        request.input = QString::fromLocal8Bit(argv[2]);
        request.output = QString::fromLocal8Bit(argv[3]);
        const auto extension = request.output.section('.', -1).toLower();
        request.config.format = extension == "mkv" ? "mp4" : extension;
        request.config.audio.system = true;
        for (int i = 4; i < argc; ++i) {
            const QString arg = QString::fromLocal8Bit(argv[i]);
            if (arg == "--codec" && i + 1 < argc) request.config.codec = QString::fromLocal8Bit(argv[++i]);
            else if (arg == "--max-edge" && i + 1 < argc) request.config.maxEdge = std::atoi(argv[++i]);
            else if (arg == "--trim" && i + 2 < argc) { request.edit.trimStart = std::atoll(argv[++i]); request.edit.trimEnd = std::atoll(argv[++i]); }
            else if (arg == "--no-audio") request.config.audio.system = false;
            else { std::fprintf(stderr, "unknown export option %s\n", argv[i]); return 2; }
        }
        if (request.config.codec.isEmpty()) request.config.codec = request.config.format == "mp4" ? "h264" : request.config.format == "webm" ? "vp9" : QString{};
        if (request.config.animation()) request.config.audio.system = false;
        if (auto error = record::probeClip(request.input, request.durationUs, request.canvas)) { std::fprintf(stderr, "%s: %s\n", qPrintable(error.code), qPrintable(error.message)); return 1; }
        record::ExportResult result;
        std::atomic_bool cancelled{false};
        QElapsedTimer clock;
        clock.start();
        const auto error = record::exportClip(request, result, cancelled);
        const auto ms = clock.elapsed();
        if (error) { std::fprintf(stderr, "%s: %s\n", qPrintable(error.code), qPrintable(error.message)); return 1; }
        std::printf("encoder=%s audio=%s frames=%d size=%dx%d durationMs=%lld bytes=%lld wallMs=%lld speed=%.2fx\n", qPrintable(result.videoEncoder), qPrintable(result.audioEncoder.isEmpty() ? QStringLiteral("none") : result.audioEncoder), result.frames, result.size.width(), result.size.height(), static_cast<long long>(result.durationUs / 1000), static_cast<long long>(result.bytes), static_cast<long long>(ms), ms > 0 ? double(result.durationUs) / 1000.0 / double(ms) : 0.0);
        return 0;
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
