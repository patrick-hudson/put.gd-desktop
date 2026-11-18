#include "core/RecordingModel.h"
#include "core/KeptSpans.h"
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
    void helperAcceptsExactlyWhatTheResidentTableAllows() {
        // #1434: a helper that refuses a command the resident's table allows (or the
        // reverse) strands the session, holding the clip, the bar or the dimming.
        const QList<std::pair<QString, QString>> commands{{"start", "start"}, {"pause", "pause"}, {"resume", "resume"}, {"stop", "stop"}, {"cancel", "cancel"},
            {"select", "select"}, {"audio", "audio"}, {"raise", "raise"}, {"overlay", "overlay"}, {"review", "review"}, {"preview", "preview"},
            {"edit", "setEdit"}, {"saveCopy", "export"}, {"upload", "export"}};
        for (int value = int(HelperPhase::Idle); value <= int(HelperPhase::Failed); ++value) {
            const auto phase = HelperPhase(value);
            const auto state = recording::residentState(phase);
            for (const auto& [resident, helper] : commands) {
                const bool helperAccepts = !recording::checkHelperCommand(phase, helper);
                const bool residentAllows = state && !recording::checkTransition(*state, resident);
                QVERIFY2(helperAccepts == residentAllows, qPrintable(QStringLiteral("%1 in phase %2: helper %3, resident %4").arg(helper, recording::phaseName(phase), helperAccepts ? "accepts" : "refuses", residentAllows ? "allows" : "refuses")));
            }
            QCOMPARE(!recording::checkHelperCommand(phase, "prepare"), phase == HelperPhase::Idle);
        }
    }
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

    // ---- #1613: kept spans ----
private:
    static RecordingEdit editWith(qint64 trimStart, std::optional<qint64> trimEnd, const QList<std::pair<qint64, qint64>>& cuts) {
        RecordingEdit edit;
        edit.trimStart = trimStart;
        edit.trimEnd = trimEnd;
        int n = 0;
        for (const auto& [start, end] : cuts) edit.cuts.append({QString("c%1").arg(n++), start, end});
        return edit;
    }
    static QList<recording::Span> spans(std::initializer_list<std::pair<qint64, qint64>> list) {
        QList<recording::Span> out;
        for (const auto& [a, b] : list) out.append({a, b});
        return out;
    }
    static QJsonObject cutOp(const char* op, double start, double end, const QString& id = {}) {
        QJsonObject o{{"op", op}, {"start", start}, {"end", end}};
        if (!id.isEmpty()) o["cutId"] = id;
        return o;
    }
    static Error apply(RecordingEdit& edit, const QJsonArray& commands, RecordingEditHistory* history = nullptr, qint64 duration = 100, int fps = 1000000) {
        return recording::applyEditCommands(edit, commands, duration, QSize(1280, 720), fps, history);
    }
