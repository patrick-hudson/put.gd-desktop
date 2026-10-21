// Offscreen widget contract for the review window (#283): the video takes the
// window, one timeline sets the trim and mask times, mask controls show only
// when they apply, and recording.review drives the same controls. Not desktop
// proof; the lab captures are in the docs.
#include "record/ReviewWindow.h"
#include "ui/Theme.h"
#include <QApplication>
#include <QComboBox>
#include <QFileDialog>
#include <QPushButton>
#include <QTemporaryDir>
#include <QMouseEvent>
#include <QScreen>
#include <QShortcut>
#include <QSignalSpy>
#include <QSpinBox>
#include <QTest>
#include <QToolButton>
using namespace putgd;
using namespace putgd::record;

namespace {
constexpr qint64 duration = 12000000;
ReviewWindow::Options options() {
    ReviewWindow::Options o;
    o.workingClip = QStringLiteral("/nonexistent/putgd-review-test.mkv");
    o.durationUs = duration;
    o.canvas = QSize(1280, 720);
    o.formats = QStringList{"mp4", "webm", "gif"};
    o.codecs = QJsonObject{{"mp4", QJsonArray{"h264", "h265"}}, {"webm", QJsonArray{"vp9"}}};
    return o;
}
RecordingConfig config() { RecordingConfig c; c.format = "mp4"; c.codec = "h264"; c.fps = 30; return c; }
RecordingMask mask(const QString& id, const QString& kind, qint64 start, qint64 end) {
    RecordingMask m;
    m.id = id;
    m.kind = kind;
    m.rect = QRect(100, 80, 320, 120);
    m.start = start;
    m.end = end;
    return m;
}
RecordingEdit twoMasks(quint64 revision = 3) {
    RecordingEdit e;
    e.revision = revision;
    e.masks = {mask("m-blur", "blur", 1000000, 6000000), mask("m-solid", "solid", 4000000, 10000000)};
    return e;
}
// The last proposal's single command.
QJsonObject proposed(const QSignalSpy& spy) { return spy.last().at(1).toJsonArray().first().toObject(); }
void drag(QWidget* w, QPoint from, QPoint to) {
    QTest::mousePress(w, Qt::LeftButton, {}, from);
    QMouseEvent move(QEvent::MouseMove, QPointF(to), w->mapToGlobal(QPointF(to)), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(w, &move);
    QTest::mouseRelease(w, Qt::LeftButton, {}, to);
}
}

class ReviewWindowTest : public QObject {
    Q_OBJECT
    // One pixel of the timeline is duration / track width; edges land within two.
    static qint64 slack(const ReviewTimeline& t) { return 2 * duration / t.trackRect().width() + 1; }
    static ReviewTimeline* timeline(ReviewWindow& w) { return w.findChild<ReviewTimeline*>("timeline"); }
private slots:
    void defaultSizeFitsAndTheVideoTakesMostOfTheWindow() {
        ReviewWindow window(options(), config());
        // The one-monitor fixture is 1366×768: the default fits with room for a panel and title bar.
        QCOMPARE(QGuiApplication::primaryScreen()->geometry().size(), QSize(1366, 768));
        QVERIFY(ReviewWindow::defaultSize().width() <= 1366 - 24 && ReviewWindow::defaultSize().height() <= 768 - 120);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QCOMPARE(window.size(), ReviewWindow::defaultSize());
        auto state = window.inspect();
        // Before #283 the frame had about 30% of a 900×700 window; now most of it.
        QVERIFY2(state["video"].toObject()["share"].toDouble() >= 0.6, qPrintable(QString::number(state["video"].toObject()["share"].toDouble())));
        window.setEdit(twoMasks());
        QApplication::processEvents();
        state = window.inspect();
        QCOMPARE(state["timeline"].toObject()["lanes"].toInt(), 2);
        QVERIFY2(state["video"].toObject()["share"].toDouble() >= 0.6, qPrintable(QString::number(state["video"].toObject()["share"].toDouble())));
        // The minimum leaves the frame usable and fits a small laptop screen.
        QVERIFY(window.minimumSizeHint().width() <= 760 && window.minimumSizeHint().height() <= 420);
        // The video scales with the window.
        const auto before = state["video"].toObject()["rect"].toArray();
        window.resize(1280, 760);
        QTRY_VERIFY(window.inspect()["video"].toObject()["rect"].toArray()[2].toInt() > before[2].toInt());
    }
    void largeTextStillFitsA1366x768Desktop() {
        const auto font = QApplication::font();
        auto large = font;
        if (font.pointSizeF() > 0) large.setPointSizeF(font.pointSizeF() * 1.5);
        else large.setPixelSize(font.pixelSize() * 3 / 2);
        QApplication::setFont(large);
        {
            ReviewWindow window(options(), config());
            window.setEdit(twoMasks());
            window.show();
            QVERIFY(QTest::qWaitForWindowExposed(&window));
            const auto minimum = window.minimumSizeHint();
            QVERIFY2(minimum.width() <= 1366 - 24 && minimum.height() <= 768 - 120, qPrintable(QStringLiteral("%1x%2").arg(minimum.width()).arg(minimum.height())));
            QVERIFY(window.width() <= 1366 && window.height() <= 768);
            QVERIFY2(window.inspect()["video"].toObject()["share"].toDouble() >= 0.45, qPrintable(QString::number(window.inspect()["video"].toObject()["share"].toDouble())));
            // Every control is inside the window.
            for (auto* w : window.findChildren<QWidget*>()) if (w->isVisible() && w->isWindow() == false) QVERIFY2(window.rect().contains(QRect(w->mapTo(&window, QPoint()), w->size())), qPrintable(w->accessibleName() + w->objectName()));
        }
        QApplication::setFont(font);
    }
    void maskControlsShowOnlyWhenTheyApply() {
        ReviewWindow window(options(), config());
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        auto controls = [&] { return window.inspect()["maskControls"].toObject(); };
        QCOMPARE(controls(), QJsonObject({{"list", false}, {"times", false}, {"strength", false}, {"remove", false}}));
        // Two masks arrive (the last added is selected): the list, times and Remove; a cover has no strength.
        window.setEdit(twoMasks());
        QCOMPARE(window.inspect()["selectedMaskId"].toString(), QString("m-solid"));
        QCOMPARE(controls(), QJsonObject({{"list", true}, {"times", true}, {"strength", false}, {"remove", true}}));
        auto* list = window.findChild<QComboBox*>("masks");
        QVERIFY(list);
        QCOMPARE(list->count(), 2);
        QCOMPARE(list->itemText(0), QString("Blur 1"));
        list->setCurrentIndex(0);
        QCOMPARE(window.inspect()["selectedMaskId"].toString(), QString("m-blur"));
        QCOMPARE(controls()["strength"].toBool(), true);
        // A click on a mask's bar on the timeline selects it.
        auto* t = timeline(window);
        QTest::mouseClick(t, Qt::LeftButton, {}, t->maskRect("m-solid").center());
        QCOMPARE(window.inspect()["selectedMaskId"].toString(), QString("m-solid"));
        // A click on the frame outside every mask clears the selection; the list stays.
        auto* frame = window.findChild<FrameView*>();
        QTest::mouseClick(frame, Qt::LeftButton, {}, frame->imageRect().bottomRight() - QPoint(4, 4));
        QVERIFY(window.inspect()["selectedMaskId"].isNull());
        QCOMPARE(controls(), QJsonObject({{"list", true}, {"times", false}, {"strength", false}, {"remove", false}}));
        // Later edits keep "nothing selected"; removing every mask hides the group.
        auto edit = twoMasks(4);
        edit.trimStart = 2000000;
        window.setEdit(edit);
        QVERIFY(window.inspect()["selectedMaskId"].isNull());
        RecordingEdit empty;
        empty.revision = 5;
        window.setEdit(empty);
        QCOMPARE(controls()["list"].toBool(), false);
    }
    void timelineSetsTrimAndMaskTimes() {
        ReviewWindow window(options(), config());
        QSignalSpy proposals(&window, &ReviewWindow::editProposed);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        window.activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(&window));
        auto* t = timeline(window);
        QVERIFY(t);
        const int y = t->trackRect().center().y();
        // A click on the track moves the playhead; I sets the trim start there.
        QTest::mouseClick(t, Qt::LeftButton, {}, QPoint(t->xFor(3000000), y));
        QVERIFY(std::abs(t->playhead() - 3000000) <= slack(*t));
        QTest::keyClick(t, Qt::Key_I);
        QCOMPARE(proposals.size(), 1);
        QCOMPARE(proposed(proposals)["op"].toString(), QString("setTrim"));
        QCOMPARE(qint64(proposed(proposals)["start"].toDouble()), t->playhead());
        QVERIFY(proposed(proposals)["end"].isNull());
        // O sets the trim end at the playhead.
        QTest::mouseClick(t, Qt::LeftButton, {}, QPoint(t->xFor(9000000), y));
        QTest::keyClick(t, Qt::Key_O);
        QCOMPARE(proposals.size(), 2);
        QCOMPARE(qint64(proposed(proposals)["end"].toDouble()), t->playhead());
        // The resident's answer shows on the timeline; dragging its start handle retimes it.
        RecordingEdit edit;
        edit.revision = 3;
        edit.trimStart = 3000000;
        edit.trimEnd = 9000000;
        window.setEdit(edit);
        drag(t, QPoint(t->xFor(3000000), y), QPoint(t->xFor(5000000), y));
        QCOMPARE(proposals.size(), 3);
        QCOMPARE(proposals.last().at(0).toULongLong(), quint64(3));
        QVERIFY(std::abs(qint64(proposed(proposals)["start"].toDouble()) - 5000000) <= slack(*t));
        QCOMPARE(proposed(proposals)["end"].toDouble(), 9000000.0);
        // Dragging the end handle past the clip end leaves the end open.
        drag(t, QPoint(t->xFor(9000000), y), QPoint(t->width() + 40, y));
        QVERIFY(proposed(proposals)["end"].isNull());
        // A selected mask: drag its end on its lane, then Shift+I and Delete.
        edit.revision = 4;
        edit.masks = {mask("m-blur", "blur", 4000000, 8000000)};
        window.setEdit(edit);
        QCOMPARE(window.inspect()["selectedMaskId"].toString(), QString("m-blur"));
        const auto bar = t->maskRect("m-blur");
        drag(t, QPoint(bar.left() + bar.width(), bar.center().y()), QPoint(t->xFor(10000000), bar.center().y()));
        QCOMPARE(proposed(proposals)["op"].toString(), QString("updateMask"));
        QCOMPARE(proposed(proposals)["maskId"].toString(), QString("m-blur"));
        QCOMPARE(proposed(proposals)["mask"].toObject()["start"].toDouble(), 4000000.0);
        QVERIFY(std::abs(qint64(proposed(proposals)["mask"].toObject()["end"].toDouble()) - 10000000) <= slack(*t));
        QTest::mouseClick(t, Qt::LeftButton, {}, QPoint(t->xFor(5000000), y));
        QTest::keyClick(t, Qt::Key_I, Qt::ShiftModifier);
        QCOMPARE(qint64(proposed(proposals)["mask"].toObject()["start"].toDouble()), t->playhead());
        QCOMPARE(proposed(proposals)["mask"].toObject()["end"].toDouble(), 8000000.0);
        QTest::keyClick(t, Qt::Key_O, Qt::ShiftModifier);
        QCOMPARE(qint64(proposed(proposals)["mask"].toObject()["end"].toDouble()), t->playhead());
        QTest::keyClick(t, Qt::Key_Delete);
        QCOMPARE(proposed(proposals), QJsonObject({{"op", "removeMask"}, {"maskId", "m-blur"}}));
        // Arrows step the playhead; Home goes to the trim start.
        const auto at = t->playhead();
        QTest::keyClick(t, Qt::Key_Right);
        QCOMPARE(t->playhead(), at + 100000);
        QTest::keyClick(t, Qt::Key_Home);
        QCOMPARE(t->playhead(), qint64(3000000));
    }
    void keyboardAndNamedIconControlsInBothSchemes() {
        for (const auto scheme : {Theme::Scheme::Dark, Theme::Scheme::Light}) {
            Theme::setScheme(scheme);
            ReviewWindow window(options(), config());
            window.setEdit(twoMasks());
            window.show();
            QVERIFY(QTest::qWaitForWindowExposed(&window));
            // No two shortcuts share a key; ambiguous shortcuts never fire.
            QList<QKeySequence> keys;
            for (auto* s : window.findChildren<QShortcut*>()) { QVERIFY2(!keys.contains(s->key()), qPrintable(s->key().toString())); keys.append(s->key()); }
            for (const auto& key : {QKeySequence(Qt::Key_Space), QKeySequence(Qt::Key_I), QKeySequence(Qt::Key_O), QKeySequence(Qt::SHIFT | Qt::Key_I), QKeySequence(Qt::SHIFT | Qt::Key_O), QKeySequence(Qt::Key_Delete)}) QVERIFY2(keys.contains(key), qPrintable(key.toString()));
            // The tools are icon buttons with a name and a tooltip.
            const auto tools = window.findChildren<QToolButton*>();
            QVERIFY(tools.size() >= 9);
            for (auto* b : tools) {
                QVERIFY2(!b->icon().isNull() && b->toolButtonStyle() == Qt::ToolButtonIconOnly, qPrintable(b->accessibleName()));
                QVERIFY2(!b->accessibleName().isEmpty() && !b->toolTip().isEmpty(), qPrintable(b->objectName()));
            }
            // The window background is the scheme's.
            const auto image = window.grab().toImage();
            QCOMPARE(image.pixelColor(2, 2).rgb(), Theme::palette(scheme).bg.rgb());
            QCOMPARE(window.inspect()["scheme"].toString(), Theme::name(scheme));
            // Space starts the preview (the clip is missing, so it stops again).
            window.activateWindow();
            QVERIFY(QTest::qWaitForWindowActive(&window));
            QTest::keyClick(timeline(window), Qt::Key_Space);
            QVERIFY(window.inspect()["playing"].toBool());
            QTRY_VERIFY(!window.inspect()["playing"].toBool());
        }
        Theme::setScheme(Theme::Scheme::Dark);
    }
    void reviewMethodDrivesTheSameControls() {
        ReviewWindow window(options(), config());
        QSignalSpy proposals(&window, &ReviewWindow::editProposed);
        QSignalSpy configs(&window, &ReviewWindow::configChanged);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        auto drive = [&](const QJsonObject& params) { return window.drive(params); };
        auto seek = drive({{"action", "seek"}, {"time", 2000000}});
        QVERIFY(seek.code.isEmpty());
        QCOMPARE(seek.state["playheadUs"].toDouble(), 2000000.0);
        QCOMPARE(drive({{"action", "seek"}, {"time", double(duration)}}).code, QString("invalid_request"));
        QVERIFY(drive({{"action", "setIn"}}).code.isEmpty());
        QCOMPARE(proposed(proposals), QJsonObject({{"op", "setTrim"}, {"start", 2000000.0}, {"end", QJsonValue(QJsonValue::Null)}}));
        // setOut at or before the trim start is refused like the control (and shown).
        RecordingEdit edit;
        edit.revision = 2;
        edit.trimStart = 2000000;
        window.setEdit(edit);
        const auto refused = drive({{"action", "setOut"}});
        QCOMPARE(refused.code, QString("invalid_request"));
        QCOMPARE(window.inspect()["status"].toString(), refused.message);
        QVERIFY(drive({{"action", "addMask"}, {"kind", "blur"}, {"rect", QJsonArray{10, 10, 200, 100}}}).code.isEmpty());
        const auto added = proposed(proposals);
        QCOMPARE(added["op"].toString(), QString("addMask"));
        QCOMPARE(added["mask"].toObject()["start"].toDouble(), 2000000.0);
        QCOMPARE(added["mask"].toObject()["end"].toDouble(), double(duration));
        QCOMPARE(added["mask"].toObject()["strength"].toInt(), 16);
        QCOMPARE(drive({{"action", "addMask"}, {"kind", "blur"}, {"rect", QJsonArray{1200, 10, 200, 100}}}).code, QString("invalid_request"));
        QCOMPARE(drive({{"action", "selectMask"}, {"maskId", "nope"}}).code, QString("invalid_request"));
        QCOMPARE(drive({{"action", "strength"}, {"strength", 20}}).code, QString("invalid_request"));
        edit.revision = 3;
        edit.masks = {mask("m-blur", "blur", 2000000, duration)};
        window.setEdit(edit);
        auto selected = drive({{"action", "selectMask"}, {"maskId", "m-blur"}});
        QCOMPARE(selected.state["maskControls"].toObject()["strength"].toBool(), true);
        QVERIFY(drive({{"action", "strength"}, {"strength", 30}}).code.isEmpty());
        QCOMPARE(proposed(proposals)["mask"].toObject()["strength"].toInt(), 30);
        QVERIFY(drive({{"action", "seek"}, {"time", 7000000}}).code.isEmpty());
        QVERIFY(drive({{"action", "setMaskOut"}}).code.isEmpty());
        QCOMPARE(proposed(proposals)["mask"].toObject()["end"].toDouble(), 7000000.0);
        QVERIFY(drive({{"action", "removeMask"}}).code.isEmpty());
        QCOMPARE(proposed(proposals), QJsonObject({{"op", "removeMask"}, {"maskId", "m-blur"}}));
        QVERIFY(drive({{"action", "speed"}, {"speed", 2}}).state["speed"].toDouble() == 2.0);
        QCOMPARE(drive({{"action", "speed"}, {"speed", 3}}).code, QString("invalid_request"));
        QVERIFY(drive({{"action", "format"}, {"format", "webm"}, {"codec", "vp9"}}).code.isEmpty());
        QCOMPARE(configs.size(), 1);
        QCOMPARE(drive({{"action", "format"}, {"format", "webp"}, {"codec", ""}}).code, QString("invalid_request"));
        QCOMPARE(drive({{"action", "explode"}}).code, QString("invalid_request"));
        // While an export runs only inspect answers.
        const auto count = proposals.size();
        window.setExporting(true, "Exporting MP4…");
        QCOMPARE(drive({{"action", "setIn"}}).code, QString("recording_state"));
        QVERIFY(drive({{"action", "inspect"}}).state["exporting"].toBool());
        QCOMPARE(proposals.size(), count);
    }
    // #259: Save copy… opens the shared chooser; this test never asks a portal.
    void saveCopyDialogIsDrivenAndSavesTheChosenPath() {
        qputenv("PUTGD_FILE_DIALOG", "fallback");
        QTemporaryDir folder;
        QVERIFY(folder.isValid());
        ReviewWindow window(options(), config());
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QSignalSpy saved(&window, &ReviewWindow::saveCopyRequested);
        auto opened = window.drive({{"action", "saveCopyDialog"}});
        QVERIFY2(opened.code.isEmpty(), qPrintable(opened.message));
        auto dialog = opened.state["fileDialog"].toObject();
        QVERIFY(dialog["open"].toBool());
        QCOMPARE(dialog["backend"].toString(), QString("fallback"));
        QCOMPARE(dialog["fallbackReason"].toString(), QString("disabled"));
        QCOMPARE(dialog["mode"].toString(), QString("save"));
        // Other controls wait for the dialog, as they did behind QFileDialog::exec.
        QCOMPARE(window.drive({{"action", "setIn"}}).code, QString("recording_state"));
        QCOMPARE(window.drive({{"action", "saveCopyDialog"}}).code, QString("recording_state"));
        auto closed = window.drive({{"action", "closeFileDialog"}});
        QVERIFY(!closed.state["fileDialog"].toObject()["open"].toBool());
        QCOMPARE(closed.state["fileDialog"].toObject()["last"].toObject()["status"].toString(), QString("cancelled"));
        QCOMPARE(saved.size(), 0);
        // The button opens the same dialog; a name without the extension gets it.
        QPushButton* save = nullptr;
        for (auto* button : window.findChildren<QPushButton*>()) if (button->text() == QString::fromUtf8("Save copy…")) save = button;
        QVERIFY(save);
        save->click();
        QFileDialog* fallback = nullptr;
        QTRY_VERIFY((fallback = window.findChild<QFileDialog*>("putgdFileDialog")) && fallback->isVisible());
        QCOMPARE(fallback->windowTitle(), QString("Save a copy"));
        QCOMPARE(fallback->nameFilters(), QStringList{"MP4 (*.mp4)"});
        fallback->selectFile(folder.filePath("chosen"));
        QMetaObject::invokeMethod(fallback, "accept");
        QCOMPARE(saved.size(), 1);
        QCOMPARE(saved.first().first().toString(), folder.filePath("chosen.mp4"));
        // Starting an export closes an open chooser.
        QVERIFY(window.drive({{"action", "saveCopyDialog"}}).code.isEmpty());
        window.setExporting(true, "Exporting MP4…");
        QVERIFY(!window.inspect()["fileDialog"].toObject()["open"].toBool());
        qunsetenv("PUTGD_FILE_DIALOG");
    }
};

QTEST_MAIN(ReviewWindowTest)
#include "test_review_window.moc"
