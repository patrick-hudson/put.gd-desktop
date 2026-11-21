// #1613: the review timeline proposes edits, it never makes them. Each gesture
// below is one signal on release with frame-aligned times; the window turns it
// into one undo step. The feel spec's pointer model is pinned here too: hit
// priority and zones, the dead zone and grab offset, cancel, snapping targets,
// hover intent and the zoom ladder. Offscreen widget contract only, not
// desktop proof.
#include "record/FilmstripTimeline.h"
#include "ui/Fonts.h"
#include "ui/Theme.h"
#include "ui/Ui.h"
#include "fixtures/Waits.h"
#include <QApplication>
#include <QJsonArray>
#include <QMouseEvent>
#include <QSignalSpy>
#include <QTest>
#include <QWheelEvent>
#include <cmath>

using namespace putgd;
using namespace putgd::record;

namespace {
constexpr qint64 duration = 32000000;
constexpr int fps = 30;
bool onFrame(qint64 us) {
    const double frames = double(us) * fps / 1e6;
    return std::abs(frames - std::round(frames)) < 1e-3 || us == duration;
}
RecordingEdit edit() {
    RecordingEdit e;
    e.trimStart = 2600000;
    e.trimEnd = 29400000;
    e.cuts = {RecordingCut{"c1", 13000000, 19500000}};
    RecordingMask redact;
    redact.id = "m1"; redact.kind = "solid"; redact.rect = QRect(10, 10, 100, 40); redact.start = 8200000; redact.end = 12600000;
    RecordingMask blur;
    blur.id = "m2"; blur.kind = "blur"; blur.rect = QRect(10, 10, 100, 40); blur.start = 22100000; blur.end = 24900000;
    e.masks = {redact, blur};
    return e;
}
}

