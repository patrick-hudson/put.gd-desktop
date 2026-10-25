#include "core/RecordingModel.h"
#include "core/Settings.h"
#include "core/UploadNaming.h"
#include "core/ArtifactStore.h"
#include "record/RecordingStorage.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QTest>
using namespace putgd;

class RecordingModelContract final : public QObject {
    Q_OBJECT
    static QJsonObject mask(int x, int y, int w, int h, double start, double end, const QString& kind = "blur") {
        return {{"kind", kind}, {"rect", QJsonArray{x, y, w, h}}, {"start", start}, {"end", end}};
    }
private slots:
    void liveDiskBudgetLeavesFinalizationAndCopies() {
        using record::RecordingStorage;
        constexpr qint64 mib = 1LL << 20, gib = 1LL << 30;
        RecordingStorage disk{2 * gib, 8 * gib, 12 * mib};
        QCOMPARE(disk.reserveBytes(), ArtifactStore::reserveFor(disk.totalBytes));
        QCOMPARE(disk.requiredBytes(), 192 * mib);
        QVERIFY(!disk.low());
        // A competing writer crosses the guard before either ENOSPC or the
        // prepare-time clip cap, including when the paused clip isn't growing.
        disk.freeBytes = gib + 192 * mib + 1; QVERIFY(!disk.low());
        --disk.freeBytes; QVERIFY(disk.low());
        QCOMPARE(disk.toJson()["clipBytes"].toInteger(), 12 * mib);
        disk.clipBytes = 256 * mib;
        QCOMPARE(disk.requiredBytes(), 576 * mib);
        disk.freeBytes = gib + 576 * mib; QVERIFY(disk.low());
        disk.freeBytes = 0; QVERIFY(disk.low());
        disk.freeBytes = -1; QVERIFY(!disk.low()); // Unknown is not zero.
        QCOMPARE(disk.availableBytes(), -1);
        disk.clipBytes = std::numeric_limits<qint64>::max();
        QCOMPARE(disk.requiredBytes(), std::numeric_limits<qint64>::max());
    }
    void transitionsFollowTheRecordingLifecycle() {
        QVERIFY(!recording::checkTransition("ready", "start"));
        QVERIFY(!recording::checkTransition("recording", "pause"));
        QVERIFY(!recording::checkTransition("paused", "resume"));
        QVERIFY(!recording::checkTransition("paused", "stop"));
        QVERIFY(!recording::checkTransition("recording", "stop"));
        QVERIFY(!recording::checkTransition("reviewing", "upload"));
        QVERIFY(!recording::checkTransition("exporting", "cancel"));
        for (const auto& state : recording::states()) QVERIFY(!recording::checkTransition(state, "inspect"));
        // Nothing uploads before review, and nothing restarts after it.
        QCOMPARE(recording::checkTransition("recording", "upload").code, QString("recording_state"));
        QCOMPARE(recording::checkTransition("paused", "upload").code, QString("recording_state"));
        QCOMPARE(recording::checkTransition("reviewing", "start").code, QString("recording_state"));
        QCOMPARE(recording::checkTransition("ready", "pause").code, QString("recording_state"));
        QCOMPARE(recording::checkTransition("recording", "resume").code, QString("recording_state"));
        QCOMPARE(recording::checkTransition("submitted", "cancel").code, QString("recording_state"));
        QCOMPARE(recording::checkTransition("ready", "preview").code, QString("recording_state"));
        QCOMPARE(recording::checkTransition("ready", "bogus").code, QString("unknown_method"));
        QVERIFY(recording::terminal("submitted") && recording::terminal("cancelled") && recording::terminal("failed"));
        QVERIFY(!recording::terminal("reviewing"));
    }
    void targetsAreTyped() {
        RecordingTarget target;
        QVERIFY(!recording::parseTarget(QJsonObject{{"kind", "region"}, {"rect", QJsonArray{-100, 20, 300, 200}}}, target));
        QCOMPARE(target.rect, QRect(-100, 20, 300, 200));
        QVERIFY(!recording::parseTarget(QJsonObject{{"kind", "monitor"}, {"identity", "DP-3"}}, target));
        QVERIFY(!recording::parseTarget(QJsonObject{{"kind", "monitor"}}, target));QVERIFY(target.identity.isEmpty());
        QVERIFY(!recording::parseTarget(QJsonObject{{"kind", "window"}}, target));
        QVERIFY(!recording::parseTarget(QJsonObject{{"kind", "desktop"}}, target));
        QVERIFY(target.toJson()["kind"] == "desktop" && !target.toJson().contains("rect"));
        // A region without a rect means the user drags it; release keeps the bounds.
        QVERIFY(!recording::parseTarget(QJsonObject{{"kind", "region"}}, target));
        QVERIFY(target.rect.isEmpty() && !target.toJson().contains("rect"));
        QVERIFY(!recording::parseTarget(QJsonObject{{"kind", "monitor"}}, target));  // identity optional: primary monitor or the compositor's picker
        QVERIFY(recording::parseTarget(QJsonObject{{"kind", "region"}, {"rect", QJsonArray{0, 0, 0, 10}}}, target));
        QVERIFY(recording::parseTarget(QJsonObject{{"kind", "region"}, {"rect", QJsonArray{0, 0, 10}}}, target));
        QVERIFY(recording::parseTarget(QJsonObject{{"kind", "desktop"}, {"identity", "x"}}, target));
        QVERIFY(recording::parseTarget(QJsonObject{{"kind", "window"}, {"identity", "w"}, {"rect", QJsonArray{0, 0, 1, 1}}}, target));
        QVERIFY(recording::parseTarget(QJsonObject{{"kind", "screen"}}, target));
        QVERIFY(recording::parseTarget(QJsonObject{{"kind", "desktop"}, {"extra", 1}}, target));
        QVERIFY(recording::parseTarget(QJsonValue("desktop"), target));
    }
    void configurationPairsFormatsWithCodecsAndSilence() {
        RecordingConfig config;
        QVERIFY(!recording::parseConfig({{"format", "mp4"}}, config));
        QCOMPARE(config.codec, QString("h264"));
        QCOMPARE(config.mime(), QString("video/mp4"));
        QCOMPARE(config.fps, 30);
        QVERIFY(!recording::parseConfig({{"format", "mp4"}, {"codec", "h265"}, {"fps", 60}, {"audio", QJsonObject{{"system", true}}}}, config));
        QVERIFY(config.audio.system && config.codec == "h265" && config.fps == 60);
        QVERIFY(!recording::parseConfig({{"format", "webm"}}, config));
        QCOMPARE(config.codec, QString("vp9"));
        QCOMPARE(config.mime(), QString("video/webm"));
        QVERIFY(!recording::parseConfig({{"format", "gif"}, {"maxEdge", 640}}, config));
        QVERIFY(config.codec.isEmpty() && config.animation() && config.silent() && config.maxEdge == 640);
        QCOMPARE(config.mime(), QString("image/gif"));
        QVERIFY(!recording::parseConfig({{"format", "webp"}}, config));
        QCOMPARE(config.mime(), QString("image/webp"));
        QCOMPARE(config.toJson()["mime"].toString(), QString("image/webp"));
        QCOMPARE(recording::parseConfig({{"format", "mp4"}, {"codec", "vp9"}}, config).code, QString("invalid_request"));
        QCOMPARE(recording::parseConfig({{"format", "webm"}, {"codec", "h264"}}, config).code, QString("invalid_request"));
        QCOMPARE(recording::parseConfig({{"format", "gif"}, {"codec", "h264"}}, config).code, QString("invalid_request"));
        // Serialized configs round-trip through the helper with an empty codec for animations.
        QVERIFY(!recording::parseConfig({{"format", "webp"}, {"codec", ""}}, config));
        QCOMPARE(recording::parseConfig({{"format", "mp4"}, {"codec", ""}}, config).code, QString("invalid_request"));
        QCOMPARE(recording::parseConfig({{"format", "avi"}}, config).code, QString("invalid_request"));
        QCOMPARE(recording::parseConfig({{"format", "mp4"}, {"fps", 0}}, config).code, QString("invalid_request"));
        QCOMPARE(recording::parseConfig({{"format", "mp4"}, {"fps", 61}}, config).code, QString("invalid_request"));
        QCOMPARE(recording::parseConfig({{"format", "mp4"}, {"fps", 29.97}}, config).code, QString("invalid_request"));
        // Silent formats cannot include audio; the microphone is reserved and refused.
        QCOMPARE(recording::parseConfig({{"format", "gif"}, {"audio", QJsonObject{{"system", true}}}}, config).code, QString("invalid_request"));
        QCOMPARE(recording::parseConfig({{"format", "mp4"}, {"audio", QJsonObject{{"microphone", true}}}}, config).code, QString("recording_unsupported"));
        QCOMPARE(recording::parseConfig({{"format", "mp4"}, {"audio", QJsonObject{{"device", "x"}}}}, config).code, QString("invalid_request"));
        QCOMPARE(recording::parseConfig({{"format", "mp4"}, {"maxEdge", 8}}, config).code, QString("invalid_request"));
        QCOMPARE(recording::parseConfig({{"format", "mp4"}, {"bitrate", 1}}, config).code, QString("invalid_request"));
    }
    void trimAndMasksUseSourceTimeAndRebaseAtExport() {
        // The issue's example: retaining 10-30 s makes a 12-15 s blur active at 2-5 s of output.
        RecordingEdit edit;
        const qint64 duration = 40000000;
        const QSize canvas(1280, 720);
        QVERIFY(!recording::applyEditCommands(edit, QJsonArray{QJsonObject{{"op", "setTrim"}, {"start", 10000000.0}, {"end", 30000000.0}},
            QJsonObject{{"op", "addMask"}, {"mask", mask(100, 100, 200, 50, 12000000.0, 15000000.0)}},
            QJsonObject{{"op", "addMask"}, {"mask", mask(0, 0, 10, 10, 0.0, 5000000.0, "solid")}},
            QJsonObject{{"op", "addMask"}, {"mask", mask(0, 0, 10, 10, 28000000.0, 40000000.0)}}}, duration, canvas, 30));
        QCOMPARE(edit.revision, quint64(2));
        QCOMPARE(edit.trimStart, qint64(10000000));
        QCOMPARE(*edit.trimEnd, qint64(30000000));
        QCOMPARE(edit.exportDuration(duration), qint64(20000000));
        QCOMPARE(edit.masks.size(), 3);
        const auto first = RecordingEdit::exportInterval(edit.masks[0], edit.trimStart, edit.exportEnd(duration));
        QVERIFY(first);
        QCOMPARE(first->first, qint64(2000000));
        QCOMPARE(first->second, qint64(5000000));
        // A mask wholly before the retained interval disappears; a mask crossing the end is clipped.
        QVERIFY(!RecordingEdit::exportInterval(edit.masks[1], edit.trimStart, edit.exportEnd(duration)));
        const auto third = RecordingEdit::exportInterval(edit.masks[2], edit.trimStart, edit.exportEnd(duration));
        QCOMPARE(third->first, qint64(18000000));
        QCOMPARE(third->second, qint64(20000000));
        // Changing the trim never moves a mask on the source timeline.
        QVERIFY(!recording::applyEditCommands(edit, QJsonArray{QJsonObject{{"op", "setTrim"}, {"start", 0.0}, {"end", QJsonValue::Null}}}, duration, canvas, 30));
        QCOMPARE(edit.masks[0].start, qint64(12000000));
        QVERIFY(!edit.trimEnd);
        QCOMPARE(RecordingEdit::exportInterval(edit.masks[0], 0, duration)->first, qint64(12000000));
        QVERIFY(!recording::applyEditCommands(edit, QJsonArray{QJsonObject{{"op", "resetTrim"}}}, duration, canvas, 30));
        QCOMPARE(edit.revision, quint64(4));
        const auto id = edit.masks[0].id;
        QVERIFY(!recording::applyEditCommands(edit, QJsonArray{QJsonObject{{"op", "updateMask"}, {"maskId", id}, {"mask", mask(5, 5, 20, 20, 1000000.0, 2000000.0, "solid")}}}, duration, canvas, 30));
        QCOMPARE(edit.masks[0].kind, QString("solid"));
        QCOMPARE(edit.masks[0].rect, QRect(5, 5, 20, 20));
        QVERIFY(!recording::applyEditCommands(edit, QJsonArray{QJsonObject{{"op", "removeMask"}, {"maskId", id}}}, duration, canvas, 30));
        QCOMPARE(edit.masks.size(), 2);
        QCOMPARE(edit.toJson()["masks"].toArray().size(), 2);
    }
    void invalidEditsLeaveTheModelUntouched() {
        RecordingEdit edit;
        const qint64 duration = 10000000;
        const QSize canvas(640, 480);
        QVERIFY(!recording::applyEditCommands(edit, QJsonArray{QJsonObject{{"op", "addMask"}, {"mask", mask(0, 0, 640, 480, 0.0, 10000000.0)}}}, duration, canvas, 30));
        const auto before = edit.toJson();
        const QList<QJsonArray> rejected{
            {QJsonObject{{"op", "setTrim"}, {"start", 5000000.0}, {"end", 5000000.0}}},            // empty
            {QJsonObject{{"op", "setTrim"}, {"start", 5000000.0}, {"end", 5020000.0}}},            // under one frame
            {QJsonObject{{"op", "setTrim"}, {"start", -1.0}, {"end", 5000000.0}}},
            {QJsonObject{{"op", "setTrim"}, {"start", 0.0}, {"end", 10000001.0}}},
            {QJsonObject{{"op", "setTrim"}, {"start", 0.5}, {"end", 5000000.0}}},
            {QJsonObject{{"op", "addMask"}, {"mask", mask(0, 0, 641, 480, 0.0, 1.0)}}},             // outside canvas
            {QJsonObject{{"op", "addMask"}, {"mask", mask(-1, 0, 10, 10, 0.0, 1.0)}}},
            {QJsonObject{{"op", "addMask"}, {"mask", mask(0, 0, 10, 10, 5.0, 5.0)}}},               // empty interval
            {QJsonObject{{"op", "addMask"}, {"mask", mask(0, 0, 10, 10, 0.0, 10000001.0)}}},        // past the end
            {QJsonObject{{"op", "addMask"}, {"mask", mask(0, 0, 10, 10, 0.0, 1.0, "pixelate")}}},
            {QJsonObject{{"op", "addMask"}, {"mask", QJsonObject{{"kind", "blur"}, {"rect", QJsonArray{0, 0, 1, 1}}, {"start", 0.0}, {"end", 1.0}, {"strength", 65}}}}},
            {QJsonObject{{"op", "updateMask"}, {"maskId", "missing"}, {"mask", mask(0, 0, 1, 1, 0.0, 1.0)}}},
            {QJsonObject{{"op", "removeMask"}, {"maskId", "missing"}}},
            {QJsonObject{{"op", "setTrim"}, {"start", 0.0}, {"end", 5000000.0}}, QJsonObject{{"op", "explode"}}},   // atomic: a later failure undoes the trim
            {QJsonObject{{"op", "setTrim"}, {"start", 0.0}, {"end", 5000000.0}, {"extra", 1}}},
            {}};
        for (const auto& commands : rejected) {
            const auto error = recording::applyEditCommands(edit, commands, duration, canvas, 30);
            QVERIFY2(error, qPrintable(QJsonDocument(commands).toJson()));
            QCOMPARE(edit.toJson(), before);
        }
        QJsonArray tooMany;
        for (int i = 0; i < 101; ++i) tooMany.append(QJsonObject{{"op", "resetTrim"}});
        QVERIFY(recording::applyEditCommands(edit, tooMany, duration, canvas, 30));
        QJsonArray flood;
        for (int i = 0; i < 64; ++i) flood.append(QJsonObject{{"op", "addMask"}, {"mask", mask(0, 0, 1, 1, 0.0, 1.0)}});
        QCOMPARE(recording::applyEditCommands(edit, flood, duration, canvas, 30).code, QString("invalid_request"));
        QCOMPARE(edit.toJson(), before);
    }
    void frameAlignmentAndAnimationBudgets() {
        QCOMPARE(recording::alignToFrame(0, 30), qint64(0));
        QCOMPARE(recording::alignToFrame(16000, 30), qint64(0));
        QCOMPARE(recording::alignToFrame(17000, 30), qint64(33333));
        QCOMPARE(recording::alignToFrame(1000000, 30), qint64(1000000));
        QCOMPARE(recording::alignToFrame(1000000, 24), qint64(1000000));
        QCOMPARE(recording::outputSize(QSize(2560, 1440), std::nullopt), QSize(2560, 1440));
        QCOMPARE(recording::outputSize(QSize(2560, 1440), 1280), QSize(1280, 720));
        QCOMPARE(recording::outputSize(QSize(1000, 1000), 333), QSize(334, 334));
        QCOMPARE(recording::outputSize(QSize(640, 480), 1280), QSize(640, 480));
        RecordingLimits limits;
        RecordingEdit edit;
        RecordingConfig gif;
        QVERIFY(!recording::parseConfig({{"format", "gif"}}, gif));
        RecordingConfig mp4;
        QVERIFY(!recording::parseConfig({{"format", "mp4"}}, mp4));
        // MP4 has no animation budget; GIF is limited by retained duration and output edge.
        QVERIFY(!recording::checkExportBudget(mp4, edit, 3600000000LL, QSize(5120, 1440), limits));
        QCOMPARE(recording::checkExportBudget(gif, edit, 61000000, QSize(640, 480), limits).code, QString("recording_animation_too_long"));
        QVERIFY(!recording::applyEditCommands(edit, QJsonArray{QJsonObject{{"op", "setTrim"}, {"start", 1000000.0}, {"end", 61000000.0}}}, 61000000, QSize(640, 480), 30));
        QVERIFY(!recording::checkExportBudget(gif, edit, 61000000, QSize(640, 480), limits));
        QCOMPARE(recording::checkExportBudget(gif, edit, 61000000, QSize(2560, 1440), limits).code, QString("recording_animation_too_large"));
        gif.maxEdge = 1280;
        QVERIFY(!recording::checkExportBudget(gif, edit, 61000000, QSize(2560, 1440), limits));
        limits.animationMaxEdge = 320;
        QCOMPARE(recording::checkExportBudget(gif, edit, 61000000, QSize(2560, 1440), limits).code, QString("recording_animation_too_large"));
    }
    void settingsCarryTheRecordingRouteTemplateAndLimits() {
        auto doc = Settings::developmentTemplate();
        auto validator = [](const QString&, const QJsonObject&) { return Error{}; };
        QVERIFY(!Settings::validate(doc, validator));
        auto settings = Settings::fromValidated(doc);
        // Absent: inherit the region route without a shortcut.
        QCOMPARE(settings.actions.value("record").destination, QString("upload"));
        QCOMPARE(settings.actions.value("record").profileId, QString("development-s3"));
        QVERIFY(settings.actions.value("record").shortcut.isEmpty());
        QCOMPARE(settings.recordingTemplate, QString("Screen-Recording-{yyyy}-{MM}-{dd}-{HH}-{mm}-{ss}.{ext}"));
        QCOMPARE(settings.limits.recordingAnimationSeconds, qint64(60));
        QCOMPARE(settings.limits.recordingAnimationEdge, 1280);
        QVERIFY(!Settings::actionNames().contains("record"));
        auto actions = doc["actions"].toObject();
        actions["record"] = QJsonObject{{"destination", "local"}, {"profileId", QJsonValue::Null}, {"shortcut", "Ctrl+Shift+9"}};
        doc["actions"] = actions;
        auto naming = doc["naming"].toObject();
        naming["recordingTemplate"] = "Rec-{yyyy}.{ext}";
        doc["naming"] = naming;
        auto limits = doc["limits"].toObject();
        limits["recordingAnimationSeconds"] = 15;
        limits["recordingAnimationEdge"] = 640;
        doc["limits"] = limits;
        QVERIFY(!Settings::validate(doc, validator));
        settings = Settings::fromValidated(doc);
        QCOMPARE(settings.actions.value("record").destination, QString("local"));
        QCOMPARE(settings.actions.value("record").shortcut, QString("Ctrl+Shift+9"));
        QCOMPARE(settings.recordingTemplate, QString("Rec-{yyyy}.{ext}"));
        QCOMPARE(settings.limits.recordingAnimationSeconds, qint64(15));
        QCOMPARE(settings.limits.recordingAnimationEdge, 640);
        limits["recordingAnimationSeconds"] = 0;
        doc["limits"] = limits;
        QCOMPARE(Settings::validate(doc, validator).code, QString("config_invalid"));
        limits["recordingAnimationSeconds"] = 15;
        limits["recordingAnimationEdge"] = 8;
        doc["limits"] = limits;
        QCOMPARE(Settings::validate(doc, validator).code, QString("config_invalid"));
        limits["recordingAnimationEdge"] = 640;
        doc["limits"] = limits;
        naming["recordingTemplate"] = "{bogus}";
        doc["naming"] = naming;
        QCOMPARE(Settings::validate(doc, validator).code, QString("config_invalid"));
        naming.remove("recordingTemplate");
        doc["naming"] = naming;
        actions["record"] = QJsonObject{{"destination", "upload"}, {"profileId", "missing"}, {"shortcut", ""}};
        doc["actions"] = actions;
        QCOMPARE(Settings::validate(doc, validator).code, QString("config_invalid"));
    }
    void recordingArtifactsStreamExactBytesWithDeclaredTypeAndTemplate() {
        QTemporaryDir dir;
        const auto source = dir.filePath("export-1.mp4");
        QFile file(source);
        QVERIFY(file.open(QIODevice::WriteOnly));
        const QByteArray bytes = QByteArray("\0\0\0\x18" "ftypmp42", 12) + QByteArray(5000, 'x');
        file.write(bytes);
        file.close();
        ArtifactStore store(dir.filePath("artifacts"));
        auto settings = Settings::fromValidated(Settings::defaults());
        settings.namingMode = "template";
        ClipboardPayload payload;
        payload.kind = "recording";
        payload.files = {source};
        payload.mime = "video/mp4";
        const auto when = QDateTime(QDate(2026, 10, 17), QTime(22, 30, 5));
        auto result = store.acquire("op-1", payload, settings.limits, "record", settings, when);
        QVERIFY2(!result.error, qPrintable(result.error.message));
        QCOMPARE(result.artifacts.size(), 1);
        const auto& artifact = result.artifacts.first();
        QCOMPARE(artifact.mime, QString("video/mp4"));
        QCOMPARE(artifact.category, QString("videos"));
        QCOMPARE(artifact.bytes, qint64(bytes.size()));
        QVERIFY(!artifact.originalFile);
        QCOMPARE(artifact.name, QString("Screen-Recording-2026-10-17-22-30-05.mp4"));
        QFile stored(artifact.path);
        QVERIFY(stored.open(QIODevice::ReadOnly));
        QCOMPARE(stored.readAll(), bytes);
        QVERIFY(!store.verify(artifact));
        // Animations keep their declared image type; the bytes are never normalized.
        payload.mime = "image/webp";
        const auto webp = store.acquire("op-2", payload, settings.limits, "record", settings, when);
        QVERIFY(!webp.error);
        QCOMPARE(webp.artifacts.first().category, QString("images"));
        QCOMPARE(webp.artifacts.first().mime, QString("image/webp"));
        payload.mime = "text/plain";
        QCOMPARE(store.acquire("op-3", payload, settings.limits, "record", settings, when).error.code, QString("source_unsupported"));
        payload.mime = "video/mp4";
        payload.files = {dir.filePath("missing.mp4")};
        QCOMPARE(store.acquire("op-4", payload, settings.limits, "record", settings, when).error.code, QString("source_unavailable"));
    }
};
QTEST_GUILESS_MAIN(RecordingModelContract)
#include "test_recording_model.moc"
