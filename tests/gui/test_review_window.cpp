// Offscreen widget contract for the review window (#283): the video takes the
// window, one timeline sets the trim and mask times, mask controls show only
// when they apply, and recording.review drives the same controls. Not desktop
// proof; the lab captures are in the docs.
#include "record/ReviewWindow.h"
#include "record/Duration.h"
#include "record/Overlays.h"
#include "ui/RegionOverlay.h"
#include <QAccessible>
#include <QAbstractItemView>
#include <QScrollBar>
#include "ui/Graphite.h"
#include "ui/Fonts.h"
#include "ui/Theme.h"
#include <QLabel>
#include <QApplication>
#include <QGridLayout>
#include <QClipboard>
#include <QWheelEvent>
#include "platform/HostOpen.h"
#include <QFontInfo>
#include <QComboBox>
#include <QFileDialog>
#include <QFile>
#include <QLineEdit>
#include <QMessageBox>
#include <QMenu>
#include <QAction>
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
#include <tuple>
#include <cmath>
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
QAbstractButton* buttonNamed(QWidget& window, const QString& text) {
    for (auto* button : window.findChildren<QAbstractButton*>()) if ((button->text() == text || button->accessibleName() == text) && button->isVisible()) return button;
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
    QFont baselineFont_;
    Theme::Scheme baselineScheme_;
    // One pixel of the timeline is duration / track width; edges land within two.
    static qint64 slack(const ReviewTimeline& t) { return 2 * duration / t.trackRect().width() + 1; }
    static ReviewTimeline* timeline(ReviewWindow& w) { return w.findChild<ReviewTimeline*>("timeline"); }
private slots:
    void labelsStayInsideTheirOwnMaskCoverage() {
        FrameView frame;
        frame.setCanvas({320, 180}); frame.resize(640, 360);
        QImage media(320, 180, QImage::Format_RGB32); media.fill(QColor(130, 60, 120));
        frame.setFrame(media, 0);
        auto small = mask("small", "solid", 1000000, 2000000); small.rect = QRect(80, 60, 50, 24);
        frame.setMasks({small}, small.id); frame.show(); QApplication::processEvents();
        const auto image = frame.grab().toImage();
        const QRect permitted(155, 115, 110, 58);
        for (int y = 0; y < image.height(); ++y) for (int x = 0; x < image.width(); ++x)
            if (!permitted.contains(x, y)) QCOMPARE(image.pixelColor(x, y), media.pixelColor(0, 0));
        ReviewTimeline t;
        t.setDuration(duration); t.setTrim(0, duration); t.setPlayhead(duration - 1);
        t.setMasks({mask("long", "solid", 0, duration - 1000000)}, {});
        t.resize(260, t.sizeHint().height()); t.show(); QApplication::processEvents();
        const auto pixels = t.grab().toImage();
        const auto bar = t.maskRect("long").adjusted(4, 4, -4, -4);
        // A name may paint text, but never a background-coloured label box over coverage.
        for (int y = bar.top(); y <= bar.bottom(); ++y) for (int x = bar.left(); x <= bar.right(); ++x)
            QVERIFY(pixels.pixelColor(x, y) != Theme::current().bg);
        QCOMPARE(t.trackRect().height(), 8);
    }
    void blurGuidanceStaysInTheMaskRowDuringExportRecovery() {
        ReviewWindow window(options(), config()); window.setEdit(twoMasks()); window.show();
        QVERIFY(putgd::test::waitForWindowExposed(&window));
        const auto visible = [&] { return window.inspect()["blurGuidance"].toObject()["visible"].toBool(); };
        QVERIFY(visible());
        const auto frame = window.findChild<FrameView*>()->geometry();
        window.drive({{"action", "selectMask"}, {"maskId", "m-blur"}});
        QApplication::processEvents();
        QCOMPARE(window.findChild<FrameView*>()->geometry(), frame);
        QVERIFY(window.findChild<QLabel*>("maskActiveInterval")->accessibleDescription().contains(Graphite::blurWarningText()));
        QVERIFY(window.inspect()["statusRow"].toArray().isEmpty());
        window.showError("Export failed: disk full");
        QCOMPARE(window.inspect()["statusRow"].toArray().size(), 1);
        QVERIFY(visible());
        QVERIFY(buttonNamed(window, "Try again"));
        window.drive({{"action", "dismissStatus"}});
        QVERIFY(window.inspect()["statusRow"].toArray().isEmpty());
        window.drive({{"action", "dismissBlurTip"}});
        QVERIFY(!visible());
        window.drive({{"action", "selectMask"}, {"maskId", "m-solid"}});
        window.drive({{"action", "selectMask"}, {"maskId", "m-blur"}});
        QVERIFY(!visible());
    }
    void recoveryStaysWithItsMessageAboveTheFooter() {
        ReviewWindow window(options(), config()); window.setEdit(twoMasks()); window.show();
        QVERIFY(putgd::test::waitForWindowExposed(&window));
        window.showError("Export failed: disk full");
        QApplication::processEvents();
        auto* row = window.findChild<QWidget*>("statusRow");
        auto* footer = window.findChild<QWidget*>("reviewFooter");
        auto* details = window.findChild<QPushButton*>("reviewErrorDetails");
        auto* retry = buttonNamed(window, "Try again");
        auto* dismiss = buttonNamed(window, "Dismiss");
        QVERIFY(row && details && retry && dismiss);
        for (auto* action : QList<QAbstractButton*>{details, retry, dismiss}) {
            QVERIFY(row->isAncestorOf(action) && action->isVisible());
            QVERIFY(row->rect().contains(QRect(action->mapTo(row, QPoint()), action->size())));
        }
        QVERIFY(row->geometry().top() > timeline(window)->geometry().bottom());
        QCOMPARE(row->geometry().bottom() + 1, footer->geometry().top());
        QSignalSpy retried(&window, &ReviewWindow::retryExportRequested);
        retry->click(); QCOMPARE(retried.size(), 1);
        dismiss->click(); QVERIFY(row->isHidden());
    }
    void readoutAndRecordingBarUseTheSameTenths() {
        auto o = options(); o.durationUs = 125678901;
        ReviewWindow window(o, config()); window.show();
        QVERIFY(putgd::test::waitForWindowExposed(&window));
        ControlBar::Options barOptions; barOptions.formats = {"mp4"};
        ControlBar bar(barOptions, config());
        for (const qint64 at : {qint64(0), qint64(999999), qint64(60567890)}) {
            window.drive({{"action", "seek"}, {"time", double(at)}});
            bar.setRetained(at);
            QCOMPARE(window.findChild<QLabel*>("time")->text().section("/", 0, 0), bar.findChild<QLabel*>("time")->text());
        }
        QCOMPARE(window.findChild<QLabel*>("time")->text(), QString("1:00.5/2:05.6"));
    }
    void stableMaskNamesSurviveRemoval() {
        ReviewWindow window(options(), config());
        window.setEdit(twoMasks()); window.show();
        QVERIFY(putgd::test::waitForWindowExposed(&window));
        QCOMPARE(window.inspect()["selectedMaskName"].toString(), QString("Redaction 2"));
        auto edit = twoMasks(4); edit.masks.removeFirst(); window.setEdit(edit);
        QCOMPARE(window.findChild<QComboBox*>("masks")->currentText(), QString("Redaction 2"));
        QCOMPARE(window.inspect()["masks"].toArray().first().toObject()["name"].toString(), QString("Redaction 2"));
        QVERIFY(window.findChild<FrameView*>()->accessibleDescription().contains("Redaction 2 selected"));
        edit.revision++; edit.masks.append(mask("third", "blur", 0, duration)); window.setEdit(edit);
        QCOMPARE(window.inspect()["selectedMaskName"].toString(), QString("Blur 3"));
    }
    void maskChooserKeepsFullNamesInItsPopup() {
        ReviewWindow window(options(), config());
        RecordingEdit edit;
        for (int i = 0; i < 12; ++i) edit.masks.append(mask(QString::number(i), "solid", 0, duration));
        window.setEdit(edit); window.show();
        QVERIFY(putgd::test::waitForWindowExposed(&window));
        auto* chooser = window.findChild<QComboBox*>("masks");
        QVERIFY(chooser);
        for (int i = 0; i < chooser->count(); ++i)
            QVERIFY(chooser->width() >= chooser->fontMetrics().horizontalAdvance(chooser->itemData(i).toString()) + Graphite::Metric::splitZone + 18);
        chooser->showPopup(); QApplication::processEvents();
        for (int i = 0; i < chooser->count(); ++i)
            QVERIFY(chooser->view()->width() >= chooser->fontMetrics().horizontalAdvance(chooser->itemText(i)) + 32);
        chooser->hidePopup();
        QVERIFY(chooser->accessibleDescription().contains(chooser->currentText()));
        auto* metadata = window.findChild<QLabel*>("maskActiveInterval");
        QVERIFY(metadata && metadata->isVisible());
        QVERIFY(metadata->mapTo(&window, QPoint()).y() >= window.findChild<FrameView*>()->geometry().bottom());
        auto* audio = window.findChild<QLabel*>("exportAudioSummary");
        QVERIFY(audio && audio->isVisible());
        QVERIFY(audio->accessibleName().endsWith(" · Preview muted"));
    }
    void stackedToolsKeepTheirScopeAndTrimLabels() {
        auto edit = twoMasks(); edit.trimStart = 2000000; edit.trimEnd = 29000000;
        auto o = options(); o.durationUs = 32000000;
        ReviewWindow window(o, config()); window.setEdit(edit); window.show();
        QVERIFY(putgd::test::waitForWindowExposed(&window));
        window.resize(window.minimumSizeHint().width(), 680); QApplication::processEvents();
        auto* trim = window.findChild<QWidget*>("trimControls");
        auto* masks = window.findChild<QWidget*>("maskControls");
        auto* interval = window.findChild<QLabel*>("maskActiveInterval");
        auto* timing = window.findChild<QWidget*>("maskTimingControls");
        QVERIFY(masks->isAncestorOf(timing) && masks->isAncestorOf(interval)); QVERIFY(!trim->isAncestorOf(interval) && !trim->isAncestorOf(timing));
        QVERIFY(trim->isVisible());
        // Timing actions read as words on 32 px secondary buttons, in both rows.
        const QList<std::tuple<QWidget*, QString, QString>> actions{{trim, "Set trim start at playhead", "Start here"}, {trim, "Set trim end at playhead", "End here"}, {trim, "Reset trim", "Reset"}, {timing, "Set mask start at playhead", "Start here"}, {timing, "Set mask end at playhead", "End here"}};
        for (const auto& [row, name, text] : actions) {
            QToolButton* found = nullptr;
            for (auto* button : row->findChildren<QToolButton*>()) if (button->accessibleName() == name) found = button;
            QVERIFY2(found && found->isVisible(), qPrintable(name)); QCOMPARE(found->toolButtonStyle(), Qt::ToolButtonTextOnly); QCOMPARE(found->text(), text);
            QVERIFY(!found->autoRaise());
            QVERIFY(found->height() >= std::max(32, found->fontMetrics().height() + 12));
            QVERIFY(found->width() >= QFontMetricsF(found->font()).horizontalAdvance(text) + 24);
        }
        QVERIFY(!window.findChild<QLabel*>("maskTimingGroup"));
        QVERIFY(window.findChild<QLabel*>("muted")->text().startsWith("Keep 0:02–0:29 · 27 s"));
        auto* add = window.findChild<QToolButton*>("addMask");
        QVERIFY(add && add->menu());
        QCOMPARE(add->menu()->actions().size(), 2);
        QCOMPARE(add->menu()->actions()[0]->text(), QString("Add redaction"));
        QCOMPARE(add->menu()->actions()[1]->text(), QString("Add blur"));
        QVERIFY(timeline(window)->isAncestorOf(add));

    }
    void contextualControlsReclaimFrameAndBlurWarningIsOncePerSession() {
        auto o = options(); o.canvas = {1600, 900};
        ReviewWindow window(o, config()); window.show();
        QVERIFY(putgd::test::waitForWindowExposed(&window));
        window.resize(640, 480); QApplication::processEvents();
        auto* controls = window.findChild<QWidget*>("maskControls");
        auto* frame = window.findChild<FrameView*>();
        QVERIFY(!controls->isVisible());
        QVERIFY(frame->height() >= window.height() / 2);
        QVERIFY(window.findChild<QToolButton*>("addMask")->isVisible());
        QVERIFY(window.findChild<QWidget*>("trimControls")->isVisible());
        auto* footer = window.findChild<QWidget*>("reviewFooter");
        QVERIFY(footer->height() <= 84);
        window.setEdit(twoMasks());
        QVERIFY(window.drive({{"action", "selectMask"}, {"maskId", "m-blur"}}).code.isEmpty());
        QVERIFY(controls->isVisible());
        QVERIFY(window.inspect()["blurGuidance"].toObject()["visible"].toBool());
        window.drive({{"action", "selectMask"}, {"maskId", "m-solid"}});
        window.drive({{"action", "selectMask"}, {"maskId", "m-blur"}});
        QVERIFY(window.inspect()["blurGuidance"].toObject()["visible"].toBool());
        window.drive({{"action", "dismissBlurTip"}});
        QApplication::processEvents();
        const auto before = frame->geometry();
        QTest::keyClick(window.findChild<FrameView*>(), Qt::Key_Escape);
        QApplication::processEvents();
        QVERIFY(controls->isVisible());
        QVERIFY(!window.findChild<QToolButton*>("maskOptions")->isEnabled());
        QCOMPARE(window.findChild<QLabel*>("maskActiveInterval")->text(), QString("—"));
        QCOMPARE(frame->geometry(), before);
    }
    void selectingAMaskDuringAPressNeverRefitsTheFrame_data() {
        QTest::addColumn<double>("scale");
        QTest::newRow("standard") << 1.0;
        QTest::newRow("150%") << 1.5;
    }
    void selectingAMaskDuringAPressNeverRefitsTheFrame() {
        QFETCH(double, scale);
        const auto original = QApplication::font();
        const auto restore = qScopeGuard([&] { QApplication::setFont(original); });
        auto font = original;
        if (font.pointSizeF() > 0) font.setPointSizeF(font.pointSizeF() * scale);
        else font.setPixelSize(qRound(font.pixelSize() * scale));
        QApplication::setFont(font);
        ReviewWindow window(options(), config()); window.setEdit(twoMasks()); window.show();
        QVERIFY(putgd::test::waitForWindowExposed(&window));
        window.resize(window.minimumSizeHint()); QApplication::processEvents();
        auto* frame = window.findChild<FrameView*>();
        auto* combo = window.findChild<QComboBox*>("masks");
        auto* interval = window.findChild<QLabel*>("maskActiveInterval");
        auto* group = window.findChild<QWidget*>("maskControls");
        const auto settle = [] { for (int i = 0; i < 3; ++i) QApplication::processEvents(); };
        window.drive({{"action", "selectMask"}, {"maskId", QJsonValue(QJsonValue::Null)}}); settle();
        const auto geometry = frame->geometry(), image = frame->imageRect(), row = group->geometry();
        const int comboWidth = combo->width(), intervalWidth = interval->width();
        QVERIFY(group->isVisible()); QCOMPARE(combo->currentIndex(), -1); QCOMPARE(combo->placeholderText(), QString("Select a mask"));
        QCOMPARE(interval->text(), QString("—"));
        auto* maskOptions = window.findChild<QToolButton*>("maskOptions");
        QVERIFY(!maskOptions->isEnabled()); QCOMPARE(maskOptions->toolTip(), QString("Select a mask first"));
        for (const auto id : {"m-blur", "m-solid"}) {
            window.drive({{"action", "selectMask"}, {"maskId", id}}); settle();
            QCOMPARE(frame->geometry(), geometry); QCOMPARE(frame->imageRect(), image); QCOMPARE(group->geometry(), row);
            QCOMPARE(combo->width(), comboWidth); QCOMPARE(interval->width(), intervalWidth);
        }
        window.drive({{"action", "showMaskOptions"}});
        window.drive({{"action", "selectMask"}, {"maskId", QJsonValue(QJsonValue::Null)}}); settle();
        QVERIFY(!maskOptions->menu()->isVisible());
        const auto m = twoMasks().masks.first();
        const QPoint point = image.topLeft() + QPoint(qRound(m.rect.center().x() * double(image.width()) / options().canvas.width()), qRound(m.rect.center().y() * double(image.height()) / options().canvas.height()));
        QTest::mousePress(frame, Qt::LeftButton, {}, point); settle();
        QVERIFY(!window.inspect()["selectedMaskId"].isNull());
        QCOMPARE(frame->geometry(), geometry); QCOMPARE(frame->imageRect(), image); QCOMPARE(group->geometry(), row);
        QTest::mouseRelease(frame, Qt::LeftButton, {}, point);
    }
    void bottomArrangementStaysTogetherAcrossReviewWidths() {
        ReviewWindow window(options(), config()); window.setEdit(twoMasks()); window.show();
        QVERIFY(putgd::test::waitForWindowExposed(&window));
        auto* trim = window.findChild<QWidget*>("trimControls");
        auto* masks = window.findChild<QWidget*>("maskControls");
        auto* track = timeline(window);
        for (int width : {720, 820, 1060}) {
            window.resize(width, 720); QApplication::processEvents();
            QVERIFY(track->geometry().bottom() < trim->y());
            QVERIFY(trim->geometry().bottom() < masks->y());
            QVERIFY(!track->maskRect("m-blur").isEmpty());
            QVERIFY(!track->maskRect("m-solid").isEmpty());
            auto* interval = window.findChild<QLabel*>("maskActiveInterval");
            auto* footer = window.findChild<QWidget*>("reviewFooter");
            QVERIFY(interval->mapTo(&window, interval->rect().bottomLeft()).y() < footer->y());
        }
    }
    void lanesSpanReviewAboveControlsAndTransportStaysOnOneLine_data() {
        QTest::addColumn<double>("scale");
        QTest::newRow("standard") << 1.0;
        QTest::newRow("150%") << 1.5;
    }
    void lanesSpanReviewAboveControlsAndTransportStaysOnOneLine() {
        QFETCH(double, scale);
        const auto original = QApplication::font();
        const auto restore = qScopeGuard([&] { QApplication::setFont(original); });
        auto font = original;
        if (font.pointSizeF() > 0) font.setPointSizeF(font.pointSizeF() * scale);
        else font.setPixelSize(qRound(font.pixelSize() * scale));
        QApplication::setFont(font);
        auto edit = twoMasks(); edit.trimEnd = duration - 1000;
        ReviewWindow window(options(), config()); window.setEdit(edit); window.show();
        QVERIFY(putgd::test::waitForWindowExposed(&window));
        auto* t = timeline(window);
        auto* trim = window.findChild<QWidget*>("trimControls");
        for (int width : {720, 820, 1060}) {
            window.resize(width, 720); QApplication::processEvents();
            auto* metadata = window.findChild<QWidget*>("reviewTransport");
            QVERIFY(metadata && t->isAncestorOf(metadata));
            QVERIFY(metadata->geometry().bottom() < t->trackRect().top());
            auto* maskControls = window.findChild<QWidget*>("maskControls");
            QVERIFY(metadata->isAncestorOf(window.findChild<QComboBox*>("speed")));
            auto* add = window.findChild<QToolButton*>("addMask");
            QVERIFY(add && t->isAncestorOf(add) && !maskControls->isAncestorOf(add));
            for (auto* control : window.findChildren<QToolButton*>()) if (control->accessibleName() == "Delete mask") {
                QVERIFY(window.findChild<QMenu*>("maskOptionsMenu")->isAncestorOf(control));
            }
            auto* clip = window.findChild<QLabel*>("clipGroup");
            QVERIFY(clip && !clip->isVisible());
            const auto lane = t->trackRect().translated(t->pos());
            QVERIFY(t->geometry().bottom() < trim->y());
            QCOMPARE(t->x(), 8);
            QCOMPARE(t->width(), window.width() - 16);
            QVERIFY(lane.width() >= QFontMetricsF(Graphite::font(Graphite::Type::metadata)).horizontalAdvance(durationText(duration)) * 2 + 24);
            for (const auto& mask : twoMasks().masks) {
                const auto bar = t->maskRect(mask.id).translated(t->pos());
                QVERIFY(bar.top() > lane.bottom());
                QVERIFY(bar.bottom() < window.findChild<QWidget*>("maskControls")->y());
                QVERIFY(bar.left() >= lane.left() && bar.right() <= lane.right());
            }
            window.drive({{"action", "seek"}, {"time", double(duration - 1)}});
            auto* time = window.findChild<QLabel*>("time");
            auto* status = window.findChild<QLabel*>("playbackStatus");
            auto* playback = window.findChild<QToolButton*>("play");
            auto* speed = window.findChild<QComboBox*>("speed");
            auto* masks = window.findChild<QWidget*>("maskControls");
            QVERIFY(t->isAncestorOf(metadata));
            QCOMPARE(speed->width(), std::max(72, speed->width()));
            for (auto* control : std::initializer_list<QWidget*>{playback, speed, time}) {
                QVERIFY(metadata->isAncestorOf(control));
                const QRect bounds(control->mapTo(&window, QPoint()), control->size());
                QVERIFY(window.rect().contains(bounds));
                QVERIFY(!bounds.intersects(QRect(trim->mapTo(&window, QPoint()), trim->size())));
                QVERIFY(!bounds.intersects(masks->geometry()));
                if (control != time) QCOMPARE(control->mapTo(&window, control->rect().center()).y(), playback->mapTo(&window, playback->rect().center()).y());
            }
            QVERIFY(!time->text().contains('\n'));
            QVERIFY(status->isVisible());
            QVERIFY(status->width() >= QFontMetricsF(status->font()).horizontalAdvance(status->text()) + 2);
            QVERIFY(window.findChild<QWidget*>("reviewTransport")->isAncestorOf(status));
            QVERIFY(window.rect().contains(QRect(status->mapTo(&window, QPoint()), status->size())));
            QVERIFY(t->trackRect().width() >= QFontMetricsF(Graphite::font(Graphite::Type::metadata)).horizontalAdvance(durationText(duration)) * 2 + 24);
            for (auto* button : window.findChildren<QToolButton*>()) if (button->isVisible() && button->toolButtonStyle() == Qt::ToolButtonTextOnly && button->objectName() != "uploadDestinationMenu") {
                QVERIFY(button->width() - (button->objectName() == "useRedact" ? 16 : button->objectName() == "showMoreMasks" ? 8 : 24) >= QFontMetricsF(button->font()).horizontalAdvance(button->text()));
            }
            auto* interval = window.findChild<QLabel*>("maskActiveInterval");
            auto* footer = window.findChild<QWidget*>("reviewFooter");
            const int inkPadding = (interval->height() - interval->fontMetrics().tightBoundingRect(interval->text()).height()) / 2;
            QVERIFY(footer->y() - interval->mapTo(&window, interval->rect().bottomLeft()).y() + inkPadding >= 4);
        }
    }
    void compactFooterPrioritizesDestinationName_data() {
        QTest::addColumn<double>("scale");
        QTest::newRow("standard") << 1.0;
        QTest::newRow("150%") << 1.5;
    }
    void compactFooterPrioritizesDestinationName() {
        QFETCH(double, scale);
        const auto original = QApplication::font();
        const auto restore = qScopeGuard([&] { QApplication::setFont(original); });
        auto font = original;
        if (font.pointSizeF() > 0) font.setPointSizeF(font.pointSizeF() * scale);
        else font.setPixelSize(qRound(font.pixelSize() * scale));
        QApplication::setFont(font);
        auto o = options(); o.audioRecorded = true;
        auto c = config(); c.audio.system = true;
        ReviewWindow window(o, c); window.setEdit(twoMasks()); window.show();
        QVERIFY(putgd::test::waitForWindowExposed(&window));
        auto* destination = window.findChild<Graphite::ElidedLabel*>("uploadDestination");
        auto* output = window.findChild<QToolButton*>("uploadDestinationMenu");
        const auto nameFits = [&] {
            return output && output->isVisible() && output->toolTip().contains(o.destination) && output->text().contains("Shared link")
                && output->toolTip().contains(o.destination) && output->menu()
                && output->height() >= 32 && window.rect().contains(QRect(output->mapTo(&window, QPoint()), output->size()))
                && output->fontMetrics().horizontalAdvance(output->text()) + Graphite::Metric::splitZone + 24 <= output->width();
        };
        window.resize(window.minimumSizeHint()); QApplication::processEvents();
        QVERIFY2(nameFits(), qPrintable(destination->text()));
        window.drive({{"action", "selectMask"}, {"maskId", "m-blur"}});
        window.drive({{"action", "format"}, {"format", "mp4"}, {"codec", "h265"}});
        window.showError("Export failed: disk full. Free up space and try again.");
        for (int i = 0; i < 3; ++i) {
            window.resize(window.minimumSizeHint().width() + i, 680);
            QApplication::processEvents();
            QVERIFY2(nameFits(), qPrintable(destination->text()));
        }
    }
    // Every review control lies whole inside the window and its own container,
    // and no two overlap, at the default and minimum sizes with normal and 150%
    // text: steady, with a blur selected (radius and guidance) and with an error.
    void narrowTimelineUsesTheFullRowAndStatusDoesNotShiftIt_data() {
        QTest::addColumn<double>("scale");
        QTest::newRow("standard") << 1.0;
        QTest::newRow("150%") << 1.5;
    }
    void narrowTimelineUsesTheFullRowAndStatusDoesNotShiftIt() {
        QFETCH(double, scale);
        const auto original = QApplication::font();
        const auto restore = qScopeGuard([&] { QApplication::setFont(original); });
        auto font = original;
        if (font.pointSizeF() > 0) font.setPointSizeF(font.pointSizeF() * scale);
        else font.setPixelSize(qRound(font.pixelSize() * scale));
        QApplication::setFont(font);
        auto edit = twoMasks(); edit.trimStart = 2000000;
        ReviewWindow window(options(), config()); window.setEdit(edit); window.show();
        QVERIFY(putgd::test::waitForWindowExposed(&window));
        auto* track = timeline(window);
        auto* transport = window.findChild<QWidget*>("reviewTransport");
        track->setFixedWidth(520); QApplication::processEvents();
        QVERIFY(transport->property("aboveTimeline").toBool());
        QCOMPARE(track->trackRect().left(), ReviewTimeline::gutterWidth());
        QVERIFY(track->trackRect().width() >= 360);
        QVERIFY(track->trackRect().top() >= transport->geometry().bottom());
        const auto axis = track->trackRect();
        const auto before = window.geometry();
        QVERIFY(window.drive({{"action", "seek"}, {"time", 0}}).code.isEmpty()); QApplication::processEvents();
        QVERIFY(window.findChild<QLabel*>("playbackStatus")->isVisible());
        QCOMPARE(track->trackRect(), axis);
        QCOMPARE(window.geometry(), before);
        track->setMinimumWidth(0); track->setMaximumWidth(QWIDGETSIZE_MAX);
        window.resize(1342, 720); QApplication::processEvents();
        QVERIFY(transport->property("aboveTimeline").toBool());
        QVERIFY(track->trackRect().width() >= 360);
        QVERIFY(track->trackRect().top() > transport->geometry().bottom());
    }
    void purposeFormatChoicesKeepTheCanonicalIds() {
        auto o = options();
        ReviewWindow window(o, config()); window.show();
        QVERIFY(putgd::test::waitForWindowExposed(&window));
        QComboBox* formats = nullptr;
        for (auto* combo : window.findChildren<QComboBox*>()) if (combo->accessibleName() == "Output format") formats = combo;
        QVERIFY(formats);
        const auto choose = [&](QComboBox* combo, const QStringList& id) {
            const int index = combo->findData(id);
            QVERIFY(index >= 0);
            combo->setCurrentIndex(index);
            QCOMPARE(combo->currentData().toStringList(), id);
            QVERIFY(!combo->itemData(index, Qt::ToolTipRole).toString().isEmpty());
            QCOMPARE(combo->toolTip(), combo->itemData(index, Qt::ToolTipRole).toString());
        };
        choose(formats, {"mp4", "h265"});
        QCOMPARE(window.inspect()["format"].toObject()["codec"].toString(), QString("h265"));
        choose(formats, {"mp4", "h264"});
        QVERIFY(formats->currentText().contains("Recommended"));
        ControlBar::Options b; b.formats = o.formats; b.codecs = o.codecs;
        ControlBar bar(b, config());
        QSignalSpy changed(&bar, &ControlBar::configChanged);
        auto* barFormats = bar.findChild<QComboBox*>("format");
        QVERIFY(barFormats);
        choose(barFormats, {"webm", "vp9"});
        QVERIFY(!changed.isEmpty());
        QCOMPARE(qvariant_cast<RecordingConfig>(changed.last().first()).format, QString("webm"));
        QCOMPARE(qvariant_cast<RecordingConfig>(changed.last().first()).codec, QString("vp9"));
    }
    void maskPlacementCommandsArePlainActions() {
        ReviewWindow window(options(), config()); window.show();
        QVERIFY(putgd::test::waitForWindowExposed(&window));
        auto* add = window.findChild<QToolButton*>("addMask");
        const auto entries = add->menu()->actions();
        QCOMPARE(entries.size(), 2);
        for (auto* entry : entries) QVERIFY(!entry->isCheckable());
        entries[0]->trigger();
        QCOMPARE(window.inspect()["armed"].toString(), QString("solid"));
        QVERIFY(window.findChild<FrameView*>()->accessibleDescription().contains("redaction", Qt::CaseInsensitive));
        entries[1]->trigger();
        QCOMPARE(window.inspect()["armed"].toString(), QString("blur"));
        QVERIFY(window.findChild<FrameView*>()->accessibleDescription().contains("blur", Qt::CaseInsensitive));
    }
    void footerMovesTheSameWidgetsAtMeasuredFit() {
        ReviewWindow window(options(), config()); window.show();
        QVERIFY(putgd::test::waitForWindowExposed(&window));
        auto* output = window.findChild<QToolButton*>("uploadDestinationMenu");
        auto* audio = window.findChild<QLabel*>("exportAudioSummary");
        auto* save = buttonNamed(window, "Save a copy…");
        auto* discard = window.findChild<QPushButton*>("discard");
        auto* grid = qobject_cast<QGridLayout*>(window.findChild<QWidget*>("reviewActions")->layout());
        QVERIFY(output && audio && save && discard && grid);
        for (const int width : {1366, 640, 1366}) {
            window.resize(width, 720);
            for (int i = 0; i < 3; ++i) QApplication::processEvents();
            QCOMPARE(grid->property("singleRow").toBool(), width == 1366);
            QVERIFY(output->text().contains("Shared link"));
            QVERIFY(output->toolTip().contains(options().destination));
            QCOMPARE(save->text(), QString("Save a copy…"));
            QCOMPARE(discard->text(), QString("Discard…"));
            const int outputY = output->mapTo(&window, output->rect().center()).y();
            QCOMPARE(audio->mapTo(&window, audio->rect().center()).y(), outputY);
            if (width == 1366) QCOMPARE(save->mapTo(&window, save->rect().center()).y(), outputY);
            else QVERIFY(save->mapTo(&window, save->rect().center()).y() > outputY);
        }
    }
    void maskLanePagingPreservesSelectionAndDoesNotWrap() {
        ReviewWindow window(options(), config());
        RecordingEdit edit;
        for (int i = 0; i < 8; ++i) edit.masks.append(mask(QString::number(i), "solid", 0, duration));
        window.setEdit(edit); window.show();
        QVERIFY(putgd::test::waitForWindowExposed(&window));
        auto* next = window.findChild<QToolButton*>("showMoreMasks");
        auto* previous = window.findChild<QToolButton*>("previousMaskLanes");
        auto* scroll = window.findChild<QScrollBar*>("maskLaneScroll");
        QVERIFY(next && next->isVisible() && previous && previous->isVisible());
        QVERIFY(scroll && !scroll->isVisible());
        QVERIFY(window.drive({{"action", "selectMask"}, {"maskId", "0"}}).code.isEmpty());
        QCOMPARE(window.inspect()["timeline"].toObject()["laneSummary"].toString(), QString("Lanes 1–2 of 8"));
        QVERIFY(!previous->isEnabled());
        for (const int first : {2, 4, 6, 6}) {
            QVERIFY(window.drive({{"action", "nextMaskLanes"}}).code.isEmpty());
            QCOMPARE(scroll->value(), first);
            QCOMPARE(window.inspect()["timeline"].toObject()["laneSummary"].toString(), QString("Lanes %1–%2 of 8").arg(first + 1).arg(first + 2));
            QCOMPARE(window.inspect()["selectedMaskId"].toString(), QString("0"));
            window.drive({{"action", "seek"}, {"time", double(first * 100000)}});
            QCOMPARE(scroll->value(), first);
        }
        QVERIFY(!next->isEnabled());
        previous->click(); QCOMPARE(scroll->value(), 4);
        QVERIFY(window.drive({{"action", "previousMaskLanes"}}).code.isEmpty()); QCOMPARE(scroll->value(), 2);
        QCOMPARE(window.inspect()["selectedMaskId"].toString(), QString("0"));
        // Selecting a different mask still reveals its lane through session/CLI/MCP.
        QVERIFY(window.drive({{"action", "selectMask"}, {"maskId", "7"}}).code.isEmpty());
        QVERIFY(!timeline(window)->maskRect("7").isEmpty());
        QVERIFY(timeline(window)->maskRect("7").height() >= 20);
    }
    void maskOptionsExposeRadiusAndDeletionThroughTheSameDrive_data() {
        QTest::addColumn<double>("scale");
        QTest::newRow("standard") << 1.0;
        QTest::newRow("150%") << 1.5;
    }
    void maskOptionsExposeRadiusAndDeletionThroughTheSameDrive() {
        QFETCH(double, scale);
        const auto original = QApplication::font();
        const auto restore = qScopeGuard([&] { QApplication::setFont(original); });
        auto font = original;
        if (font.pointSizeF() > 0) font.setPointSizeF(font.pointSizeF() * scale);
        else font.setPixelSize(qRound(font.pixelSize() * scale));
        QApplication::setFont(font);
        ReviewWindow window(options(), config()); window.setEdit(twoMasks()); window.show();
        QVERIFY(putgd::test::waitForWindowExposed(&window));
        const auto geometry = window.geometry();
        QVERIFY(window.drive({{"action", "selectMask"}, {"maskId", "m-blur"}}).code.isEmpty());
        QVERIFY(window.drive({{"action", "showMaskOptions"}}).state["maskOptions"].toObject()["open"].toBool());
        auto* menu = window.findChild<QMenu*>("maskOptionsMenu");
        QVERIFY(menu && menu->isVisible());
        auto* radius = menu->findChild<QSpinBox*>(); QVERIFY(radius && radius->isVisible());
        QToolButton* remove = nullptr;
        for (auto* button : menu->findChildren<QToolButton*>()) if (button->accessibleName() == "Delete mask") remove = button;
        QVERIFY(remove && remove->isVisible());
        QVERIFY(remove->width() - remove->iconSize().width() - 32 >= std::ceil(QFontMetricsF(remove->font()).horizontalAdvance(remove->text())));
        QSignalSpy edits(&window, &ReviewWindow::editProposed);
        QVERIFY(window.drive({{"action", "strength"}, {"strength", 24}}).code.isEmpty());
        QCOMPARE(edits.size(), 1);
        QVERIFY(!window.drive({{"action", "closeMaskOptions"}}).state["maskOptions"].toObject()["open"].toBool());
        QCOMPARE(window.geometry(), geometry);
    }
    void maskBadgesFollowHoverSelectionAndPlayback() {
        FrameView frame;
        frame.setCanvas({640, 360}); frame.resize(640, 360);
        QImage media(640, 360, QImage::Format_RGB32); media.fill(QColor(130, 60, 120));
        frame.setFrame(media, 0);
        auto first = mask("first", "solid", 0, duration); first.rect = QRect(30, 60, 230, 140);
        auto second = first; second.id = "second"; second.rect.moveLeft(350);
        frame.setMasks({first, second}, first.id); frame.show(); QApplication::processEvents();
        QTest::mouseMove(&frame, QPoint(10, 10));
        const auto idle = frame.grab().toImage().convertToFormat(media.format());
        const QRect afterNumber(390, 66, 60, 10);
        QCOMPARE(idle.copy(afterNumber), media.copy(afterNumber));
        QTest::mouseMove(&frame, second.rect.center());
        const auto hovered = frame.grab().toImage().convertToFormat(media.format());
        QVERIFY(hovered.copy(afterNumber) != idle.copy(afterNumber));
        frame.setPlaying(true);
        QCOMPARE(frame.grab().toImage().convertToFormat(media.format()), media);
        frame.setPlaying(false);
        QVERIFY(frame.grab().toImage().convertToFormat(media.format()) != media);
    }
    void controlsStayInsideTheWindowAndApart_data() {
        QTest::addColumn<double>("scale");
        QTest::newRow("standard") << 1.0;
        QTest::newRow("150%") << 1.5;
    }
    void controlsStayInsideTheWindowAndApart() {
        QFETCH(double, scale);
        const auto original = QApplication::font();
        const auto restore = qScopeGuard([&] { QApplication::setFont(original); });
        auto font = original;
        if (font.pointSizeF() > 0) font.setPointSizeF(font.pointSizeF() * scale);
        else font.setPixelSize(qRound(font.pixelSize() * scale));
        QApplication::setFont(font);
        auto o = options(); o.audioRecorded = true; o.durationUs = 32000000;
        auto edit = twoMasks(); edit.trimStart = 2000000; edit.trimEnd = 29000000;
        ReviewWindow window(o, config()); window.setEdit(edit); window.show();
        QVERIFY(putgd::test::waitForWindowExposed(&window));
        const auto settle = [&] { for (int i = 0; i < 3; ++i) { QApplication::processEvents(); QApplication::sendPostedEvents(); } };
        const auto check = [&](const QString& state) {
            settle();
            const auto where = [&](QWidget* w) { return QStringLiteral("(min %1x%2 hint %3x%4) ").arg(window.minimumWidth()).arg(window.minimumHeight()).arg(window.minimumSizeHint().width()).arg(window.minimumSizeHint().height()) + QStringLiteral("%1 %2 %3 at %4x%5: %6,%7 %8x%9").arg(state, QString::fromLatin1(w->metaObject()->className()), w->objectName() + "/" + w->accessibleName()).arg(window.width()).arg(window.height()).arg(w->mapTo(&window, QPoint()).x()).arg(w->mapTo(&window, QPoint()).y()).arg(w->width()).arg(w->height()); };
            // The frameless window's resize edges lie over its border by design.
            const auto edge = [&](QWidget* w) { return w->parentWidget() == &window && QList<Qt::CursorShape>{Qt::SizeHorCursor, Qt::SizeVerCursor, Qt::SizeFDiagCursor, Qt::SizeBDiagCursor}.contains(w->cursor().shape()); };
            QList<QWidget*> shown;
            for (auto* w : window.findChildren<QWidget*>()) if (w->isVisible() && !w->isWindow() && !edge(w)) shown.append(w);
            for (auto* w : shown) {
                QVERIFY2(window.contentsRect().contains(QRect(w->mapTo(&window, QPoint()), w->size())), qPrintable(where(w)));
                QVERIFY2(w->parentWidget()->rect().contains(w->geometry()), qPrintable(where(w)));
            }
            for (int i = 0; i < shown.size(); ++i) for (int j = i + 1; j < shown.size(); ++j) {
                auto *a = shown[i], *b = shown[j];
                if (a->isAncestorOf(b) || b->isAncestorOf(a)) continue;
                const QRect first(a->mapTo(&window, QPoint()), a->size()), second(b->mapTo(&window, QPoint()), b->size());
                QVERIFY2(!first.intersects(second), qPrintable(where(a) + " overlaps " + where(b)));
            }
        };
        for (const bool minimum : {false, true}) {
            if (minimum) { for (int i = 0; i < 3; ++i) { window.resize(window.minimumSizeHint()); settle(); } }
            const QString size = minimum ? QStringLiteral("minimum") : QStringLiteral("default");
            window.drive({{"action", "selectMask"}, {"maskId", "m-solid"}});
            check(size + " steady");
            window.drive({{"action", "selectMask"}, {"maskId", "m-blur"}});
            check(size + " blur");
            window.drive({{"action", "format"}, {"format", "mp4"}, {"codec", "h265"}});
            window.showError("Export failed: the encoder stopped after 412 frames because the disk holding the private recording is full; free some space and try again");
            check(size + " error");
            window.drive({{"action", "dismissStatus"}});
            window.drive({{"action", "format"}, {"format", "mp4"}, {"codec", "h264"}});
        }
    }
    void formatAdviceSitsBesideTheFormatAndRecoveryIsBordered() {
        ReviewWindow window(options(), config()); window.show();
        QVERIFY(putgd::test::waitForWindowExposed(&window));
        window.drive({{"action", "format"}, {"format", "mp4"}, {"codec", "h265"}});
        auto* advice = window.findChild<QToolButton*>("formatAdvice");
        QComboBox* format = nullptr;
        for (auto* combo : window.findChildren<QComboBox*>()) if (combo->accessibleName() == "Output format") format = combo;
        QVERIFY(advice && format);
        QVERIFY(advice->isVisible());
        QCOMPARE(advice->mapTo(&window, advice->rect().center()).y(), format->mapTo(&window, format->rect().center()).y());
        QVERIFY(advice->mapTo(&window, QPoint()).x() > format->mapTo(&window, format->rect().topRight()).x());
        window.showError("No space left on device");
        auto* retry = buttonNamed(window, "Try again");
        QVERIFY(retry && retry->isVisible());
        QVERIFY(retry->property("visualRole").toString() != "link");
        QSignalSpy retried(&window, &ReviewWindow::retryExportRequested);
        retry->click(); QCOMPARE(retried.size(), 1);
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
    void initTestCase() {
        // A case must never restore Qt's pre-install font after Graphite's lazy
        // install: later data rows would then measure smaller text.
        Fonts::load();
        Graphite::install(*qApp);
        baselineFont_ = QApplication::font();
        baselineScheme_ = Theme::scheme();
    }
    void init() {
        QApplication::setFont(baselineFont_);
        Theme::setScheme(baselineScheme_);
        Graphite::WindowHeader::setCustomFrameOverride(1);
    }
    void cleanup() {
        QApplication::setFont(baselineFont_);
        Theme::setScheme(baselineScheme_);
        Graphite::WindowHeader::setCustomFrameOverride(1);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
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
            RegionOverlay screenshot({frame}, nullptr, "Uploads immediately to Proton Drive");
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
            if (!recording) QVERIFY(surface->accessibleDescription().endsWith("Uploads immediately to Proton Drive"));
            if (mode == 0) {
                QTest::keyClick(surface, Qt::Key_Return);
                QCOMPARE(selected, QRectF(screen));
            } else if (mode == 4) {
                QTest::mousePress(surface, Qt::LeftButton, {}, {100, 100});
                QTest::mouseMove(surface, {300, 280});
                QVERIFY(surface->accessibleDescription().contains(recording ? "Release to choose recording area" : "Release to capture"));
                QVERIFY(!surface->accessibleDescription().contains("Enter to"));
                QTest::mouseRelease(surface, Qt::LeftButton, {}, {300, 280});
                QCOMPARE(selected, QRectF(QPointF(screen.topLeft()) + QPointF(100, 100), QSizeF(200, 180)));
            } else {
                QTest::keyClick(surface, Qt::Key_Right);
                QVERIFY(surface->accessibleDescription().contains(recording ? "Enter to choose" : "Enter to capture"));
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
        QVERIFY(surface->accessibleDescription().contains("Display 2 · Entire display 320 × 240 px · "));
        QTest::keyClick(surface,Qt::Key_Right);
        QTest::keyClick(surface,Qt::Key_Backtab);
        QVERIFY(surface->accessibleDescription().contains("Display 1 · "));
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
        QVERIFY(frame->accessibleDescription().contains("Applies from 0:04.0 to 0:10.0"));
        QTest::keyClick(frame, Qt::Key_Right);
        QVERIFY(frame->accessibleDescription().contains("at 101, 80"));
        frame->commitNudge();
        auto edit = twoMasks(4); edit.masks[1].end = 11000000; window.setEdit(edit);
        QVERIFY(frame->accessibleDescription().contains("to 0:11.0"));
    }
    void concurrentMasksHaveDistinctScrollableLanes() {
        ReviewTimeline t; t.setDuration(duration);
        QList<RecordingMask> masks;
        for (int i = 0; i < 8; ++i) masks.append(mask(QString::number(i), "solid", 1000000, 10000000));
        t.setMasks(masks, "0"); t.resize(600, t.sizeHint().height()); t.show();
        QCOMPARE(t.lanes(), 8);
        auto* scroll = t.findChild<QScrollBar*>("maskLaneScroll");
        QVERIFY(scroll && !scroll->isVisible()); QCOMPARE(scroll->maximum(), 6);
        QVERIFY(t.maskHitRect("0").center().y() < t.maskHitRect("1").center().y());
        QVERIFY(t.maskRect("7").isEmpty());
        const int boundedHeight = t.sizeHint().height();
        t.setMasks(masks, "7"); QCOMPARE(scroll->value(), 6);
        QVERIFY(!t.maskRect("7").isEmpty()); QVERIFY(t.maskRect("0").isEmpty());
        QCOMPARE(t.sizeHint().height(), boundedHeight);
        QSignalSpy selected(&t, &ReviewTimeline::maskSelected);
        scroll->setValue(6);
        QTest::mouseClick(&t, Qt::LeftButton, {}, t.maskRect("6").center());
        QCOMPARE(selected.size(), 1); QCOMPARE(selected.first()[0].toString(), QString("6"));
        // A lane can hold several non-overlapping masks; count hidden masks.
        t.setMasks({mask("a", "solid", 0, 5000000), mask("b", "solid", 0, 5000000), mask("c", "solid", 0, 5000000), mask("d", "solid", 6000000, 10000000)}, "a");
        QCOMPARE(t.findChild<QToolButton*>("showMoreMasks")->accessibleName(), QString("Next mask lanes"));
        scroll->setValue(1);
        QCOMPARE(t.findChild<QToolButton*>("showMoreMasks")->accessibleName(), QString("Next mask lanes"));
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
        auto* target = window.findChild<QToolButton*>("uploadDestinationMenu");
        auto* upload = window.findChild<QPushButton*>("upload");
        auto* audio = window.findChild<QLabel*>("exportAudioSummary");
        QVERIFY(target && target->isVisible() && target->toolTip().contains(o.uploadSummary));
        auto* grid = qobject_cast<QGridLayout*>(window.findChild<QWidget*>("reviewActions")->layout());
        QVERIFY(grid);
        QVERIFY(target->text().contains("Shared link"));
        if (grid->property("singleRow").toBool()) QCOMPARE(target->mapTo(&window, target->rect().center()).y(), upload->mapTo(&window, upload->rect().center()).y());
        else QVERIFY(target->mapTo(&window, target->rect().bottomLeft()).y() < upload->mapTo(&window, QPoint()).y());
        QVERIFY(window.rect().contains(QRect(target->mapTo(&window, QPoint()), target->size())));
        auto* full = window.findChild<QLabel*>("uploadDestinationWrapped");
        QVERIFY(full && !full->isVisible());
        target->menu()->popup(target->mapToGlobal(QPoint(0, target->height())));
        QApplication::processEvents();
        QVERIFY(full->isVisible());
        QVERIFY(full->text().contains(o.destination));
        target->menu()->close();
        QVERIFY(audio->isVisible());
        QVERIFY(!QRect(audio->mapTo(&window, QPoint()), audio->size()).intersects(QRect(target->mapTo(&window, QPoint()), target->size())));
        QVERIFY(audio->mapTo(&window, audio->rect().topRight()).x() <= window.width() - 8);
        QVERIFY(state["audio"].toString().endsWith("Preview muted"));
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
    void timelineTargetsAre32PixelsAndNearestHandleWins_data() {
        QTest::addColumn<double>("scale");
        QTest::newRow("standard") << 1.0;
        QTest::newRow("150%") << 1.5;
    }
    void timelineTargetsAre32PixelsAndNearestHandleWins() {
        QFETCH(double, scale);
        const auto original = QApplication::font();
        const auto restore = qScopeGuard([&] { QApplication::setFont(original); });
        auto font = original;
        if (font.pointSizeF() > 0) font.setPointSizeF(font.pointSizeF() * scale);
        else font.setPixelSize(qRound(font.pixelSize() * scale));
        QApplication::setFont(font);
        ReviewTimeline t;
        t.setDuration(duration);
        t.setTrim(5000000, 5200000);
        t.setMasks({mask("one", "solid", 1000000, 2000000), mask("two", "solid", 1500000, 1900000)}, "one");
        t.resize(600, t.sizeHint().height());
        t.show();
        const auto first = t.maskHitRect("one"), second = t.maskHitRect("two");
        QVERIFY(first.height() >= 40);
        QVERIFY(second.height() >= 40);
        QVERIFY(t.rect().contains(first));
        QVERIFY(t.rect().contains(second));
        // Compact lanes retain 32 px targets; the nearest lane wins where they overlap.
        QSignalSpy selected(&t, &ReviewTimeline::maskSelected);
        QTest::mouseClick(&t, Qt::LeftButton, {}, second.center());
        QCOMPARE(selected.size(), 1);
        QCOMPARE(selected.last().first().toString(), QString("two"));
        const auto overlap = first.intersected(second);
        QVERIFY(!overlap.isEmpty());
        QTest::mouseClick(&t, Qt::LeftButton, {}, QPoint(overlap.center().x(), overlap.top()));
        QCOMPARE(selected.last().first().toString(), QString("one"));
        QTest::mouseClick(&t, Qt::LeftButton, {}, QPoint(overlap.center().x(), overlap.bottom()));
        QCOMPARE(selected.last().first().toString(), QString("two"));
        auto* scroll = t.findChild<QScrollBar*>("maskLaneScroll");
        QVERIFY(scroll->isHidden());
        const auto secondTarget = t.maskHitRect("two");
        QVERIFY(secondTarget.height() >= 40);
        QVERIFY(!t.maskHitRect("one").isEmpty());
        QVERIFY(secondTarget.top() > t.trackRect().center().y());
        scroll->setValue(0);
        QVERIFY(first.top() >= 0);
        QSignalSpy trim(&t, &ReviewTimeline::trimChanged);
        drag(&t, QPoint(t.xFor(5200000) + 12, 2), QPoint(t.xFor(7000000), 2));
        QCOMPARE(trim.size(), 1);
        QCOMPARE(trim.last()[0].toLongLong(), 5000000);
        QVERIFY(trim.last()[1].toLongLong() > 6000000);

        t.setMasks({mask("one", "solid", 1000000, 2000000), mask("two", "solid", 1500000, 1900000)}, "one");
        QSignalSpy retimed(&t, &ReviewTimeline::maskRetimed);
        const auto bar = t.maskRect("one");
        drag(&t, QPoint(bar.left() - 12, bar.center().y()), QPoint(t.xFor(500000), bar.center().y()));
        QCOMPARE(retimed.size(), 1);
        QCOMPARE(retimed.last()[0].toString(), QString("one"));
    }
    void destinationCanBeRevealedByKeyboardAndClick() {
        auto o = options(); o.destination = QString("Long project archive ").repeated(12); o.sharingMode = "private";
        o.uploadSummary = "Private upload to " + o.destination;
        ReviewWindow window(o, config()); window.show();
        QVERIFY(putgd::test::waitForWindowExposed(&window));
        auto* button = window.findChild<QToolButton*>("uploadDestinationMenu");
        QVERIFY(button && button->focusPolicy() & Qt::TabFocus);
        bool keyboardOpened = false;
        QTimer::singleShot(0, button, [&] { keyboardOpened = button->menu()->isVisible(); QTest::keyClick(button->menu(), Qt::Key_Escape); });
        button->setFocus(Qt::TabFocusReason); QTest::keyClick(button, Qt::Key_Space);
        QVERIFY(keyboardOpened);
        auto* value = window.findChild<QLabel*>("uploadDestinationWrapped");
        QVERIFY(value->text().startsWith("Destination: " + o.destination + "\nSharing: Private upload"));
        QVERIFY(value->textInteractionFlags() & Qt::TextSelectableByMouse);
        QVERIFY(value->textInteractionFlags() & Qt::TextSelectableByKeyboard);
        QTest::keyClick(button->menu(), Qt::Key_Escape);
        QVERIFY(!button->menu()->isVisible());
        button->menu()->popup(button->mapToGlobal(QPoint(0, button->height())));
        PUTGD_TRY_VERIFY(button->menu()->isVisible());
        button->menu()->close();

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
        // The compact header keeps the name and upload state on one line.
        auto* name = header->findChild<QWidget*>("documentName");
        QVERIFY(name && name->isVisible());
        QCOMPARE(name->accessibleName(), QStringLiteral("Review recording"));
        auto* state = header->findChild<QLabel*>("documentState");
        QVERIFY(!state || !state->isVisible());
        auto* draft = window.findChild<QLabel*>("draftState");
        QVERIFY(draft && draft->isVisible() && draft->text() == QStringLiteral("Not uploaded"));
        QVERIFY(header->isAncestorOf(draft));
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
        QVERIFY(window.width() <= 1342 && window.height() <= 720);
        QVERIFY(window.width() >= window.minimumSizeHint().width());
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
        QVERIFY(!window.findChild<QWidget*>("statusRow")->isVisible());
        qInfo("review with in-row guidance share %.3f", window.inspect()["video"].toObject()["share"].toDouble());
        QVERIFY2(window.inspect()["video"].toObject()["share"].toDouble() >= 0.53, qPrintable(QString::number(window.inspect()["video"].toObject()["share"].toDouble())));
        auto* row = window.findChild<QWidget*>("statusRow");
        auto* frame = window.findChild<FrameView*>();
        QVERIFY(!row->isVisible());
        QVERIFY(window.findChild<QWidget*>("maskControls")->mapTo(&window, QPoint()).y() >= frame->geometry().bottom());
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
            for (auto* label : window.findChild<QWidget*>("statusRow")->findChildren<QLabel*>("status")) if (label->isVisible())
                QVERIFY2(label->contentsRect().width() >= label->fontMetrics().horizontalAdvance(label->text()), qPrintable(QString("Blur guidance wraps: %1 < %2").arg(label->contentsRect().width()).arg(label->fontMetrics().horizontalAdvance(label->text()))));
            QCOMPARE(window.geometry(), userGeometry);
            QVERIFY2(window.inspect()["video"].toObject()["share"].toDouble() >= 0.53, qPrintable(QString::number(window.inspect()["video"].toObject()["share"].toDouble())));
            auto* status = window.findChild<QWidget*>("statusRow");
            QVERIFY(!status->isVisible());
            QVERIFY(window.inspect()["blurGuidance"].toObject()["visible"].toBool());
            QVERIFY(window.drive({{"action", "dismissBlurTip"}}).code.isEmpty());
            QApplication::processEvents();
            QVERIFY(window.inspect()["video"].toObject()["share"].toDouble() >= 0.60);
            // The compact header grows with the text instead of clipping it.
            auto* header = window.findChild<Graphite::WindowHeader*>("windowHeader");
            QVERIFY(header && header->isVisible() && header->height() == header->compactHeight());
            auto* name = header->findChild<QWidget*>("documentName");
            QVERIFY(name && name->height() >= name->fontMetrics().height() && name->y() >= 0 && name->y() + name->height() <= header->height());
            for (const auto& text : {QString("Save a copy…"), QString("Discard…"), QString("Upload")}) {
                auto* action = buttonNamed(window, text); QVERIFY(action);
                const auto font = action->font(); QCOMPARE(font.weight(), QFont::Medium);
                const int icon = action->icon().isNull() ? 0 : action->iconSize().width() + 8;
                QVERIFY(action->width() >= QFontMetrics(font).horizontalAdvance(text) + icon + 24);
            }
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
            for (auto* label : window.findChild<QWidget*>("statusRow")->findChildren<QLabel*>("status")) if (label->isVisible())
                qInfo("blur copy %d px in %d px; status %d px, frame %d px, window %dx%d", label->fontMetrics().horizontalAdvance(label->text()), label->contentsRect().width(), window.findChild<QWidget*>("statusRow")->height(), window.findChild<FrameView*>()->height(), window.width(), window.height());
            QVERIFY2(steady >= 0.60, qPrintable(QString::number(steady)));
            QVERIFY2(transient >= 0.53, qPrintable(QString::number(transient)));
            // Small user-chosen windows retain their geometry; frame gates apply at defaults.
            window.resize(ReviewWindow::defaultSize()); QApplication::processEvents();
            window.drive({{"action", "format"}, {"format", "mp4"}, {"codec", "h265"}});
            window.showError("Export failed: the encoder stopped after 412 frames because the disk holding the private recording is full; free some space and try again");
            PUTGD_TRY_VERIFY(window.inspect()["status"].toString().contains("Disk is full. Free up space, then try again."));
            qInfo("visible recovery frame share %.3f at %dx%d", window.inspect()["video"].toObject()["share"].toDouble(), window.width(), window.height());
            qInfo("recovery status %d px, frame %d px", window.findChild<QWidget*>("statusRow")->height(), window.findChild<FrameView*>()->height());
            QVERIFY(window.inspect()["video"].toObject()["share"].toDouble() >= 0.45);
            window.resize(ReviewWindow::defaultSize());
            PUTGD_TRY_VERIFY2(window.height() <= 768 && window.inspect()["status"].toString().contains("Free up space, then try again."),
                qPrintable(QString("Window %1x%2 minimum %3x%4 frame %5 status %6 share %7").arg(window.width()).arg(window.height()).arg(window.minimumSizeHint().width()).arg(window.minimumSizeHint().height()).arg(window.findChild<FrameView*>()->height()).arg(window.findChild<QWidget*>("statusRow")->height()).arg(window.inspect()["video"].toObject()["share"].toDouble())));
        }
    }
    void enlargedPreviewSequenceKeepsFrameGates() {
        const auto original = QApplication::font();
        const auto scheme = Theme::scheme();
        const auto restore = qScopeGuard([&] { QApplication::setFont(original); Theme::setScheme(scheme); });
        // Match the preview's 21 px body, including rounding from the 14 px base.
        auto enlarged = original; enlarged.setPixelSize(21); QApplication::setFont(enlarged);
        for (const auto theme : {Theme::Scheme::Dark, Theme::Scheme::Light}) for (const int height : {720, 680}) {
            Theme::setScheme(theme);
            auto o = options(); o.durationUs = 32000000; o.canvas = QSize(960, 600); o.audioRecorded = true; o.availableSize = QSize(1366, height);
            auto c = config(); c.audio.system = true;
            RecordingEdit edit; edit.trimStart = 2000000; edit.trimEnd = 29000000;
            edit.masks = {mask("solid", "solid", 8000000, 21000000), mask("blur", "blur", 3000000, 12000000)};
            ReviewWindow window(o, c); window.setEdit(edit); window.show();
            QVERIFY(putgd::test::waitForWindowExposed(&window));
            QVERIFY(QFontInfo(window.findChild<QLabel*>("trimGroup")->font()).pixelSize() >= 18);
            const auto geometry = window.geometry();
            window.drive({{"action", "selectMask"}, {"maskId", "solid"}});
            QApplication::processEvents();
            qInfo("sequence steady %dx%d share %.3f frame %d mask %d status %d timeline %d", window.width(), window.height(), window.inspect()["video"].toObject()["share"].toDouble(), window.findChild<FrameView*>()->height(), window.findChild<QWidget*>("maskControls")->height(), window.findChild<QWidget*>("statusRow")->height(), timeline(window)->height());

            QVERIFY2(window.inspect()["video"].toObject()["share"].toDouble() >= 0.60, qPrintable(QString::number(window.inspect()["video"].toObject()["share"].toDouble())));
            window.drive({{"action", "selectMask"}, {"maskId", "blur"}});
            QApplication::processEvents();
            qInfo("sequence transient %dx%d share %.3f frame %d status %d footer %d", window.width(), window.height(), window.inspect()["video"].toObject()["share"].toDouble(), window.findChild<FrameView*>()->height(), window.findChild<QWidget*>("statusRow")->height(), window.findChild<QWidget*>("reviewFooter")->height());
            for (auto* group : window.findChild<QWidget*>("maskControls")->findChildren<QWidget*>(QString(), Qt::FindDirectChildrenOnly)) if (group->isVisible())
                qInfo("mask group %s %dx%d at %d,%d", qPrintable(group->objectName()), group->width(), group->height(), group->x(), group->y());
            qInfo("mask row %d; desired %d", window.findChild<QWidget*>("maskControls")->height(), window.findChild<QWidget*>("maskControls")->heightForWidth(window.findChild<QWidget*>("maskControls")->width()));
            for (auto* label : window.findChild<QWidget*>("statusRow")->findChildren<QLabel*>("status")) if (label->isVisible())
                QVERIFY2(label->contentsRect().width() >= QFontMetricsF(label->font()).horizontalAdvance(label->text()), "Blur guidance must stay on one line in a constrained workspace");
            QVERIFY(window.inspect()["video"].toObject()["share"].toDouble() >= 0.53);
            window.drive({{"action", "format"}, {"format", "mp4"}, {"codec", "h265"}});
            window.showError("Export failed: disk full");
            QApplication::processEvents();
            qInfo("enlarged preview sequence %dx%d recovery %.3f", window.width(), window.height(), window.inspect()["video"].toObject()["share"].toDouble());
            QVERIFY(window.inspect()["video"].toObject()["share"].toDouble() >= 0.45);
            QCOMPARE(window.geometry(), geometry);
            for (auto* button : window.findChild<QWidget*>("statusRow")->findChildren<QPushButton*>("statusAction")) {
                QVERIFY2(button->width() >= button->fontMetrics().horizontalAdvance(button->text()) + 28, qPrintable(button->text()));
            }
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
            PUTGD_TRY_VERIFY(window.inspect()["status"].toString().contains("Disk is full. Free up space, then try again."));
            qInfo("visible recovery frame share %.3f at %dx%d", window.inspect()["video"].toObject()["share"].toDouble(), window.width(), window.height());
            QVERIFY(window.inspect()["video"].toObject()["share"].toDouble() >= 0.45);
            QVERIFY(!window.inspect()["status"].toString().contains(Graphite::blurWarningText()));
            QVERIFY(!buttonNamed(window, "Use Redact"));
            QVERIFY(window.inspect()["blurGuidance"].toObject()["visible"].toBool());
            QVERIFY(window.inspect()["errorDetails"].toObject()["visible"].toBool());
            QCOMPARE(window.inspect()["errorDetails"].toObject()["message"].toString(), failure);
            for (auto* w : window.findChildren<QWidget*>()) if (w->isVisible() && !w->isWindow())
                QVERIFY2(window.rect().contains(QRect(w->mapTo(&window, QPoint()), w->size())), qPrintable(w->accessibleName() + w->objectName()));
            QVERIFY(window.drive({{"action", "showAdvice"}}).state["adviceDialog"].toObject()["open"].toBool());
            auto* details = window.findChild<QWidget*>("reviewAdviceText");
            QVERIFY(details);
            PUTGD_TRY_VERIFY(details->hasFocus());
            QVERIFY(details->property("plainText").toString().contains(failure));
            QVERIFY(!buttonNamed(window, "Use MP4 (H.264)"));
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
            QCOMPARE(window.findChild<QLabel*>("exportAudioSummary")->accessibleName(), QString("No audio in %1 · Preview muted").arg(QString(format).toUpper()));
            QVERIFY(window.drive({{"action", "format"}, {"format", "mp4"}, {"codec", "h264"}}).code.isEmpty());
            QVERIFY(qvariant_cast<RecordingConfig>(changed.last().first()).audio.system);
            QCOMPARE(window.findChild<QLabel*>("exportAudioSummary")->accessibleName(), QString("System audio included · Preview muted"));
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
        // Before the mask: when it starts; inside it: the interval alone; after it: not at the playhead.
        QCOMPARE(label->text(), QString("0:05.0–0:20.0 · Starts at 0:05.0"));
        QCOMPARE(window.inspect()["selectedMaskInterval"].toObject()["text"].toString(), label->text());
        const auto geometry = label->geometry();
        window.drive({{"action", "seek"}, {"time", 8000000}});
        QCOMPARE(label->text(), QString("0:05.0–0:20.0"));
        QCOMPARE(label->geometry(), geometry);
        window.drive({{"action", "seek"}, {"time", 25000000}});
        QCOMPARE(label->text(), QString("0:05.0–0:20.0 · Not at playhead"));
        QCOMPARE(label->geometry(), geometry);
        QVERIFY(!label->text().contains("Inactive"));
        window.drive({{"action", "seek"}, {"time", 0}});
        const auto position = label->pos();
        QTest::keyClick(window.findChild<FrameView*>(), Qt::Key_Right, Qt::ControlModifier);
        QCOMPARE(label->pos(), position);
        QVERIFY(!window.findChild<FrameView*>()->isAncestorOf(label));
        QCOMPARE(label->text(), QString("0:05.0–0:20.0 · Starts at 0:05.0"));
        QCOMPARE(QFontInfo(label->font()).family(), QFontInfo(Graphite::font(Graphite::Type::metadata)).family());
        edit.revision++; edit.trimStart = 2000000; edit.trimEnd = 29000000; window.setEdit(edit);
        window.drive({{"action", "seek"}, {"time", 0}});
        QVERIFY(window.inspect()["playbackStatus"].toString().contains("Outside kept part"));
        QCOMPARE(window.findChild<QLabel*>("muted")->text(), QString("Keep 0:02–0:29 · 27 s"));
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
        // Two masks arrive: the list and timing stay inline; secondary controls live in Mask options.
        window.setEdit(twoMasks());
        QCOMPARE(window.inspect()["selectedMaskId"].toString(), QString("m-solid"));
        QCOMPARE(controls(), QJsonObject({{"list", true}, {"times", true}, {"strength", false}, {"remove", false}}));
        auto* list = window.findChild<QComboBox*>("masks");
        QVERIFY(list);
        QCOMPARE(list->count(), 2);
        QCOMPARE(list->itemText(0), QString("Blur 1"));
        list->setCurrentIndex(0);
        QCOMPARE(window.inspect()["selectedMaskId"].toString(), QString("m-blur"));
        QCOMPARE(controls()["strength"].toBool(), false);
        QVERIFY(window.inspect()["maskOptions"].toObject()["blurRadiusAvailable"].toBool());
        QVERIFY(window.drive({{"action", "showMaskOptions"}}).code.isEmpty());
        QCOMPARE(controls()["strength"].toBool(), true);
        QVERIFY(window.drive({{"action", "closeMaskOptions"}}).code.isEmpty());
        // A click on a mask's bar on the timeline selects it.
        auto* t = timeline(window);
        t->findChild<QScrollBar*>("maskLaneScroll")->setValue(1);
        QTest::mouseClick(t, Qt::LeftButton, {}, t->maskRect("m-solid").center());
        QCOMPARE(window.inspect()["selectedMaskId"].toString(), QString("m-solid"));
        // A click outside every mask clears the selection and hides its controls.
        auto* frame = window.findChild<FrameView*>();
        QTest::mouseClick(frame, Qt::LeftButton, {}, frame->imageRect().bottomRight() - QPoint(4, 4));
        QVERIFY(window.inspect()["selectedMaskId"].isNull());
        QCOMPARE(controls(), QJsonObject({{"list", true}, {"times", true}, {"strength", false}, {"remove", false}}));
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
            // Timing actions use words; the remaining tools retain their named glyphs.
            const auto tools = window.findChildren<QToolButton*>();
            QVERIFY(tools.size() >= 9);
            for (auto* b : tools) {
                QVERIFY2((b->toolButtonStyle() == Qt::ToolButtonTextOnly && (QStringList{"Set trim start at playhead", "Set trim end at playhead", "Reset trim", "Set mask start at playhead", "Set mask end at playhead", "Add mask"}.contains(b->accessibleName()) || QStringList{"showMoreMasks", "previousMaskLanes", "uploadDestinationMenu"}.contains(b->objectName()))) || ((!b->icon().isNull() || b->arrowType() != Qt::NoArrow) && (b->toolButtonStyle() == Qt::ToolButtonIconOnly || (b->toolButtonStyle() == Qt::ToolButtonTextBesideIcon && QStringList{"Start", "End", "Delete mask"}.contains(b->text())))), qPrintable(b->accessibleName()));
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
        QCOMPARE(selected.state["maskControls"].toObject()["strength"].toBool(), false);
        QVERIFY(selected.state["maskOptions"].toObject()["blurRadiusAvailable"].toBool());
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
        QCOMPARE(preview["error"].toString(), QString("The frame at 0:00.0 couldn't be decoded (the private recording file is missing)."));
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
        QCOMPARE(preview["error"].toString(), QString("The frame at 0:04.0 couldn't be decoded (the private recording file is missing)."));
        QCOMPARE(preview["frameTimeUs"].toDouble(), 0.0);
        QVERIFY(window.findChild<QLabel*>("previewErrorText")->text().contains(QStringLiteral("earlier frame (0:00.0)")));
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
        QVERIFY(audio->accessibleName().contains("System audio included"));
        QCOMPARE(window.findChild<QLabel*>("exportAudioSummary")->accessibleName(), QString("System audio included · Preview muted"));
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
        QCOMPARE(window.findChild<QLabel*>("exportAudioSummary")->accessibleName(), QString("No audio in GIF · Preview muted"));
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
        QVERIFY(!buttonNamed(*dialog, "Use MP4 (H.264)"));
        QCOMPARE(dialog->findChild<QLabel*>("reviewAdviceText")->property("plainText").toString(), QString("Export failed: disk full"));
        auto* primary = qobject_cast<QPushButton*>(buttonNamed(*dialog, "Try again"));
        QVERIFY(primary && primary->isDefault());
        primary->click(); QCOMPARE(retry.size(), 2);
        QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        window.drive({{"action", "showAdvice"}});
        dialog = window.findChild<QDialog*>("reviewAdvice"); QVERIFY(dialog);
        const QJsonValue errorFacts = window.inspect()["statusFacts"];
        auto* close = buttonNamed(*dialog, "Close"); QVERIFY(close); close->click();
        QCOMPARE(window.inspect()["statusFacts"], errorFacts);
        QVERIFY(!window.inspect()["adviceDialog"].toObject()["open"].toBool());
        QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        const auto tips = window.drive({{"action", "showFormatTips"}});
        QVERIFY(tips.code.isEmpty());
        QVERIFY(!tips.state["adviceDialog"].toObject()["text"].toString().contains("disk full"));
        dialog = window.findChild<QDialog*>("reviewAdvice"); QVERIFY(dialog);
        QVERIFY(buttonNamed(*dialog, "Use MP4 (H.264)"));
        buttonNamed(*dialog, "Close")->click();
        QCOMPARE(window.inspect()["statusFacts"], errorFacts);
        window.drive({{"action", "dismissStatus"}});
        QVERIFY(window.inspect()["statusFacts"] != errorFacts);
        window.showError("Couldn't open the folder. Copy the path and open it in your file manager.", false);
        QVERIFY(!buttonNamed(window, "Try again"));
        QVERIFY(!window.drive({{"action", "retryExport"}}).code.isEmpty());
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
        window.setEdit(twoMasks()); window.show();
        QVERIFY(putgd::test::waitForWindowExposed(&window));
        QVERIFY(window.inspect()["blurGuidance"].toObject()["visible"].toBool());
        auto* list = window.findChild<QComboBox*>("masks");
        QCOMPARE(list->itemText(1), QString("Redaction 2"));
        QVERIFY(window.drive({{"action", "selectMask"}, {"maskId", "m-blur"}}).code.isEmpty());
        const auto frame = window.findChild<FrameView*>()->geometry();
        QVERIFY(window.drive({{"action", "showMaskOptions"}}).code.isEmpty());
        auto* fix = buttonNamed(window, "Use Redact");
        QVERIFY(fix);
        fix->click();
        QCOMPARE(proposed(proposals)["maskId"].toString(), QString("m-blur"));
        QCOMPARE(proposed(proposals)["mask"].toObject()["kind"].toString(), QString("solid"));
        QVERIFY(!proposed(proposals)["mask"].toObject().contains("strength"));
        QCOMPARE(window.findChild<FrameView*>()->geometry(), frame);
        window.drive({{"action", "dismissBlurTip"}});
        window.drive({{"action", "selectMask"}, {"maskId", QJsonValue(QJsonValue::Null)}});
        auto* add = window.findChild<QToolButton*>("addMask");
        add->menu()->actions()[1]->trigger();
        QVERIFY(!window.inspect()["blurGuidance"].toObject()["visible"].toBool());
        QVERIFY(window.drive({{"action", "useSolidCover"}}).state["armed"].toString() == "solid");
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
        auto* add = window.findChild<QToolButton*>("addMask");
        QVERIFY(add && add->menu());
        add->setFocus(Qt::TabFocusReason);
        // The actual menu is keyboard navigable, then Enter on the frame places it.
        QTimer::singleShot(0, add, [add] {
            add->menu()->setActiveAction(add->menu()->actions().first());
            QTest::keyClick(add->menu(), Qt::Key_Return);
        });
        add->showMenu();
        QCOMPARE(window.inspect()["armed"].toString(), QString("solid"));
        PUTGD_TRY_VERIFY(!add->menu()->isVisible());
        window.activateWindow();
        QVERIFY(putgd::test::waitForWindowActive(&window));
        frame->setFocus(Qt::TabFocusReason);
        QApplication::processEvents();
        QTest::keyClick(frame, Qt::Key_Return);
        QVERIFY(!proposals.isEmpty());
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
        QVERIFY(frame->accessibleDescription().contains(QStringLiteral("Redaction 1 selected: 640 × 360 at 320, 180")));
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
    // Review timecodes share one-decimal Sans figures across theme changes.
    void timecodesUseOneSansFormatAndContextIsNamed() {
        ReviewWindow window(options(), config());
        window.show();
        QVERIFY(putgd::test::waitForWindowExposed(&window));
        for (const auto* name : {"time"}) {
            QLabel* label = nullptr;
            for (auto* l : window.findChildren<QLabel*>(name)) if (l->accessibleName() == "Playhead time" || l->accessibleName() == "Kept interval") label = l;
            QVERIFY2(label, name);
            QCOMPARE(QFontInfo(label->font()).family(), QFontInfo(Graphite::font(Graphite::Type::metadata)).family());
            const auto scheme = Theme::scheme();
            Theme::setScheme(scheme == Theme::Scheme::Dark ? Theme::Scheme::Light : Theme::Scheme::Dark);
            QApplication::processEvents();
            QCOMPARE(QFontInfo(label->font()).family(), QFontInfo(Graphite::font(Graphite::Type::metadata)).family());
            QCOMPARE(label->font().pixelSize(), Graphite::font(Graphite::Type::metadata).pixelSize());
            Theme::setScheme(scheme);
            QVERIFY(label->width() >= label->sizeHint().width());
        }
        auto* trim = window.findChild<QLabel*>("trimGroup");
        QVERIFY(trim && trim->isVisible());
        auto* mask = window.findChild<QLabel*>("maskGroup");
        QVERIFY(mask && !mask->isVisible());
        QCOMPARE(mask->text(), QString("Mask"));

    }
    // #259: Save copy… opens the shared chooser; this test never asks a portal.
    void saveCopyDialogIsDrivenAndSavesTheChosenPath() {
        const auto previous = qgetenv("PUTGD_FILE_DIALOG");
        const auto restore = qScopeGuard([&] { if (previous.isNull()) qunsetenv("PUTGD_FILE_DIALOG"); else qputenv("PUTGD_FILE_DIALOG", previous); });
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
    }
};

QTEST_MAIN(ReviewWindowTest)
#include "test_review_window.moc"