class FilmstripTimelineTest : public QObject {
    Q_OBJECT
    std::unique_ptr<FilmstripTimeline> make(int width = 1160) {
        auto timeline = std::make_unique<FilmstripTimeline>(std::make_shared<MaskNames>());
        timeline->setDuration(duration, fps);
        timeline->setEdit(edit());
        timeline->resize(width, timeline->sizeHint().height());
        timeline->show();
        return timeline;
    }
    static void drag(QWidget* w, QPoint from, QPoint to, Qt::KeyboardModifiers modifiers = {}) {
        QTest::mousePress(w, Qt::LeftButton, {}, from);
        const int steps = 6;
        for (int i = 1; i <= steps; ++i) move(w, from + (to - from) * i / steps, modifiers);
        QTest::mouseRelease(w, Qt::LeftButton, modifiers, to);
    }
    // A pointer move with the left button held (QTest::mouseMove carries no modifiers).
    static void move(QWidget* w, QPoint at, Qt::KeyboardModifiers modifiers = {}) {
        QMouseEvent event(QEvent::MouseMove, QPointF(at), QPointF(w->mapToGlobal(at)), Qt::NoButton, Qt::LeftButton, modifiers);
        QApplication::sendEvent(w, &event);
    }
    // What the pointer hovers at a point: a parts() kind, or empty.
    static QString hoverAt(FilmstripTimeline* t, QPoint at) {
        QTest::mouseMove(t, at);
        const auto hover = t->inspect()["hover"];
        return hover.isNull() ? QString() : hover.toObject()["kind"].toString();
    }
    static qint64 pixels(const FilmstripTimeline* t, int px) { return qint64(std::llround(double(px) * t->duration() / t->zoom() / t->stripRect().width())); }
private slots:
    void initTestCase() { Fonts::load(); Ui::install(*qApp); }
    // Taller cover lanes (A12) and the overview track's reserved row (A13).
    void heightIs166AtNormalText() {
        if (!qFuzzyCompare(Ui::textScale(), 1.0)) QSKIP("normal text only");
        QCOMPARE(FilmstripTimeline(std::make_shared<MaskNames>()).sizeHint().height(), 166);
    }
    // U5: a bracket dragged from revision r never writes over a newer edit: the
    // drag ends when the edit changes, and its release proposes nothing.
    void aNewerEditEndsAnEdgeDrag() {
        auto t = make();
        auto e = edit();
        e.revision = 4;
        t->setEdit(e);
        QSignalSpy proposed(t.get(), &FilmstripTimeline::trimProposed), finished(t.get(), &FilmstripTimeline::edgeDragFinished);
        const QPoint from = t->trimHandleRect(true).center(), to(t->xFor(5000000), from.y());
        QTest::mousePress(t.get(), Qt::LeftButton, {}, from);
        QTest::mouseMove(t.get(), (from + to) / 2);
        QTest::mouseMove(t.get(), to);
        QVERIFY(t->dragging());
        auto newer = e;
        newer.revision = 5;
        newer.trimStart = 1000000;
        t->setEdit(newer);
        QVERIFY(!t->dragging());
        QCOMPARE(finished.size(), 1);
        QTest::mouseRelease(t.get(), Qt::LeftButton, {}, to);
        QCOMPARE(proposed.size(), 0);
        // The same revision again (a refused proposal's answer) leaves a drag alone.
        QTest::mousePress(t.get(), Qt::LeftButton, {}, t->trimHandleRect(true).center());
        QTest::mouseMove(t.get(), to);
        t->setEdit(newer);
        QVERIFY(t->dragging());
        QTest::mouseRelease(t.get(), Qt::LeftButton, {}, to);
        QCOMPARE(proposed.size(), 1);
    }
    void bracketDragProposesOnceOnFrames() {
        auto t = make();
        QSignalSpy dragged(t.get(), &FilmstripTimeline::trimEdgeDragged), proposed(t.get(), &FilmstripTimeline::trimProposed);
        // The bracket moves by the pointer's travel, wherever on it the press landed.
        const QPoint from = t->trimHandleRect(true).center();
        drag(t.get(), from, from + QPoint(t->xFor(4012345) - t->xFor(2600000), 0));
        QVERIFY(dragged.size() >= 1);
        QCOMPARE(proposed.size(), 1);
        const qint64 start = proposed[0][0].toLongLong(), end = proposed[0][1].toLongLong();
        QVERIFY(onFrame(start));
        QCOMPARE(end, 29400000);
        QVERIFY(std::abs(start - 4012345) < 2 * 1000000 / fps);
        // Brackets never cross: dragging the end past the start leaves one frame.
        proposed.clear();
        drag(t.get(), t->trimHandleRect(false).center(), QPoint(t->xFor(1000000), t->trimHandleRect(false).center().y()));
        QCOMPARE(proposed.size(), 1);
        QVERIFY(proposed[0][1].toLongLong() - proposed[0][0].toLongLong() >= 1000000 / fps - 1);
        QVERIFY(onFrame(proposed[0][1].toLongLong()));
    }
    // A3: a press on empty strip seeks at once; a drag from it selects time,
    // a click clears the selection.
    void stripPressSeeksDragSelectsClickClears() {
        auto t = make();
        QSignalSpy selected(t.get(), &FilmstripTimeline::timeSelected), seek(t.get(), &FilmstripTimeline::seekRequested), cleared(t.get(), &FilmstripTimeline::selectionCleared),
            edge(t.get(), &FilmstripTimeline::editEdgeDragged);
        const int y = t->stripRect().center().y();
        QTest::mousePress(t.get(), Qt::LeftButton, {}, QPoint(t->xFor(4000000), y));
        QCOMPARE(seek.size(), 1);
        QVERIFY(std::abs(seek[0][0].toLongLong() - 4000000) < 30000);
        // 3 px is still a click; 4 px starts the selection.
        move(t.get(), QPoint(t->xFor(4000000) + 3, y));
        QCOMPARE(edge.size(), 0);
        for (int i = 1; i <= 6; ++i) move(t.get(), QPoint(t->xFor(4000000) + i * (t->xFor(7000000) - t->xFor(4000000)) / 6, y));
        QTest::mouseRelease(t.get(), Qt::LeftButton, {}, QPoint(t->xFor(7000000), y));
        QCOMPARE(selected.size(), 1);
        QCOMPARE(seek.size(), 1);  // the playhead stays at the anchor
        QVERIFY(!edge.isEmpty());
        QCOMPARE(edge.last()[0].toString(), QString("selection"));
        QVERIFY(onFrame(selected[0][0].toLongLong()) && onFrame(selected[0][1].toLongLong()));
        QVERIFY(t->timeSelection().has_value());
        QCOMPARE(t->inspect()["selection"].toObject()["start"].toDouble(), double(selected[0][0].toLongLong()));
        // A click seeks on press and clears the selection; nothing else is emitted.
        QTest::mouseClick(t.get(), Qt::LeftButton, {}, QPoint(t->xFor(5000000), y));
        QCOMPARE(seek.size(), 2);
        QCOMPARE(cleared.size(), 1);
        QVERIFY(!t->timeSelection().has_value());
        QCOMPARE(selected.size(), 1);
        // Shift+click moves the selection's nearer edge, without seeking.
        t->setTimeSelection(recording::Span{4000000, 7000000});
        QTest::mouseClick(t.get(), Qt::LeftButton, Qt::ShiftModifier, QPoint(t->xFor(10000000), y));
        QCOMPARE(selected.size(), 2);
        QCOMPARE(selected[1][0].toLongLong(), 4000000);
        QVERIFY(std::abs(selected[1][1].toLongLong() - 10000000) < 40000);
        QCOMPARE(seek.size(), 2);
    }
    void markSelectionEdgeSelectsAtThePlayhead() {
        auto t = make();
        QSignalSpy selected(t.get(), &FilmstripTimeline::timeSelected);
        t->setPlayhead(6000000);
        t->markSelectionEdge(true);
        QCOMPARE(selected.size(), 1);
        QCOMPARE(selected[0][0].toLongLong(), 6000000);
        QCOMPARE(selected[0][1].toLongLong(), 29400000);
        t->setPlayhead(9000000);
        t->markSelectionEdge(false);
        QCOMPARE(selected.size(), 2);
        QCOMPARE(selected[1][0].toLongLong(), 6000000);
        QCOMPARE(selected[1][1].toLongLong(), 9000000);
    }
    // A1, A2, A12: one hit test for hover, press and parts(), in priority order.
    void hitPriorityAndZones() {
        auto t = make();
        const int y = t->stripRect().center().y(), ruler = t->rulerRect().center().y();
        // Trim bracket: its body plus 8 px each side.
        const int trim = t->xFor(2600000);
        QCOMPARE(hoverAt(t.get(), QPoint(trim + 7, y)), QString("trim-start"));
        QCOMPARE(hoverAt(t.get(), QPoint(trim - 13 - 7, y)), QString("trim-start"));
        QCOMPARE(hoverAt(t.get(), QPoint(trim + 11, y)), QString());
        // Cut edges without selecting first; the body between them.
        const int a = t->xFor(13000000), b = t->xFor(19500000);
        QCOMPARE(hoverAt(t.get(), QPoint(a + 7, y)), QString("cut-start"));
        QCOMPARE(hoverAt(t.get(), QPoint(a - 7, y)), QString("cut-start"));
        QCOMPARE(hoverAt(t.get(), QPoint(b - 7, y)), QString("cut-end"));
        QCOMPARE(hoverAt(t.get(), QPoint((a + b) / 2, y)), QString("cut"));
        // An edge beats the playhead on the strip; the playhead's cap owns the ruler.
        t->setPlayhead(13000000);
        QCOMPARE(hoverAt(t.get(), QPoint(a + 2, y)), QString("cut-start"));
        QCOMPARE(hoverAt(t.get(), QPoint(a + 8, ruler)), QString("playhead"));
        t->setPlayhead(5000000);
        QCOMPARE(hoverAt(t.get(), QPoint(t->xFor(5000000) + 5, y)), QString("playhead"));
        // A bar's hit area is its whole lane row; its edges work unselected.
        const auto bar = t->coverBarRect("m1");
        QCOMPARE(hoverAt(t.get(), QPoint(bar.center().x(), bar.top() - 1)), QString("cover"));
        QCOMPARE(hoverAt(t.get(), QPoint(bar.left() + 2, bar.center().y())), QString("cover-start"));
        // parts() reports the same zones.
        const auto parts = t->parts();
        const auto cutStart = std::find_if(parts.cbegin(), parts.cend(), [](const auto& p) { return p.kind == "cut-start" && p.id == "c1"; });
        QVERIFY(cutStart != parts.cend() && cutStart->rect.contains(QPoint(a + 7, y)) && cutStart->rect.contains(QPoint(a - 7, y)));
        QCOMPARE(parts.value(t->partAt(QPoint(a + 2, y))).kind, QString("cut-start"));  // the accessible childAt
        // A narrow cut keeps a body between its shrunk edge zones.
        auto e = edit();
        e.cuts.append(RecordingCut{"c2", 5000000, 5300000});
        t->setEdit(e);
        t->setPlayhead(0);
        const int n = (t->xFor(5000000) + t->xFor(5300000)) / 2;
        QCOMPARE(hoverAt(t.get(), QPoint(n, y)), QString("cut"));
        QCOMPARE(hoverAt(t.get(), QPoint(n - 8, y)), QString("cut-start"));
        QCOMPARE(hoverAt(t.get(), QPoint(n + 8, y)), QString("cut-end"));
        // The "+n" chip has its own space and asks for the Edits list.
        RecordingMask third = e.masks[0];
        third.id = "m3";
        third.start = 8000000;
        third.end = 13000000;
        RecordingMask fourth = third;
        fourth.id = "m4";
        e.masks << third << fourth;
        t->setEdit(e);
        QCOMPARE(t->inspect()["hiddenCovers"].toInt(), 1);
        const auto withMore = t->parts();
        const auto more = std::find_if(withMore.cbegin(), withMore.cend(), [](const auto& p) { return p.kind == "more"; });
        QVERIFY(more != withMore.cend());
        QSignalSpy asked(t.get(), &FilmstripTimeline::moreCoversRequested), cover(t.get(), &FilmstripTimeline::coverSelected);
        QTest::mouseClick(t.get(), Qt::LeftButton, {}, more->rect.center());
        QCOMPARE(asked.size(), 1);
        QCOMPARE(cover.size(), 0);
    }
    void clickingACutOrACoverSelectsIt() {
        auto t = make();
        QSignalSpy cut(t.get(), &FilmstripTimeline::cutSelected), cover(t.get(), &FilmstripTimeline::coverSelected), seek(t.get(), &FilmstripTimeline::seekRequested), retimed(t.get(), &FilmstripTimeline::coverRetimed);
        QTest::mouseClick(t.get(), Qt::LeftButton, {}, t->cutRect("c1").center() + QPoint(0, -20));
        QCOMPARE(cut.size(), 1);
        QCOMPARE(cut[0][0].toString(), QString("c1"));
        QCOMPARE(t->inspect()["selectedEdit"].toObject()["kind"].toString(), QString("cut"));
        QTest::mouseClick(t.get(), Qt::LeftButton, {}, t->coverBarRect("m2").center());
        QCOMPARE(cover.size(), 1);
        QCOMPARE(cover[0][0].toString(), QString("m2"));
        QCOMPARE(t->inspect()["selectedEdit"].toObject()["id"].toString(), QString("m2"));
        QCOMPARE(seek.size(), 0);
        QCOMPARE(retimed.size(), 0);
    }
    // A4, A5: an unselected cut's edge drags in one gesture; nothing moves in
    // the first 3 px, then the edge keeps its grab offset.
    void cutEdgeDragSelectsAndAdjustsOnceOnRelease() {
        auto t = make();
        QSignalSpy adjusted(t.get(), &FilmstripTimeline::cutAdjusted), selected(t.get(), &FilmstripTimeline::cutSelected), dragged(t.get(), &FilmstripTimeline::editEdgeDragged);
        const int y = t->stripRect().center().y();
        const int from = t->xFor(19500000) - 5;
        QTest::mousePress(t.get(), Qt::LeftButton, {}, QPoint(from, y));
        QCOMPARE(selected.size(), 1);
        move(t.get(), QPoint(from + 2, y));
        QCOMPARE(dragged.size(), 0);
        for (int i = 1; i <= 6; ++i) move(t.get(), QPoint(from + i * 9, y));
        QTest::mouseRelease(t.get(), Qt::LeftButton, {}, QPoint(from + 54, y));
        QCOMPARE(adjusted.size(), 1);
        QCOMPARE(adjusted[0][0].toString(), QString("c1"));
        QCOMPARE(adjusted[0][1].toLongLong(), 13000000);
        QVERIFY(onFrame(adjusted[0][2].toLongLong()));
        // The edge moved by 54 px of time, not to the pointer's x (5 px further).
        QVERIFY(std::abs(adjusted[0][2].toLongLong() - (19500000 + pixels(t.get(), 54))) <= 1000000 / fps);
    }
    void coverEndAndSlideRetimeOnceOnRelease() {
        auto t = make();
        QSignalSpy retimed(t.get(), &FilmstripTimeline::coverRetimed);
        const auto bar = t->coverBarRect("m1");
        drag(t.get(), QPoint(bar.right() + 1, bar.center().y()), QPoint(t->xFor(14000000), bar.center().y()));
        QCOMPARE(retimed.size(), 1);
        QCOMPARE(retimed[0][1].toLongLong(), 8200000);
        QVERIFY(onFrame(retimed[0][2].toLongLong()));
        // The body slides the whole cover, its length kept.
        retimed.clear();
        t->setEdit(edit());
        const auto m1 = t->coverBarRect("m1");
        drag(t.get(), m1.center(), m1.center() + QPoint(50, 0));
        QCOMPARE(retimed.size(), 1);
        const qint64 start = retimed[0][1].toLongLong(), end = retimed[0][2].toLongLong();
        QCOMPARE(end - start, 12600000 - 8200000);
        QVERIFY(onFrame(start));
        QVERIFY(std::abs(start - (8200000 + pixels(t.get(), 50))) <= 1000000 / fps);
        // Clamped to the recording.
        retimed.clear();
        const auto m2 = t->coverBarRect("m2");
        drag(t.get(), m2.center(), QPoint(t->width() + 40, m2.center().y()));
        QCOMPARE(retimed.size(), 1);
        QCOMPARE(retimed[0][2].toLongLong(), duration);
        QCOMPARE(retimed[0][2].toLongLong() - retimed[0][1].toLongLong(), 24900000 - 22100000);
    }
    // A6: Esc or a right press ends a drag, restores what it changed and proposes nothing.
    void escapeAndRightPressCancel() {
        auto t = make();
        connect(t.get(), &FilmstripTimeline::seekRequested, t.get(), &FilmstripTimeline::setPlayhead);
        QSignalSpy adjusted(t.get(), &FilmstripTimeline::cutAdjusted), retimed(t.get(), &FilmstripTimeline::coverRetimed), finished(t.get(), &FilmstripTimeline::edgeDragFinished), seek(t.get(), &FilmstripTimeline::seekRequested);
        const int y = t->stripRect().center().y();
        const int end = t->xFor(19500000);
        QTest::mousePress(t.get(), Qt::LeftButton, {}, QPoint(end, y));
        for (int i = 1; i <= 5; ++i) move(t.get(), QPoint(end + i * 10, y));
        QTest::keyClick(t.get(), Qt::Key_Escape);
        QVERIFY(!t->dragging());
        QCOMPARE(finished.size(), 1);
        QTest::mouseRelease(t.get(), Qt::LeftButton, {}, QPoint(end + 50, y));
        QCOMPARE(adjusted.size(), 0);
        QCOMPARE(t->inspect()["selectedEdit"].toObject()["id"].toString(), QString("c1"));  // the selection stays
        // A right press during a cover slide.
        const auto bar = t->coverBarRect("m2");
        QTest::mousePress(t.get(), Qt::LeftButton, {}, bar.center());
        for (int i = 1; i <= 4; ++i) move(t.get(), bar.center() + QPoint(i * 10, 0));
        QTest::mousePress(t.get(), Qt::RightButton, {}, bar.center() + QPoint(40, 0));
        QVERIFY(!t->dragging());
        QTest::mouseRelease(t.get(), Qt::RightButton, {}, bar.center() + QPoint(40, 0));
        QTest::mouseRelease(t.get(), Qt::LeftButton, {}, bar.center() + QPoint(40, 0));
        QCOMPARE(retimed.size(), 0);
        QCOMPARE(finished.size(), 2);
        // A cancelled scrub puts the playhead back; a scrub keeps the time selection (A14).
        t->setPlayhead(5000000);
        t->setTimeSelection(recording::Span{4000000, 7000000});
        seek.clear();
        const int ruler = t->rulerRect().center().y();
        QTest::mousePress(t.get(), Qt::LeftButton, {}, QPoint(t->xFor(20000000), ruler));
        for (int i = 1; i <= 4; ++i) move(t.get(), QPoint(t->xFor(20000000) + i * 10, ruler));
        QVERIFY(t->playhead() > 20000000);
        QTest::keyClick(t.get(), Qt::Key_Escape);
        QCOMPARE(seek.last()[0].toLongLong(), 5000000);
        QCOMPARE(t->playhead(), 5000000);
        QTest::mouseRelease(t.get(), Qt::LeftButton, {}, QPoint(t->xFor(20000000) + 40, ruler));
        QVERIFY(t->timeSelection().has_value());
    }
    // A7: edges snap to other edges and the playhead within 6 px; Ctrl and N bypass it.
    void snappingTargets() {
        auto t = make();
        QSignalSpy adjusted(t.get(), &FilmstripTimeline::cutAdjusted), toggled(t.get(), &FilmstripTimeline::snappingChanged);
        const int y = t->stripRect().center().y();
        const QPoint from(t->xFor(19500000), y), near(t->xFor(22100000) - 4, y);  // cover m2 starts at 22.1 s
        drag(t.get(), from, near);
        QCOMPARE(adjusted.size(), 1);
        QCOMPARE(adjusted[0][2].toLongLong(), 22100000);
        t->setEdit(edit());
        drag(t.get(), from, near, Qt::ControlModifier);
        QCOMPARE(adjusted.size(), 2);
        QVERIFY(adjusted[1][2].toLongLong() != 22100000 && onFrame(adjusted[1][2].toLongLong()));
        t->setSnapping(false);
        t->setSnapping(false);
        QCOMPARE(toggled.size(), 1);
        QCOMPARE(t->inspect()["snapping"].toBool(), false);
        t->setEdit(edit());
        drag(t.get(), from, near);
        QCOMPARE(adjusted.size(), 3);
        QVERIFY(adjusted[2][2].toLongLong() != 22100000);
        // A slid cover's end snaps too (to the playhead here).
        t->setSnapping(true);
        t->setEdit(edit());
        t->setPlayhead(15000000);
        QSignalSpy retimed(t.get(), &FilmstripTimeline::coverRetimed);
        const auto bar = t->coverBarRect("m1");
        drag(t.get(), bar.center(), bar.center() + QPoint(t->xFor(15000000) - t->xFor(12600000) - 3, 0));
        QCOMPARE(retimed.size(), 1);
        QCOMPARE(retimed[0][2].toLongLong(), 15000000);
    }
    // A9: a drag held near a strip end above Fit scrolls the view and the edge follows.
    void dragAutoscrollsAboveFit() {
        auto t = make();
        t->setZoom(4, qint64(20000000));
        const auto before = t->visibleSpan();
        QVERIFY(before.end < 26000000);
        QSignalSpy adjusted(t.get(), &FilmstripTimeline::cutAdjusted);
        const int y = t->stripRect().center().y();
        const QPoint from(t->xFor(19500000), y), past(t->width() + 30, y);
        QTest::mousePress(t.get(), Qt::LeftButton, {}, from);
        for (int i = 1; i <= 4; ++i) move(t.get(), from + (past - from) * i / 4);
        PUTGD_TRY_VERIFY(t->visibleSpan().start > before.start + 2000000);
        QTest::mouseRelease(t.get(), Qt::LeftButton, {}, past);
        QCOMPARE(adjusted.size(), 1);
        QVERIFY(adjusted[0][2].toLongLong() > before.end);
        QVERIFY(!t->dragging());
    }
    void zoomLadder() {
        auto t = make();
        // 32 s at 30 fps over 1120 px: up to the first level at about 8 px a frame.
        QCOMPARE(t->zoomLevels(), (QList<int>{1, 2, 3, 4, 6, 8}));
        QCOMPARE(t->inspect()["zoomLevels"].toArray().size(), 6);
        t->setPlayhead(10000000);
        const int x = t->xFor(10000000);
        t->zoomIn();
        QCOMPARE(t->zoom(), 2);
        QVERIFY(std::abs(t->xFor(10000000) - x) <= 1);  // the playhead stays put
        t->zoomIn();
        QCOMPARE(t->zoom(), 3);
        t->zoomOut();
        QCOMPARE(t->zoom(), 2);
        t->setZoom(16);
        QCOMPARE(t->zoom(), 8);
        QVERIFY(!t->overviewRect().isNull());
        t->zoomFit();
        QCOMPARE(t->zoom(), 1);
        QVERIFY(t->overviewRect().isNull());
        // A long recording gets the whole ladder.
        t->setDuration(30LL * 60 * 1000000, 30);
        const auto levels = t->zoomLevels();
        QCOMPARE(levels.mid(0, 10), (QList<int>{1, 2, 3, 4, 6, 8, 12, 16, 24, 32}));
        const double frames = 30.0 * 60 * 30, width = t->stripRect().width();
        QVERIFY(levels.last() * width / frames >= 8 && levels[levels.size() - 2] * width / frames < 8);
    }
    void ctrlWheelKeepsTheTimeUnderThePointer() {
        auto t = make();
        QSignalSpy span(t.get(), &FilmstripTimeline::visibleSpanChanged), zoomed(t.get(), &FilmstripTimeline::zoomChanged);
        const QPoint at(t->xFor(20000000) + 3, t->stripRect().center().y());
        const qint64 before = t->timeAt(at.x());
        for (const int expected : {2, 3}) {
            QWheelEvent wheel(QPointF(at), QPointF(t->mapToGlobal(at)), {}, QPoint(0, 120), Qt::NoButton, Qt::ControlModifier, Qt::NoScrollPhase, false);
            QApplication::sendEvent(t.get(), &wheel);
            QCOMPARE(t->zoom(), expected);
            QVERIFY(std::abs(t->xFor(before) - at.x()) <= 1);
        }
        QCOMPARE(zoomed.size(), 2);
        QVERIFY(span.size() >= 2);
        // Two notches at once are two levels.
        QWheelEvent wheel(QPointF(at), QPointF(t->mapToGlobal(at)), {}, QPoint(0, 240), Qt::NoButton, Qt::ControlModifier, Qt::NoScrollPhase, false);
        QApplication::sendEvent(t.get(), &wheel);
        QCOMPARE(t->zoom(), 6);
        // setZoom with an anchor does the same; Fit returns to the whole clip.
        t->setZoom(4, qint64(10000000));
        const int x = t->xFor(10000000);
        t->setZoom(8, qint64(10000000));
        QVERIFY(std::abs(t->xFor(10000000) - x) <= 1);
        t->setZoom(1);
        QCOMPARE(t->visibleSpan().start, 0);
        QCOMPARE(t->visibleSpan().end, duration);
    }
    void tileTimesCoverTheVisibleSpanEndsFirst() {
        auto t = make();
        auto times = t->tileTimes();
        QCOMPARE(times.size(), 12);
        QVERIFY(times[0] < times[1]);
        QVERIFY(times[0] < times[2] && times[1] > times[2]);
        QCOMPARE(*std::max_element(times.begin(), times.end()), times[1]);
        t->setZoom(4, qint64(20000000));
        times = t->tileTimes();
        QCOMPARE(times.size(), 12);
        const auto span = t->visibleSpan();
        for (const qint64 at : times) QVERIFY(at >= span.start && at < span.end);
        QVERIFY(times[0] < times[1] && times[1] < span.end);
        QCOMPARE(*std::min_element(times.begin(), times.end()), times[0]);
        // A wide timeline gets more tiles, never more than 24.
        auto wide = make(3000);
        QCOMPARE(wide->tileTimes().size(), 24);
    }
    // B1: skimming starts only after hover intent; then it moves, rests and leaves.
    void skimNeedsIntentThenMovesRestsAndLeaves() {
        auto t = make();
        QSignalSpy moved(t.get(), &FilmstripTimeline::skimMoved), rested(t.get(), &FilmstripTimeline::skimRested), left(t.get(), &FilmstripTimeline::skimLeft);
        const QPoint at(t->xFor(5300000), t->stripRect().center().y());
        QTest::mouseMove(t.get(), at);
        QCOMPARE(moved.size(), 0);
        QVERIFY(putgd::test::waitForSignal(moved));
        QCOMPARE(t->inspect()["skim"].toObject()["exact"].toBool(), false);
        QVERIFY(putgd::test::waitForSignal(rested));
        QCOMPARE(rested.size(), 1);
        QVERIFY(std::abs(rested[0][0].toLongLong() - 5300000) < 40000);
        t->setSkimExact(true);
        QCOMPARE(t->inspect()["skim"].toObject()["exact"].toBool(), true);
        QEvent leave(QEvent::Leave);
        QApplication::sendEvent(t.get(), &leave);
        QCOMPARE(left.size(), 1);
        QVERIFY(t->inspect()["skim"].isNull());
        t->setPlaying(true);
        QTest::mouseMove(t.get(), at + QPoint(30, 0));
        PUTGD_COMPARE_STEADY(moved.size(), 1, 150);
        QCOMPARE(left.size(), 1);
    }
    // B1, B4: a pointer crossing the timeline on its way elsewhere, or any
    // hover with skimming off, never changes the viewer.
    void crossingOrSkimmingOffNeverSkims() {
        auto t = make();
        QSignalSpy moved(t.get(), &FilmstripTimeline::skimMoved), toggled(t.get(), &FilmstripTimeline::skimmingChanged);
        const int x = t->xFor(9000000);
        for (int y = 0; y <= t->coversRect().center().y(); y += 12) QTest::mouseMove(t.get(), QPoint(x + y / 24, y));
        PUTGD_COMPARE_STEADY(moved.size(), 0, 150);
        t->setSkimming(false);
        t->setSkimming(false);
        QCOMPARE(toggled.size(), 1);
        QCOMPARE(t->inspect()["skimming"].toBool(), false);
        for (int i = 0; i < 8; ++i) QTest::mouseMove(t.get(), QPoint(x + i * 8, t->stripRect().center().y()));
        PUTGD_COMPARE_STEADY(moved.size(), 0, 150);
        QVERIFY(t->inspect()["skim"].isNull());
    }
    // A3: after a click nothing skims until the pointer has moved 2 px.
    void noSkimAfterAClickUntilThePointerMoves() {
        auto t = make();
        QSignalSpy moved(t.get(), &FilmstripTimeline::skimMoved);
        const QPoint at(t->xFor(9000000), t->stripRect().center().y());
        for (int i = 0; i < 4; ++i) QTest::mouseMove(t.get(), at + QPoint(i * 4 - 12, 0));
        QVERIFY(putgd::test::waitForSignal(moved));
        QTest::mouseClick(t.get(), Qt::LeftButton, {}, at);
        moved.clear();
        QTest::mouseMove(t.get(), at + QPoint(1, 0));
        PUTGD_COMPARE_STEADY(moved.size(), 0, 120);
        QTest::mouseMove(t.get(), at + QPoint(3, 0));
        QVERIFY(putgd::test::waitForSignal(moved));
    }
    // A3, A14: dragging from the ruler scrubs and keeps the selection.
    void rulerDragScrubs() {
        auto t = make();
        t->setSelectedEdit("cover", "m1");
        QSignalSpy started(t.get(), &FilmstripTimeline::scrubStarted), seek(t.get(), &FilmstripTimeline::seekRequested), cleared(t.get(), &FilmstripTimeline::selectionCleared);
        drag(t.get(), QPoint(t->xFor(3000000), t->rulerRect().center().y()), QPoint(t->xFor(9000000), t->rulerRect().center().y()));
        QCOMPARE(started.size(), 1);
        QVERIFY(seek.size() >= 3);
        QVERIFY(std::abs(seek.last()[0].toLongLong() - 9000000) < 40000);
        QCOMPARE(cleared.size(), 0);
        QCOMPARE(t->inspect()["selectedEdit"].toObject()["id"].toString(), QString("m1"));
        // A plain click on the ruler seeks, never scrubs, and keeps the selection too.
        QTest::mouseClick(t.get(), Qt::LeftButton, {}, QPoint(t->xFor(25000000), t->rulerRect().center().y()));
        QCOMPARE(started.size(), 1);
        QVERIFY(std::abs(seek.last()[0].toLongLong() - 25000000) < 40000);
        QCOMPARE(t->inspect()["selectedEdit"].toObject()["id"].toString(), QString("m1"));
    }
};

QTEST_MAIN(FilmstripTimelineTest)
#include "test_filmstrip_timeline.moc"