private slots:
    void keptSpansAreTheTrimMinusEveryCut() {
        struct Row { const char* name; RecordingEdit edit; QList<recording::Span> spans; qint64 total; };
        const QList<Row> rows{
            {"no cuts", editWith(0, std::nullopt, {}), spans({{0, 100}}), 100},
            {"trim only", editWith(10, 90, {}), spans({{10, 90}}), 80},
            {"cut at start", editWith(0, std::nullopt, {{0, 20}}), spans({{20, 100}}), 80},
            {"cut at end", editWith(0, std::nullopt, {{80, 100}}), spans({{0, 80}}), 80},
            {"cut in middle", editWith(0, std::nullopt, {{30, 50}}), spans({{0, 30}, {50, 100}}), 80},
            {"two cuts", editWith(0, std::nullopt, {{10, 20}, {40, 60}}), spans({{0, 10}, {20, 40}, {60, 100}}), 70},
            {"cut across the trim start", editWith(20, std::nullopt, {{10, 30}}), spans({{30, 100}}), 70},
            {"cuts wholly outside the trim", editWith(20, 80, {{0, 10}, {90, 100}}), spans({{20, 80}}), 60},
            {"cut across the trim end", editWith(0, 50, {{40, 70}}), spans({{0, 40}}), 40},
            {"everything cut", editWith(0, std::nullopt, {{0, 100}}), spans({}), 0},
            {"unsorted cuts", editWith(0, std::nullopt, {{60, 70}, {10, 20}}), spans({{0, 10}, {20, 60}, {70, 100}}), 80},
        };
        for (const auto& row : rows) {
            const recording::KeptSpans kept(row.edit, 100);
            QVERIFY2(kept.spans() == row.spans, row.name);
            QVERIFY2(kept.outputDuration() == row.total, row.name);
            QVERIFY2(row.edit.exportDuration(100) == row.total, row.name);
            QVERIFY2(kept.empty() == (row.total == 0), row.name);
        }
    }
    void keptSpansMapTimeBothWaysAndSnapToSeams() {
        const recording::KeptSpans kept(editWith(10, 90, {{30, 50}, {70, 75}}), 100);
        // Kept: [10,30) [50,70) [75,90); output 0-20, 20-40, 40-55.
        QCOMPARE(kept.outputDuration(), qint64(55));
        QCOMPARE(kept.seams(), (QList<qint64>{20, 40}));
        QList<std::pair<qint64, qint64>> sourceToOutput{{10, 0}, {29, 19}, {50, 20}, {69, 39}, {75, 40}, {89, 54}};
        for (const auto& [source, output] : sourceToOutput) {
            QVERIFY(kept.kept(source));
            QCOMPARE(kept.toOutput(source), output);
            QCOMPARE(kept.toSource(output), source);
        }
        // A time inside a cut, before the trim or after it maps to the seam that follows it.
        QCOMPARE(kept.toOutput(30), qint64(20));
        QCOMPARE(kept.toOutput(45), qint64(20));
        QCOMPARE(kept.toOutput(72), qint64(40));
        QCOMPARE(kept.toOutput(0), qint64(0));
        QCOMPARE(kept.toOutput(95), qint64(55));
        QVERIFY(!kept.kept(30) && !kept.kept(5) && !kept.kept(90));
        // Output times on a seam start the span after it; the end is the last span's end; out of range clamps.
        QCOMPARE(kept.toSource(20), qint64(50));
        QCOMPARE(kept.toSource(55), qint64(90));
        QCOMPARE(kept.toSource(500), qint64(90));
        QCOMPARE(kept.toSource(-5), qint64(10));
        // Nothing kept.
        const recording::KeptSpans none(editWith(0, std::nullopt, {{0, 100}}), 100);
        QCOMPARE(none.toOutput(40), qint64(0));
        QVERIFY(!none.nextKept(0) && !none.keptEndAtOrBefore(100));
    }
    void keptSpansFindKeptFootageAtSpanEdges() {
        const recording::KeptSpans kept(editWith(0, std::nullopt, {{30, 50}}), 100);  // kept [0,30) [50,100)
        QCOMPARE(*kept.nextKept(0), qint64(0));
        QCOMPARE(*kept.nextKept(29), qint64(29));
        QCOMPARE(*kept.nextKept(30), qint64(50));
        QCOMPARE(*kept.nextKept(49), qint64(50));
        QCOMPARE(*kept.nextKept(99), qint64(99));
        QVERIFY(!kept.nextKept(100));
        QVERIFY(!kept.keptEndAtOrBefore(0));
        QCOMPARE(*kept.keptEndAtOrBefore(1), qint64(1));
        QCOMPARE(*kept.keptEndAtOrBefore(30), qint64(30));
        QCOMPARE(*kept.keptEndAtOrBefore(31), qint64(30));
        QCOMPARE(*kept.keptEndAtOrBefore(50), qint64(30));
        QCOMPARE(*kept.keptEndAtOrBefore(51), qint64(51));
        QCOMPARE(*kept.keptEndAtOrBefore(500), qint64(100));
        // A trim leading the clip: nothing is kept at or before its start.
        const recording::KeptSpans trimmed(editWith(20, 80, {}), 100);
        QVERIFY(!trimmed.keptEndAtOrBefore(20));
        QCOMPARE(*trimmed.keptEndAtOrBefore(21), qint64(21));
        QCOMPARE(*trimmed.nextKept(0), qint64(20));
        QVERIFY(!trimmed.nextKept(80));
    }
    void keptSpansJoinACoverAcrossACut() {
        const recording::KeptSpans kept(editWith(0, std::nullopt, {{30, 50}, {70, 80}}), 100);  // kept [0,30) [50,70) [80,100)
        QCOMPARE(kept.keptParts(20, 90), spans({{20, 30}, {50, 70}, {80, 90}}));
        // The cover crosses a cut: one output interval, not two.
        QCOMPARE(kept.outputParts(20, 60), spans({{20, 40}}));
        QCOMPARE(kept.outputParts(0, 100), spans({{0, 70}}));
        // A cover that starts inside a cut begins at the seam.
        QCOMPARE(kept.outputParts(40, 90), spans({{30, 60}}));
        // Wholly inside a cut, or outside the clip: nothing.
        QVERIFY(kept.outputParts(32, 48).isEmpty());
        QVERIFY(kept.keptParts(100, 120).isEmpty());
        // A trim and a cut together.
        const recording::KeptSpans trimmed(editWith(10, 60, {{30, 40}}), 100);
        QCOMPARE(trimmed.outputParts(0, 100), spans({{0, 40}}));
    }

    // ---- #1613: cut commands ----
    void addCutClampsMergesAndKeepsTheListSorted() {
        RecordingEdit edit;
        QVERIFY(!apply(edit, {cutOp("addCut", 40, 50)}));
        QVERIFY(!apply(edit, {cutOp("addCut", 10, 20)}));
        QCOMPARE(edit.cuts.size(), 2);
        QCOMPARE(edit.cuts[0].start, qint64(10));
        QCOMPARE(edit.cuts[1].start, qint64(40));
        QVERIFY(!edit.cuts[0].id.isEmpty() && edit.cuts[0].id != edit.cuts[1].id);
        // Overlapping the second: one cut, a new id.
        const auto oldId = edit.cuts[1].id;
        QVERIFY(!apply(edit, {cutOp("addCut", 45, 60)}));
        QCOMPARE(edit.cuts.size(), 2);
        QCOMPARE(edit.cuts[1].start, qint64(40));
        QCOMPARE(edit.cuts[1].end, qint64(60));
        QVERIFY(edit.cuts[1].id != oldId);
        // Touching the first (end == start) merges too; one that swallows both leaves one.
        QVERIFY(!apply(edit, {cutOp("addCut", 20, 30)}));
        QCOMPARE(edit.cuts.size(), 2);
        QCOMPARE(edit.cuts[0].start, qint64(10));
        QCOMPARE(edit.cuts[0].end, qint64(30));
        QVERIFY(!apply(edit, {cutOp("addCut", 5, 70)}));
        QCOMPARE(edit.cuts.size(), 1);
        QCOMPARE(edit.cuts[0].start, qint64(5));
        QCOMPARE(edit.cuts[0].end, qint64(70));
        // Out-of-range times clamp to the clip.
        QVERIFY(!apply(edit, {cutOp("addCut", 90, 500)}));
        QCOMPARE(edit.cuts.last().start, qint64(90));
        QCOMPARE(edit.cuts.last().end, qint64(100));
        QVERIFY(!apply(edit, {cutOp("addCut", -50, 1)}));
        QCOMPARE(edit.cuts.first().start, qint64(0));
        // Refused: reversed, empty, non-integer, wholly outside, unknown field.
        const auto before = edit.toJson();
        for (const auto& bad : {cutOp("addCut", 30, 20), cutOp("addCut", 30, 30), cutOp("addCut", 200, 300), cutOp("addCut", 1.5, 3)})
            QCOMPARE(apply(edit, {bad}).code, QString("invalid_request"));
        QJsonObject extra = cutOp("addCut", 30, 31);
        extra["strength"] = 3;
        QCOMPARE(apply(edit, {extra}).code, QString("invalid_request"));
        QCOMPARE(edit.toJson(), before);
        // The json carries the cuts.
        QCOMPARE(edit.toJson()["cuts"].toArray().size(), edit.cuts.size());
        QCOMPARE(edit.toJson()["cuts"].toArray()[0].toObject()["start"].toDouble(), 0.0);
    }
    void updateCutKeepsItsIdAndAbsorbsNeighbours() {
        RecordingEdit edit;
        QVERIFY(!apply(edit, {cutOp("addCut", 10, 20), cutOp("addCut", 40, 50), cutOp("addCut", 70, 80)}));
        QCOMPARE(edit.cuts.size(), 3);
        const auto id = edit.cuts[0].id;
        QVERIFY(!apply(edit, {cutOp("updateCut", 15, 45, id)}));
        QCOMPARE(edit.cuts.size(), 2);
        QCOMPARE(edit.cuts[0].id, id);
        QCOMPARE(edit.cuts[0].start, qint64(15));
        QCOMPARE(edit.cuts[0].end, qint64(50));
        QCOMPARE(edit.cuts[1].start, qint64(70));
        // Clamping applies, and the cut it grows into absorbs the next one but keeps its own id.
        QVERIFY(!apply(edit, {cutOp("updateCut", 60, 999, id)}));
        QCOMPARE(edit.cuts.size(), 1);
        QCOMPARE(edit.cuts[0].id, id);
        QCOMPARE(edit.cuts[0].start, qint64(60));
        QCOMPARE(edit.cuts[0].end, qint64(100));
        // Unknown ids are named.
        auto error = apply(edit, {cutOp("updateCut", 1, 2, "nope")});
        QCOMPARE(error.code, QString("recording_cut_unknown"));
        error = apply(edit, {QJsonObject{{"op", "removeCut"}, {"cutId", "nope"}}});
        QCOMPARE(error.code, QString("recording_cut_unknown"));
        QCOMPARE(apply(edit, {QJsonObject{{"op", "removeCut"}}}).code, QString("invalid_request"));
        QVERIFY(!apply(edit, {QJsonObject{{"op", "removeCut"}, {"cutId", id}}}));
        QVERIFY(edit.cuts.isEmpty());
    }
    void aCutThatLeavesLessThanAFrameIsRefused() {
        const qint64 duration = 1000000;
        RecordingEdit edit;
        // One frame at 10 fps is 100000 us.
        auto run = [&](const QJsonArray& commands, RecordingEditHistory* history = nullptr) { return recording::applyEditCommands(edit, commands, duration, QSize(64, 64), 10, history); };
        // Exactly one frame kept is allowed.
        QVERIFY(!run({cutOp("addCut", 0, 900000)}));
        // 50 ms more kept is less than a frame at 10 fps.
        QCOMPARE(run({cutOp("addCut", 950000, 1000000)}).code, QString("recording_cut_empty"));
        QCOMPARE(run({cutOp("addCut", 0, 1000000)}).code, QString("recording_cut_empty"));
        QCOMPARE(edit.cuts.size(), 1);
        QCOMPARE(edit.revision, quint64(2));
    }
    void setTrimIsRefusedWhenCutsLeaveLessThanAFrame() {
        const qint64 duration = 1000000;
        RecordingEdit edit;
        auto run = [&](const QJsonArray& commands, RecordingEditHistory* history = nullptr) { return recording::applyEditCommands(edit, commands, duration, QSize(64, 64), 10, history); };
        QVERIFY(!run({cutOp("addCut", 200000, 800000)}));
        QCOMPARE(run({QJsonObject{{"op", "setTrim"}, {"start", 300000.0}, {"end", 850000.0}}}).code, QString("recording_cut_empty"));
        QCOMPARE(edit.trimStart, qint64(0));
        // Trimming to the cut's neighbour leaves a frame.
        QVERIFY(!run({QJsonObject{{"op", "setTrim"}, {"start", 700000.0}, {"end", 900000.0}}}));
        QCOMPARE(edit.exportDuration(duration), qint64(100000));
        // Removing the cut in the same batch lifts the refusal.
        QVERIFY(!run({QJsonObject{{"op", "setTrim"}, {"start", 300000.0}, {"end", 900000.0}}, QJsonObject{{"op", "removeCut"}, {"cutId", edit.cuts[0].id}}}));
    }
    void atMostMaximumCutsAreKept() {
        RecordingEdit edit;
        const qint64 duration = 100000;
        for (int i = 0; i < recording::maximumCuts; ++i) QVERIFY(!apply(edit, {cutOp("addCut", i * 1000, i * 1000 + 500)}, nullptr, duration));
        QCOMPARE(edit.cuts.size(), recording::maximumCuts);
        QCOMPARE(apply(edit, {cutOp("addCut", 90000, 90500)}, nullptr, duration).code, QString("invalid_request"));
        QCOMPARE(edit.cuts.size(), recording::maximumCuts);
        // A cut that merges into an existing one does not add to the count.
        QVERIFY(!apply(edit, {cutOp("addCut", 600, 1100)}, nullptr, duration));
        QCOMPARE(edit.cuts.size(), recording::maximumCuts);
        QCOMPARE(edit.cuts[1].start, qint64(600));
        QCOMPARE(edit.cuts[1].end, qint64(1500));
    }

    // ---- #1613: history ----
    void undoAndRedoStepTheHistoryAndAlwaysAdvanceTheRevision() {
        RecordingEdit edit;
        RecordingEditHistory history;
        QVERIFY(!edit.canUndo && !edit.canRedo);
        QVERIFY(!apply(edit, {cutOp("addCut", 10, 20)}, &history));  // revision 2
        QVERIFY(!apply(edit, {cutOp("addCut", 40, 50)}, &history));  // revision 3
        QVERIFY(edit.canUndo && !edit.canRedo);
        QCOMPARE(edit.cuts.size(), 2);
        QVERIFY(!apply(edit, {QJsonObject{{"op", "undo"}}}, &history));
        QCOMPARE(edit.cuts.size(), 1);
        QCOMPARE(edit.revision, quint64(4));
        QVERIFY(edit.canUndo && edit.canRedo);
        QVERIFY(!apply(edit, {QJsonObject{{"op", "undo"}}}, &history));
        QCOMPARE(edit.cuts.size(), 0);
        QCOMPARE(edit.revision, quint64(5));
        QVERIFY(!edit.canUndo && edit.canRedo);
        QCOMPARE(apply(edit, {QJsonObject{{"op", "undo"}}}, &history).code, QString("recording_history_empty"));
        QCOMPARE(edit.revision, quint64(5));
        // Redo walks forward in order.
        QVERIFY(!apply(edit, {QJsonObject{{"op", "redo"}}}, &history));
        QCOMPARE(edit.cuts.size(), 1);
        QCOMPARE(edit.cuts[0].start, qint64(10));
        QCOMPARE(edit.revision, quint64(6));
        QVERIFY(!apply(edit, {QJsonObject{{"op", "redo"}}}, &history));
        QCOMPARE(edit.cuts.size(), 2);
        QCOMPARE(edit.revision, quint64(7));
        QVERIFY(edit.canUndo && !edit.canRedo);
        QCOMPARE(apply(edit, {QJsonObject{{"op", "redo"}}}, &history).code, QString("recording_history_empty"));
        // A new batch clears redo.
        QVERIFY(!apply(edit, {QJsonObject{{"op", "undo"}}}, &history));
        QVERIFY(edit.canRedo);
        QVERIFY(!apply(edit, {cutOp("addCut", 60, 70)}, &history));
        QVERIFY(!edit.canRedo);
        QVERIFY(history.redo.isEmpty());
        QCOMPARE(apply(edit, {QJsonObject{{"op", "redo"}}}, &history).code, QString("recording_history_empty"));
    }
    void undoRestoresTrimAndMasksToo() {
        RecordingEdit edit;
        RecordingEditHistory history;
        const qint64 duration = 40000000;
        QVERIFY(!recording::applyEditCommands(edit, {QJsonObject{{"op", "setTrim"}, {"start", 1000000.0}, {"end", 30000000.0}}, QJsonObject{{"op", "addMask"}, {"mask", mask(0, 0, 10, 10, 2000000.0, 3000000.0)}}}, duration, QSize(1280, 720), 30, &history));
        QCOMPARE(history.undo.size(), 1);
        QVERIFY(!recording::applyEditCommands(edit, {QJsonObject{{"op", "undo"}}}, duration, QSize(1280, 720), 30, &history));
        QCOMPARE(edit.trimStart, qint64(0));
        QVERIFY(!edit.trimEnd);
        QVERIFY(edit.masks.isEmpty());
    }
    void undoMixedWithOtherCommandsOrWithoutAHistoryIsRefused() {
        RecordingEdit edit;
        RecordingEditHistory history;
        QVERIFY(!apply(edit, {cutOp("addCut", 10, 20)}, &history));
        const auto before = edit.toJson();
        QCOMPARE(apply(edit, {QJsonObject{{"op", "undo"}}, cutOp("addCut", 30, 40)}, &history).code, QString("invalid_request"));
        QCOMPARE(apply(edit, {cutOp("addCut", 30, 40), QJsonObject{{"op", "redo"}}}, &history).code, QString("invalid_request"));
        QCOMPARE(apply(edit, {QJsonObject{{"op", "undo"}, {"extra", 1}}}, &history).code, QString("invalid_request"));
        QCOMPARE(apply(edit, {QJsonObject{{"op", "undo"}}}).code, QString("invalid_request"));
        QCOMPARE(edit.toJson(), before);
        QCOMPARE(history.undo.size(), 1);
    }
    void aRefusedBatchLeavesTheEditAndHistoryUnchanged() {
        RecordingEdit edit;
        RecordingEditHistory history;
        QVERIFY(!apply(edit, {cutOp("addCut", 10, 20)}, &history));
        QVERIFY(!apply(edit, {QJsonObject{{"op", "undo"}}}, &history));
        const auto json = edit.toJson();
        const auto undo = history.undo.size(), redo = history.redo.size();
        // The first command is fine, the second is not: nothing applies.
        QVERIFY(apply(edit, {cutOp("addCut", 30, 40), QJsonObject{{"op", "removeCut"}, {"cutId", "nope"}}}, &history));
        QVERIFY(apply(edit, {QJsonObject{{"op", "bogus"}}}, &history));
        QCOMPARE(edit.toJson(), json);
        QCOMPARE(history.undo.size(), undo);
        QCOMPARE(history.redo.size(), redo);
        // The refusal lists every op, the new ones included.
        const auto message = apply(edit, {QJsonObject{{"op", "bogus"}}}, &history).message;
        QVERIFY(message.contains("addCut") && message.contains("removeCut") && message.contains("undo"));
    }
    void theOldestUndoStepIsDroppedPastTheLimit() {
        RecordingEdit edit;
        RecordingEditHistory history;
        const qint64 duration = 1000000;
        for (int i = 0; i < recording::maximumUndoSteps + 5; ++i)
            QVERIFY(!recording::applyEditCommands(edit, {QJsonObject{{"op", "setTrim"}, {"start", double(i)}, {"end", QJsonValue::Null}}}, duration, QSize(64, 64), 30, &history));
        QCOMPARE(history.undo.size(), recording::maximumUndoSteps);
        // Batches 0..4 fell off the front: undoing every kept step ends at the edit batch 5 replaced.
        for (int i = 0; i < recording::maximumUndoSteps; ++i)
            QVERIFY(!recording::applyEditCommands(edit, {QJsonObject{{"op", "undo"}}}, duration, QSize(64, 64), 30, &history));
        QCOMPARE(edit.trimStart, qint64(4));
        QVERIFY(!edit.canUndo);
        QCOMPARE(recording::applyEditCommands(edit, {QJsonObject{{"op", "undo"}}}, duration, QSize(64, 64), 30, &history).code, QString("recording_history_empty"));
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
