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
        QCOMPARE(disk.requiredBytes(), 160 * mib);
        QVERIFY(!disk.low());
        disk.freeBytes = gib + 192 * mib; disk.clipBytes = 0;
        QVERIFY(!disk.low()); // Exactly the minimum admission still starts.
        disk.clipBytes = 12 * mib;
        // A competing writer crosses the guard before either ENOSPC or the
        // prepare-time clip cap, including when the paused clip isn't growing.
        disk.freeBytes = gib + 160 * mib + 1; QVERIFY(!disk.low());
        disk.freeBytes += 64 * mib; QVERIFY(!disk.low()); // Writer releases space before threshold.
        disk.freeBytes -= 64 * mib;
        --disk.freeBytes; QVERIFY(disk.low());
        QCOMPARE(disk.toJson()["clipBytes"].toInteger(), 12 * mib);
        disk.clipBytes = 256 * mib;
        QCOMPARE(disk.requiredBytes(), 544 * mib);
        disk.freeBytes = gib + 544 * mib; QVERIFY(disk.low());
        disk.freeBytes = 0; QVERIFY(disk.low());
        disk.freeBytes = -1; QVERIFY(!disk.low()); // Unknown is not zero.
        QCOMPARE(disk.availableBytes(), -1);
        disk.clipBytes = std::numeric_limits<qint64>::max();
        QCOMPARE(disk.requiredBytes(), std::numeric_limits<qint64>::max());
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
