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
    // #1613: every control an agent can reach calls the code its gesture calls, so
    // the proposals match the gesture's, and inspect reports the state it leaves.
    void agentActionsProposeWhatTheGesturesPropose() {
        ReviewWindow window(options(), config());
        QSignalSpy spy(&window, &ReviewWindow::editProposed);
        window.show();
        QVERIFY(putgd::test::waitForWindowExposed(&window));
        RecordingEdit edit;
        edit.revision = 5;
        edit.canUndo = edit.canRedo = true;
        edit.cuts = {RecordingCut{"c1", 1000000, 2000000}};
        edit.masks = {mask("m1", "blur", 4000000, 8000000)};
        window.setEdit(edit);
        // The proposal after `agent` equals the one after `gesture`.
        const auto same = [&](const QJsonObject& agent, const std::function<void()>& gesture) {
            const auto before = spy.size();
            const auto result = window.drive(agent);
            QVERIFY2(result.code.isEmpty(), qPrintable(agent["action"].toString() + ": " + result.message));
            QCOMPARE(spy.size(), before + 1);
            const auto driven = spy.last().at(1).toJsonArray();
            gesture();
            QCOMPARE(spy.size(), before + 2);
            QCOMPARE(spy.last().at(1).toJsonArray(), driven);
        };
        const auto drive = [&](const QJsonObject& params) { return window.drive(params); };
        const auto button = [&](const char* name) { return window.findChild<QAbstractButton*>(name); };
        auto* strip = timeline(window);
        // Time selection: selectRange is the drag; cut is C.
        QCOMPARE(drive({{"action", "cut"}}).code, QString("recording_state"));
        QCOMPARE(drive({{"action", "selectRange"}, {"start", 5000000}, {"end", 5000000}}).code, QString("invalid_request"));
        auto state = drive({{"action", "selectRange"}, {"start", 5000000}, {"end", 7000000}}).state;
        QCOMPARE(state["selection"].toObject()["start"].toDouble(), 5000000.0);
        QCOMPARE(state["selection"].toObject()["lengthUs"].toDouble(), 2000000.0);
        QVERIFY(state["selectedEdit"].isNull() && state["tools"].toObject()["cut"].toBool());
        QVERIFY(drive({{"action", "clearSelection"}}).state["selection"].isNull());
        drive({{"action", "selectRange"}, {"start", 5000000}, {"end", 7000000}});
        same({{"action", "cut"}}, [&] { emit strip->timeSelected(5000000, 7000000); QTest::keyClick(&window, Qt::Key_C); });
        QCOMPARE(proposed(spy)["op"].toString(), QString("addCut"));
        // Cuts: select (a click on it), adjust (a drag), restore (the panel's button).
        state = drive({{"action", "selectCut"}, {"cutId", "c1"}}).state;
        QCOMPARE(state["selectedEdit"].toObject()["kind"].toString(), QString("cut"));
        QCOMPARE(state["panel"].toObject()["mode"].toString(), QString("cut"));
        QCOMPARE(drive({{"action", "restoreCut"}, {"cutId", "nope"}}).code, QString("invalid_request"));
        same({{"action", "adjustCut"}, {"cutId", "c1"}, {"start", 1500000}, {"end", 2500000}}, [&] { emit strip->cutAdjusted("c1", 1500000, 2500000); });
        QCOMPARE(proposed(spy)["op"].toString(), QString("updateCut"));
        same({{"action", "restoreCut"}, {"cutId", "c1"}}, [&] { button("reviewPanelRestore")->click(); });
        QCOMPARE(proposed(spy)["op"].toString(), QString("removeCut"));
        // Undo and redo step the resident's history.
        same({{"action", "undo"}}, [&] { button("toolUndo")->click(); });
        same({{"action", "redo"}}, [&] { button("toolRedo")->click(); });
        // Covers: arm (X / B), draw (a drag), the panel's Redact | Blur.
        state = drive({{"action", "armBlur"}}).state;
        QCOMPARE(state["placement"].toObject()["kind"].toString(), QString("blur"));
        QVERIFY(state["warning"].toString().contains("reversible"));
        QCOMPARE(drive({{"action", "redo"}}).code, QString("recording_state"));
        QVERIFY(drive({{"action", "cancelPlacement"}}).state["placement"].isNull());
        QCOMPARE(drive({{"action", "cancelPlacement"}}).code, QString("recording_state"));
        QCOMPARE(drive({{"action", "armRedact"}}).state["placement"].toObject()["kind"].toString(), QString("solid"));
        same({{"action", "placeMask"}, {"rect", QJsonArray{10, 10, 100, 50}}}, [&] { drive({{"action", "armRedact"}}); emit window.findChild<FrameView*>("frame")->maskCreated("solid", QRect(10, 10, 100, 50)); });
        QCOMPARE(proposed(spy)["op"].toString(), QString("addMask"));
        state = drive({{"action", "selectEdit"}, {"kind", "cover"}, {"editId", "m1"}}).state;
        QCOMPARE(state["selectedEdit"].toObject()["id"].toString(), QString("m1"));
        QCOMPARE(state["panel"].toObject()["mode"].toString(), QString("cover"));
        same({{"action", "coverKind"}, {"kind", "solid"}}, [&] { button("reviewPanelCoverRedact")->click(); });
        QCOMPARE(proposed(spy)["mask"].toObject()["kind"].toString(), QString("solid"));
        // Stepping edits in source order: the cut at 1 s, then the cover at 4 s.
        drive({{"action", "clearSelection"}});
        QCOMPARE(drive({{"action", "nextEdit"}}).state["selectedEdit"].toObject()["kind"].toString(), QString("cut"));
        QCOMPARE(drive({{"action", "nextEdit"}}).state["selectedEdit"].toObject()["id"].toString(), QString("m1"));
        QCOMPARE(drive({{"action", "previousEdit"}}).state["selectedEdit"].toObject()["kind"].toString(), QString("cut"));
        QCOMPARE(drive({{"action", "selectEdit"}, {"kind", "trim"}}).code, QString("invalid_request"));
        QCOMPARE(window.inspect()["edits"].toArray().size(), 2);
        // The brackets.
        same({{"action", "setTrim"}, {"start", 1000000}, {"end", 9000000}}, [&] { emit strip->trimProposed(1000000, 9000000); });
        same({{"action", "setTrim"}, {"start", 1000000}}, [&] { emit strip->trimProposed(1000000, duration); });
        QVERIFY(proposed(spy)["end"].isNull());
    }
    // #1613: the cover edges and the view controls change the state their widgets do.
    void agentViewControlsMatchTheirWidgets() {
        ReviewWindow window(options(), config());
        QSignalSpy spy(&window, &ReviewWindow::editProposed);
        window.show();
        QVERIFY(putgd::test::waitForWindowExposed(&window));
        RecordingEdit edit;
        edit.revision = 2;
        edit.masks = {mask("m1", "solid", 4000000, 8000000)};
        window.setEdit(edit);
        const auto at = [&](const QJsonObject& params) { return window.drive(params).state["playhead"].toObject()["sourceUs"].toDouble(); };
        window.drive({{"action", "clearSelection"}});
        QCOMPARE(window.drive({{"action", "seekCoverStart"}}).code, QString("recording_state"));
        // Shows from / Until seek to the first and last covered frame; Set to playhead retimes.
        window.drive({{"action", "selectMask"}, {"maskId", "m1"}});
        QCOMPARE(at({{"action", "seekCoverEnd"}}), 7966667.0);
        QCOMPARE(at({{"action", "seekCoverStart"}}), 4000000.0);
        QCOMPARE(at({{"action", "frameStep"}, {"delta", 1}}), 4033333.0);
        QCOMPARE(window.drive({{"action", "frameStep"}, {"delta", 2}}).code, QString("invalid_request"));
        QVERIFY(window.drive({{"action", "setCoverEdgeToPlayhead"}, {"edge", "start"}}).code.isEmpty());
        const auto driven = spy.last().at(1).toJsonArray();
        QCOMPARE(driven.first().toObject()["mask"].toObject()["start"].toDouble(), 4033333.0);
        window.drive({{"action", "selectMask"}, {"maskId", "m1"}});
        window.drive({{"action", "frameStep"}, {"delta", 1}});
        window.findChild<QAbstractButton*>("reviewPanelShowsFromPlayhead")->click();
        QCOMPARE(spy.last().at(1).toJsonArray(), driven);
        // Timeline zoom and viewer zoom.
        QCOMPARE(window.drive({{"action", "timelineZoom"}, {"level", 4}}).state["timeline"].toObject()["zoom"].toInt(), 4);
        QCOMPARE(window.drive({{"action", "timelineFit"}}).state["timeline"].toObject()["zoom"].toInt(), 1);
        QCOMPARE(window.drive({{"action", "timelineZoom"}, {"level", 3}}).code, QString("invalid_request"));
        auto* view = window.findChild<FrameView*>("frame");
        QImage frame(1280, 720, QImage::Format_RGB32);
        frame.fill(Qt::gray);
        view->setFrame(frame, 0);
        QCOMPARE(window.drive({{"action", "viewerPan"}, {"dx", 5}, {"dy", 5}}).code, QString("recording_state"));
        QCOMPARE(window.drive({{"action", "viewerZoom"}, {"mode", "actual"}}).state["viewer"].toObject()["zoom"].toString(), QString("actual"));
        QVERIFY(view->actualSize());
        view->panBy({-100000, 0});
        const auto left = view->pan();
        QVERIFY(window.drive({{"action", "viewerPan"}, {"dx", 30}, {"dy", 0}}).code.isEmpty());
        QCOMPARE(view->pan().x(), left.x() + 30);
        QCOMPARE(window.inspect()["viewer"].toObject()["pan"].toObject()["x"].toDouble(), view->pan().x());
        QCOMPARE(window.drive({{"action", "viewerZoom"}, {"mode", "fit"}}).state["viewer"].toObject()["zoom"].toString(), QString("fit"));
        // Skim shows the flag at once and ends with the pointer leaving.
        auto state = window.drive({{"action", "skim"}, {"time", 3000000}}).state;
        QCOMPARE(state["timeline"].toObject()["skim"].toObject()["time"].toDouble(), 3000000.0);
        QVERIFY(!state["timeline"].toObject()["skim"].toObject()["exact"].toBool());
        QVERIFY(window.drive({{"action", "skimEnd"}}).state["timeline"].toObject()["skim"].isNull());
        QCOMPARE(window.drive({{"action", "skim"}, {"time", duration}}).code, QString("invalid_request"));
    }
    // #1613: the Edits panel is beside the viewer at 1440×900 and folds below ~1180×720;
    // `panel` is the Edits toggle and is refused while nothing is folded.
    void agentCanFoldAndOpenTheEditsPanel() {
        ReviewWindow window(options(), config());
        window.show();
        QVERIFY(putgd::test::waitForWindowExposed(&window));
        QVERIFY(window.drive({{"action", "resize"}, {"width", 1440}, {"height", 900}}).code.isEmpty());
        QVERIFY(!window.inspect()["folded"].toBool());
        QCOMPARE(window.drive({{"action", "panel"}, {"open", true}}).code, QString("recording_state"));
        QVERIFY(window.drive({{"action", "resize"}, {"width", 1000}, {"height", 680}}).state["folded"].toBool());
        const auto opened = window.drive({{"action", "panel"}, {"open", true}}).state;
        QVERIFY(opened["panel"].toObject()["overlayOpen"].toBool());
        QVERIFY(window.findChild<QToolButton*>("editsToggle")->isChecked());
        QVERIFY(!window.drive({{"action", "panel"}, {"open", false}}).state["panel"].toObject()["overlayOpen"].toBool());
        QVERIFY(!window.drive({{"action", "resize"}, {"width", 1440}, {"height", 900}}).state["folded"].toBool());
        QCOMPARE(window.drive({{"action", "resize"}, {"width", 10}, {"height", 900}}).code, QString("invalid_request"));
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
