// Offscreen widget contract for the review window (#283): the video takes the
// window, one timeline sets the trim and mask times, mask controls show only
// when they apply, and recording.review drives the same controls. Not desktop
// proof; the lab captures are in the docs.
#include "record/ReviewWindow.h"
#include "record/Overlays.h"
#include "ui/RegionOverlay.h"
#include <QAccessible>
#include <QScrollBar>
#include "ui/Graphite.h"
#include "ui/Theme.h"
#include <QLabel>
#include <QApplication>
#include <QClipboard>
#include <QWheelEvent>
#include "platform/HostOpen.h"
#include <QFontInfo>
#include <QComboBox>
#include <QFileDialog>
#include <QFile>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QTemporaryDir>
#include <QMouseEvent>
#include <QScreen>
#include <QScopeGuard>
#include <QShortcut>
#include <QSignalSpy>
#include <QSpinBox>
#include <QTest>
#include <QToolButton>
#include "fixtures/Waits.h"
using namespace putgd;
using namespace putgd::record;

namespace {
constexpr qint64 duration = 12000000;
ReviewWindow::Options options() {
    ReviewWindow::Options o;
    o.workingClip = QStringLiteral("/nonexistent/putgd-review-test.mkv");
    o.durationUs = duration;
    o.destination = "Project archive";
    o.sharingMode = "share";
    o.uploadSummary = "Upload to Project archive · Shared link";
    o.canvas = QSize(1280, 720);
    o.formats = QStringList{"mp4", "webm", "gif"};
    o.codecs = QJsonObject{{"mp4", QJsonArray{"h264", "h265"}}, {"webm", QJsonArray{"vp9"}}};
    return o;
}
RecordingConfig config() { RecordingConfig c; c.format = "mp4"; c.codec = "h264"; c.fps = 30; return c; }
QPushButton* buttonNamed(QWidget& window, const QString& text) {
    for (auto* button : window.findChildren<QPushButton*>()) if (button->text() == text && button->isVisible()) return button;
    return nullptr;
}
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
    void stableMaskNamesSurviveRemoval() {
        ReviewWindow window(options(), config());
        window.setEdit(twoMasks()); window.show();
        QVERIFY(putgd::test::waitForWindowExposed(&window));
        QCOMPARE(window.inspect()["selectedMaskName"].toString(), QString("Redaction 2"));
        auto edit = twoMasks(4); edit.masks.removeFirst(); window.setEdit(edit);
        QCOMPARE(window.findChild<QComboBox*>("masks")->currentText(), QString("Redaction 2"));
        QCOMPARE(window.inspect()["masks"].toArray().first().toObject()["name"].toString(), QString("Redaction 2"));
        QVERIFY(window.findChild<FrameView*>()->accessibleDescription().startsWith("Redaction 2 selected"));
        edit.revision++; edit.masks.append(mask("third", "blur", 0, duration)); window.setEdit(edit);
        QCOMPARE(window.inspect()["selectedMaskName"].toString(), QString("Blur 3"));
    }
    void maskNamesKeepTheirMeasuredWidth() {
        ReviewWindow window(options(), config());
        RecordingEdit edit;
        for (int i = 0; i < 12; ++i) edit.masks.append(mask(QString::number(i), "solid", 0, duration));
        window.setEdit(edit); window.show();
        QVERIFY(putgd::test::waitForWindowExposed(&window));
        auto* chooser = window.findChild<QComboBox*>("masks");
        QVERIFY(chooser);
        for (int i = 0; i < chooser->count(); ++i)
            QVERIFY(chooser->width() >= chooser->fontMetrics().horizontalAdvance(chooser->itemText(i)) + Graphite::Metric::splitZone + 18);
        auto* metadata = window.findChild<QLabel*>("maskActiveInterval");
        QVERIFY(metadata && metadata->isVisible());
        QVERIFY(metadata->mapTo(&window, QPoint()).y() >= window.findChild<FrameView*>()->geometry().bottom());
        auto* audio = window.findChild<QLabel*>("exportAudioSummary");
        QVERIFY(audio && audio->isVisible());
        QVERIFY(audio->text().startsWith("Silent preview · "));
    }
    void stackedToolsKeepTheirScopeAndTrimLabels() {
        ReviewWindow window(options(), config()); window.setEdit(twoMasks()); window.show();
        QVERIFY(putgd::test::waitForWindowExposed(&window));
        window.resize(window.minimumSizeHint().width(), 680); QApplication::processEvents();
        auto* trim = window.findChild<QWidget*>("trimControls");
        auto* masks = window.findChild<QWidget*>("maskControls");
        auto* interval = window.findChild<QLabel*>("maskActiveInterval");
        QVERIFY(masks->isAncestorOf(interval)); QVERIFY(!trim->isAncestorOf(interval));
        QVERIFY(masks->y() > trim->geometry().bottom());
        for (const auto& name : {QString("Set trim start"), QString("Set trim end"), QString("Reset trim")}) {
            QToolButton* found = nullptr;
            for (auto* button : trim->findChildren<QToolButton*>()) if (button->accessibleName() == name) found = button;
            QVERIFY(found); QCOMPARE(found->toolButtonStyle(), Qt::ToolButtonTextBesideIcon); QVERIFY(!found->text().isEmpty());
        }
        QVERIFY(trim->findChild<QLabel*>("muted")->text().startsWith("Kept: "));
        QToolButton *redact = nullptr, *blur = nullptr;
        for (auto* button : masks->findChildren<QToolButton*>()) {
            if (button->accessibleName() == "Redact") redact = button;
            if (button->accessibleName() == "Blur") blur = button;
        }
        QVERIFY(redact && blur); QVERIFY(redact->x() < blur->x());
    }
    void bottomArrangementStaysTogetherAcrossReviewWidths() {
        ReviewWindow window(options(), config()); window.setEdit(twoMasks()); window.show();
        QVERIFY(putgd::test::waitForWindowExposed(&window));
        auto* trim = window.findChild<QWidget*>("trimControls");
        auto* masks = window.findChild<QWidget*>("maskControls");
        auto* track = timeline(window);
        QList<int> offsets;
        for (int width : {720, 820, 1060}) {
            window.resize(width, 720); QApplication::processEvents();
            const QList<int> current{trim->y() - track->y(), masks->y() - track->y(), track->height()};
            if (offsets.isEmpty()) offsets = current;
            else QCOMPARE(current, offsets);
            QVERIFY(!track->maskRect("m-blur").isEmpty());
            QVERIFY(!track->maskRect("m-solid").isEmpty());
            auto* interval = window.findChild<QLabel*>("maskActiveInterval");
            auto* footer = window.findChild<QWidget*>("reviewFooter");
            QVERIFY(interval->mapTo(&window, interval->rect().bottomLeft()).y() < footer->y());
        }
    }
    void closedChoicesIgnoreWheel() {
        ReviewWindow window(options(), config()); window.setEdit(twoMasks()); window.show();
        QVERIFY(putgd::test::waitForWindowExposed(&window));
        for (auto* combo : window.findChildren<QComboBox*>()) {
            const int index = combo->currentIndex();
            QWheelEvent event(QPointF(5, 5), combo->mapToGlobal(QPoint(5, 5)), {}, QPoint(0, -120), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
            QApplication::sendEvent(combo, &event);
            QCOMPARE(combo->currentIndex(), index);
            QVERIFY(!event.isAccepted());
            QTest::keyClick(combo, Qt::Key_Down);
            QVERIFY(combo->currentIndex() != index || index == combo->count() - 1);
        }
    }
    void savedCopyIdentifiesFileAndBecomesEarlier() {
        ReviewWindow window(options(), config()); window.setEdit(twoMasks()); window.show();
        QVERIFY(putgd::test::waitForWindowExposed(&window));
        QCOMPARE(window.drive({{"action", "copyPath"}}).code, QString("invalid_request"));
        QTemporaryDir dir; QVERIFY(dir.isValid());
        const QString saved = dir.filePath("review copy.mp4");
        QFile file(saved); QVERIFY(file.open(QIODevice::WriteOnly)); file.close();
        window.setSavedCopy(saved, 3, config());
        QVERIFY(window.inspect()["status"].toString().contains("Saved review copy.mp4"));
        QVERIFY(window.inspect()["savedCopy"].toObject()["current"].toBool());
        QVERIFY(window.drive({{"action", "copyPath"}}).code.isEmpty());
        QCOMPARE(QApplication::clipboard()->text(), saved);
        QUrl opened; host::setTestOpener([&](const QUrl& url) { opened = url; });
        const auto restore = qScopeGuard([] { host::setTestOpener({}); });
        QVERIFY(window.drive({{"action", "showInFolder"}}).code.isEmpty());
        QCOMPARE(opened.toLocalFile(), dir.path());
        window.setEdit(twoMasks(4));
        QVERIFY(window.inspect()["status"].toString().contains("Earlier copy saved: review copy.mp4"));
        QVERIFY(!window.inspect()["savedCopy"].toObject()["current"].toBool());
        window.setSavedCopy("/tmp/new.mp4", 4, config());
        window.drive({{"action", "format"}, {"format", "webm"}, {"codec", "vp9"}});
        QVERIFY(!window.inspect()["savedCopy"].toObject()["current"].toBool());
    }
    void resizeCornerUsesFullTargetAndNearestMask() {
        FrameView frame; frame.resize(640, 360); frame.setCanvas({640, 360});
        auto one = mask("one", "solid", 0, duration); one.rect = QRect(20, 20, 100, 100);
        auto two = one; two.id = "two"; two.rect.translate(20, 0);
        frame.setMasks({one, two}, "one"); frame.show();
        QSignalSpy moved(&frame, &FrameView::maskMoved);
        // Twelve pixels beyond the painted corner is still in its 32 px target.
        drag(&frame, QPoint(148, 116), QPoint(158, 126));
        QCOMPARE(moved.size(), 1);
        QCOMPARE(moved.first()[0].toString(), QString("two"));
        QCOMPARE(moved.first()[1].toRect().size(), QSize(110, 110));
    }
    // put.gd's own frame everywhere below, also offscreen: the frame share,
    // minimum size and fit include the compact header's pixels.
    void initTestCase() { Graphite::WindowHeader::setCustomFrameOverride(1); }
    void cleanupTestCase() { Graphite::WindowHeader::setCustomFrameOverride(0); }
    void liveSelectorsSupportKeyboard_data() {
        QTest::addColumn<bool>("recording");
        QTest::newRow("screenshot") << false;
        QTest::newRow("recording") << true;
    }
    void liveSelectorsSupportKeyboard() {
        QFETCH(bool, recording);
        for (int mode = 0; mode < 5; ++mode) {
            const auto screen = QGuiApplication::primaryScreen()->geometry();
            CaptureFrame frame{"fixture", screen, screen, 1, 0, {}};
            RegionOverlay screenshot({frame});
            SelectionOverlay record;
            QRectF selected;
            int cancelled = 0, accepted = 0;
            connect(&screenshot, &RegionSelector::selected, this, [&](const QRectF& r) { selected = r; ++accepted; });
            connect(&record, &SelectionOverlay::selected, this, [&](const QRect& r) { selected = r; ++accepted; });
            connect(&screenshot, &RegionSelector::cancelled, this, [&](const Error&) { ++cancelled; });
            connect(&record, &SelectionOverlay::cancelled, this, [&](const Error&) { ++cancelled; });
            if (recording) record.start(); else screenshot.start();
            QWidget* surface = nullptr;
            for (auto* w : QApplication::topLevelWidgets())
                if (w->isVisible() && w->objectName() == (recording ? "putgd-record-select" : "putgd-region-overlay")) surface = w;
            QVERIFY(surface);
            auto* accessible = QAccessible::queryAccessibleInterface(surface);
            QVERIFY(accessible);
            QCOMPARE(accessible->text(QAccessible::Description), surface->accessibleDescription());
            QVERIFY(surface->accessibleDescription().contains("Drag to select"));
            QVERIFY(surface->accessibleDescription().contains(recording ? "Enter chooses this region" : "Enter to capture"));
            if (mode == 0) {
                QTest::keyClick(surface, Qt::Key_Return);
                QCOMPARE(selected, QRectF(screen));
            } else if (mode == 4) {
                drag(surface, {100, 100}, {300, 280});
                QCOMPARE(selected, QRectF(QPointF(screen.topLeft()) + QPointF(100, 100), QSizeF(200, 180)));
            } else {
                QTest::keyClick(surface, Qt::Key_Right);
                QVERIFY(surface->accessibleDescription().contains(recording ? "Enter chooses this region" : "Enter to capture"));
                QVERIFY(!surface->accessibleDescription().contains("logical pixels"));
                QRectF expected(QPointF(screen.topLeft()) + QPointF(screen.width() / 4.0, screen.height() / 4.0), QSizeF(screen.size()) / 2);
                QVERIFY(accessible->text(QAccessible::Description).contains(QString::number(expected.width())));
                QTest::keyClick(surface, Qt::Key_Right);
                expected.translate(1, 0);
                QTest::keyClick(surface, Qt::Key_Down, Qt::ControlModifier);
                expected.translate(0, 10);
                QTest::keyClick(surface, Qt::Key_Right, Qt::ShiftModifier);
                expected.setWidth(expected.width() + 1);
                QTest::keyClick(surface, Qt::Key_Up, Qt::ShiftModifier | Qt::ControlModifier);
                expected.setHeight(expected.height() - 10);
                if (mode == 1) {
                    QTest::keyClick(surface, Qt::Key_Enter);
                    QCOMPARE(selected, recording ? QRectF(expected.toAlignedRect()) : expected);
                } else if (mode == 2) {
                    QTest::keyClick(surface, Qt::Key_Escape);
                    QCOMPARE(cancelled, 1);
                    QCOMPARE(accepted, 0);
                } else {
                    // Oversized keyboard moves and resizes stay on the active screen.
                    for (int i = 0; i < 200; ++i) QTest::keyClick(surface, Qt::Key_Right, Qt::ControlModifier);
                    for (int i = 0; i < 200; ++i) QTest::keyClick(surface, Qt::Key_Down, Qt::ShiftModifier | Qt::ControlModifier);
                    QTest::keyClick(surface, Qt::Key_Return);
                    QVERIFY(QRectF(screen).contains(selected));
                    QVERIFY(selected.width() > 0 && selected.height() > 0);
                }
            }
            QVERIFY(!surface->isVisible());
            QCOMPARE(accepted + cancelled, 1);
        }
    }
    void screenshotSelectorCyclesSyntheticDisplays() {
        const QList<CaptureFrame> frames{{"Left",QRectF(-800,0,800,600),QRect(-800,0,800,600),1,0,{}},{"Right",QRectF(0,100,320,240),QRect(0,100,320,240),1,0,{}}};
        RegionOverlay selector(frames);selector.start();QSignalSpy selected(&selector,&RegionSelector::selected);
        QWidget* surface=nullptr;
        for(auto* w:QApplication::topLevelWidgets())if(w->isVisible()&&w->objectName()=="putgd-region-overlay"&&w->width()==800)surface=w;
        QVERIFY(surface);
        QTest::keyClick(surface,Qt::Key_Tab);
        QVERIFY(surface->accessibleDescription().contains("Right\n"));
        QTest::keyClick(surface,Qt::Key_Right);
        QTest::keyClick(surface,Qt::Key_Backtab);
        QVERIFY(surface->accessibleDescription().contains("Left · "));
        QTest::keyClick(surface,Qt::Key_Return);QCOMPARE(selected.size(),1);
        QCOMPARE(qvariant_cast<QRectF>(selected.first()[0]),QRectF(-720,60,160,120));
    }
    void accessiblePlayheadAndLiveMaskGeometry() {
        ReviewWindow window(options(), config());
        window.setEdit(twoMasks()); window.show();
        QVERIFY(putgd::test::waitForWindowExposed(&window));
        auto* t = timeline(window);
        auto* accessible = QAccessible::queryAccessibleInterface(t);
        QVERIFY(accessible && accessible->valueInterface());
        auto* value = accessible->valueInterface();
        QCOMPARE(value->minimumValue().toDouble(), 0.0);
        QCOMPARE(value->maximumValue().toDouble(), double(duration - 1) / 1000000);
        QCOMPARE(value->minimumStepSize().toDouble(), 0.1);
        value->setCurrentValue(2.3);
        QCOMPARE(value->currentValue().toDouble(), 2.3);
        QCOMPARE(accessible->text(QAccessible::Value), QString("2.3 seconds of 12 seconds"));
        QCOMPARE(window.inspect()["playheadUs"].toDouble(), 2300000);
        auto* frame = window.findChild<FrameView*>();
        QVERIFY(frame->accessibleDescription().contains("Redaction 2 selected: 320 × 120 at 100, 80"));
        QVERIFY(frame->accessibleDescription().contains("Applies from 0:04.00 to 0:10.00"));
        QTest::keyClick(frame, Qt::Key_Right);
        QVERIFY(frame->accessibleDescription().contains("at 101, 80"));
        frame->commitNudge();
        auto edit = twoMasks(4); edit.masks[1].end = 11000000; window.setEdit(edit);
        QVERIFY(frame->accessibleDescription().contains("to 0:11.00"));
    }
    void concurrentMasksHaveDistinctScrollableLanes() {
        ReviewTimeline t; t.setDuration(duration);
        QList<RecordingMask> masks;
        for (int i = 0; i < 8; ++i) masks.append(mask(QString::number(i), "solid", 1000000, 10000000));
        t.setMasks(masks, "0"); t.resize(600, t.sizeHint().height()); t.show();
        QCOMPARE(t.lanes(), 8);
        auto* scroll = t.findChild<QScrollBar*>("maskLaneScroll");
        QVERIFY(scroll && scroll->isHidden()); QCOMPARE(scroll->maximum(), 6);
        QVERIFY(!t.maskHitRect("0").intersects(t.maskHitRect("1")));
        QVERIFY(t.maskRect("7").isEmpty());
        const int boundedHeight = t.sizeHint().height();
        t.setMasks(masks, "7"); QCOMPARE(scroll->value(), 6);
        QVERIFY(!t.maskRect("7").isEmpty()); QVERIFY(t.maskRect("0").isEmpty());
        QCOMPARE(t.sizeHint().height(), boundedHeight);
        QSignalSpy selected(&t, &ReviewTimeline::maskSelected);
        scroll->setValue(6);
        QTest::mouseClick(&t, Qt::LeftButton, {}, t.maskRect("6").center());
        QCOMPARE(selected.size(), 1); QCOMPARE(selected.first()[0].toString(), QString("6"));
    }
    void previewRecoveryStacksAndMeasuresWrappedText() {
        FrameView view; auto large = view.font(); large.setPixelSize(21); view.setFont(large); view.setCanvas({1280,720}); view.resize(360,320); view.show();
        view.setPreviewError("The decoder could not refresh the selected frame. Try again after closing other applications.");
        auto* label = view.findChild<QLabel*>("previewErrorText");
        auto* retry = view.findChild<QPushButton*>("retryPreview");
        QApplication::processEvents();
        QVERIFY(label && retry);
        QVERIFY(label->height() >= label->heightForWidth(label->width()));
        QVERIFY(retry->y() >= label->y() + label->height());
        view.resize(900,600); QApplication::processEvents();
        QVERIFY(label->height() >= label->heightForWidth(label->width()));
        QVERIFY(retry->x() >= label->x() + label->width());
    }
    void destinationAndMutedPreviewRemainVisible() {
        auto o = options();
        o.audioRecorded = true;
        o.destination = "Project archive for customer support in the European Union";
        o.uploadSummary = "Upload to " + o.destination + " · Shared link";
        ReviewWindow window(o, config());
        window.show();
        QVERIFY(putgd::test::waitForWindowExposed(&window));
        window.resize(760, 420);
        QApplication::processEvents();
        const auto state = window.inspect();
        QCOMPARE(state["destination"].toString(), o.destination);
        QCOMPARE(state["sharingMode"].toString(), QString("share"));
        QCOMPARE(state["uploadSummary"].toString(), o.uploadSummary);
        QVERIFY(state["previewMuted"].toBool());
        auto* target = window.findChild<QLabel*>("uploadDestination");
        auto* upload = window.findChild<QPushButton*>("upload");
        QVERIFY(target && target->isVisible() && target->toolTip() == o.uploadSummary);
        QVERIFY(target->mapTo(&window, target->rect().bottomLeft()).y() < upload->mapTo(&window, QPoint()).y());
        QVERIFY(window.rect().contains(QRect(target->mapTo(&window, QPoint()), target->size())));
        auto* full = window.findChild<QLabel*>("uploadDestinationWrapped");
        QVERIFY(full && !full->isVisible());
        static_cast<Graphite::ElidedLabel*>(target)->reveal();
        QVERIFY(static_cast<Graphite::ElidedLabel*>(target)->fullTextPopup());
        static_cast<Graphite::ElidedLabel*>(target)->fullTextPopup()->close();
        auto* audio = window.findChild<QLabel*>("exportAudioSummary");
        QVERIFY(audio->isVisible());
        QVERIFY(audio->mapTo(&window, QPoint()).y() < upload->mapTo(&window, QPoint()).y());
        QVERIFY(state["audio"].toString().startsWith("Silent preview"));
        QVERIFY(window.findChild<QLabel*>("exportAudioSummary")->isVisible());
    }
    void stoppingKeepsTheClipAndDiscardStillAsks() {
        ControlBar::Options o;
        o.targetLabel = "Region"; o.formats = {"mp4"}; o.codecs = {{"mp4", QJsonArray{"h264"}}};
        ControlBar bar(o, config());
        bar.setState("recording"); bar.show();
        auto* stop = bar.findChild<QPushButton*>("stop");
        auto* discard = bar.findChild<QPushButton*>("cancel");
        QVERIFY(stop && discard);
        QCOMPARE(stop->property("visualRole").toString(), QString("primary"));
        QCOMPARE(discard->property("visualRole").toString(), QString("quiet"));
        QSignalSpy stopped(&bar, &ControlBar::stopRequested), cancelled(&bar, &ControlBar::cancelRequested);
        stop->click(); QCOMPARE(stopped.size(), 1); QCOMPARE(cancelled.size(), 0);
        discard->click(); QVERIFY(bar.confirming()); QCOMPARE(cancelled.size(), 0);
        bar.findChild<QPushButton*>("keepRecording")->click(); QVERIFY(!bar.confirming());
    }
    void timelineTargetsAre32PixelsAndNearestHandleWins() {
        ReviewTimeline t;
        t.setDuration(duration);
        t.setTrim(5000000, 5200000);
        t.setMasks({mask("one", "solid", 1000000, 2000000), mask("two", "solid", 1500000, 1900000)}, "one");
        t.resize(600, t.sizeHint().height());
        t.show();
        const auto first = t.maskHitRect("one"), second = t.maskHitRect("two");
        QCOMPARE(first.height(), 32);
        QCOMPARE(second.height(), 32);
        QVERIFY(!first.intersects(second));
        auto* scroll = t.findChild<QScrollBar*>("maskLaneScroll");
        QVERIFY(scroll->isHidden());
        const auto secondTarget = t.maskHitRect("two");
        QCOMPARE(secondTarget.height(), 32);
        QVERIFY(!t.maskHitRect("one").isEmpty());
        QVERIFY(secondTarget.top() >= 32);
        scroll->setValue(0);
        QVERIFY(first.top() >= 32);
        QSignalSpy trim(&t, &ReviewTimeline::trimChanged);
        drag(&t, QPoint(t.xFor(5200000) + 12, 2), QPoint(t.xFor(7000000), 2));
        QCOMPARE(trim.size(), 1);
        QCOMPARE(trim.last()[0].toLongLong(), 5000000);
        QVERIFY(trim.last()[1].toLongLong() > 6000000);
        drag(&t, QPoint(t.xFor(5200000) + 12, 31), QPoint(t.xFor(7000000), 31));
        QCOMPARE(trim.size(), 2);
        QSignalSpy retimed(&t, &ReviewTimeline::maskRetimed);
        const auto bar = t.maskRect("one");
        drag(&t, QPoint(bar.left() - 12, first.bottom()), QPoint(t.xFor(500000), first.bottom()));
        QCOMPARE(retimed.size(), 1);
        QCOMPARE(retimed.last()[0].toString(), QString("one"));
    }
    void destinationCanBeRevealedByKeyboardAndClick() {
        auto o = options(); o.destination = QString("Long project archive ").repeated(12); o.sharingMode = "private";
        o.uploadSummary = "Private upload to " + o.destination;
        ReviewWindow window(o, config()); window.show();
        QVERIFY(putgd::test::waitForWindowExposed(&window));
        auto* label = window.findChild<Graphite::ElidedLabel*>("uploadDestination");
        QVERIFY(label && label->focusPolicy() & Qt::TabFocus);
        label->setFocus(Qt::TabFocusReason); QTest::keyClick(label, Qt::Key_Return);
        PUTGD_TRY_VERIFY(label->fullTextPopup());
        auto* popup = label->fullTextPopup();
        const auto text = popup->findChild<QLabel*>("elidedLabelFullTextValue")->text();
        QCOMPARE(text, "Destination: " + o.destination + "\nSharing: Private upload");
        auto* value = popup->findChild<QLabel*>("elidedLabelFullTextValue");
        QVERIFY(value->textInteractionFlags() & Qt::TextSelectableByMouse);
        QVERIFY(value->textInteractionFlags() & Qt::TextSelectableByKeyboard);
        popup->close(); QTest::mouseClick(label, Qt::LeftButton); PUTGD_TRY_VERIFY(label->fullTextPopup());
        label->fullTextPopup()->close();
    }
    void compactHeaderIsOneLineNamedForTheWindow() {
        ReviewWindow window(options(), config());
        window.show();
        QVERIFY(putgd::test::waitForWindowExposed(&window));
        auto* header = window.findChild<Graphite::WindowHeader*>("windowHeader");
        QVERIFY(header && header->isVisible());
        QCOMPARE(header->density(), Graphite::WindowHeader::Density::Compact);
        QVERIFY(window.windowFlags().testFlag(Qt::FramelessWindowHint));
        // Its height keeps the full title line and the 32 px control targets.
        QCOMPARE(header->height(), header->compactHeight());
        QVERIFY2(header->height() >= 32 && header->height() <= 40, qPrintable(QString::number(header->height())));
        QCOMPARE(window.inspect()["header"].toObject()["rect"].toArray()[3].toInt(), header->height());
        // One line: the window's name; its draft state stays in the footer.
        auto* name = header->findChild<QWidget*>("documentName");
        QVERIFY(name && name->isVisible());
        QCOMPARE(name->accessibleName(), QStringLiteral("Review recording"));
        auto* state = header->findChild<QLabel*>("documentState");
        QVERIFY(!state || !state->isVisible());
        auto* draft = window.findChild<QLabel*>("draftState");
        QVERIFY(draft && draft->isVisible() && draft->text() == QStringLiteral("Not uploaded"));
        QVERIFY(draft->mapTo(&window, QPoint()).y() > header->height());
        // The shared window controls, each at least a 32 px target.
        for (const auto* name : {"windowMinimize", "windowMaximize", "windowClose"}) {
            auto* button = header->findChild<QToolButton*>(name);
            QVERIFY2(button && button->isVisible(), name);
            QVERIFY(button->width() >= 32 && button->height() >= 32);
            QVERIFY(button->y() >= 0 && button->y() + button->height() <= header->height());
        }
        // Double-click on the band maximizes and restores, as on every put.gd window.
        const QPoint empty(header->width() / 4, header->height() / 2);
        QTest::mouseDClick(header, Qt::LeftButton, {}, empty);
        PUTGD_TRY_VERIFY(window.isMaximized());
        QTest::mouseDClick(header, Qt::LeftButton, {}, empty);
        PUTGD_TRY_VERIFY(!window.isMaximized());
    }
    void desktopDecorationsHideTheCompactHeader() {
        Graphite::WindowHeader::setCustomFrameOverride(-1);
        {
            ReviewWindow window(options(), config());
            window.show();
            QVERIFY(putgd::test::waitForWindowExposed(&window));
            auto* header = window.findChild<Graphite::WindowHeader*>("windowHeader");
            QVERIFY(header && !header->isVisible());
            QVERIFY(!window.windowFlags().testFlag(Qt::FramelessWindowHint));
            QCOMPARE(window.windowTitle(), QStringLiteral("Review recording"));
            QVERIFY(!window.inspect()["header"].toObject()["visible"].toBool());
        }
        Graphite::WindowHeader::setCustomFrameOverride(1);
    }
    void defaultSizeFitsAndTheVideoTakesMostOfTheWindow() {
        ReviewWindow window(options(), config());
        // The one-monitor fixture is 1366×768: the default fits with room for a panel and title bar.
        // Offscreen the fixture is 1366×768; a native run (real compositor) checks
        // the same shares on the desktop's own screen.
        if (QGuiApplication::platformName() == QLatin1String("offscreen")) QCOMPARE(QGuiApplication::primaryScreen()->geometry().size(), QSize(1366, 768));
        QVERIFY(ReviewWindow::defaultSize().width() <= 1366 - 24 && ReviewWindow::defaultSize().height() <= 768 - 48);
        window.show();
        QVERIFY(putgd::test::waitForWindowExposed(&window));
        QCOMPARE(window.size(), ReviewWindow::defaultSize());
        auto state = window.inspect();
        qInfo("review default %dx%d header %d share %.3f", window.width(), window.height(), state["header"].toObject()["rect"].toArray()[3].toInt(), state["video"].toObject()["share"].toDouble());
        // Before #283 the frame had about 30% of a 900×700 window; now most of it.
        QVERIFY2(state["video"].toObject()["share"].toDouble() >= 0.6, qPrintable(QString::number(state["video"].toObject()["share"].toDouble())));
        window.setEdit(twoMasks());
        QApplication::processEvents();
        state = window.inspect();
        QCOMPARE(state["timeline"].toObject()["lanes"].toInt(), 2);
        qInfo("review two masks share %.3f timeline %d", state["video"].toObject()["share"].toDouble(), state["timeline"].toObject()["rect"].toArray()[3].toInt());
        QVERIFY2(state["video"].toObject()["share"].toDouble() >= 0.6, qPrintable(QString::number(state["video"].toObject()["share"].toDouble())));
        // Transient guidance sits below the frame, leaving the video unobscured.
        window.drive({{"action", "selectMask"}, {"maskId", "m-blur"}});
        QApplication::processEvents();
        QVERIFY(window.findChild<QWidget*>("statusRow")->isVisible());
        qInfo("review with a status line share %.3f", window.inspect()["video"].toObject()["share"].toDouble());
        QVERIFY2(window.inspect()["video"].toObject()["share"].toDouble() >= 0.53, qPrintable(QString::number(window.inspect()["video"].toObject()["share"].toDouble())));
        auto* row = window.findChild<QWidget*>("statusRow");
        auto* frame = window.findChild<FrameView*>();
        QVERIFY(row->mapTo(&window, QPoint()).y() >= frame->mapTo(&window, frame->rect().bottomLeft()).y());
        // Got it hides the guidance for the rest of the review.
        QVERIFY(window.drive({{"action", "dismissBlurTip"}}).code.isEmpty());
        QApplication::processEvents();
        QVERIFY(!window.findChild<QWidget*>("statusRow")->isVisible());
        window.drive({{"action", "selectMask"}, {"maskId", "m-solid"}});
        QApplication::processEvents();
        QVERIFY(window.inspect()["video"].toObject()["share"].toDouble() >= 0.6);
        // The minimum leaves the frame usable and fits a small laptop screen.
        qInfo("review minimum %dx%d", window.minimumSizeHint().width(), window.minimumSizeHint().height());
        window.resize(window.minimumSizeHint());
        QApplication::processEvents();
        QVERIFY(window.findChild<FrameView*>()->height() >= window.findChild<FrameView*>()->minimumHeight());
        // The video scales with the window.
        const auto before = state["video"].toObject()["rect"].toArray();
        window.resize(1280, 760);
        PUTGD_TRY_VERIFY(window.inspect()["video"].toObject()["rect"].toArray()[2].toInt() > before[2].toInt());
    }
    void largeTextStillFitsA1366x768Desktop() {
        const auto font = QApplication::font();
        const auto scheme = Theme::scheme();
        const auto restore = qScopeGuard([&] { QApplication::setFont(font); Theme::setScheme(scheme); });
        auto large = font;
        if (font.pointSizeF() > 0) large.setPointSizeF(font.pointSizeF() * 1.5);
        else large.setPixelSize(font.pixelSize() * 3 / 2);
        QApplication::setFont(large);
        {
            ReviewWindow window(options(), config());
            // Constructing the window keeps the enlarged text (the shared style
            // installs once and never resets the application font).
            QVERIFY2(QFontInfo(QApplication::font()).pixelSize() >= QFontInfo(large).pixelSize() - 1, qPrintable(QString("%1 < %2").arg(QFontInfo(QApplication::font()).pixelSize()).arg(QFontInfo(large).pixelSize())));
            window.setEdit(twoMasks());
            window.show();
            QVERIFY(putgd::test::waitForWindowExposed(&window));
            QVERIFY(QFontInfo(window.font()).pixelSize() >= QFontInfo(large).pixelSize() - 1);
            const auto minimum = window.minimumSizeHint();
            QVERIFY2(minimum.width() <= 1366 - 24 && minimum.height() <= 768 - 120, qPrintable(QStringLiteral("%1x%2").arg(minimum.width()).arg(minimum.height())));
            QVERIFY(window.width() <= 1366 && window.height() <= 768);
            qInfo("review 150%% text %dx%d minimum %dx%d share %.3f", window.width(), window.height(), minimum.width(), minimum.height(), window.inspect()["video"].toObject()["share"].toDouble());
            const auto userGeometry = window.geometry();
            QVERIFY2(window.inspect()["video"].toObject()["share"].toDouble() >= 0.60, qPrintable(QString::number(window.inspect()["video"].toObject()["share"].toDouble())));
            window.drive({{"action", "selectMask"}, {"maskId", "m-blur"}});
            QApplication::processEvents();
            qInfo("review 150%% text transient %dx%d share %.3f", window.width(), window.height(), window.inspect()["video"].toObject()["share"].toDouble());
            QCOMPARE(window.geometry(), userGeometry);
            QVERIFY2(window.inspect()["video"].toObject()["share"].toDouble() >= 0.53, qPrintable(QString::number(window.inspect()["video"].toObject()["share"].toDouble())));
            auto* status = window.findChild<QWidget*>("statusRow");
            auto* frame = window.findChild<FrameView*>();
            QVERIFY(status->isVisible() && !frame->geometry().intersects(status->geometry()));
            QVERIFY(window.drive({{"action", "dismissBlurTip"}}).code.isEmpty());
            QApplication::processEvents();
            QVERIFY(window.inspect()["video"].toObject()["share"].toDouble() >= 0.60);
            // The compact header grows with the text instead of clipping it.
            auto* header = window.findChild<Graphite::WindowHeader*>("windowHeader");
            QVERIFY(header && header->isVisible() && header->height() == header->compactHeight());
            auto* name = header->findChild<QWidget*>("documentName");
            QVERIFY(name && name->height() >= name->fontMetrics().height() && name->y() >= 0 && name->y() + name->height() <= header->height());
            // Every control is inside the window.
            for (auto* w : window.findChildren<QWidget*>()) if (w->isVisible() && w->isWindow() == false) QVERIFY2(window.rect().contains(QRect(w->mapTo(&window, QPoint()), w->size())), qPrintable(w->accessibleName() + w->objectName()));
        }
    }
    void frameGatesCoverThePreviewAspectRatio_data() {
        QTest::addColumn<QSize>("canvas");
        QTest::addColumn<double>("scale");
        QTest::addColumn<bool>("longDestination");
        QTest::newRow("16:9") << QSize(1280, 720) << 1.0 << false;
        QTest::newRow("16:10") << QSize(960, 600) << 1.0 << false;
        QTest::newRow("16:9 large text") << QSize(1280, 720) << 1.5 << false;
        QTest::newRow("16:10 large text") << QSize(960, 600) << 1.5 << false;
        QTest::newRow("16:10 long destination") << QSize(960, 600) << 1.0 << true;
        QTest::newRow("16:10 long destination large text") << QSize(960, 600) << 1.5 << true;
    }
    void frameGatesCoverThePreviewAspectRatio() {
        QFETCH(QSize, canvas); QFETCH(double, scale); QFETCH(bool, longDestination);
        const auto font = QApplication::font();
        const auto scheme = Theme::scheme();
        const auto restore = qScopeGuard([&] { QApplication::setFont(font); Theme::setScheme(scheme); });
        auto enlarged = font;
        if (font.pointSizeF() > 0) enlarged.setPointSizeF(font.pointSizeF() * scale);
        else enlarged.setPixelSize(qRound(font.pixelSize() * scale));
        QApplication::setFont(enlarged);
        for (const auto theme : {Theme::Scheme::Dark, Theme::Scheme::Light}) {
            Theme::setScheme(theme);
            auto o = options(); o.canvas = canvas; o.audioRecorded = true;
            if (longDestination) {
                o.destination = "Project archive for customer support in the European Union";
                o.uploadSummary = "Upload to " + o.destination + " · Shared link";
            }
            ReviewWindow window(o, config()); window.setEdit(twoMasks()); window.show();
            QVERIFY(putgd::test::waitForWindowExposed(&window));
            QVERIFY(window.height() <= 720 && window.width() <= 1342);
            const auto steady = window.inspect()["video"].toObject()["share"].toDouble();
            window.drive({{"action", "selectMask"}, {"maskId", "m-blur"}});
            QApplication::processEvents();
            const auto transient = window.inspect()["video"].toObject()["share"].toDouble();
            qInfo("review %dx%d text %.1f %s steady %.3f transient %.3f", canvas.width(), canvas.height(), scale, qPrintable(Theme::name(theme)), steady, transient);
            QVERIFY2(steady >= 0.60, qPrintable(QString::number(steady)));
            QVERIFY2(transient >= 0.53, qPrintable(QString::number(transient)));
            // Small user-chosen windows retain their geometry; frame gates apply at defaults.
            window.resize(ReviewWindow::defaultSize()); QApplication::processEvents();
            window.drive({{"action", "format"}, {"format", "mp4"}, {"codec", "h265"}});
            window.showError("Export failed: the encoder stopped after 412 frames because the disk holding the private recording is full; free some space and try again");
            PUTGD_TRY_VERIFY(window.inspect()["status"].toString().contains("Disk full. Free up space."));
            qInfo("visible recovery frame share %.3f at %dx%d", window.inspect()["video"].toObject()["share"].toDouble(), window.width(), window.height());
            QVERIFY(window.inspect()["video"].toObject()["share"].toDouble() >= 0.45);
            window.resize(ReviewWindow::defaultSize());
            PUTGD_TRY_VERIFY2(window.height() <= 768 && window.inspect()["status"].toString().contains("Free up space."),
                qPrintable(QString("Window %1x%2 minimum %3x%4 frame %5 status %6 share %7").arg(window.width()).arg(window.height()).arg(window.minimumSizeHint().width()).arg(window.minimumSizeHint().height()).arg(window.findChild<FrameView*>()->height()).arg(window.findChild<QWidget*>("statusRow")->height()).arg(window.inspect()["video"].toObject()["share"].toDouble())));
        }
    }
    void constrainedWorkspaceKeepsWarningsAndActionsReachable_data() {
        QTest::addColumn<QSize>("canvas");
        QTest::newRow("16:9") << QSize(1280, 720);
        QTest::newRow("16:10") << QSize(960, 600);
    }
    void constrainedWorkspaceKeepsWarningsAndActionsReachable() {
        QFETCH(QSize, canvas);
        const auto font = QApplication::font();
        const auto scheme = Theme::scheme();
        const auto restore = qScopeGuard([&] { QApplication::setFont(font); Theme::setScheme(scheme); });
        auto large = font;
        if (font.pointSizeF() > 0) large.setPointSizeF(font.pointSizeF() * 1.5);
        else large.setPixelSize(font.pixelSize() * 3 / 2);
        QApplication::setFont(large);
        for (const auto scheme : {Theme::Scheme::Dark, Theme::Scheme::Light}) {
            Theme::setScheme(scheme);
            auto o = options(); o.availableSize = QSize(1366, 680); o.canvas = canvas; o.durationUs = 32000000;
            auto edit = twoMasks(); edit.trimStart = 2000000; edit.trimEnd = 29000000;
            ReviewWindow window(o, config()); window.setEdit(edit); window.show();
            QVERIFY(putgd::test::waitForWindowExposed(&window));
            QCOMPARE(window.inspect()["playheadUs"].toDouble(), double(edit.trimStart));
            if (window.height() > 680 || window.inspect()["video"].toObject()["share"].toDouble() < 0.60) {
                const auto render = qEnvironmentVariable("PUTGD_REVIEW_FAILURE_IMAGE");
                if (!render.isEmpty()) window.grab().save(render);
                for (auto* w : window.findChildren<QWidget*>()) if (w->isVisible() && !w->isWindow())
                    qInfo() << w->metaObject()->className() << w->objectName() << w->accessibleName() << w->geometry() << w->minimumSizeHint() << w->sizeHint();
            }
            PUTGD_TRY_VERIFY(window.height() <= 680);
            QVERIFY(window.inspect()["video"].toObject()["share"].toDouble() >= 0.60);
            window.drive({{"action", "selectMask"}, {"maskId", "m-blur"}});
            window.drive({{"action", "format"}, {"format", "mp4"}, {"codec", "h265"}});
            const QString failure = "Export failed: the encoder stopped after 412 frames because the disk holding the private recording is full; free some space and try again";
            window.showError(failure);
            PUTGD_TRY_VERIFY2(window.height() <= 680 && window.minimumSizeHint().height() <= 680,
                qPrintable(QString("%1x%2 min %3x%4 share %5").arg(window.width()).arg(window.height()).arg(window.minimumSizeHint().width()).arg(window.minimumSizeHint().height()).arg(window.inspect()["video"].toObject()["share"].toDouble())));
            PUTGD_TRY_VERIFY(window.inspect()["status"].toString().contains("Disk full. Free up space."));
            qInfo("visible recovery frame share %.3f at %dx%d", window.inspect()["video"].toObject()["share"].toDouble(), window.width(), window.height());
            QVERIFY(window.inspect()["video"].toObject()["share"].toDouble() >= 0.45);
            QVERIFY(window.inspect()["status"].toString().contains("For private information, use Redact."));
            QVERIFY(buttonNamed(window, "Use Redact")->isVisible());
            QVERIFY(window.inspect()["errorDetails"].toObject()["visible"].toBool());
            QCOMPARE(window.inspect()["errorDetails"].toObject()["message"].toString(), failure);
            for (auto* w : window.findChildren<QWidget*>()) if (w->isVisible() && !w->isWindow())
                QVERIFY2(window.rect().contains(QRect(w->mapTo(&window, QPoint()), w->size())), qPrintable(w->accessibleName() + w->objectName()));
            QVERIFY(window.drive({{"action", "showAdvice"}}).state["adviceDialog"].toObject()["open"].toBool());
            auto* details = window.findChild<QWidget*>("reviewAdviceText");
            QVERIFY(details);
            PUTGD_TRY_VERIFY(details->hasFocus());
            QVERIFY(details->property("plainText").toString().contains(failure));
            auto* compatible = buttonNamed(window, "Use MP4 (H.264)");
            QVERIFY(compatible && !compatible->visibleRegion().isEmpty());
            QVERIFY(!window.drive({{"action", "showAdvice"}}).state["adviceDialog"].toObject()["open"].toBool());
        }
    }
    void audioSurvivesSilentFormatRoundTrips() {
        auto o = options(); o.audioRecorded = true; o.formats.append("webp");
        auto c = config(); c.audio.system = true;
        ReviewWindow window(o, c); window.show(); QVERIFY(putgd::test::waitForWindowExposed(&window));
        QSignalSpy changed(&window, &ReviewWindow::configChanged);
        for (const auto& format : {"gif", "webp"}) {
            QVERIFY(window.drive({{"action", "format"}, {"format", format}}).code.isEmpty());
            QVERIFY(!qvariant_cast<RecordingConfig>(changed.last().first()).audio.system);
            QCOMPARE(window.findChild<QLabel*>("exportAudioSummary")->text(), QString("Silent preview · No audio in %1").arg(QString(format).toUpper()));
            QVERIFY(window.drive({{"action", "format"}, {"format", "mp4"}, {"codec", "h264"}}).code.isEmpty());
            QVERIFY(qvariant_cast<RecordingConfig>(changed.last().first()).audio.system);
            QCOMPARE(window.findChild<QLabel*>("exportAudioSummary")->text(), QString("Silent preview · Export includes system audio"));
            QVERIFY(qvariant_cast<RecordingConfig>(changed.last().first()).audio.system);
        }
    }
    void controlsAndStatusKeepTheUsersGeometry() {
        ReviewWindow window(options(), config()); window.show(); QVERIFY(putgd::test::waitForWindowExposed(&window));
        QApplication::processEvents();window.resize(780,600);QApplication::processEvents();const auto geometry=window.geometry();
        window.setEdit(twoMasks());window.drive({{"action","selectMask"},{"maskId","m-blur"}});
        window.showError("Export failed: free some space and try again");QApplication::processEvents();QCOMPARE(window.geometry(),geometry);
        window.drive({{"action","dismissBlurTip"}});window.drive({{"action","dismissStatus"}});QApplication::processEvents();QCOMPARE(window.geometry(),geometry);
    }
    void selectedMaskTimingStaysVisibleAfterEdits() {
        auto o = options(); o.durationUs = 32000000;
        ReviewWindow window(o, config()); auto edit = twoMasks(); window.setEdit(edit); window.show();
        window.drive({{"action", "selectMask"}, {"maskId", "m-blur"}});
        auto* label = window.findChild<QLabel*>("maskActiveInterval");
        QVERIFY(label && label->isVisible());
        edit.revision++; edit.masks[0].start = 5000000; edit.masks[0].end = 20000000; window.setEdit(edit);
        QCOMPARE(label->text(), QString("Applies 0:05.00–0:20.00 · Inactive at playhead"));
        QCOMPARE(window.inspect()["selectedMaskInterval"].toObject()["text"].toString(), label->text());
        window.drive({{"action", "seek"}, {"time", 8000000}});
        QCOMPARE(label->text(), QString("Applies 0:05.00–0:20.00"));
        window.drive({{"action", "seek"}, {"time", 0}});
        const auto position = label->pos();
        QTest::keyClick(window.findChild<FrameView*>(), Qt::Key_Right, Qt::ControlModifier);
        QCOMPARE(label->pos(), position);
        QVERIFY(!window.findChild<FrameView*>()->isAncestorOf(label));
        QCOMPARE(label->text(), QString("Applies 0:05.00–0:20.00 · Inactive at playhead"));
        QCOMPARE(QFontInfo(label->font()).family(), QFontInfo(Graphite::monoFont()).family());
        edit.revision++; edit.trimStart = 2000000; edit.trimEnd = 29000000; window.setEdit(edit);
        window.drive({{"action", "seek"}, {"time", 0}});
        QVERIFY(window.inspect()["playbackStatus"].toString().contains("Outside kept part"));
        QCOMPARE(window.findChild<QLabel*>("muted")->text(), QString("Kept: 27 s"));
    }
    void adviceOnlyOffersAvailableFormats() {
        auto o = options(); o.codecs["mp4"] = QJsonArray{"h265"};
        auto c = config(); c.codec = "h265";
        ReviewWindow window(o, c); window.show();
        QVERIFY(putgd::test::waitForWindowExposed(&window));
        QVERIFY(window.drive({{"action", "showAdvice"}}).state["adviceDialog"].toObject()["open"].toBool());
        QVERIFY(!buttonNamed(window, "Use MP4 (H.264)"));
    }
    void maskControlsShowOnlyWhenTheyApply() {
        ReviewWindow window(options(), config());
        window.show();
        QVERIFY(putgd::test::waitForWindowExposed(&window));
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
        t->findChild<QScrollBar*>("maskLaneScroll")->setValue(1);
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
        QVERIFY(putgd::test::waitForWindowExposed(&window));
        window.activateWindow();
        QVERIFY(putgd::test::waitForWindowActive(&window));
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
    void draggingAMaskBarMovesItInTime() {
        ReviewWindow window(options(), config());
        QSignalSpy proposals(&window, &ReviewWindow::editProposed);
        window.setEdit(twoMasks());
        window.show();
        QVERIFY(putgd::test::waitForWindowExposed(&window));
        auto* t = timeline(window);
        QVERIFY(t);
        // Press inside the unselected redaction (4 s to 10 s) and drag it 3 s
        // earlier: it is selected and moves as a whole, keeping its 6 s length.
        const auto bar = t->maskRect("m-solid");
        const QPoint from(t->xFor(7000000), bar.center().y());
        drag(t, from, QPoint(t->xFor(4000000), bar.center().y()));
        QCOMPARE(window.inspect()["selectedMaskId"].toString(), QString("m-solid"));
        QCOMPARE(proposed(proposals)["op"].toString(), QString("updateMask"));
        QCOMPARE(proposed(proposals)["maskId"].toString(), QString("m-solid"));
        const auto moved = proposed(proposals)["mask"].toObject();
        QVERIFY(std::abs(qint64(moved["start"].toDouble()) - 1000000) <= slack(*t));
        QCOMPARE(qint64(moved["end"].toDouble() - moved["start"].toDouble()), qint64(6000000));
        // Past the clip's end it stops there, still 6 s long.
        RecordingEdit edit = twoMasks(4);
        window.setEdit(edit);
        const auto again = t->maskRect("m-solid");
        drag(t, QPoint(again.center().x(), again.center().y()), QPoint(t->width() + 80, again.center().y()));
        const auto clamped = proposed(proposals)["mask"].toObject();
        QCOMPARE(qint64(clamped["end"].toDouble()), t->duration());
        QCOMPARE(qint64(clamped["end"].toDouble() - clamped["start"].toDouble()), qint64(6000000));
    }
    // A click that wobbles less than the drag distance only selects the mask:
    // no retime (no edit, no undo step) and the playhead stays where it was.
    void clickingAMaskBarWithJitterOnlySelectsIt() {
        ReviewWindow window(options(), config());
        QSignalSpy proposals(&window, &ReviewWindow::editProposed);
        window.setEdit(twoMasks());
        window.show();
        QVERIFY(putgd::test::waitForWindowExposed(&window));
        auto* t = timeline(window);
        QVERIFY(t);
        const auto before = proposals.size();
        const auto playhead = t->playhead();
        const auto bar = t->maskRect("m-solid");
        const QPoint from(t->xFor(7000000), bar.center().y());
        drag(t, from, from + QPoint(QApplication::startDragDistance() - 1, 0));
        QCOMPARE(window.inspect()["selectedMaskId"].toString(), QString("m-solid"));
        QCOMPARE(proposals.size(), before);
        QCOMPARE(t->playhead(), playhead);
    }
    void keyboardAndNamedIconControlsInBothSchemes() {
        for (const auto scheme : {Theme::Scheme::Dark, Theme::Scheme::Light}) {
            Theme::setScheme(scheme);
            ReviewWindow window(options(), config());
            window.setEdit(twoMasks());
            window.show();
            QVERIFY(putgd::test::waitForWindowExposed(&window));
            // No two shortcuts share a key; ambiguous shortcuts never fire.
            QList<QKeySequence> keys;
            for (auto* s : window.findChildren<QShortcut*>()) { QVERIFY2(!keys.contains(s->key()), qPrintable(s->key().toString())); keys.append(s->key()); }
            for (const auto& key : {QKeySequence(Qt::Key_I), QKeySequence(Qt::Key_O), QKeySequence(Qt::SHIFT | Qt::Key_I), QKeySequence(Qt::SHIFT | Qt::Key_O), QKeySequence(Qt::Key_Delete)}) QVERIFY2(keys.contains(key), qPrintable(key.toString()));
            // The tools are icon buttons with a name and a tooltip.
            const auto tools = window.findChildren<QToolButton*>();
            QVERIFY(tools.size() >= 9);
            for (auto* b : tools) {
                QVERIFY2(!b->icon().isNull() && (b->toolButtonStyle() == Qt::ToolButtonIconOnly || (b->toolButtonStyle() == Qt::ToolButtonTextBesideIcon && QStringList{"Start", "End", "Reset"}.contains(b->text()))), qPrintable(b->accessibleName()));
                QVERIFY2(!b->accessibleName().isEmpty() && !b->toolTip().isEmpty(), qPrintable(b->objectName()));
                if(b->isVisible()&&b->toolButtonStyle()==Qt::ToolButtonTextBesideIcon)QVERIFY2(b->width()>=b->fontMetrics().horizontalAdvance(b->text())+b->iconSize().width()+16,qPrintable(b->accessibleName()));
            }
            // The window background is the scheme's.
            const auto image = window.grab().toImage();
            QCOMPARE(image.pixelColor(2, 2).rgb(), Theme::palette(scheme).bg.rgb());
            QCOMPARE(window.inspect()["scheme"].toString(), Theme::name(scheme));
            // Space starts the preview (the clip is missing, so it stops again).
            window.activateWindow();
            QVERIFY(putgd::test::waitForWindowActive(&window));
            QTest::keyClick(timeline(window), Qt::Key_Space);
            QVERIFY(window.inspect()["playing"].toBool());
            PUTGD_TRY_VERIFY(!window.inspect()["playing"].toBool());
        }
        Theme::setScheme(Theme::Scheme::Dark);
    }
    void reviewMethodDrivesTheSameControls() {
        ReviewWindow window(options(), config());
        QSignalSpy proposals(&window, &ReviewWindow::editProposed);
        QSignalSpy configs(&window, &ReviewWindow::configChanged);
        window.show();
        QVERIFY(putgd::test::waitForWindowExposed(&window));
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
    // Discard…, the close button and drive's discard ask before the recording
    // and its edits go; the safe answer is the default and Escape.
    void discardAsksBeforeTheRecordingGoes() {
        ReviewWindow window(options(), config());
        QSignalSpy cancelled(&window, &ReviewWindow::cancelRequested);
        window.show();
        QVERIFY(putgd::test::waitForWindowExposed(&window));
        auto* discard = window.findChild<QPushButton*>("discard");
        QVERIFY(discard && discard->text() == QString::fromUtf8("Discard…"));
        discard->click();
        QMessageBox* box = nullptr;
        PUTGD_TRY_VERIFY((box = window.findChild<QMessageBox*>("confirmAction")) && box->isVisible());
        QCOMPARE(cancelled.size(), 0);
        QVERIFY(box->text().startsWith(QStringLiteral("Discard this recording and its edits?")));
        QCOMPARE(box->findChild<QPushButton*>("confirmDiscard")->text(), QString("Discard recording"));
        QCOMPARE(box->defaultButton()->text(), QString("Keep reviewing"));
        QCOMPARE(box->escapeButton(), static_cast<QAbstractButton*>(box->findChild<QPushButton*>("keepReviewing")));
        QVERIFY(box->findChild<QPushButton*>("keepReviewing")->x() < box->findChild<QPushButton*>("confirmDiscard")->x());
        QVERIFY(window.inspect()["discardConfirmation"].toObject()["open"].toBool());
        // While it is open, other controls wait.
        QCOMPARE(window.drive({{"action", "setIn"}}).code, QString("recording_state"));
        QTest::keyClick(box, Qt::Key_Escape);
        PUTGD_TRY_VERIFY(!window.inspect()["discardConfirmation"].toObject()["open"].toBool());
        QCOMPARE(cancelled.size(), 0);
        QVERIFY(window.isVisible());
        // The window's close button asks the same question and stays open.
        window.close();
        QVERIFY(window.isVisible());
        QVERIFY(window.inspect()["discardConfirmation"].toObject()["open"].toBool());
        QVERIFY(window.drive({{"action", "keepReviewing"}}).code.isEmpty());
        QCOMPARE(cancelled.size(), 0);
        QCOMPARE(window.drive({{"action", "confirmDiscard"}}).code, QString("recording_state"));
        // Through drive: ask, then discard; one cancel.
        QVERIFY(window.drive({{"action", "discard"}}).state["discardConfirmation"].toObject()["open"].toBool());
        QVERIFY(window.drive({{"action", "confirmDiscard"}}).code.isEmpty());
        QCOMPARE(cancelled.size(), 1);
        // Once discarded, closing asks nothing more.
        window.close();
        QVERIFY(!window.isVisible());
        QCOMPARE(cancelled.size(), 1);
    }
    // A frame that could not be decoded says so over the frame until a refresh
    // succeeds; an earlier frame left on screen is marked stale.
    void previewFailureShowsOverTheFrame() {
        ReviewWindow window(options(), config());
        window.show();
        QVERIFY(putgd::test::waitForWindowExposed(&window));
        // The fixture's clip does not exist: the first refresh fails.
        PUTGD_TRY_VERIFY(window.inspect()["preview"].toObject()["error"].isString());
        auto preview = window.inspect()["preview"].toObject();
        QCOMPARE(preview["error"].toString(), QString("The frame at 0:00.00 couldn't be decoded (the private recording file is missing)."));
        QVERIFY(!preview["stale"].toBool());
        auto* card = window.findChild<QWidget*>("previewError");
        auto* retry = window.findChild<QPushButton*>("retryPreview");
        QVERIFY(card && card->isVisible() && retry && retry->isVisible());
        auto* frame = window.findChild<FrameView*>();
        QVERIFY(frame->rect().contains(card->geometry()));
        QVERIFY(frame->accessibleDescription().startsWith(QStringLiteral("The frame at")));
        // A decoded frame clears it; the next failure marks that frame as stale.
        QImage image(1280, 720, QImage::Format_RGB32);
        image.fill(Qt::darkCyan);
        frame->setFrame(image, 0);
        QVERIFY(!card->isVisible());
        QVERIFY(window.inspect()["preview"].toObject()["error"].isNull());
        QVERIFY(window.drive({{"action", "seek"}, {"time", 4000000}}).code.isEmpty());
        PUTGD_TRY_VERIFY(window.inspect()["preview"].toObject()["stale"].toBool());
        preview = window.inspect()["preview"].toObject();
        QCOMPARE(preview["error"].toString(), QString("The frame at 0:04.00 couldn't be decoded (the private recording file is missing)."));
        QCOMPARE(preview["frameTimeUs"].toDouble(), 0.0);
        QVERIFY(window.findChild<QLabel*>("previewErrorText")->text().contains(QStringLiteral("earlier frame (0:00.00)")));
        // Retry tries again at once and, failing, keeps the error.
        retry->click();
        QVERIFY(window.inspect()["preview"].toObject()["stale"].toBool());
        QVERIFY(window.drive({{"action", "retryPreview"}}).state["preview"].toObject()["error"].isString());
    }
    // Errors and warnings get their own wrapping row with actions, only when
    // needed; the audio summary beside Output stays short and whole.
    void statusRowAndAudioSummary() {
        auto o = options();
        o.audioRecorded = true;
        auto c = config(); c.audio.system = true;
        ReviewWindow window(o, c);
        QSignalSpy proposals(&window, &ReviewWindow::editProposed);
        QSignalSpy configs(&window, &ReviewWindow::configChanged);
        window.show();
        QVERIFY(putgd::test::waitForWindowExposed(&window));
        auto* row = window.findChild<QWidget*>("statusRow");
        auto* audio = window.findChild<QLabel*>("audioSummary");
        QVERIFY(row && audio);
        QVERIFY(!row->isVisible());
        QCOMPARE(window.inspect()["statusRow"].toArray().size(), 0);
        QCOMPARE(audio->text(), QString("Silent preview"));
        QVERIFY(audio->accessibleName().contains("Export includes system audio"));
        QCOMPARE(window.findChild<QLabel*>("exportAudioSummary")->text(), QString("Silent preview · Export includes system audio"));
        QVERIFY(window.findChild<QLabel*>("exportAudioSummary")->isVisible());
        // H.265: a warning with its fix; the fix is the format control's own path.
        QVERIFY(window.drive({{"action", "format"}, {"format", "mp4"}, {"codec", "h265"}}).code.isEmpty());
        auto lines = window.inspect()["statusRow"].toArray();
        QCOMPARE(lines.size(), 1);
        QCOMPARE(lines[0].toObject()["tone"].toString(), QString("warning"));
        QVERIFY(row->isVisible());
        auto* fix = buttonNamed(window, "Use MP4 (H.264)");
        QVERIFY(fix);
        fix->click();
        QCOMPARE(window.inspect()["format"].toObject()["codec"].toString(), QString("h264"));
        PUTGD_TRY_VERIFY(!row->isVisible());
        // A long export error wraps in full at the minimum width and stays until dismissed.
        window.resize(window.minimumSizeHint());
        const QString failure = QStringLiteral("Export failed: the encoder stopped after 412 frames because the disk holding the private recording is full; free some space and try again");
        window.showError(failure);
        QApplication::processEvents();
        QVERIFY(window.inspect()["errorDetails"].toObject()["visible"].toBool());
        QCOMPARE(window.inspect()["errorDetails"].toObject()["message"].toString(), failure);
        auto details=window.drive({{"action", "showAdvice"}});
        QVERIFY(details.state["adviceDialog"].toObject()["text"].toString().contains(failure));
        window.drive({{"action", "showAdvice"}});
        QVERIFY(window.drive({{"action", "seek"}, {"time", 3000000}}).code.isEmpty());
        QCOMPARE(window.inspect()["errorDetails"].toObject()["message"].toString(), failure);
        QVERIFY(window.drive({{"action", "dismissStatus"}}).state["statusRow"].toArray().isEmpty());
        window.resize(ReviewWindow::defaultSize());
        // GIF: silent, and over its length limit; the trim it needs is one click.
        window.setLimits(5000000, 4096);
        QVERIFY(window.drive({{"action", "format"}, {"format", "gif"}, {"codec", ""}}).code.isEmpty());
        QCOMPARE(audio->text(), QString("Silent preview"));
        QVERIFY(audio->accessibleName().contains("No audio in GIF"));
        QCOMPARE(window.findChild<QLabel*>("exportAudioSummary")->text(), QString("Silent preview · No audio in GIF"));
        lines = window.inspect()["statusRow"].toArray();
        QCOMPARE(lines.size(), 1);
        QVERIFY(lines[0].toObject()["text"].toString().startsWith(QStringLiteral("GIF is limited to 5 s")));
        auto* keep = buttonNamed(window, "Keep the first 5 s");
        QVERIFY(keep);
        keep->click();
        QCOMPARE(proposed(proposals), QJsonObject({{"op", "setTrim"}, {"start", 0.0}, {"end", 5000000.0}}));
        Q_UNUSED(configs);
    }
    void exportRecoveryUsesTheSameRetryAction() {
        ReviewWindow window(options(), config()); window.show();
        QVERIFY(putgd::test::waitForWindowExposed(&window));
        QSignalSpy retry(&window, &ReviewWindow::retryExportRequested);
        QVERIFY(!window.drive({{"action", "retryExport"}}).code.isEmpty());
        window.showError("Export failed: disk full");
        QApplication::processEvents();
        auto* again = buttonNamed(window, "Try again");
        auto* dismiss = buttonNamed(window, "Dismiss");
        QVERIFY(again && dismiss && again->isVisible());
        QVERIFY(again->mapTo(&window, QPoint()).x() < dismiss->mapTo(&window, QPoint()).x());
        again->click(); QCOMPARE(retry.size(), 1);
        window.drive({{"action", "format"}, {"format", "mp4"}, {"codec", "h265"}});
        window.drive({{"action", "showAdvice"}});
        auto* dialog = window.findChild<QDialog*>("reviewAdvice");
        QVERIFY(dialog); QCOMPARE(dialog->windowTitle(), QString("Export failed"));
        auto* compatible = buttonNamed(*dialog, "Use MP4 (H.264)");
        QVERIFY(compatible);
        const auto position = compatible->mapTo(dialog, QPoint());
        QVERIFY(dialog->rect().contains(QRect(position, compatible->size())));
        auto* primary = buttonNamed(*dialog, "Try again");
        QVERIFY(primary && primary->isDefault());
        primary->click(); QCOMPARE(retry.size(), 2);
        QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        window.drive({{"action", "showAdvice"}});
        dialog = window.findChild<QDialog*>("reviewAdvice"); QVERIFY(dialog);
        auto* close = buttonNamed(*dialog, "Close"); QVERIFY(close); close->click();
        QVERIFY(!window.inspect()["status"].toString().contains("Export failed"));
        QVERIFY(!window.inspect()["adviceDialog"].toObject()["open"].toBool());
    }
    void letterboxFollowsThePalette() {
        const auto scheme = Theme::scheme();
        const auto restore = qScopeGuard([&] { Theme::setScheme(scheme); });
        for (const auto theme : {Theme::Scheme::Dark, Theme::Scheme::Light}) {
            Theme::setScheme(theme);
            FrameView frame; frame.setCanvas({160, 90}); frame.resize(200, 200);
            QImage image(160, 90, QImage::Format_RGB32); image.fill(Qt::red);
            frame.setFrame(image, 0); frame.show();
            const auto rendered = frame.grab().toImage();
            QCOMPARE(rendered.pixelColor(0, 0), Theme::current().bg);
            QCOMPARE(rendered.pixelColor(100, 100), QColor(Qt::red));
        }
    }
    void replacingVisibleStatusShowsTextAndRecoveryActions() {
        ReviewWindow window(options(), config());
        window.show();
        QVERIFY(putgd::test::waitForWindowExposed(&window));
        window.showError("First export failed");
        auto* row = window.findChild<QWidget*>("statusRow");
        QVERIFY(row && row->isVisible());
        // Replace a row that is already visible, then add a second line.
        const QString failure = QStringLiteral("Export failed: free disk space and try again");
        window.showError(failure);
        QVERIFY(window.drive({{"action", "format"}, {"format", "mp4"}, {"codec", "h265"}}).code.isEmpty());
        auto* tips = window.findChild<QToolButton*>("formatAdvice");
        QVERIFY(tips && tips->isVisible());
        QVERIFY(window.inspect()["formatAdvice"].toObject()["visible"].toBool());
        tips->click();
        QApplication::processEvents();
        int lines = 0;
        for (auto* line : row->findChildren<QWidget*>("statusLine", Qt::FindDirectChildrenOnly)) {
            if (line->isHidden()) continue;
            ++lines;
            QVERIFY(line->isVisible() && !line->visibleRegion().isEmpty());
            auto* text = line->findChild<QLabel*>("status");
            QVERIFY(text && text->isVisible() && !text->visibleRegion().isEmpty());
            QVERIFY(text->height() >= text->fontMetrics().height());
            QVERIFY(row->rect().contains(QRect(text->mapTo(row, QPoint()), text->size())));
            for (auto* button : line->findChildren<QPushButton*>("statusAction")) {
                QVERIFY(button->isVisible() && !button->visibleRegion().isEmpty());
                QVERIFY(button->height() >= 32 && button->width() >= 32);
                QVERIFY(row->rect().contains(QRect(button->mapTo(row, QPoint()), button->size())));
            }
        }
        QCOMPARE(lines, 1);
        QVERIFY(window.inspect()["adviceDialog"].toObject()["open"].toBool());
        auto* advice = window.findChild<QWidget*>("reviewAdviceText");
        QVERIFY(advice && advice->property("plainText").toString().contains("MP4 (H.264)"));
        auto* dismiss = buttonNamed(window, "Dismiss");
        auto* fix = buttonNamed(window, "Use MP4 (H.264)");
        QVERIFY(dismiss && fix);
        fix->click();
        QVERIFY(!window.inspect()["adviceDialog"].toObject()["open"].toBool());
        dismiss = buttonNamed(window, "Dismiss"); QVERIFY(dismiss); dismiss->click();
        QVERIFY(!window.inspect()["status"].toString().contains(failure));
        QApplication::processEvents();
        QVERIFY(window.inspect()["statusRow"].toArray().isEmpty());
        QVERIFY(!row->isVisible());
    }
    // Blur is a visual effect: the editor's guidance shows, readable without
    // hover, while a blur is placed or selected, with a switch to Redaction.
    void blurGuidanceAndRedactionNames() {
        ReviewWindow window(options(), config());
        QSignalSpy proposals(&window, &ReviewWindow::editProposed);
        window.setEdit(twoMasks());
        window.show();
        QVERIFY(putgd::test::waitForWindowExposed(&window));
        const QString guidance = QStringLiteral("For private information, use Redact.");
        QVERIFY(!window.inspect()["status"].toString().contains(guidance));
        auto* list = window.findChild<QComboBox*>("masks");
        QCOMPARE(list->itemText(1), QString("Redaction 2"));
        QVERIFY(window.drive({{"action", "selectMask"}, {"maskId", "m-blur"}}).code.isEmpty());
        QCOMPARE(window.inspect()["status"].toString(), guidance);
        auto* fix = buttonNamed(window, "Use Redact");
        QVERIFY(fix);
        fix->click();
        const auto command = proposed(proposals);
        QCOMPARE(command["op"].toString(), QString("updateMask"));
        QCOMPARE(command["maskId"].toString(), QString("m-blur"));
        QCOMPARE(command["mask"].toObject()["kind"].toString(), QString("solid"));
        QVERIFY(!command["mask"].toObject().contains("strength"));
        // Arming Add blur shows it too; its action arms the redaction instead.
        QVERIFY(window.drive({{"action", "selectMask"}, {"maskId", QJsonValue(QJsonValue::Null)}}).code.isEmpty());
        QToolButton* addBlur = nullptr;
        for (auto* b : window.findChildren<QToolButton*>()) if (b->accessibleName() == "Blur") addBlur = b;
        QVERIFY(addBlur);
        addBlur->click();
        QCOMPARE(window.inspect()["status"].toString(), guidance);
        QVERIFY(window.drive({{"action", "useSolidCover"}}).state["armed"].toString() == "solid");
        QVERIFY(!window.inspect()["status"].toString().contains(guidance));
        // One name for opaque redaction across controls.
        for (auto* b : window.findChildren<QToolButton*>()) QVERIFY2(!b->toolTip().contains(QStringLiteral("the cover")), qPrintable(b->toolTip()));
    }
    // Keyboard-only masks: Enter places an armed mask centred at half the
    // frame's size; with the frame focused, arrows move the selected mask,
    // Shift+arrows resize it, Ctrl makes the step 10 px (as in the editor); one edit per burst.
    void masksByKeyboard() {
        ReviewWindow window(options(), config());
        QSignalSpy proposals(&window, &ReviewWindow::editProposed);
        window.show();
        QVERIFY(putgd::test::waitForWindowExposed(&window));
        window.activateWindow();
        QVERIFY(putgd::test::waitForWindowActive(&window));
        auto* frame = window.findChild<FrameView*>();
        QVERIFY(frame && frame->focusPolicy() == Qt::StrongFocus);
        QToolButton* addSolid = nullptr;
        for (auto* b : window.findChildren<QToolButton*>()) if (b->accessibleName() == "Redact") addSolid = b;
        QVERIFY(addSolid);
        addSolid->setFocus(Qt::TabFocusReason);
        QTest::keyClick(addSolid, Qt::Key_Space);
        QCOMPARE(window.inspect()["armed"].toString(), QString("solid"));
        QTest::keyClick(addSolid, Qt::Key_Return);
        auto added = proposed(proposals);
        QCOMPARE(added["op"].toString(), QString("addMask"));
        QCOMPARE(added["mask"].toObject()["rect"].toArray(), QJsonArray({320, 180, 640, 360}));
        QVERIFY(window.inspect()["armed"].isNull());
        QVERIFY(frame->hasFocus());
        // The resident's answer selects it; arrows nudge it as one proposal.
        RecordingEdit edit;
        edit.revision = 2;
        RecordingMask m = mask("m-key", "solid", 0, duration);
        m.rect = QRect(320, 180, 640, 360);
        edit.masks = {m};
        window.setEdit(edit);
        QVERIFY(frame->accessibleDescription().startsWith(QStringLiteral("Redaction 1 selected: 640 × 360 at 320, 180")));
        const auto before = proposals.size();
        for (int i = 0; i < 3; ++i) QTest::keyClick(frame, Qt::Key_Right);
        QTest::keyClick(frame, Qt::Key_Up, Qt::ControlModifier);
        QCOMPARE(proposals.size(), before);
        PUTGD_TRY_VERIFY(proposals.size() == before + 1);
        QCOMPARE(proposed(proposals)["mask"].toObject()["rect"].toArray(), QJsonArray({323, 170, 640, 360}));
        // Shift+arrows resize from the bottom-right, inside the canvas.
        QTest::keyClick(frame, Qt::Key_Down, Qt::ShiftModifier);
        QTest::keyClick(frame, Qt::Key_Left, Qt::ShiftModifier);
        frame->commitNudge();
        QCOMPARE(proposed(proposals)["mask"].toObject()["rect"].toArray(), QJsonArray({323, 170, 639, 361}));
        // Escape clears the selection; arrows then do nothing.
        QTest::keyClick(frame, Qt::Key_Escape);
        QVERIFY(window.inspect()["selectedMaskId"].isNull());
        const auto after = proposals.size();
        QTest::keyClick(frame, Qt::Key_Right);
        frame->commitNudge();
        QCOMPARE(proposals.size(), after);
        // Agents: addMask without a rect is the centred mask; placeMask sets the selected one's.
        QVERIFY(window.drive({{"action", "addMask"}, {"kind", "blur"}}).code.isEmpty());
        QCOMPARE(proposed(proposals)["mask"].toObject()["rect"].toArray(), QJsonArray({320, 180, 640, 360}));
        QCOMPARE(window.drive({{"action", "placeMask"}, {"rect", QJsonArray{0, 0, 10, 10}}}).code, QString("invalid_request"));
        QVERIFY(window.drive({{"action", "selectMask"}, {"maskId", "m-key"}}).code.isEmpty());
        QVERIFY(window.drive({{"action", "placeMask"}, {"rect", QJsonArray{8, 8, 100, 50}}}).code.isEmpty());
        QCOMPARE(proposed(proposals)["mask"].toObject()["rect"].toArray(), QJsonArray({8, 8, 100, 50}));
        QCOMPARE(window.drive({{"action", "placeMask"}, {"rect", QJsonArray{1200, 8, 100, 50}}}).code, QString("invalid_request"));
    }
    // Timecodes in Plex Mono (as on the recording bar), and visible Trim/Mask groups.
    void timecodesAreMonoAndGroupsAreNamed() {
        ReviewWindow window(options(), config());
        window.show();
        QVERIFY(putgd::test::waitForWindowExposed(&window));
        for (const auto* name : {"time", "muted"}) {
            QLabel* label = nullptr;
            for (auto* l : window.findChildren<QLabel*>(name)) if (l->accessibleName() == "Playhead time" || l->accessibleName() == "Kept interval") label = l;
            QVERIFY2(label, name);
            QCOMPARE(QFontInfo(label->font()).family(), QFontInfo(Graphite::monoFont()).family());
            const auto scheme = Theme::scheme();
            Theme::setScheme(scheme == Theme::Scheme::Dark ? Theme::Scheme::Light : Theme::Scheme::Dark);
            QApplication::processEvents();
            QCOMPARE(QFontInfo(label->font()).family(), QFontInfo(Graphite::monoFont()).family());
            QCOMPARE(label->font().pixelSize(), Graphite::monoFont().pixelSize());
            Theme::setScheme(scheme);
            QVERIFY(label->width() >= label->sizeHint().width());
        }
        for (const auto* name : {"trimGroup", "maskGroup"}) { auto* label = window.findChild<QLabel*>(name); QVERIFY2(label && label->isVisible(), name); }
        QCOMPARE(window.findChild<QLabel*>("trimGroup")->text(), QString("Trim"));
        QCOMPARE(window.findChild<QLabel*>("maskGroup")->text(), QString("Mask"));
    }
    // #259: Save copy… opens the shared chooser; this test never asks a portal.
    void saveCopyDialogIsDrivenAndSavesTheChosenPath() {
        qputenv("PUTGD_FILE_DIALOG", "fallback");
        QTemporaryDir folder;
        QVERIFY(folder.isValid());
        ReviewWindow window(options(), config());
        window.show();
        QVERIFY(putgd::test::waitForWindowExposed(&window));
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
        for (auto* button : window.findChildren<QPushButton*>()) if (button->text() == QString::fromUtf8("Save a copy…")) save = button;
        QVERIFY(save);
        save->click();
        QFileDialog* fallback = nullptr;
        PUTGD_TRY_VERIFY((fallback = window.findChild<QFileDialog*>("putgdFileDialog")) && fallback->isVisible());
        QCOMPARE(fallback->windowTitle(), QString("Save a copy"));
        QCOMPARE(fallback->nameFilters(), QStringList{"MP4 (*.mp4)"});
        fallback->setDirectory(folder.path());
        auto* name = fallback->findChild<QLineEdit*>("fileNameEdit");
        QVERIFY(name);
        name->setText("chosen");
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
