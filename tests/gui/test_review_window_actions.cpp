#include "ReviewWindowFixture.h"
#include "fixtures/GopClip.h"
#include <QThread>
#include <QToolTip>

namespace {
// Ctrl+Enter, as its window shortcut fires it.
void ctrlEnter(ReviewWindow& window) {
    for (auto* shortcut : window.findChildren<QShortcut*>())
        if (shortcut->key() == QKeySequence(Qt::CTRL | Qt::Key_Return)) { emit shortcut->activated(); return; }
    QFAIL("no Ctrl+Enter shortcut");
}
QJsonObject answer(quint64 proposal, const QString& refusedCode = {}) {
    QJsonObject a{{"proposal", double(proposal)}};
    if (!refusedCode.isEmpty()) a["refused"] = QJsonObject{{"code", refusedCode}, {"message", "At most 64 masks are supported"}, {"revision", 2}};
    return a;
}
}

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
        window.setEdit(cut, answer(spy.last().at(2).toULongLong()));
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
    // U1: Ctrl+Enter uploads only once the resident accepted the change just
    // made. A refusal or no answer within 2 s cancels the upload with a status
    // line, so a refused cover or cut is never uploaded without the user knowing.
    void uploadWaitsForTheChangeToBeAccepted() {
        ReviewWindow window(options(), config());
        QSignalSpy proposals(&window, &ReviewWindow::editProposed), uploads(&window, &ReviewWindow::uploadRequested);
        window.show();
        QVERIFY(putgd::test::waitForWindowExposed(&window));
        RecordingEdit edit;
        edit.revision = 2;
        window.setEdit(edit);
        window.drive({{"action", "selectRange"}, {"start", 4000000}, {"end", 6000000}});
        QVERIFY(window.drive({{"action", "cut"}}).state["proposals"].toArray() == QJsonArray{1});
        QCOMPARE(proposals.last().at(2).toULongLong(), 1ull);
        ctrlEnter(window);
        QCOMPARE(uploads.size(), 0);
        QVERIFY(window.inspect()["uploadWaiting"].toBool());
        window.setEdit(edit, answer(1, "invalid_request"));
        QCOMPARE(uploads.size(), 0);
        QVERIFY(window.isVisible());
        auto state = window.inspect();
        QVERIFY2(state["status"].toString().contains("wasn't applied"), qPrintable(state["status"].toString()));
        QCOMPARE(state["lastRefusal"].toObject()["proposal"].toDouble(), 1.0);
        QCOMPARE(state["lastRefusal"].toObject()["code"].toString(), QString("invalid_request"));
        QVERIFY(!state["uploadWaiting"].toBool());
        PUTGD_COMPARE_STEADY(uploads.size(), 0, 2300);
        // No answer at all: after 2 s the upload is called off and says so.
        window.drive({{"action", "selectRange"}, {"start", 4000000}, {"end", 6000000}});
        window.drive({{"action", "cut"}});
        window.findChild<QPushButton*>("upload")->click();
        QCOMPARE(uploads.size(), 0);
        PUTGD_TRY_VERIFY_WITH_TIMEOUT(window.inspect()["status"].toString().contains("wasn't confirmed"), 4000);
        QCOMPARE(uploads.size(), 0);
        // Accepted: the upload goes ahead at the revision that holds the change.
        window.drive({{"action", "selectRange"}, {"start", 4000000}, {"end", 6000000}});
        window.drive({{"action", "cut"}});
        ctrlEnter(window);
        auto cut = edit;
        cut.revision = 3;
        cut.cuts = {RecordingCut{"c1", 4000000, 6000000}};
        window.setEdit(cut, answer(2));
        QCOMPARE(uploads.size(), 0);  // proposal 3 is still unanswered
        window.setEdit(cut, answer(3));
        QCOMPARE(uploads.size(), 1);
        QCOMPARE(uploads[0][0].toULongLong(), 3ull);
    }
    // U2: Ctrl+Enter during a pointer drag does nothing: the drag's edit is not
    // proposed until release.
    void ctrlEnterWaitsOutAPointerDrag() {
        ReviewWindow window(options(), config());
        QSignalSpy uploads(&window, &ReviewWindow::uploadRequested);
        window.show();
        QVERIFY(putgd::test::waitForWindowExposed(&window));
        RecordingEdit edit;
        edit.revision = 2;
        window.setEdit(edit);
        auto* strip = timeline(window);
        const QPoint handle = strip->trimHandleRect(true).center();
        QTest::mousePress(strip, Qt::LeftButton, {}, handle);
        QTest::mouseMove(strip, QPoint(strip->xFor(3000000), handle.y()));
        QVERIFY(strip->dragging());
        ctrlEnter(window);
        QCOMPARE(uploads.size(), 0);
        QVERIFY(!window.inspect()["uploadWaiting"].toBool());
        QTest::mouseRelease(strip, Qt::LeftButton, {}, QPoint(strip->xFor(3000000), handle.y()));
        QCOMPARE(uploads.size(), 0);
    }
    // U3: Try again waits while a cover is being placed, as Save and Upload do.
    void tryAgainWaitsWhilePlacing() {
        ReviewWindow window(options(), config());
        QSignalSpy retries(&window, &ReviewWindow::retryExportRequested);
        window.show();
        QVERIFY(putgd::test::waitForWindowExposed(&window));
        RecordingEdit edit;
        edit.revision = 2;
        window.setEdit(edit);
        window.showError("Export failed: synthetic");
        QVERIFY(window.drive({{"action", "armRedact"}}).code.isEmpty());
        QCOMPARE(window.drive({{"action", "retryExport"}}).code, QString("recording_state"));
        QCOMPARE(retries.size(), 0);
        for (auto* button : window.findChildren<QPushButton*>("statusAction"))
            if (button->property("driveAction").toJsonObject()["action"] == "retryExport") QVERIFY(!button->isEnabled());
        window.drive({{"action", "cancelPlacement"}});
        // U31: nor while a change is unanswered.
        window.drive({{"action", "resetTrim"}});
        QCOMPARE(window.drive({{"action", "retryExport"}}).code, QString("recording_state"));
        QCOMPARE(retries.size(), 0);
        window.setEdit(edit, answer(1));
        QVERIFY(window.drive({{"action", "retryExport"}}).code.isEmpty());
        QCOMPARE(retries.size(), 1);
    }
    // U7: Ctrl+Z during a nudge not yet proposed drops the nudge locally and
    // undoes at the current revision; the nudge is never sent.
    void undoDropsAPendingNudge() {
        ReviewWindow window(options(), config());
        QSignalSpy proposals(&window, &ReviewWindow::editProposed);
        window.show();
        QVERIFY(putgd::test::waitForWindowExposed(&window));
        RecordingEdit edit = twoMasks(6);
        edit.canUndo = true;
        window.setEdit(edit);
        window.drive({{"action", "selectMask"}, {"maskId", "m-solid"}});
        auto* view = window.findChild<FrameView*>("frame");
        QTest::keyClick(view, Qt::Key_Right);
        QVERIFY(view->nudging());
        for (auto* shortcut : window.findChildren<QShortcut*>()) if (shortcut->key() == QKeySequence(Qt::CTRL | Qt::Key_Z)) emit shortcut->activated();
        QVERIFY(!view->nudging());
        QCOMPARE(proposals.size(), 1);
        QCOMPARE(proposals[0].at(0).toULongLong(), 6ull);
        QCOMPARE(proposals[0].at(1).toJsonArray(), (QJsonArray{QJsonObject{{"op", "undo"}}}));
    }
    // U8: Shows from finds a one-frame cover whose start is a rounded frame time
    // (30 fps: frame 2 is 66,667 µs).
    void showsFromFindsAOneFrameCoverAtARoundedTime() {
        ReviewWindow window(options(), config());
        window.show();
        QVERIFY(putgd::test::waitForWindowExposed(&window));
        RecordingEdit edit;
        edit.revision = 2;
        edit.masks = {mask("m1", "solid", 66667, 100000)};
        window.setEdit(edit);
        window.drive({{"action", "selectMask"}, {"maskId", "m1"}});
        const auto result = window.drive({{"action", "seekCoverStart"}});
        QVERIFY2(result.code.isEmpty(), qPrintable(result.message));
        QCOMPARE(result.state["playheadUs"].toDouble(), 66667.0);
        QCOMPARE(window.drive({{"action", "seekCoverEnd"}}).state["playheadUs"].toDouble(), 66667.0);
    }
    // U11: setCoverEdge types a time into Shows from / Until: frame-snapped,
    // proposed as the field's commit proposes it.
    void setCoverEdgeIsTheTimeFieldsCommit() {
        ReviewWindow window(options(), config());
        QSignalSpy proposals(&window, &ReviewWindow::editProposed);
        window.show();
        QVERIFY(putgd::test::waitForWindowExposed(&window));
        RecordingEdit edit;
        edit.revision = 2;
        edit.masks = {mask("m1", "solid", 4000000, 8000000)};
        window.setEdit(edit);
        window.drive({{"action", "clearSelection"}});
        QCOMPARE(window.drive({{"action", "setCoverEdge"}, {"edge", "start"}, {"time", 5010000}}).code, QString("recording_state"));
        window.drive({{"action", "selectMask"}, {"maskId", "m1"}});
        QCOMPARE(window.drive({{"action", "setCoverEdge"}, {"edge", "middle"}, {"time", 5010000}}).code, QString("invalid_request"));
        QVERIFY(window.drive({{"action", "setCoverEdge"}, {"edge", "start"}, {"time", 5010000}}).code.isEmpty());
        QCOMPARE(proposals.size(), 1);
        QCOMPARE(proposed(proposals)["op"].toString(), QString("updateMask"));
        QCOMPARE(proposed(proposals)["mask"].toObject()["start"].toDouble(), 5000000.0);
        auto* field = window.findChild<QLineEdit*>("reviewPanelUntilTime");
        QVERIFY(field);
        field->setText("0:07.0");
        auto count = proposals.size();
        emit field->editingFinished();
        QCOMPARE(proposals.size(), count + 1);
        const auto typed = proposals.last().at(1).toJsonArray();
        QVERIFY(window.drive({{"action", "setCoverEdge"}, {"edge", "end"}, {"time", 7000000}}).code.isEmpty());
        QCOMPARE(proposals.size(), count + 2);
        QCOMPARE(proposals.last().at(1).toJsonArray(), typed);
    }
    // U13: Enter by focus and state: placing, it places the centred cover; a
    // selected cut, Restore; a focused time field keeps it; otherwise Upload.
    void enterFollowsFocusAndState() {
        ReviewWindow window(options(), config());
        QSignalSpy proposals(&window, &ReviewWindow::editProposed), uploads(&window, &ReviewWindow::uploadRequested);
        window.show();
        QVERIFY(putgd::test::waitForWindowExposed(&window));
        RecordingEdit edit;
        edit.revision = 2;
        edit.cuts = {RecordingCut{"c1", 1000000, 2000000}};
        edit.masks = {mask("m1", "solid", 4000000, 8000000)};
        window.setEdit(edit);
        window.drive({{"action", "armBlur"}});
        QTest::keyClick(&window, Qt::Key_Return);
        QCOMPARE(proposals.size(), 1);
        QCOMPARE(proposed(proposals)["op"].toString(), QString("addMask"));
        QCOMPARE(proposed(proposals)["mask"].toObject()["rect"].toArray(), (QJsonArray{320, 180, 640, 360}));
        window.drive({{"action", "selectCut"}, {"cutId", "c1"}});
        timeline(window)->setFocus();
        QTest::keyClick(&window, Qt::Key_Enter);
        QCOMPARE(proposals.size(), 2);
        QCOMPARE(proposed(proposals)["op"].toString(), QString("removeCut"));
        window.drive({{"action", "selectMask"}, {"maskId", "m1"}});
        auto* field = window.findChild<QLineEdit*>("reviewPanelShowsFromTime");
        QVERIFY(field);
        window.activateWindow();
        field->setFocus();
        const auto before = proposals.size();
        QTest::keyClick(field, Qt::Key_Return);
        QCOMPARE(proposals.size(), before);
        QCOMPARE(uploads.size(), 0);
        QVERIFY(window.isVisible());
        // Nothing selected, every proposal answered: Enter uploads.
        window.setEdit(edit, answer(proposals.last().at(2).toULongLong()));
        window.drive({{"action", "clearSelection"}});
        timeline(window)->setFocus();
        QTest::keyClick(&window, Qt::Key_Return);
        QCOMPARE(uploads.size(), 1);
    }
    // U9, U10 on a real clip: an exact skim frame stays exact only for the
    // revision it was decoded at, and 100 % decodes source pixels.
    void skimAndZoomFollowTheEditAndTheSource() {
        QTemporaryDir dir;
        const QString clip = dir.filePath("gops.mkv");
        QVERIFY(test::gopclip::write(clip, 3, 1600, 900));
        auto o = options();
        o.workingClip = clip;
        o.durationUs = 3000000;
        o.canvas = QSize(1600, 900);
        ReviewWindow window(o, config());
        window.show();
        QVERIFY(putgd::test::waitForWindowExposed(&window));
        RecordingEdit edit;
        edit.revision = 2;
        window.setEdit(edit);
        const auto frameSize = [&] { return window.inspect()["preview"].toObject()["frameSize"].toArray(); };
        PUTGD_TRY_COMPARE(frameSize(), (QJsonArray{1280, 720}));
        window.drive({{"action", "viewerZoom"}, {"mode", "actual"}});
        PUTGD_TRY_COMPARE(frameSize(), (QJsonArray{1600, 900}));
        window.drive({{"action", "frameStep"}, {"delta", 1}});
        PUTGD_TRY_COMPARE(window.inspect()["preview"].toObject()["frameTimeUs"].toDouble(), window.inspect()["playheadUs"].toDouble());
        QCOMPARE(frameSize(), (QJsonArray{1600, 900}));
        window.drive({{"action", "viewerZoom"}, {"mode", "fit"}});
        const auto exact = [&] { return window.inspect()["timeline"].toObject()["skim"].toObject()["exact"].toBool(); };
        window.drive({{"action", "skim"}, {"time", 1500000}});
        PUTGD_TRY_VERIFY(exact());
        auto covered = edit;
        covered.revision = 3;
        covered.masks = {mask("m1", "solid", 0, 3000000)};
        window.setEdit(covered);
        QVERIFY(!exact());
        PUTGD_TRY_VERIFY(exact());
        // U21: 100 % drops the Fit skim frame at once and asks at source size.
        window.drive({{"action", "viewerZoom"}, {"mode", "actual"}});
        QVERIFY(!exact());
        PUTGD_TRY_VERIFY(exact());
        QCOMPARE(window.inspect()["preview"].toObject()["skimSize"].toArray(), (QJsonArray{1600, 900}));
        window.drive({{"action", "viewerZoom"}, {"mode", "fit"}});
        PUTGD_TRY_VERIFY(exact());
        // U20: a seek ends the skim, so the same time skimmed again is asked again.
        window.drive({{"action", "seek"}, {"time", 500000}});
        QVERIFY(!exact());
        QVERIFY(window.inspect()["timeline"].toObject()["skim"].isNull());
        window.drive({{"action", "skim"}, {"time", 1500000}});
        PUTGD_TRY_VERIFY(exact());
    }
    // U21, U22 on a real clip: playback at 100 % decodes source pixels, and an
    // edit that leaves nothing kept ahead of the playhead pauses it there
    // rather than starting over.
    void playbackFollowsZoomAndPausesWithNothingAhead() {
        QTemporaryDir dir;
        const QString clip = dir.filePath("gops.mkv");
        QVERIFY(test::gopclip::write(clip, 3, 1600, 900));
        auto o = options();
        o.workingClip = clip;
        o.durationUs = 3000000;
        o.canvas = QSize(1600, 900);
        ReviewWindow window(o, config());
        window.show();
        QVERIFY(putgd::test::waitForWindowExposed(&window));
        RecordingEdit edit;
        edit.revision = 2;
        window.setEdit(edit);
        window.drive({{"action", "viewerZoom"}, {"mode", "actual"}});
        QVERIFY(window.drive({{"action", "play"}}).code.isEmpty());
        const auto preview = [&] { return window.inspect()["preview"].toObject(); };
        PUTGD_TRY_VERIFY(preview()["frameTimeUs"].toDouble() >= 300000);
        QVERIFY(window.inspect()["playing"].toBool());
        QCOMPARE(preview()["frameSize"].toArray(), (QJsonArray{1600, 900}));
        PUTGD_TRY_VERIFY(window.inspect()["playheadUs"].toDouble() >= 1000000);
        auto trimmed = edit;
        trimmed.revision = 3;
        trimmed.trimEnd = 500000;
        window.setEdit(trimmed);
        auto state = window.inspect();
        QVERIFY(!state["playing"].toBool());
        QVERIFY2(state["playheadUs"].toDouble() >= 1000000, qPrintable(QString::number(state["playheadUs"].toDouble())));
    }
    // The cancel rule (#1635): a waiting Upload is cancelled only when what it
    // would export changes (the revision moves for anything but the awaited
    // answer, or the output choice changes), or by a refusal or the timeout.
    // Each row starts an Upload waiting on proposal 1 and fires one thing. A
    // cancelling row then finds no upload or export requested and the cancel on
    // the status line; any other row finds the upload sent once proposal 1's
    // answer arrives.
    void aWaitingUploadIsCancelledOnlyByAChange_data() {
        QTest::addColumn<QString>("trigger");
        QTest::addColumn<bool>("cancels");
        for (const char* row : {"accepted edit", "undo", "redo", "agent edit", "format change", "audio change", "agent config change", "refusal", "timeout"})
            QTest::newRow(row) << QString::fromLatin1(row) << true;
        for (const char* row : {"hover", "save a copy dialog", "discard dialog", "menu", "inspect", "dismissStatus", "raise", "saved copy", "unchanged push", "key press", "review action", "second upload"})
            QTest::newRow(row) << QString::fromLatin1(row) << false;
    }
    void aWaitingUploadIsCancelledOnlyByAChange() {
        QFETCH(QString, trigger);
        QFETCH(bool, cancels);
        qputenv("PUTGD_FILE_DIALOG", "fallback");
        const auto restore = qScopeGuard([] { qunsetenv("PUTGD_FILE_DIALOG"); });
        ReviewWindow window(options(), config());
        QSignalSpy uploads(&window, &ReviewWindow::uploadRequested), saves(&window, &ReviewWindow::saveCopyRequested), retries(&window, &ReviewWindow::retryExportRequested), proposals(&window, &ReviewWindow::editProposed);
        window.show();
        QVERIFY(putgd::test::waitForWindowExposed(&window));
        RecordingEdit edit;
        edit.revision = 2;
        edit.canUndo = edit.canRedo = true;
        window.setEdit(edit);
        window.drive({{"action", "resetTrim"}});
        ctrlEnter(window);
        QVERIFY(window.inspect()["uploadWaiting"].toBool());
        const auto status = [&] { return window.inspect()["status"].toString(); };
        // The thing that happens.
        if (trigger == "accepted edit") { window.drive({{"action", "selectRange"}, {"start", 4000000}, {"end", 6000000}}); QTest::keyClick(&window, Qt::Key_C); }
        else if (trigger == "undo") window.findChild<QToolButton*>("toolUndo")->click();
        else if (trigger == "redo") window.findChild<QToolButton*>("toolRedo")->click();
        else if (trigger == "agent edit") { auto other = edit; other.revision = 3; window.setEdit(other); }
        else if (trigger == "format change") { for (auto* action : window.findChild<QMenu*>("formatMenu")->actions()) if (action->data().toStringList().value(0) == "webm") action->trigger(); }
        else if (trigger == "audio change") window.findChild<QAction*>("includeAudio")->toggle();
        else if (trigger == "agent config change") { auto webm = config(); webm.format = "webm"; webm.codec = "vp9"; QVERIFY(window.setConfig(webm)); }
        else if (trigger == "refusal") window.setEdit(edit, answer(1, "invalid_request"));
        else if (trigger == "timeout") QTest::qWait(2300);
        else if (trigger == "hover") QToolTip::showText(window.findChild<QPushButton*>("upload")->mapToGlobal(QPoint(4, 4)), "Upload", window.findChild<QPushButton*>("upload"));
        else if (trigger == "save a copy dialog") { window.findChild<QPushButton*>("saveCopy")->click(); QVERIFY(window.inspect()["fileDialog"].toObject()["open"].toBool()); }
        else if (trigger == "discard dialog") { window.drive({{"action", "discard"}}); QVERIFY(window.inspect()["discardConfirmation"].toObject()["open"].toBool()); }
        else if (trigger == "menu") window.findChild<QToolButton*>("uploadDestinationMenu")->menu()->popup(window.mapToGlobal(QPoint(20, 20)));
        else if (trigger == "inspect") QVERIFY(window.drive({{"action", "inspect"}}).code.isEmpty());
        else if (trigger == "dismissStatus") QVERIFY(window.drive({{"action", "dismissStatus"}}).code.isEmpty());
        else if (trigger == "raise") window.reopen({});
        else if (trigger == "saved copy") window.setSavedCopy("/tmp/putgd-review-copy.mp4", 2, config());
        else if (trigger == "unchanged push") window.setEdit(edit);
        else if (trigger == "key press") QTest::keyClick(&window, Qt::Key_Period);
        else if (trigger == "review action") QVERIFY(window.drive({{"action", "frameStep"}, {"delta", 1}}).code.isEmpty());
        else if (trigger == "second upload") { ctrlEnter(window); QVERIFY2(status().contains("already waiting"), qPrintable(status())); }
        else QFAIL("unknown trigger");
        // Proposal 1's answer, then the answer to a proposal the trigger made.
        auto accepted = edit;
        accepted.revision = 3;
        window.setEdit(accepted, answer(1));
        if (proposals.size() > 1) { auto later = accepted; later.revision = 4; window.setEdit(later, answer(proposals.last().at(2).toULongLong())); }
        if (cancels) {
            QCOMPARE(uploads.size(), 0);
            QCOMPARE(saves.size(), 0);
            QCOMPARE(retries.size(), 0);
            QVERIFY2(status().contains("Upload cancelled"), qPrintable(status()));
            if (trigger == "timeout") QVERIFY2(status().contains("wasn't confirmed in time"), qPrintable(status()));
            if (trigger == "refusal") QVERIFY2(status().contains("wasn't applied"), qPrintable(status()));
        } else {
            QCOMPARE(uploads.size(), 1);
            QCOMPARE(uploads[0][0].toULongLong(), 3ull);
            QCOMPARE(saves.size() + retries.size(), 0);
        }
        if (auto* menu = window.findChild<QToolButton*>("uploadDestinationMenu")->menu()) menu->close();
        QToolTip::hideText();
    }
    // U46: dismissing the status leaves the cancel notice; it stays until Upload is pressed again.
    void theCancelNoticeOutlastsDismiss() {
        ReviewWindow window(options(), config());
        window.show();
        QVERIFY(putgd::test::waitForWindowExposed(&window));
        RecordingEdit edit;
        edit.revision = 2;
        window.setEdit(edit);
        window.drive({{"action", "resetTrim"}});
        ctrlEnter(window);
        window.setEdit(edit, answer(1, "invalid_request"));
        QVERIFY(window.drive({{"action", "dismissStatus"}}).code.isEmpty());
        QVERIFY(window.inspect()["status"].toString().contains("Upload cancelled"));
        QVERIFY(!window.inspect()["status"].toString().contains("That change wasn't applied"));
    }
    // U44 (privacy): "Set to playhead", Set Out and the cover's end edge keep the
    // frame the viewer shows. At an off-grid playhead that is the frame after it
    // (30 fps, 40 ms shows frame 2 at 66,667 µs), never the nominal one.
    void endEdgesKeepTheFrameShownAtAnOffGridPlayhead_data() {
        QTest::addColumn<int>("fps");
        QTest::addColumn<qint64>("playhead");
        QTest::addColumn<qint64>("end");
        QTest::newRow("30 fps at 40 ms") << 30 << qint64(40000) << qint64(100000);
        QTest::newRow("30 fps at 1.01 s") << 30 << qint64(1010000) << qint64(1066667);
        QTest::newRow("60 fps at 20 ms") << 60 << qint64(20000) << qint64(50000);
        QTest::newRow("60 fps at 1.01 s") << 60 << qint64(1010000) << qint64(1033333);
    }
    void endEdgesKeepTheFrameShownAtAnOffGridPlayhead() {
        QFETCH(int, fps);
        QFETCH(qint64, playhead);
        QFETCH(qint64, end);
        auto c = config();
        c.fps = fps;
        ReviewWindow window(options(), c);
        QSignalSpy proposals(&window, &ReviewWindow::editProposed);
        window.show();
        QVERIFY(putgd::test::waitForWindowExposed(&window));
        RecordingEdit edit;
        edit.revision = 2;
        edit.masks = {mask("m1", "solid", 0, 12000000)};
        window.setEdit(edit);
        QVERIFY(window.drive({{"action", "seek"}, {"time", double(playhead)}}).code.isEmpty());
        QVERIFY(window.drive({{"action", "setOut"}}).code.isEmpty());
        QCOMPARE(proposed(proposals)["end"].toDouble(), double(end));
        window.drive({{"action", "selectMask"}, {"maskId", "m1"}});
        window.drive({{"action", "seek"}, {"time", double(playhead)}});
        QVERIFY(window.drive({{"action", "setCoverEdgeToPlayhead"}, {"edge", "end"}}).code.isEmpty());
        QCOMPARE(proposed(proposals)["mask"].toObject()["end"].toDouble(), double(end));
    }
    // U44 on a real clip: the decoded frame's own time decides, so the cover set
    // to the playhead covers the frame on screen.
    void aCoverSetToThePlayheadCoversTheFrameOnScreen() {
        QTemporaryDir dir;
        const QString clip = dir.filePath("gops.mkv");
        QVERIFY(test::gopclip::write(clip, 1, 320, 180));
        auto o = options();
        o.workingClip = clip;
        o.durationUs = 1000000;
        o.canvas = QSize(320, 180);
        ReviewWindow window(o, config());
        QSignalSpy proposals(&window, &ReviewWindow::editProposed);
        window.show();
        QVERIFY(putgd::test::waitForWindowExposed(&window));
        RecordingEdit edit;
        edit.revision = 2;
        edit.masks = {mask("m1", "solid", 0, 1000000)};
        edit.masks[0].rect = QRect(10, 10, 100, 60);
        window.setEdit(edit);
        window.drive({{"action", "selectMask"}, {"maskId", "m1"}});
        window.drive({{"action", "seek"}, {"time", 40000}});
        PUTGD_TRY_VERIFY(!window.inspect()["preview"].toObject()["shownUs"].isNull());
        const double shown = window.inspect()["preview"].toObject()["shownUs"].toDouble();
        QVERIFY2(shown >= 40000, qPrintable(QString::number(shown)));
        QVERIFY(window.drive({{"action", "setCoverEdgeToPlayhead"}, {"edge", "end"}}).code.isEmpty());
        const double end = proposed(proposals)["mask"].toObject()["end"].toDouble();
        QVERIFY2(end > shown, qPrintable(QStringLiteral("cover ends at %1, the frame shown is at %2").arg(end).arg(shown)));
    }
    // U25 with the rule: Upload sends a pending nudge first and waits for its answer too.
    void uploadSendsAPendingNudgeFirst() {
        ReviewWindow window(options(), config());
        QSignalSpy proposals(&window, &ReviewWindow::editProposed), uploads(&window, &ReviewWindow::uploadRequested);
        window.show();
        QVERIFY(putgd::test::waitForWindowExposed(&window));
        RecordingEdit edit;
        edit.revision = 2;
        edit.masks = {mask("m1", "solid", 0, 12000000)};
        window.setEdit(edit);
        window.drive({{"action", "selectMask"}, {"maskId", "m1"}});
        QTest::keyClick(window.findChild<FrameView*>("frame"), Qt::Key_Right);
        ctrlEnter(window);
        QCOMPARE(proposals.size(), 1);
        QCOMPARE(proposed(proposals)["op"].toString(), QString("updateMask"));
        QCOMPARE(uploads.size(), 0);
        window.setEdit(edit, answer(1));
        QCOMPARE(uploads.size(), 1);
    }
    // U32/U35: the window's output choice is never replaced by a config the
    // resident sent before it saw the choice; the upload carries what the window
    // shows; a stale or busy refusal comes back without Try again.
    void theWindowsOutputChoiceWins() {
        ReviewWindow window(options(), config());
        QSignalSpy changed(&window, &ReviewWindow::configChanged), uploads(&window, &ReviewWindow::uploadRequested);
        window.show();
        QVERIFY(putgd::test::waitForWindowExposed(&window));
        RecordingEdit edit;
        edit.revision = 2;
        window.setEdit(edit);
        for (auto* action : window.findChild<QMenu*>("formatMenu")->actions()) if (action->data().toStringList().value(0) == "webm") action->trigger();
        QCOMPARE(changed.size(), 1);
        QCOMPARE(changed[0][1].toULongLong(), 1ull);
        QVERIFY(!window.setConfig(config(), 0));  // an older mp4 push
        QCOMPARE(window.inspect()["format"].toObject()["format"].toString(), QString("webm"));
        auto clamped = config();
        clamped.format = "webm";
        clamped.codec = "vp9";
        clamped.maxEdge = 960;
        QVERIFY(window.setConfig(clamped, 1));  // the resident's answer to that choice
        ctrlEnter(window);
        QCOMPARE(uploads.size(), 1);
        const auto sent = uploads[0][1].value<RecordingConfig>();
        QCOMPARE(sent.format, QString("webm"));
        QCOMPARE(sent.maxEdge.value_or(0), 960);
        window.reopen("The edits changed after Upload was pressed, so nothing was uploaded.", false);
        for (const auto& line : window.inspect()["statusRow"].toArray())
            for (const auto& action : line.toObject()["actions"].toArray()) QVERIFY(action.toObject()["drive"].toObject()["action"] != "retryExport");
    }
    // E24: at 30 and 60 fps, Set Out at frame k keeps frame k, and a cover set to
    // the playhead ends exactly at edge k+1 = round((k+1)·10⁶/fps).
    void endEdgesComeFromTheFrameIndex_data() {
        QTest::addColumn<int>("fps");
        QTest::addColumn<int>("frame");
        QTest::newRow("30 fps frame 1") << 30 << 1;
        QTest::newRow("30 fps frame 2") << 30 << 2;
        QTest::newRow("30 fps frame 100") << 30 << 100;
        QTest::newRow("60 fps frame 1") << 60 << 1;
        QTest::newRow("60 fps frame 2") << 60 << 2;
        QTest::newRow("60 fps frame 301") << 60 << 301;
    }
    void endEdgesComeFromTheFrameIndex() {
        QFETCH(int, fps);
        QFETCH(int, frame);
        auto c = config();
        c.fps = fps;
        ReviewWindow window(options(), c);
        QSignalSpy proposals(&window, &ReviewWindow::editProposed);
        window.show();
        QVERIFY(putgd::test::waitForWindowExposed(&window));
        RecordingEdit edit;
        edit.revision = 2;
        edit.masks = {mask("m1", "solid", 0, 12000000)};
        window.setEdit(edit);
        const auto edgeAt = [&](qint64 k) { return double(std::llround(double(k) * 1e6 / fps)); };
        QVERIFY(window.drive({{"action", "seek"}, {"time", edgeAt(frame)}}).code.isEmpty());
        QVERIFY(window.drive({{"action", "setOut"}}).code.isEmpty());
        QCOMPARE(proposed(proposals)["end"].toDouble(), edgeAt(frame + 1));
        window.drive({{"action", "selectMask"}, {"maskId", "m1"}});
        window.drive({{"action", "seek"}, {"time", edgeAt(frame)}});
        QVERIFY(window.drive({{"action", "setCoverEdgeToPlayhead"}, {"edge", "end"}}).code.isEmpty());
        QCOMPARE(proposed(proposals)["mask"].toObject()["end"].toDouble(), edgeAt(frame + 1));
    }
    // U18: a held Enter (auto-repeat) never reaches Upload; Ctrl+Enter does not repeat.
    void aHeldEnterNeverUploads() {
        ReviewWindow window(options(), config());
        QSignalSpy proposals(&window, &ReviewWindow::editProposed), uploads(&window, &ReviewWindow::uploadRequested);
        window.show();
        QVERIFY(putgd::test::waitForWindowExposed(&window));
        RecordingEdit edit;
        edit.revision = 2;
        edit.cuts = {RecordingCut{"c1", 1000000, 2000000}};
        window.setEdit(edit);
        int found = 0;
        for (auto* shortcut : window.findChildren<QShortcut*>())
            if (shortcut->key() == QKeySequence(Qt::CTRL | Qt::Key_Return) || shortcut->key() == QKeySequence(Qt::CTRL | Qt::Key_Enter)) { ++found; QVERIFY(!shortcut->autoRepeat()); }
        QCOMPARE(found, 2);
        timeline(window)->setFocus();
        const auto repeat = [&] { QKeyEvent held(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier, QString(), true); QApplication::sendEvent(&window, &held); };
        window.drive({{"action", "selectCut"}, {"cutId", "c1"}});
        repeat();
        QCOMPARE(proposals.size(), 0);
        window.drive({{"action", "clearSelection"}});
        repeat();
        QCOMPARE(uploads.size(), 0);
        QVERIFY(window.isVisible());
    }
    // U23: only the cut's own accepted answer moves the playhead to its seam;
    // an earlier refusal or an unrelated push leaves it alone.
    void onlyTheCutsAnswerMovesThePlayhead() {
        ReviewWindow window(options(), config());
        window.show();
        QVERIFY(putgd::test::waitForWindowExposed(&window));
        RecordingEdit edit;
        edit.revision = 2;
        window.setEdit(edit);
        window.drive({{"action", "resetTrim"}});
        window.drive({{"action", "selectRange"}, {"start", 4000000}, {"end", 6000000}});
        QVERIFY(window.drive({{"action", "cut"}}).state["proposals"].toArray() == QJsonArray{2});
        window.setEdit(edit, answer(1, "invalid_request"));
        auto unrelated = edit;
        unrelated.revision = 3;
        unrelated.masks = {mask("m1", "solid", 0, 1000000)};
        window.setEdit(unrelated);
        QCOMPARE(window.inspect()["playheadUs"].toDouble(), 0.0);
        auto cut = unrelated;
        cut.revision = 4;
        cut.cuts = {RecordingCut{"c1", 4000000, 6000000}};
        window.setEdit(cut, answer(2));
        QCOMPARE(window.inspect()["playheadUs"].toDouble(), 3000000.0);
    }
    // U27: the resident's retry asks only for the window's guard; the window
    // then doesn't ask for a second retry itself.
    void theResidentsRetryOnlyAsksTheGuard() {
        ReviewWindow window(options(), config());
        QSignalSpy retries(&window, &ReviewWindow::retryExportRequested);
        window.show();
        QVERIFY(putgd::test::waitForWindowExposed(&window));
        window.showError("Export failed: synthetic");
        QVERIFY(window.drive({{"action", "retryExport"}, {"residentRetries", true}}).code.isEmpty());
        QCOMPARE(retries.size(), 0);
        window.drive({{"action", "armBlur"}});
        QCOMPARE(window.drive({{"action", "retryExport"}, {"residentRetries", true}}).code, QString("recording_state"));
    }
    // U29: a paused playback's queued frames and end are dropped: its end never
    // seeks after the user paused.
    void aPausedPlaybacksEndNeverSeeks() {
        QTemporaryDir dir;
        const QString clip = dir.filePath("gops.mkv");
        QVERIFY(test::gopclip::write(clip, 1, 320, 180));
        auto o = options();
        o.workingClip = clip;
        o.durationUs = 1000000;
        o.canvas = QSize(320, 180);
        ReviewWindow window(o, config());
        window.show();
        QVERIFY(putgd::test::waitForWindowExposed(&window));
        RecordingEdit edit;
        edit.revision = 2;
        edit.trimEnd = 300000;
        window.setEdit(edit);
        QCOMPARE(window.inspect()["playheadUs"].toDouble(), 0.0);
        QVERIFY(window.drive({{"action", "play"}}).code.isEmpty());
        QThread::msleep(1200);  // the run plays out and queues its frames and its end
        QVERIFY(window.inspect()["playbackQueuedEnd"].toBool());
        QVERIFY(window.drive({{"action", "pause"}}).code.isEmpty());
        QTest::qWait(200);
        QCOMPARE(window.inspect()["playheadUs"].toDouble(), 0.0);
        QVERIFY(!window.inspect()["playing"].toBool());
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
