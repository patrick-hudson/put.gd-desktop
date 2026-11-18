// #1613: the review timeline proposes edits, it never makes them. Each gesture
// below is one signal on release with frame-aligned times; the window turns it
// into one undo step. Offscreen widget contract only, not desktop proof.
#include "record/FilmstripTimeline.h"
#include "ui/Fonts.h"
#include "ui/Theme.h"
#include "ui/Ui.h"
#include "fixtures/Waits.h"
#include <QApplication>
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
    static void drag(QWidget* w, QPoint from, QPoint to) {
        QTest::mousePress(w, Qt::LeftButton, {}, from);
        const int steps = 6;
        for (int i = 1; i <= steps; ++i) QTest::mouseMove(w, from + (to - from) * i / steps);
        QTest::mouseRelease(w, Qt::LeftButton, {}, to);
    }
private slots:
    void initTestCase() { Fonts::load(); Ui::install(*qApp); }
    void heightIs142AtNormalText() {
        if (!qFuzzyCompare(Ui::textScale(), 1.0)) QSKIP("normal text only");
        QCOMPARE(FilmstripTimeline(std::make_shared<MaskNames>()).sizeHint().height(), 142);
    }
    void bracketDragProposesOnceOnFrames() {
        auto t = make();
        QSignalSpy dragged(t.get(), &FilmstripTimeline::trimEdgeDragged), proposed(t.get(), &FilmstripTimeline::trimProposed);
        drag(t.get(), t->trimHandleRect(true).center(), QPoint(t->xFor(4012345), t->trimHandleRect(true).center().y()));
        QVERIFY(dragged.size() >= 1);
        QCOMPARE(proposed.size(), 1);
        const qint64 start = proposed[0][0].toLongLong(), end = proposed[0][1].toLongLong();
        QVERIFY(onFrame(start));
        QCOMPARE(end, 29400000);
        QVERIFY(std::abs(start - 4012345) < 1000000 / fps);
        // Brackets never cross: dragging the end past the start leaves one frame.
        proposed.clear();
        drag(t.get(), t->trimHandleRect(false).center(), QPoint(t->xFor(1000000), t->trimHandleRect(false).center().y()));
        QCOMPARE(proposed.size(), 1);
        QVERIFY(proposed[0][1].toLongLong() - proposed[0][0].toLongLong() >= 1000000 / fps - 1);
        QVERIFY(onFrame(proposed[0][1].toLongLong()));
    }
    void stripDragSelectsTimeAndClickSeeks() {
        auto t = make();
        QSignalSpy selected(t.get(), &FilmstripTimeline::timeSelected), seek(t.get(), &FilmstripTimeline::seekRequested), cleared(t.get(), &FilmstripTimeline::selectionCleared);
        const int y = t->stripRect().center().y();
        drag(t.get(), QPoint(t->xFor(4000000), y), QPoint(t->xFor(7000000), y));
        QCOMPARE(selected.size(), 1);
        QCOMPARE(seek.size(), 0);
        QVERIFY(onFrame(selected[0][0].toLongLong()) && onFrame(selected[0][1].toLongLong()));
        QVERIFY(t->timeSelection().has_value());
        QCOMPARE(t->inspect()["selection"].toObject()["start"].toDouble(), double(selected[0][0].toLongLong()));
        // A click seeks and clears the selection; nothing else is emitted.
        QTest::mouseClick(t.get(), Qt::LeftButton, {}, QPoint(t->xFor(5000000), y));
        QCOMPARE(seek.size(), 1);
        QCOMPARE(cleared.size(), 1);
        QVERIFY(!t->timeSelection().has_value());
        QCOMPARE(selected.size(), 1);
    }
    void bracketKeysSelectAtThePlayheadAndEscapeClears() {
        auto t = make();
        QSignalSpy selected(t.get(), &FilmstripTimeline::timeSelected), cleared(t.get(), &FilmstripTimeline::selectionCleared);
        t->setPlayhead(6000000);
        QTest::keyClick(t.get(), Qt::Key_BracketLeft);
        QCOMPARE(selected.size(), 1);
        QCOMPARE(selected[0][0].toLongLong(), 6000000);
        t->setPlayhead(9000000);
        QTest::keyClick(t.get(), Qt::Key_BracketRight);
        QCOMPARE(selected.size(), 2);
        QCOMPARE(selected[1][0].toLongLong(), 6000000);
        QCOMPARE(selected[1][1].toLongLong(), 9000000);
        QTest::keyClick(t.get(), Qt::Key_Escape);
        QCOMPARE(cleared.size(), 1);
        QVERIFY(!t->timeSelection().has_value());
    }
    void clickingACutOrACoverSelectsIt() {
        auto t = make();
        QSignalSpy cut(t.get(), &FilmstripTimeline::cutSelected), cover(t.get(), &FilmstripTimeline::coverSelected), seek(t.get(), &FilmstripTimeline::seekRequested);
        QTest::mouseClick(t.get(), Qt::LeftButton, {}, t->cutRect("c1").center() + QPoint(0, -20));
        QCOMPARE(cut.size(), 1);
        QCOMPARE(cut[0][0].toString(), QString("c1"));
        QCOMPARE(t->inspect()["selectedEdit"].toObject()["kind"].toString(), QString("cut"));
        QTest::mouseClick(t.get(), Qt::LeftButton, {}, t->coverBarRect("m2").center());
        QCOMPARE(cover.size(), 1);
        QCOMPARE(cover[0][0].toString(), QString("m2"));
        QCOMPARE(t->inspect()["selectedEdit"].toObject()["id"].toString(), QString("m2"));
        QCOMPARE(seek.size(), 0);
    }
    void selectedCutEdgeDragAdjustsOnceOnRelease() {
        auto t = make();
        t->setSelectedEdit("cut", "c1");
        QSignalSpy adjusted(t.get(), &FilmstripTimeline::cutAdjusted), selected(t.get(), &FilmstripTimeline::cutSelected);
        const int y = t->stripRect().center().y();
        drag(t.get(), QPoint(t->xFor(19500000), y), QPoint(t->xFor(21000000), y));
        QCOMPARE(adjusted.size(), 1);
        QCOMPARE(selected.size(), 0);
        QCOMPARE(adjusted[0][0].toString(), QString("c1"));
        QCOMPARE(adjusted[0][1].toLongLong(), 13000000);
        QVERIFY(onFrame(adjusted[0][2].toLongLong()));
        QVERIFY(std::abs(adjusted[0][2].toLongLong() - 21000000) < 2 * duration / 1120);
    }
    void selectedCoverEndRetimesOnceOnRelease() {
        auto t = make();
        t->setSelectedEdit("cover", "m1");
        QSignalSpy retimed(t.get(), &FilmstripTimeline::coverRetimed);
        const auto bar = t->coverBarRect("m1");
        drag(t.get(), QPoint(bar.right() + 1, bar.center().y()), QPoint(t->xFor(14000000), bar.center().y()));
        QCOMPARE(retimed.size(), 1);
        QCOMPARE(retimed[0][1].toLongLong(), 8200000);
        QVERIFY(onFrame(retimed[0][2].toLongLong()));
    }
    void ctrlWheelKeepsTheTimeUnderThePointer() {
        auto t = make();
        QSignalSpy span(t.get(), &FilmstripTimeline::visibleSpanChanged), zoomed(t.get(), &FilmstripTimeline::zoomChanged);
        const QPoint at(t->xFor(20000000) + 3, t->stripRect().center().y());
        const qint64 before = t->timeAt(at.x());
        for (const int expected : {2, 4}) {
            QWheelEvent wheel(QPointF(at), QPointF(t->mapToGlobal(at)), {}, QPoint(0, 120), Qt::NoButton, Qt::ControlModifier, Qt::NoScrollPhase, false);
            QApplication::sendEvent(t.get(), &wheel);
            QCOMPARE(t->zoom(), expected);
            QVERIFY(std::abs(t->xFor(before) - at.x()) <= 1);
        }
        QCOMPARE(zoomed.size(), 2);
        QVERIFY(span.size() >= 2);
        // setZoom with an anchor does the same; Fit returns to the whole clip.
        t->setZoom(8, qint64(10000000));
        const int x = t->xFor(10000000);
        t->setZoom(16, qint64(10000000));
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
    void skimMovesRestsAndLeavesButNotWhilePlaying() {
        auto t = make();
        QSignalSpy moved(t.get(), &FilmstripTimeline::skimMoved), rested(t.get(), &FilmstripTimeline::skimRested), left(t.get(), &FilmstripTimeline::skimLeft);
        const QPoint at(t->xFor(5300000), t->stripRect().center().y());
        QTest::mouseMove(t.get(), at);
        QCOMPARE(moved.size(), 1);
        QCOMPARE(rested.size(), 0);
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
        QCOMPARE(moved.size(), 1);
        QCOMPARE(left.size(), 1);
    }
    void rulerDragScrubs() {
        auto t = make();
        QSignalSpy started(t.get(), &FilmstripTimeline::scrubStarted), seek(t.get(), &FilmstripTimeline::seekRequested);
        drag(t.get(), QPoint(t->xFor(3000000), t->rulerRect().center().y()), QPoint(t->xFor(9000000), t->rulerRect().center().y()));
        QCOMPARE(started.size(), 1);
        QVERIFY(seek.size() >= 3);
        QVERIFY(std::abs(seek.last()[0].toLongLong() - 9000000) < 40000);
    }
};

QTEST_MAIN(FilmstripTimelineTest)
#include "test_filmstrip_timeline.moc"
