#include "ReviewWindowFixture.h"

// Discarding a recording is irreversible: it happens only after the explicit
// confirmation, never from the first click, Escape or the window's close button.
class ReviewWindowTest : public ReviewWindowFixture {
    Q_OBJECT
private slots:
    void discardAsksBeforeTheRecordingGoes() {
        ReviewWindow window(options(), config());
        QSignalSpy cancelled(&window, &ReviewWindow::cancelRequested);
        window.show();
        QVERIFY(putgd::test::waitForWindowExposed(&window));
        auto* discard = window.findChild<QPushButton*>("discard");
        QVERIFY(discard);
        discard->click();
        QMessageBox* box = nullptr;
        PUTGD_TRY_VERIFY((box = window.findChild<QMessageBox*>("confirmAction")) && box->isVisible());
        QCOMPARE(cancelled.size(), 0);
        QTest::keyClick(box, Qt::Key_Escape);
        PUTGD_TRY_VERIFY(!window.inspect()["discardConfirmation"].toObject()["open"].toBool());
        QCOMPARE(cancelled.size(), 0);
        QVERIFY(window.isVisible());
        window.close();
        QVERIFY(window.isVisible());
        QVERIFY(window.drive({{"action", "keepReviewing"}}).code.isEmpty());
        QCOMPARE(cancelled.size(), 0);
        QCOMPARE(window.drive({{"action", "confirmDiscard"}}).code, QString("recording_state"));
        QVERIFY(window.drive({{"action", "discard"}}).state["discardConfirmation"].toObject()["open"].toBool());
        QVERIFY(window.drive({{"action", "confirmDiscard"}}).code.isEmpty());
        QCOMPARE(cancelled.size(), 1);
    }
    // #1613: C cuts exactly the selected time, and once the resident accepts
    // it the playhead lands 1 s of output time before the seam.
    void cuttingTheSelectionLandsBeforeTheSeam() {
        ReviewWindow window(options(), config());
        QSignalSpy spy(&window, &ReviewWindow::editProposed);
        window.show();
        QVERIFY(putgd::test::waitForWindowExposed(&window));
        RecordingEdit edit;
        edit.revision = 2;
        window.setEdit(edit);
        emit timeline(window)->timeSelected(4000000, 6000000);
        QTest::keyClick(&window, Qt::Key_C);
        QCOMPARE(spy.size(), 1);
        QCOMPARE(spy.last().at(0).toULongLong(), 2ull);
        const auto command = proposed(spy);
        QCOMPARE(command["op"].toString(), QString("addCut"));
        QCOMPARE(command["start"].toDouble(), 4000000.0);
        QCOMPARE(command["end"].toDouble(), 6000000.0);
        auto cut = edit;
        cut.revision = 3;
        cut.cuts = {RecordingCut{"c1", 4000000, 6000000}};
        window.setEdit(cut);
        const auto state = window.inspect();
        QCOMPARE(state["outputDurationUs"].toDouble(), 10000000.0);
        QCOMPARE(state["playheadUs"].toDouble(), 3000000.0);
        QCOMPARE(state["outputPlayheadUs"].toDouble(), 3000000.0);
    }
    // #1613: X/B cover the selected time snapped outward to whole frames (or the
    // whole recording), Upload waits while placing, and the drawn box is one addMask.
    void aCoverSpansTheSelectionOnWholeFrames() {
        ReviewWindow window(options(), config());
        QSignalSpy spy(&window, &ReviewWindow::editProposed);
        window.show();
        QVERIFY(putgd::test::waitForWindowExposed(&window));
        RecordingEdit edit;
        edit.revision = 2;
        window.setEdit(edit);
        auto* upload = window.findChild<QPushButton*>("upload");
        emit timeline(window)->timeSelected(2010000, 3990000);
        QTest::keyClick(&window, Qt::Key_X);
        auto placement = window.inspect()["placement"].toObject();
        QCOMPARE(placement["start"].toDouble(), 2000000.0);
        QCOMPARE(placement["end"].toDouble(), 4000000.0);
        QVERIFY(placement["banner"].toString().contains(QStringLiteral("0:02.0 – 0:04.0")));
        QVERIFY(!upload->isEnabled());
        emit window.findChild<FrameView*>("frame")->maskCreated("solid", QRect(10, 10, 100, 50));
        QCOMPARE(spy.size(), 1);
        const auto command = proposed(spy);
        QCOMPARE(command["op"].toString(), QString("addMask"));
        const auto mask = command["mask"].toObject();
        QCOMPARE(mask["kind"].toString(), QString("solid"));
        QCOMPARE(mask["start"].toDouble(), 2000000.0);
        QCOMPARE(mask["end"].toDouble(), 4000000.0);
        QCOMPARE(mask["rect"].toArray(), (QJsonArray{10, 10, 100, 50}));
        QVERIFY(upload->isEnabled());
        // Nothing selected: the whole recording; Escape cancels without an edit.
        QTest::keyClick(&window, Qt::Key_B);
        placement = window.inspect()["placement"].toObject();
        QVERIFY(placement["whole"].toBool());
        QCOMPARE(placement["end"].toDouble(), double(duration));
        QTest::keyClick(&window, Qt::Key_Escape);
        QVERIFY(window.inspect()["placement"].isNull());
        QCOMPARE(spy.size(), 1);
    }
    // #1613: Undo proposes the resident's own history step, and only when it has one.
    void undoFollowsTheResidentHistory() {
        ReviewWindow window(options(), config());
        QSignalSpy spy(&window, &ReviewWindow::editProposed);
        window.show();
        QVERIFY(putgd::test::waitForWindowExposed(&window));
        auto* undo = window.findChild<QToolButton*>("toolUndo");
        QVERIFY(undo && !undo->isEnabled());
        RecordingEdit edit;
        edit.revision = 4;
        edit.canUndo = true;
        window.setEdit(edit);
        QVERIFY(undo->isEnabled());
        undo->click();
        QCOMPARE(spy.size(), 1);
        QCOMPARE(spy.last().at(1).toJsonArray(), (QJsonArray{QJsonObject{{"op", "undo"}}}));
    }
    // #1613: the GIF/WebP limit counts kept time, so Keep the first n s trims
    // the end where the output (after cuts) reaches the limit.
    void trimToLimitCountsKeptTime() {
        auto gif = config();
        gif.format = "gif";
        gif.codec.clear();
        ReviewWindow window(options(), gif);
        QSignalSpy spy(&window, &ReviewWindow::editProposed);
        window.show();
        QVERIFY(putgd::test::waitForWindowExposed(&window));
        window.setLimits(5000000, 1280);
        RecordingEdit edit;
        edit.revision = 2;
        edit.cuts = {RecordingCut{"c1", 1000000, 4000000}};
        window.setEdit(edit);
        QVERIFY(window.drive({{"action", "trimToLimit"}}).code.isEmpty());
        const auto command = proposed(spy);
        QCOMPARE(command["op"].toString(), QString("setTrim"));
        QCOMPARE(command["end"].toDouble(), 8000000.0);
    }
    // While recording, Stop keeps the clip and Discard only asks.
    void stoppingKeepsTheClipAndDiscardStillAsks() {
        ControlBar::Options o;
        o.targetLabel = "Region"; o.formats = {"mp4"}; o.codecs = {{"mp4", QJsonArray{"h264"}}};
        ControlBar bar(o, config());
        bar.setState("recording"); bar.show();
        auto* stop = bar.findChild<QPushButton*>("stop");
        auto* discard = bar.findChild<QPushButton*>("cancel");
        QVERIFY(stop && discard);
        QSignalSpy stopped(&bar, &ControlBar::stopRequested), cancelled(&bar, &ControlBar::cancelRequested);
        stop->click(); QCOMPARE(stopped.size(), 1); QCOMPARE(cancelled.size(), 0);
        discard->click(); QVERIFY(bar.confirming()); QCOMPARE(cancelled.size(), 0);
        bar.findChild<QPushButton*>("keepRecording")->click(); QVERIFY(!bar.confirming()); QCOMPARE(cancelled.size(), 0);
    }
};

QTEST_MAIN(ReviewWindowTest)
#include "test_review_window_actions.moc"
