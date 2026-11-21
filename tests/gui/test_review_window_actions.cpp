#include "ReviewWindowFixture.h"
#include "fixtures/GopClip.h"
#include "record/ClipExporter.h"
#include "record/ReviewPanel.h"
#include "ui/Ui.h"
#include <atomic>
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
        window.drive({{"action", "seekCoverStart"}});
        window.drive({{"action", "frameStep"}, {"delta", 1}});
        window.findChild<QAbstractButton*>("reviewPanelShowsFromPlayhead")->click();
        QCOMPARE(spy.last().at(1).toJsonArray(), driven);
        // Timeline zoom and viewer zoom.
        // The ladder follows the recording (12 s at 30 fps here); any level of it, nothing else.
        const auto levels = window.inspect()["timeline"].toObject()["zoomLevels"].toArray();
        QVERIFY(levels.size() >= 2);
        const int top = levels.last().toInt();
        QCOMPARE(window.drive({{"action", "timelineZoom"}, {"level", top}}).state["timeline"].toObject()["zoom"].toInt(), top);
        QCOMPARE(window.drive({{"action", "zoomOut"}}).state["timeline"].toObject()["zoom"].toInt(), levels[levels.size() - 2].toInt());
        QCOMPARE(window.drive({{"action", "timelineFit"}}).state["timeline"].toObject()["zoom"].toInt(), 1);
        QCOMPARE(window.drive({{"action", "zoomIn"}}).state["timeline"].toObject()["zoom"].toInt(), levels[1].toInt());
        QCOMPARE(window.drive({{"action", "timelineZoom"}, {"level", top + 1}}).code, QString("invalid_request"));
        window.drive({{"action", "timelineFit"}});
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
        // Skimming off (S): hover never changes the viewer, and skim says why it does nothing.
        QVERIFY(!window.drive({{"action", "setSkimming"}, {"on", false}}).state["timeline"].toObject()["skimming"].toBool());
        QCOMPARE(window.drive({{"action", "skim"}, {"time", 3000000}}).code, QString("recording_state"));
        QVERIFY(window.drive({{"action", "setSkimming"}, {"on", true}}).state["timeline"].toObject()["skimming"].toBool());
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
    // U7, D6: the first Ctrl+Z during a nudge not yet proposed takes back only
    // the nudge (never sent); the next one undoes at the current revision.
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
        window.activateWindow();
        view->setFocus(Qt::TabFocusReason);  // W8: Tab gives the viewer the arrows
        QTest::keyClick(view, Qt::Key_Right);
        QVERIFY(view->nudging());
        const auto undo = [&] { for (auto* shortcut : window.findChildren<QShortcut*>()) if (shortcut->key() == QKeySequence(Qt::CTRL | Qt::Key_Z)) emit shortcut->activated(); };
        undo();
        QVERIFY(!view->nudging());
        QCOMPARE(proposals.size(), 0);
        QCOMPARE(window.inspect()["masks"].toArray().size(), 2);
        undo();
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
    // U13, E3, R19: Enter by focus and state: placing, it places the centred cover; a
    // selected cut is restored only by its Restore button (a row only selects); a focused
    // time field keeps it; it uploads only when Upload has focus.
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
        // P12: Enter waits (at most 1 s) for the placement frame; this clip never decodes.
        PUTGD_TRY_COMPARE(proposals.size(), 1);
        QCOMPARE(proposed(proposals)["op"].toString(), QString("addMask"));
        QCOMPARE(proposed(proposals)["mask"].toObject()["rect"].toArray(), (QJsonArray{320, 180, 640, 360}));
        window.drive({{"action", "selectCut"}, {"cutId", "c1"}});
        timeline(window)->setFocus();
        QTest::keyClick(&window, Qt::Key_Enter);
        QCOMPARE(proposals.size(), 1);
        window.activateWindow();
        auto* restore = window.findChild<QAbstractButton*>("reviewPanelRestore");
        QVERIFY(restore);
        restore->setFocus();
        QTest::keyClick(restore, Qt::Key_Enter);
        QCOMPARE(proposals.size(), 2);
        QCOMPARE(proposed(proposals)["op"].toString(), QString("removeCut"));
        auto* cutRow = window.findChild<ReviewPanel*>()->rowFor("cut", "c1");
        QVERIFY(cutRow);
        cutRow->setFocus();
        // R19: Enter on a row only selects it.
        QTest::keyClick(cutRow, Qt::Key_Return);
        QCOMPARE(proposals.size(), 2);
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
        // Nothing selected, every proposal answered: Enter uploads only from Upload itself.
        window.setEdit(edit, answer(proposals.last().at(2).toULongLong()));
        window.drive({{"action", "clearSelection"}});
        timeline(window)->setFocus();
        QTest::keyClick(&window, Qt::Key_Return);
        QCOMPARE(uploads.size(), 0);
        auto* upload = window.findChild<QPushButton*>("upload");
        upload->setFocus();
        QTest::keyClick(upload, Qt::Key_Return);
        QCOMPARE(uploads.size(), 1);
    }
    // E3 (safety): a plain Enter with the timeline, the viewer or the panel
    // focused never uploads, whatever is selected.
    void plainEnterNeverUploads() {
        ReviewWindow window(options(), config());
        QSignalSpy uploads(&window, &ReviewWindow::uploadRequested);
        window.show();
        QVERIFY(putgd::test::waitForWindowExposed(&window));
        window.activateWindow();
        RecordingEdit edit = twoMasks(2);
        edit.cuts = {RecordingCut{"c1", 11000000, 11500000}};
        window.setEdit(edit);
        auto* panel = window.findChild<ReviewPanel*>();
        const QList<QJsonObject> selections{{{"action", "clearSelection"}}, {{"action", "selectMask"}, {"maskId", "m-blur"}}, {{"action", "selectRange"}, {"start", 1000000}, {"end", 2000000}}};
        for (const auto& select : selections) {
            window.drive(select);
            for (QWidget* focus : {static_cast<QWidget*>(timeline(window)), static_cast<QWidget*>(window.findChild<FrameView*>("frame"))}) {
                focus->setFocus();
                QTest::keyClick(focus, Qt::Key_Return);
                QTest::keyClick(focus, Qt::Key_Enter);
            }
            // A row of Edits: Enter selects it (the panel then rebuilds).
            auto* row = panel->rowFor("cover", "m-solid");
            QVERIFY(row);
            row->setFocus();
            QTest::keyClick(row, Qt::Key_Return);
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        }
        QCOMPARE(uploads.size(), 0);
        QVERIFY(window.isVisible());
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
        for (const char* row : {"accepted edit", "undo", "redo", "agent edit", "format change", "audio change", "agent config change", "local refusal", "agent refusal", "refusal", "timeout"})
            QTest::newRow(row) << QString::fromLatin1(row) << true;
        for (const char* row : {"hover", "save a copy dialog", "discard dialog", "menu", "inspect", "dismissStatus", "raise", "saved copy", "unchanged push", "key press", "review action", "second upload"})
            QTest::newRow(row) << QString::fromLatin1(row) << false;
    }
    void aWaitingUploadIsCancelledOnlyByAChange() {
        QFETCH(QString, trigger);
        QFETCH(bool, cancels);
        qputenv("PUTGD_FILE_DIALOG", "fallback");
        const auto restore = qScopeGuard([] { qunsetenv("PUTGD_FILE_DIALOG"); });
        // System audio was recorded, so the audio row changes what would export.
        auto o = options();
        o.audioRecorded = o.systemRecorded = true;
        auto c = config();
        c.audio.system = true;
        ReviewWindow window(o, c);
        QSignalSpy uploads(&window, &ReviewWindow::uploadRequested), saves(&window, &ReviewWindow::saveCopyRequested), retries(&window, &ReviewWindow::retryExportRequested), proposals(&window, &ReviewWindow::editProposed), configs(&window, &ReviewWindow::configChanged);
        window.show();
        QVERIFY(putgd::test::waitForWindowExposed(&window));
        RecordingEdit edit;
        edit.revision = 2;
        edit.canUndo = edit.canRedo = true;
        window.setEdit(edit);
        if (trigger == "dismissStatus") window.showError("Export failed: synthetic", false);
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
        else if (trigger == "audio change") {
            QVERIFY(window.inspect()["audioEffective"].toBool());
            window.findChild<QAction*>("includeAudio")->toggle();
            QCOMPARE(configs.size(), 1);
            QVERIFY(!configs.last().at(0).value<RecordingConfig>().audio.system);
        }
        else if (trigger == "local refusal") QTest::keyClick(&window, Qt::Key_O, Qt::ShiftModifier);  // no cover selected: refused here, never proposed
        else if (trigger == "agent refusal") QCOMPARE(window.drive({{"action", "setCoverEdgeToPlayhead"}, {"edge", "end"}}).code, QString("recording_state"));  // no cover selected
        else if (trigger == "agent config change") { auto webm = config(); webm.format = "webm"; webm.codec = "vp9"; QVERIFY(window.setConfig(webm)); }
        else if (trigger == "refusal") window.setEdit(edit, answer(1, "invalid_request"));
        else if (trigger == "timeout") putgd::test::settle(2300);
        else if (trigger == "hover") {
            // The pointer moves over Upload and rests: Qt asks it for its tooltip.
            auto* button = window.findChild<QPushButton*>("upload");
            const QPoint at = button->rect().center();
            QTest::mouseMove(button, at);
            QHelpEvent rest(QEvent::ToolTip, at, button->mapToGlobal(at));
            QApplication::sendEvent(button, &rest);
            PUTGD_TRY_VERIFY(QToolTip::isVisible());
            QCOMPARE(QToolTip::text(), putgd::Ui::tooltipText(button->toolTip()));
        }
        else if (trigger == "save a copy dialog") { window.findChild<QPushButton*>("saveCopy")->click(); QVERIFY(window.inspect()["fileDialog"].toObject()["open"].toBool()); }
        else if (trigger == "discard dialog") { window.drive({{"action", "discard"}}); QVERIFY(window.inspect()["discardConfirmation"].toObject()["open"].toBool()); }
        else if (trigger == "menu") { auto* menu = window.findChild<QToolButton*>("uploadDestinationMenu")->menu(); menu->popup(window.mapToGlobal(QPoint(20, 20))); PUTGD_TRY_VERIFY(menu->isVisible()); }
        else if (trigger == "inspect") QVERIFY(window.drive({{"action", "inspect"}}).code.isEmpty());
        else if (trigger == "dismissStatus") {
            QVERIFY(status().contains("synthetic"));
            QVERIFY(window.drive({{"action", "dismissStatus"}}).code.isEmpty());
            QVERIFY2(!status().contains("synthetic"), qPrintable(status()));
        }
        else if (trigger == "raise") { window.hide(); QVERIFY(!window.isVisible()); window.reopen({}); QVERIFY(window.isVisible()); }
        else if (trigger == "saved copy") { window.setSavedCopy("/tmp/putgd-review-copy.mp4", 2, c); QCOMPARE(window.inspect()["savedCopy"].toObject()["path"].toString(), QString("/tmp/putgd-review-copy.mp4")); }
        else if (trigger == "unchanged push") window.setEdit(edit);
        else if (trigger == "key press") { QTest::keyClick(&window, Qt::Key_Period); QCOMPARE(window.inspect()["playheadUs"].toDouble(), 33333.0); }
        else if (trigger == "review action") { const auto result = window.drive({{"action", "frameStep"}, {"delta", 1}}); QVERIFY(result.code.isEmpty()); QCOMPARE(result.state["playheadUs"].toDouble(), 33333.0); }
        else if (trigger == "second upload") {
            // The second press keeps the first one's deadline.
            putgd::test::settle(1000);
            ctrlEnter(window);
            QVERIFY2(status().contains("already waiting"), qPrintable(status()));
            const auto left = window.inspect()["uploadDeadlineMs"].toInt();
            QVERIFY2(left > 0 && left < 1100, qPrintable(QString::number(left)));
        }
        else QFAIL("unknown trigger");
        // The accepted-edit, undo and redo rows each made a second proposal, of their own op.
        static const QHash<QString, QString> op{{"accepted edit", "addCut"}, {"undo", "undo"}, {"redo", "redo"}};
        if (op.contains(trigger)) {
            QCOMPARE(proposals.size(), 2);
            QCOMPARE(proposed(proposals)["op"].toString(), op[trigger]);
        }
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
            if (trigger == "refusal" || trigger == "local refusal" || trigger == "agent refusal") QVERIFY2(status().contains("wasn't applied"), qPrintable(status()));
        } else {
            QCOMPARE(uploads.size(), 1);
            QCOMPARE(uploads[0][0].toULongLong(), 3ull);
            QCOMPARE(saves.size() + retries.size(), 0);
            QVERIFY2(!status().contains("already waiting"), qPrintable(status()));  // V4: gone once sent
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
        window.activateWindow();
        window.findChild<FrameView*>("frame")->setFocus(Qt::TabFocusReason);
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
    // E24, U44, V1 (privacy): Set Out and Set to playhead end after the frame on
    // screen, from that frame's stored time rounded to its grid frame, also for
    // a seek not yet shown; checked through the exporter: the frame on screen is
    // the trim's last kept frame and is covered.
    void endEdgesKeepTheFrameOnScreen_data() {
        QTest::addColumn<int>("fps");
        QTest::addColumn<QString>("timeline");
        QTest::addColumn<qint64>("seek");
        QTest::addColumn<qint64>("shownUs");
        QTest::newRow("30 fps frame 1 stored at 33 ms") << 30 << "even" << qint64(33333) << qint64(33000);
        QTest::newRow("30 fps playhead at 40 ms") << 30 << "even" << qint64(40000) << qint64(67000);
        QTest::newRow("30 fps frame 100") << 30 << "even" << qint64(3333333) << qint64(3333000);
        QTest::newRow("60 fps frame 1 stored at 17 ms") << 60 << "even" << qint64(16667) << qint64(17000);
        QTest::newRow("60 fps frame 1 stored at 16 ms") << 60 << "floor" << qint64(16000) << qint64(16000);
        QTest::newRow("60 fps frame 301") << 60 << "even" << qint64(5016667) << qint64(5017000);
        QTest::newRow("30 fps seek into a frame gap") << 30 << "gap" << qint64(120000) << qint64(200000);
    }
    void endEdgesKeepTheFrameOnScreen() {
        QFETCH(int, fps);
        QFETCH(QString, timeline);
        QFETCH(qint64, seek);
        QFETCH(qint64, shownUs);
        QTemporaryDir dir;
        const QString clip = dir.filePath("clip.mkv");
        // Frame i stored at its time in whole ms (rounded, floored, or after a 100 ms gap).
        const auto pts = [&](int i) -> qint64 {
            if (timeline == "floor") return qint64(std::floor(i * 1000.0 / fps));
            if (timeline == "gap") return std::llround((i < 3 ? i : i + 3) * 1000.0 / fps);
            return std::llround(i * 1000.0 / fps);
        };
        const auto level = [](int i) { return 20 + (i * 9) % 200; };  // neighbours differ visibly
        QVERIFY(test::gopclip::write(clip, 6, 320, 180, fps, pts, level));
        auto o = options();
        o.workingClip = clip;
        o.durationUs = 6000000;
        o.canvas = QSize(320, 180);
        auto c = config();
        c.fps = fps;
        ReviewWindow window(o, c);
        QSignalSpy proposals(&window, &ReviewWindow::editProposed);
        window.show();
        QVERIFY(putgd::test::waitForWindowExposed(&window));
        RecordingEdit edit;
        edit.revision = 2;
        edit.masks = {mask("m1", "solid", 0, 6000000)};
        edit.masks[0].rect = QRect(10, 10, 100, 60);
        window.setEdit(edit);
        window.drive({{"action", "selectMask"}, {"maskId", "m1"}});
        // Proposed at once after the seek, before its frame was shown.
        QVERIFY(window.drive({{"action", "seek"}, {"time", double(seek)}}).code.isEmpty());
        QVERIFY(window.drive({{"action", "setOut"}}).code.isEmpty());
        QCOMPARE(window.inspect()["preview"].toObject()["shownUs"].toDouble(), double(shownUs));
        const qint64 k = std::llround(double(shownUs) * fps / 1e6);
        const qint64 end = std::llround(double(k + 1) * 1e6 / fps);
        QCOMPARE(qint64(proposed(proposals)["end"].toDouble()), end);
        QVERIFY(window.drive({{"action", "seek"}, {"time", double(seek)}}).code.isEmpty());
        QVERIFY(window.drive({{"action", "setCoverEdgeToPlayhead"}, {"edge", "end"}}).code.isEmpty());
        QCOMPARE(qint64(proposed(proposals)["mask"].toObject()["end"].toDouble()), end);
        // Through the exporter: its frame k is the frame on screen, kept by the trim and covered.
        int ordinal = 0;
        while (pts(ordinal) * 1000 != shownUs) ++ordinal;
        int frames = 0;
        const auto exported = [&](const QString& name, const RecordingEdit& e, QImage& last) {
            ExportRequest request;
            request.input = clip;
            request.output = dir.filePath(name);
            request.edit = e;
            request.config = c;
            request.durationUs = o.durationUs;
            request.canvas = o.canvas;
            ExportResult result;
            std::atomic_bool cancelled{false};
            if (const auto error = exportClip(request, result, cancelled)) return error.code == "recording_unsupported" ? QStringLiteral("skip") : error.message;
            frames = result.frames;
            if (auto error = previewFrame(request.output, {}, c, std::llround(double(k) * 1e6 / fps), 64, {}, last)) return error.message;
            return QString();
        };
        RecordingEdit trimmed;
        trimmed.trimEnd = end;
        QImage last;
        auto failure = exported("trim.mp4", trimmed, last);
        if (failure == "skip") QSKIP("No MP4 encoder in this FFmpeg");
        QVERIFY2(failure.isEmpty(), qPrintable(failure));
        QCOMPARE(qint64(frames), k + 1);  // frames 0 to k, the one on screen last
        const int grey = test::gopclip::grayOf(level(ordinal));
        QVERIFY2(std::abs(qGray(last.pixel(last.width() - 4, last.height() - 4)) - grey) <= 3, qPrintable(QStringLiteral("the trim's last frame is grey %1, the frame on screen %2").arg(qGray(last.pixel(last.width() - 4, last.height() - 4))).arg(grey)));
        RecordingEdit covered;
        auto cover = edit.masks[0];
        cover.end = end;
        covered.masks = {cover};
        failure = exported("cover.mp4", covered, last);
        QVERIFY2(failure.isEmpty(), qPrintable(failure));
        QVERIFY2(qGray(last.pixel(10, 10)) <= 8, qPrintable(QStringLiteral("the frame on screen is not covered (grey %1)").arg(qGray(last.pixel(10, 10)))));
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
    // D1, W1: every handle resizes from the opposite side, and past it the box
    // flips; corners win with 16 × 16 px targets, a band along each side (6 px
    // outside, 4 px inside) takes that side anywhere along it, and a tiny box
    // keeps a 6 px body at its centre.
    void everyHandleResizesFromTheOppositeSide() {
        const QRect r(100, 80, 320, 120), all(0, 0, 1280, 720);
        const QPoint d(10, -5);
        // 0 top-left, 1 top, 2 top-right, 3 right, 4 bottom-right, 5 bottom, 6 bottom-left, 7 left.
        const QList<QRect> expected{QRect(110, 75, 310, 125), QRect(100, 75, 320, 125), QRect(100, 75, 330, 125), QRect(100, 80, 330, 120),
                                    QRect(100, 80, 330, 115), QRect(100, 80, 320, 115), QRect(110, 80, 310, 115), QRect(110, 80, 310, 120)};
        for (int h = 0; h < 8; ++h) QCOMPARE(FrameView::resizedRect(r, h, d, all), expected[h]);
        QCOMPARE(FrameView::resizedRect(r, 0, QPoint(-500, -500), all), QRect(0, 0, 420, 200));
        // Past the opposite corner it flips rather than leaving a sliver.
        QCOMPARE(FrameView::resizedRect(r, 4, QPoint(-420, -160), all), QRect(0, 40, 100, 40));
        QCOMPARE(FrameView::resizedRect(r, 3, QPoint(-320, 0), all), QRect(100, 80, 2, 120));
        const QRectF big(100, 100, 200, 100), tiny(100, 100, 12, 10);
        QCOMPARE(FrameView::handleAt(big, QPointF(106, 106)), 0);   // corner, inside
        QCOMPARE(FrameView::handleAt(big, QPointF(93, 94)), 0);     // corner, outside
        QCOMPARE(FrameView::handleAt(big, QPointF(150, 96)), 1);    // top band, anywhere along it
        QCOMPARE(FrameView::handleAt(big, QPointF(250, 203)), 5);   // bottom band
        QCOMPARE(FrameView::handleAt(big, QPointF(297, 150)), 3);   // right band, inside
        QCOMPARE(FrameView::handleAt(big, QPointF(160, 150)), -1);  // the body
        QCOMPARE(FrameView::handleAt(big, QPointF(150, 92)), -1);   // beyond the band
        QCOMPARE(FrameView::handleAt(tiny, QPointF(106, 105)), -1); // a tiny box keeps its centre
        QCOMPARE(FrameView::handleAt(tiny, QPointF(98, 98)), 0);
    }
    // D1, D3, D5, D7, D8 through the pointer: a press on an unselected cover
    // selects it and nothing moves inside 3 px; the top-left handle keeps the
    // bottom-right corner; Esc or a right press mid-gesture puts the box back
    // and keeps it selected; a placing drag under 6 px or beside the picture
    // creates nothing; at 100 % a click beside the cover deselects it.
    void viewerGesturesHaveADeadZoneAndCancel() {
        // A real clip: placing waits for the decoded frame it is drawn on (R18).
        QTemporaryDir dir;
        const QString clip = dir.filePath("gops.mkv");
        QVERIFY(test::gopclip::write(clip, 1, 1280, 720));
        auto o = options();
        o.workingClip = clip;
        o.durationUs = 1000000;
        ReviewWindow window(o, config());
        QSignalSpy proposals(&window, &ReviewWindow::editProposed);
        window.show();
        QVERIFY(putgd::test::waitForWindowExposed(&window));
        RecordingEdit edit;
        edit.revision = 2;
        edit.masks = {mask("m1", "solid", 0, 1000000)};
        window.setEdit(edit);
        auto* view = window.findChild<FrameView*>("frame");
        PUTGD_TRY_VERIFY(!window.inspect()["preview"].toObject()["shownUs"].isNull());
        const auto box = [&] {
            const QRect r = view->imageRect();
            const double k = r.width() / 1280.0;
            return QRectF(r.x() + 100 * k, r.y() + 80 * k, 320 * k, 120 * k);
        };
        const auto move = [&](QPoint to, Qt::MouseButtons buttons = Qt::LeftButton) {
            QMouseEvent event(QEvent::MouseMove, QPointF(to), view->mapToGlobal(QPointF(to)), Qt::NoButton, buttons, Qt::NoModifier);
            QApplication::sendEvent(view, &event);
        };
        const QPoint body = box().center().toPoint();
        // W2: the first press on an unselected cover selects; 6 px of travel doesn't move it.
        window.drive({{"action", "clearSelection"}});
        drag(view, body, body + QPoint(6, 0));
        QCOMPARE(window.inspect()["selectedMaskId"].toString(), QString("m1"));
        QCOMPARE(proposals.size(), 0);
        // Just outside the corner: there the handle wins at any box size.
        const QPoint topLeft = box().topLeft().toPoint() - QPoint(3, 3);
        drag(view, topLeft, topLeft + QPoint(20, 10));
        QCOMPARE(proposals.size(), 1);
        const auto resized = proposed(proposals)["mask"].toObject()["rect"].toArray();
        QVERIFY2(resized[0].toInt() > 100 && resized[1].toInt() > 80, qPrintable(QJsonDocument(resized).toJson()));
        QCOMPARE(resized[0].toInt() + resized[2].toInt(), 420);
        QCOMPARE(resized[1].toInt() + resized[3].toInt(), 200);
        window.setEdit(edit, answer(1));
        // Esc mid-move, then a right press mid-resize: nothing proposed, still selected.
        QTest::mousePress(view, Qt::LeftButton, {}, body);
        move(body + QPoint(30, 30));
        QTest::keyClick(view, Qt::Key_Escape);
        QTest::mouseRelease(view, Qt::LeftButton, {}, body + QPoint(30, 30));
        const QPoint bottomRight = box().bottomRight().toPoint() + QPoint(3, 3);
        QTest::mousePress(view, Qt::LeftButton, {}, bottomRight);
        move(bottomRight + QPoint(25, 25));
        QTest::mousePress(view, Qt::RightButton, {}, bottomRight + QPoint(25, 25));
        QTest::mouseRelease(view, Qt::LeftButton, {}, bottomRight + QPoint(25, 25));
        QCOMPARE(proposals.size(), 1);
        QCOMPARE(window.inspect()["selectedMaskId"].toString(), QString("m1"));
        QCOMPARE(window.inspect()["masks"].toArray().first().toObject()["id"].toString(), QString("m1"));
        // R27: an answer that leaves the dragged cover as it was doesn't undo the drag.
        QTest::mousePress(view, Qt::LeftButton, {}, body);
        move(body + QPoint(40, 0));
        auto later = edit;
        later.revision = 3;
        window.setEdit(later);
        QTest::mouseRelease(view, Qt::LeftButton, {}, body + QPoint(40, 0));
        QCOMPARE(proposals.size(), 2);
        QVERIFY(proposed(proposals)["mask"].toObject()["rect"].toArray()[0].toInt() > 100);
        window.setEdit(later, answer(2));
        // W20: Ctrl+Z mid-move cancels only the move.
        QTest::mousePress(view, Qt::LeftButton, {}, body);
        move(body + QPoint(30, 30));
        for (auto* shortcut : window.findChildren<QShortcut*>()) if (shortcut->key() == QKeySequence(Qt::CTRL | Qt::Key_Z)) emit shortcut->activated();
        QTest::mouseRelease(view, Qt::LeftButton, {}, body + QPoint(30, 30));
        QCOMPARE(proposals.size(), 2);
        // W22: a placing drag from more than 8 px beside the picture does nothing.
        window.drive({{"action", "armRedact"}});
        const QPoint inside = view->imageRect().center();
        const QPoint beside(view->imageRect().left() - 12, view->imageRect().center().y());
        QVERIFY(view->rect().contains(beside));
        drag(view, beside, inside + QPoint(100, 60));
        QVERIFY(!window.inspect()["armed"].isNull());
        QCOMPARE(proposals.size(), 2);
        // W6: a click places a box about 20 % × 8 % of the frame centred on it.
        drag(view, inside, inside + QPoint(2, 1));
        QCOMPARE(proposals.size(), 3);
        const auto placed = proposed(proposals)["mask"].toObject()["rect"].toArray();
        QCOMPARE(placed[2].toInt(), 256);
        QCOMPARE(placed[3].toInt(), 57);
        QVERIFY2(std::abs(placed[0].toInt() + 128 - 640) <= 4 && std::abs(placed[1].toInt() + 28 - 360) <= 4, qPrintable(QJsonDocument(placed).toJson()));
        // R17: a long thin drag (over a line of text) makes that thin box.
        window.drive({{"action", "armRedact"}});
        drag(view, inside, inside + QPoint(100, 3));
        QCOMPARE(proposals.size(), 4);
        const auto thin = proposed(proposals)["mask"].toObject()["rect"].toArray();
        QVERIFY2(thin[2].toInt() > 100 && thin[3].toInt() >= 2 && thin[3].toInt() < 10, qPrintable(QJsonDocument(thin).toJson()));
        // F5: the preview is what the release makes: the default box under 8 × 8 px, then the drawn box.
        window.drive({{"action", "armRedact"}});
        QTest::mousePress(view, Qt::LeftButton, {}, inside);
        {
            QMouseEvent small(QEvent::MouseMove, QPointF(inside + QPoint(5, 4)), view->mapToGlobal(QPointF(inside + QPoint(5, 4))), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(view, &small);
        }
        const QRect clickBox = view->placementPreview();
        QCOMPARE(clickBox.size(), QSize(256, 57));
        {
            QMouseEvent large(QEvent::MouseMove, QPointF(inside + QPoint(60, 30)), view->mapToGlobal(QPointF(inside + QPoint(60, 30))), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(view, &large);
        }
        const QRect drawnBox = view->placementPreview();
        QVERIFY(drawnBox.size() != clickBox.size());
        QTest::mouseRelease(view, Qt::LeftButton, {}, inside + QPoint(60, 30));
        QCOMPARE(proposed(proposals)["mask"].toObject()["rect"].toArray(), (QJsonArray{drawnBox.x(), drawnBox.y(), drawnBox.width(), drawnBox.height()}));
        const auto beforeClick = proposals.size();
        // P12: under 8 × 8 view px (press to release) is a click: the default box at the press.
        window.drive({{"action", "armRedact"}});
        drag(view, inside, inside + QPoint(7, 7));
        QCOMPARE(proposals.size(), beforeClick + 1);
        QCOMPARE(proposed(proposals)["mask"].toObject()["rect"].toArray()[2].toInt(), 256);
        // At 100 %: a click beside the cover deselects it; a drag there pans and keeps it.
        window.drive({{"action", "selectMask"}, {"maskId", "m1"}});
        window.drive({{"action", "viewerZoom"}, {"mode", "actual"}});
        const QPoint away(view->width() - 60, view->height() - 40);
        QVERIFY(view->imageRect().contains(away) && !box().contains(away));
        drag(view, away, away + QPoint(-40, -30));
        QCOMPARE(window.inspect()["selectedMaskId"].toString(), QString("m1"));
        drag(view, away, away + QPoint(1, 0));
        QVERIFY(window.inspect()["selectedMaskId"].isNull());
    }
    // Q11: skimming past the clip's last frame settles on it as exact (its
    // stored time is earlier than the skim time); no decode loop, no stuck ≈.
    void skimAtTheLastFrameSettles() {
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
        window.setEdit(edit);
        QVERIFY(window.drive({{"action", "skim"}, {"time", 999000}}).code.isEmpty());
        const auto exact = [&] { return window.inspect()["timeline"].toObject()["skim"].toObject()["exact"].toBool(); };
        PUTGD_TRY_VERIFY(exact());
        PUTGD_VERIFY_STEADY(exact(), 300);
    }
    // Q23, Q24 (live m74 run, maximized 1920×1040, 784×434 clip): hover,
    // press and paint agree on every side of a selected box: its west band's
    // centre resizes (the cursor said so), and presses 2 px outside the right
    // and bottom edges resize too. A late press on empty picture takes the grip
    // the pointer just left; one that lands on the cover's body moves it
    // (r5-neon #6), and the drag counts from the real press point.
    void handlesAnswerOnEverySide_data() {
        QTest::addColumn<QString>("kind");
        QTest::addColumn<int>("handle");
        QTest::addColumn<QPoint>("offset");
        QTest::addColumn<QPoint>("late");
        QTest::addColumn<bool>("moves");
        QTest::newRow("blur west centre") << "blur" << 7 << QPoint(0, 0) << QPoint() << false;
        // The live run's presses arrived 5–11 px along the drag (emulated middle button).
        QTest::newRow("blur west, press 7 px late outside") << "blur" << 7 << QPoint(0, 0) << QPoint(-7, 3) << false;
        QTest::newRow("blur west, press 7 px late on the body") << "blur" << 7 << QPoint(0, 0) << QPoint(7, 3) << true;
        QTest::newRow("solid east 2 px outside, press 7 px late") << "solid" << 3 << QPoint(2, 0) << QPoint(6, 4) << false;
        QTest::newRow("solid south 2 px below, press 7 px late") << "solid" << 5 << QPoint(0, 2) << QPoint(4, 6) << false;
        QTest::newRow("solid east 2 px outside") << "solid" << 3 << QPoint(2, 0) << QPoint() << false;
        QTest::newRow("solid south-east 2 px below") << "solid" << 4 << QPoint(0, 2) << QPoint() << false;
        QTest::newRow("solid south 2 px below") << "solid" << 5 << QPoint(0, 2) << QPoint() << false;
        QTest::newRow("blur east 2 px outside") << "blur" << 3 << QPoint(2, 0) << QPoint() << false;
    }
    void handlesAnswerOnEverySide() {
        QFETCH(QString, kind);
        QFETCH(int, handle);
        QFETCH(QPoint, offset);
        QFETCH(QPoint, late);
        QFETCH(bool, moves);
        auto o = options();
        o.canvas = QSize(784, 434);
        o.availableSize = QSize(1920, 1040);
        ReviewWindow window(o, config());
        QSignalSpy proposals(&window, &ReviewWindow::editProposed);
        window.show();
        QVERIFY(putgd::test::waitForWindowExposed(&window));
        window.resize(1920, 1040);
        RecordingEdit edit;
        edit.revision = 2;
        auto m = mask("m1", kind, 0, duration);
        m.rect = kind == "blur" ? QRect(479, 182, 172, 135) : QRect(119, 101, 172, 134);
        edit.masks = {m};
        window.setEdit(edit);
        window.drive({{"action", "selectMask"}, {"maskId", "m1"}});
        auto* view = window.findChild<FrameView*>("frame");
        QImage frame(784, 434, QImage::Format_RGB32);
        frame.fill(Qt::gray);
        view->setFrame(frame, 0);
        QCoreApplication::processEvents();
        const QRect r = view->imageRect();
        const double k = r.width() / 784.0;
        const QRectF box(r.x() + m.rect.x() * k, r.y() + m.rect.y() * k, m.rect.width() * k, m.rect.height() * k);
        const QPointF points[8] = {box.topLeft(), {box.center().x(), box.top()}, box.topRight(), {box.right(), box.center().y()},
                                   box.bottomRight(), {box.center().x(), box.bottom()}, box.bottomLeft(), {box.left(), box.center().y()}};
        // inspect reports the same grips, in window coordinates (what a driver aims at).
        const auto grip = window.inspect()["viewer"].toObject()["selectedCover"].toObject()["grips"].toArray()[handle].toArray();
        QVERIFY(std::abs(view->mapTo(&window, points[handle].toPoint()).x() - grip[0].toDouble()) <= 1 && std::abs(view->mapTo(&window, points[handle].toPoint()).y() - grip[1].toDouble()) <= 1);
        const QPoint at = (points[handle] + QPointF(offset)).toPoint();
        QMouseEvent hover(QEvent::MouseMove, QPointF(at), view->mapToGlobal(QPointF(at)), Qt::NoButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(view, &hover);
        const auto shape = view->cursor().shape();
        QVERIFY2(shape == Qt::SizeHorCursor || shape == Qt::SizeVerCursor || shape == Qt::SizeFDiagCursor || shape == Qt::SizeBDiagCursor, qPrintable(QString::number(int(shape))));
        // A late press that doesn't move afterwards never edits (P1 rule).
        if (!late.isNull()) {
            QTest::mousePress(view, Qt::LeftButton, {}, at + late);
            QTest::mouseRelease(view, Qt::LeftButton, {}, at + late);
            QCOMPARE(proposals.size(), 0);
            QApplication::sendEvent(view, &hover);
        }
        drag(view, at + late, at + QPoint(30, 21));
        QCOMPARE(proposals.size(), 1);
        const auto rect = proposed(proposals)["mask"].toObject()["rect"].toArray();
        // A resize changes the size; a move keeps it.
        const bool resized = rect[2].toInt() != m.rect.width() || rect[3].toInt() != m.rect.height();
        QVERIFY2(resized != moves, qPrintable(QJsonDocument(rect).toJson()));
        if (moves) QVERIFY(rect[0].toInt() != m.rect.x());
    }
    // r5-neon #11: the speed button always names its speed and keeps one
    // width, so Play and the frame steps never shift when the speed changes.
    void theSpeedButtonKeepsItsWidth() {
        ReviewWindow window(options(), config());
        window.show();
        QVERIFY(putgd::test::waitForWindowExposed(&window));
        auto* speed = window.findChild<QToolButton*>("playSpeed");
        auto* next = window.findChild<QToolButton*>("nextFrame");
        QVERIFY(speed && next);
        QCOMPARE(speed->text(), QString("1×"));
        const int width = speed->width(), nextX = next->mapTo(&window, QPoint()).x();
        QVERIFY(speed->sizeHint().width() <= width + 1);
        for (const double factor : {0.5, 4.0, 1.0}) {
            QVERIFY(window.drive({{"action", "speed"}, {"speed", factor}}).code.isEmpty());
            QCoreApplication::processEvents();
            QCOMPARE(speed->text(), factor == 0.5 ? QString("0.5×") : factor == 4.0 ? QString("4×") : QString("1×"));
            QCOMPARE(speed->width(), width);
            QCOMPARE(next->mapTo(&window, QPoint()).x(), nextX);
        }
    }
    // P9 (real bug): a Shows from time typed but not committed is committed
    // before Upload acts; the upload waits for that edit and sends its answer.
    void typedTimesAreCommittedBeforeUpload() {
        ReviewWindow window(options(), config());
        QSignalSpy proposals(&window, &ReviewWindow::editProposed), uploads(&window, &ReviewWindow::uploadRequested);
        window.show();
        QVERIFY(putgd::test::waitForWindowExposed(&window));
        window.activateWindow();
        RecordingEdit edit;
        edit.revision = 2;
        edit.masks = {mask("m1", "solid", 6000000, 8000000)};
        window.setEdit(edit);
        window.drive({{"action", "selectMask"}, {"maskId", "m1"}});
        auto* field = window.findChild<QLineEdit*>("reviewPanelShowsFromTime");
        QVERIFY(field);
        field->setFocus();
        field->selectAll();
        QTest::keyClicks(field, "0:05.0");
        QCOMPARE(proposals.size(), 0);
        QTest::mouseClick(window.findChild<QPushButton*>("upload"), Qt::LeftButton);
        QCOMPARE(proposals.size(), 1);
        QCOMPARE(proposed(proposals)["mask"].toObject()["start"].toDouble(), 5000000.0);
        QCOMPARE(uploads.size(), 0);  // waits for the edit's answer
        auto typed = edit;
        typed.revision = 3;
        typed.masks[0].start = 5000000;
        window.setEdit(typed, answer(1));
        QCOMPARE(uploads.size(), 1);
        QCOMPARE(uploads[0][0].toULongLong(), 3ull);
    }
    // P18: with trims or cuts the transport shows the recording time too.
    void transportShowsRecordingTime() {
        ReviewWindow window(options(), config());
        window.show();
        QVERIFY(putgd::test::waitForWindowExposed(&window));
        RecordingEdit edit;
        edit.revision = 2;
        window.setEdit(edit);
        auto* caption = window.findChild<QLabel*>("clipTimeCaption");
        QCOMPARE(caption->text(), QString("clip time"));
        edit.revision = 3;
        edit.cuts = {RecordingCut{"c1", 3000000, 4000000}};
        window.setEdit(edit);
        window.drive({{"action", "seek"}, {"time", 5000000}});
        QCOMPARE(caption->text(), QString("clip · rec 0:05.0"));
        window.drive({{"action", "seek"}, {"time", 3500000}});
        QCOMPARE(caption->text(), QString("rec 0:03.5 · excluded"));
    }
    // F1, F2 on a real clip: an edge drag drops the skim picture at once; a
    // skim that ends without a press gives way to the playhead frame after
    // 250 ms, and one that comes back within 250 ms carries on.
    void skimPictureEndsCleanly() {
        QTemporaryDir dir;
        const QString clip = dir.filePath("gops.mkv");
        QVERIFY(test::gopclip::write(clip, 3, 320, 180));
        auto o = options();
        o.workingClip = clip;
        o.durationUs = 3000000;
        o.canvas = QSize(320, 180);
        ReviewWindow window(o, config());
        window.show();
        QVERIFY(putgd::test::waitForWindowExposed(&window));
        RecordingEdit edit;
        edit.revision = 2;
        window.setEdit(edit);
        auto* view = window.findChild<FrameView*>("frame");
        auto* strip = timeline(window);
        QVERIFY(window.drive({{"action", "skim"}, {"time", 1500000}}).code.isEmpty());
        PUTGD_TRY_VERIFY(view->skimming());
        emit strip->trimEdgeDragged(true, 1000000);
        QVERIFY(!view->skimming());
        QCOMPARE(window.inspect()["preview"].toObject()["edgePreviewUs"].toDouble(), 1000000.0);
        emit strip->edgeDragFinished();
        QCoreApplication::processEvents();
        QVERIFY(window.drive({{"action", "skim"}, {"time", 2000000}}).code.isEmpty());
        PUTGD_TRY_VERIFY(view->skimming());
        window.drive({{"action", "skimEnd"}});
        QVERIFY(view->skimming());
        QVERIFY(window.drive({{"action", "skim"}, {"time", 2000000}}).code.isEmpty());
        PUTGD_VERIFY_STEADY(view->skimming(), 400);
        window.drive({{"action", "skimEnd"}});
        PUTGD_TRY_VERIFY(!view->skimming());
    }
    // F6: a selected cut's Starts / Ends fields adjust it as dragging its edges does.
    void cutTimesAreTyped() {
        ReviewWindow window(options(), config());
        QSignalSpy proposals(&window, &ReviewWindow::editProposed);
        window.show();
        QVERIFY(putgd::test::waitForWindowExposed(&window));
        window.activateWindow();
        RecordingEdit edit;
        edit.revision = 2;
        edit.cuts = {RecordingCut{"c1", 3000000, 4000000}};
        window.setEdit(edit);
        window.drive({{"action", "selectCut"}, {"cutId", "c1"}});
        auto* starts = window.findChild<QLineEdit*>("reviewPanelCutStartsTime");
        QVERIFY(starts && window.findChild<QLineEdit*>("reviewPanelCutEndsTime"));
        starts->setFocus();
        starts->selectAll();
        QTest::keyClicks(starts, "0:02.5");
        QTest::keyClick(starts, Qt::Key_Return);
        QCOMPARE(proposals.size(), 1);
        QCOMPARE(proposed(proposals)["op"].toString(), QString("updateCut"));
        QCOMPARE(proposed(proposals)["start"].toDouble(), 2500000.0);
        QCOMPARE(proposed(proposals)["end"].toDouble(), 4000000.0);
    }
    // R28, P13: a placement follows a new time selection; a click that clears
    // the selection leaves its time as it was.
    void placementFollowsTheSelection() {
        ReviewWindow window(options(), config());
        window.show();
        QVERIFY(putgd::test::waitForWindowExposed(&window));
        RecordingEdit edit;
        edit.revision = 2;
        window.setEdit(edit);
        window.drive({{"action", "selectRange"}, {"start", 2000000}, {"end", 4000000}});
        auto placement = window.drive({{"action", "armRedact"}}).state["placement"].toObject();
        QCOMPARE(placement["start"].toDouble(), 2000000.0);
        emit timeline(window)->timeSelected(6000000, 7000000);
        placement = window.inspect()["placement"].toObject();
        QCOMPARE(placement["start"].toDouble(), 6000000.0);
        QCOMPARE(placement["end"].toDouble(), 7000000.0);
        QVERIFY(placement["banner"].toString().contains("0:06.0"));
        // P13: a click that clears the selection doesn't change what the placement covers.
        emit timeline(window)->timeSelectionCleared();
        emit timeline(window)->selectionCleared();
        QCOMPARE(window.inspect()["placement"].toObject()["start"].toDouble(), 6000000.0);
        QVERIFY(!window.inspect()["placement"].toObject()["whole"].toBool());
    }
    // E1, E4, E5: the window's keys work from the timeline's or the viewer's
    // focus; a text field keeps its own.
    void windowKeysWorkFromAnyFocus() {
        ReviewWindow window(options(), config());
        window.show();
        QVERIFY(putgd::test::waitForWindowExposed(&window));
        window.activateWindow();
        RecordingEdit edit;
        edit.revision = 2;
        edit.cuts = {RecordingCut{"c1", 3000000, 4000000}};
        edit.masks = {mask("m1", "solid", 6000000, 8000000)};
        window.setEdit(edit);
        const auto playhead = [&] { return qint64(window.inspect()["playheadUs"].toDouble()); };
        const auto timelineState = [&] { return window.inspect()["timeline"].toObject(); };
        auto* strip = timeline(window);
        strip->setFocus();
        window.drive({{"action", "seek"}, {"time", 1000000}});
        QTest::keyClick(strip, Qt::Key_Right);
        QCOMPARE(playhead(), qint64(1033333));
        QTest::keyClick(strip, Qt::Key_Left, Qt::ShiftModifier);
        QCOMPARE(playhead(), qint64(33333));
        // Q21: a cut's boundary is stepped to from its kept side.
        for (const qint64 point : {qint64(2966667), qint64(4000000), qint64(6000000)}) { QTest::keyClick(strip, Qt::Key_Down); QCOMPARE(playhead(), point); }
        QTest::keyClick(strip, Qt::Key_Up);
        QCOMPARE(playhead(), qint64(4000000));
        // Q10: a present skim wins: the key acts at the skimmed frame.
        QVERIFY(window.drive({{"action", "skim"}, {"time", 5000000}}).code.isEmpty());
        QTest::keyClick(strip, Qt::Key_Period);
        QCOMPARE(playhead(), qint64(5033333));
        QVERIFY(window.inspect()["timeline"].toObject()["skim"].isNull());
        const auto cleanup = qScopeGuard([&] { strip->setSkimming(true); strip->setSnapping(true); });
        QTest::keyClick(strip, Qt::Key_S);
        QVERIFY(!timelineState()["skimming"].toBool());
        QVERIFY(!window.findChild<QToolButton*>("skimToggle")->isChecked());
        QTest::keyClick(strip, Qt::Key_N);
        QVERIFY(!timelineState()["snapping"].toBool());
        QTest::keyClick(strip, Qt::Key_Equal);
        QVERIFY(timelineState()["zoom"].toInt() > 1);
        QTest::keyClick(strip, Qt::Key_Minus);
        QCOMPARE(timelineState()["zoom"].toInt(), 1);
        QTest::keyClick(strip, Qt::Key_Equal, Qt::ControlModifier);
        QTest::keyClick(strip, Qt::Key_Equal);
        QVERIFY(timelineState()["zoom"].toInt() > 2);
        QTest::keyClick(strip, Qt::Key_Z, Qt::ShiftModifier);
        QCOMPARE(timelineState()["zoom"].toInt(), 1);
        // [ and ] from the viewer: they always mark a time selection (Q7).
        window.drive({{"action", "clearSelection"}});
        auto* view = window.findChild<FrameView*>("frame");
        view->setFocus();
        window.drive({{"action", "seek"}, {"time", 1000000}});
        QTest::keyClick(view, Qt::Key_BracketLeft);
        window.drive({{"action", "seek"}, {"time", 2000000}});
        QTest::keyClick(view, Qt::Key_BracketRight);
        QCOMPARE(window.inspect()["selection"].toObject()["start"].toDouble(), 1000000.0);
        QCOMPARE(window.inspect()["selection"].toObject()["end"].toDouble(), 2000000.0);
        // A time field types them, and Esc there puts its time back and keeps the cover (E6).
        window.drive({{"action", "selectMask"}, {"maskId", "m1"}});
        auto* field = window.findChild<QLineEdit*>("reviewPanelShowsFromTime");
        QVERIFY(field);
        field->setFocus();
        QTest::keyClicks(field, "s[n");
        QVERIFY(!timelineState()["skimming"].toBool());
        QVERIFY(!timelineState()["snapping"].toBool());
        QVERIFY(window.inspect()["selection"].isNull());
        QTest::keyClick(field, Qt::Key_Escape);
        QCOMPARE(field->text(), QString("0:06.0"));
        QCOMPARE(window.inspect()["selectedMaskId"].toString(), QString("m1"));
    }
    // C3, A14, A5: a click on the timeline while playing seeks and keeps
    // playing; a scrub pauses and keeps the time selection; a click on a cover
    // bar selects it without moving the playhead.
    void timelineClicksKeepPlayingAndTheSelection() {
        QTemporaryDir dir;
        const QString clip = dir.filePath("gops.mkv");
        QVERIFY(test::gopclip::write(clip, 3, 320, 180));
        auto o = options();
        o.workingClip = clip;
        o.durationUs = 3000000;
        o.canvas = QSize(320, 180);
        ReviewWindow window(o, config());
        window.show();
        QVERIFY(putgd::test::waitForWindowExposed(&window));
        RecordingEdit edit;
        edit.revision = 2;
        edit.masks = {mask("m1", "solid", 1000000, 2000000)};
        window.setEdit(edit);
        auto* strip = timeline(window);
        QVERIFY(window.drive({{"action", "play"}}).code.isEmpty());
        QVERIFY(window.inspect()["playing"].toBool());
        emit strip->seekRequested(500000);
        QVERIFY(window.inspect()["playing"].toBool());
        emit strip->scrubStarted();
        QVERIFY(!window.inspect()["playing"].toBool());
        window.drive({{"action", "selectRange"}, {"start", 300000}, {"end", 900000}});
        emit strip->scrubStarted();
        emit strip->seekRequested(2500000);
        QCOMPARE(window.inspect()["selection"].toObject()["end"].toDouble(), 900000.0);
        // r5-neon #2: a press on the cover's bar selects it in place; the
        // Edits list still shows the cover from its start.
        window.drive({{"action", "seek"}, {"time", 2500000}});
        emit strip->coverSelected("m1");
        QCOMPARE(window.inspect()["selectedMaskId"].toString(), QString("m1"));
        QCOMPARE(window.inspect()["playheadUs"].toDouble(), 2500000.0);
        window.drive({{"action", "selectMask"}, {"maskId", QJsonValue()}});
        QVERIFY(window.drive({{"action", "selectEdit"}, {"kind", "cover"}, {"editId", "m1"}}).code.isEmpty());
        QCOMPARE(window.inspect()["playheadUs"].toDouble(), 1000000.0);
    }
    // D4 (privacy): while a cover moves, its effect follows the box. The frame
    // without it is asked for (pending, the old place still covered, until it
    // arrives), and the live state ends only once the frame for the committed
    // edit is shown.
    void aMovedCoverStaysCoveredUntilItsFrameArrives() {
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
        PUTGD_TRY_VERIFY(!window.inspect()["preview"].toObject()["shownUs"].isNull());
        window.drive({{"action", "selectMask"}, {"maskId", "m1"}});
        auto* view = window.findChild<FrameView*>("frame");
        const QRect r = view->imageRect();
        const double k = r.width() / 320.0;
        const QPoint centre(r.x() + int(60 * k), r.y() + int(40 * k));
        const auto live = [&]() -> QJsonValue { return window.inspect()["viewer"].toObject()["live"]; };
        QTest::mousePress(view, Qt::LeftButton, {}, centre);
        QVERIFY(live().isNull());
        QMouseEvent move(QEvent::MouseMove, QPointF(centre + QPoint(60, 30)), view->mapToGlobal(QPointF(centre + QPoint(60, 30))), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(view, &move);
        QCOMPARE(live().toObject()["maskId"].toString(), QString("m1"));
        PUTGD_TRY_VERIFY(!live().toObject()["pending"].toBool());
        QTest::mouseRelease(view, Qt::LeftButton, {}, centre + QPoint(60, 30));
        QCOMPARE(proposals.size(), 1);
        QVERIFY(!live().isNull());
        auto moved = edit;
        moved.revision = 3;
        const auto rect = proposed(proposals)["mask"].toObject()["rect"].toArray();
        moved.masks[0].rect = QRect(rect[0].toInt(), rect[1].toInt(), rect[2].toInt(), rect[3].toInt());
        window.setEdit(moved, answer(1));
        PUTGD_TRY_VERIFY(live().isNull());
    }
    // W4: a released edge drag keeps its frame until the proposal is answered,
    // then switches once; a cut edge shows the kept side. W9: a refused cut
    // gives its range back.
    void edgeFramesHoldUntilTheAnswer() {
        ReviewWindow window(options(), config());
        QSignalSpy proposals(&window, &ReviewWindow::editProposed);
        window.show();
        QVERIFY(putgd::test::waitForWindowExposed(&window));
        RecordingEdit edit;
        edit.revision = 2;
        edit.cuts = {RecordingCut{"c1", 3000000, 4000000}};
        window.setEdit(edit);
        auto* strip = timeline(window);
        const auto preview = [&] { return window.inspect()["preview"].toObject(); };
        emit strip->editEdgeDragged("cut", "c1", true, 2500000);
        QCOMPARE(preview()["edgePreviewUs"].toDouble(), 2466667.0);  // the last kept frame before the cut
        emit strip->editEdgeDragged("cut", "c1", false, 4500000);
        QCOMPARE(preview()["edgePreviewUs"].toDouble(), 4500000.0);  // the first kept frame after it
        emit strip->cutAdjusted("c1", 3000000, 4500000);
        emit strip->edgeDragFinished();
        QCoreApplication::processEvents();
        QVERIFY(preview()["edgeHeld"].toBool());
        QCOMPARE(preview()["edgePreviewUs"].toDouble(), 4500000.0);
        auto adjusted = edit;
        adjusted.revision = 3;
        adjusted.cuts[0].end = 4500000;
        window.setEdit(adjusted, answer(proposals.last().at(2).toULongLong()));
        QVERIFY(preview()["edgePreviewUs"].isNull());
        QVERIFY(!preview()["edgeHeld"].toBool());
        // A drag that proposes nothing lets go at once.
        emit strip->trimEdgeDragged(true, 1000000);
        emit strip->edgeDragFinished();
        QCoreApplication::processEvents();
        QVERIFY(preview()["edgePreviewUs"].isNull());
        // W9.
        window.drive({{"action", "selectRange"}, {"start", 6000000}, {"end", 7000000}});
        QVERIFY(window.drive({{"action", "cut"}}).code.isEmpty());
        QVERIFY(window.inspect()["selection"].isNull());
        window.setEdit(adjusted, answer(proposals.last().at(2).toULongLong(), "invalid_request"));
        QCOMPARE(window.inspect()["selection"].toObject()["start"].toDouble(), 6000000.0);
        QCOMPARE(window.inspect()["selection"].toObject()["end"].toDouble(), 7000000.0);
    }
    // T15 (window side), W3: a click on the timeline while playing seeks and
    // plays on, a drag there stays paused; a click on the picture while
    // playing pauses and selects nothing.
    void pressesWhilePlaying() {
        QTemporaryDir dir;
        const QString clip = dir.filePath("gops.mkv");
        QVERIFY(test::gopclip::write(clip, 3, 320, 180));
        auto o = options();
        o.workingClip = clip;
        o.durationUs = 3000000;
        o.canvas = QSize(320, 180);
        ReviewWindow window(o, config());
        window.show();
        QVERIFY(putgd::test::waitForWindowExposed(&window));
        RecordingEdit edit;
        edit.revision = 2;
        edit.masks = {mask("m1", "solid", 0, 3000000)};
        edit.masks[0].rect = QRect(0, 0, 320, 180);
        window.setEdit(edit);
        auto* strip = timeline(window);
        const auto playing = [&] { return window.inspect()["playing"].toBool(); };
        QVERIFY(window.drive({{"action", "play"}}).code.isEmpty());
        const QPoint middle = strip->stripRect().center();
        QTest::mouseClick(strip, Qt::LeftButton, {}, middle);
        PUTGD_TRY_VERIFY(playing());
        QVERIFY(std::abs(window.inspect()["playheadUs"].toDouble() - double(strip->timeAt(middle.x()))) < 400000);
        // R18t: the strip click kept the (new, so selected) cover selected.
        QCOMPARE(window.inspect()["selectedMaskId"].toString(), QString("m1"));
        window.drive({{"action", "clearSelection"}});
        auto* view = window.findChild<FrameView*>("frame");
        QTest::mouseClick(view, Qt::LeftButton, {}, view->imageRect().center());
        QVERIFY(!playing());
        QVERIFY(window.inspect()["selectedMaskId"].isNull());
    }
    // W17: Enter in a panel time field commits and hands the keys back to the
    // timeline; Esc on changed text reverts it first.
    void panelFieldsHandTheKeysBack() {
        ReviewWindow window(options(), config());
        QSignalSpy proposals(&window, &ReviewWindow::editProposed);
        window.show();
        QVERIFY(putgd::test::waitForWindowExposed(&window));
        window.activateWindow();
        RecordingEdit edit;
        edit.revision = 2;
        edit.masks = {mask("m1", "solid", 6000000, 8000000)};
        window.setEdit(edit);
        window.drive({{"action", "selectMask"}, {"maskId", "m1"}});
        auto* field = window.findChild<QLineEdit*>("reviewPanelShowsFromTime");
        QVERIFY(field);
        field->setFocus();
        field->setText("0:05.0");
        QTest::keyClick(field, Qt::Key_Escape);
        QCOMPARE(field->text(), QString("0:06.0"));
        QVERIFY(field->hasFocus());
        field->setText("0:05.0");
        QTest::keyClick(field, Qt::Key_Return);
        QCOMPARE(proposals.size(), 1);
        PUTGD_TRY_VERIFY(timeline(window)->hasFocus());
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
        // The queued end must not move the playhead once paused, however the events drain.
        PUTGD_COMPARE_STEADY(window.inspect()["playheadUs"].toDouble(), 0.0, 200);
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
