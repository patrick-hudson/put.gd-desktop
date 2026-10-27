#include "record/BarPlacement.h"
#include "record/Overlays.h"
#include "ui/Theme.h"
#include "ui/Graphite.h"
#include "ui/RegionSelection.h"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QSignalSpy>
#include <QJsonArray>
#include <QPushButton>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QScreen>
#include <QStyleOptionButton>
#include <QtTest>
#include <memory>
using namespace putgd::record;

// Issue #108: on X11 the control bar must stay out of the recorded pixels when
// any screen has room (another monitor included), never leave its screen in
// any state, and shrink to a handle when it has to sit inside.
namespace {
// Patrick's workstation: two 2560x1440 monitors and a 1920x1080 one.
const QList<QRect> threeMonitors{QRect(0, 0, 2560, 1440), QRect(2560, 0, 2560, 1440), QRect(5120, 0, 1920, 1080)};
const QSize readyBar(820, 44);
bool onItsScreen(const BarSpot& spot, const QList<QRect>& screens) { return spot.screen >= 0 && spot.screen < screens.size() && screens[spot.screen].contains(spot.rect); }
QList<QRect> qtScreens() {
    QList<QRect> rects;
    for (QScreen* screen : QGuiApplication::screens()) rects.append(screen->geometry());
    return rects;
}
int screenHolding(const QRect& rect) {
    const auto screens = qtScreens();
    for (int i = 0; i < screens.size(); ++i) if (screens[i].contains(rect)) return i;
    return -1;
}
ControlBar::Options barOptions() {
    ControlBar::Options options;
    options.targetLabel = QStringLiteral("Region 2560×1438");
    options.formats = {"mp4", "webm", "gif", "webp"};
    options.codecs = QJsonObject{{"mp4", QJsonArray{"h264", "h265"}}, {"webm", QJsonArray{"vp9"}}};
    options.audioSupported = true;
    return options;
}
// The System audio box's indicator as the bar draws it.
QImage audioIndicator(QCheckBox* audio) {
    QStyleOptionButton option;
    option.initFrom(audio);
    const QRect rect = audio->style()->subElementRect(QStyle::SE_CheckBoxIndicator, &option, audio);
    const QImage image = audio->grab().toImage();
    const qreal ratio = image.devicePixelRatio();
    return image.copy(QRect((QPointF(rect.topLeft()) * ratio).toPoint(), (QSizeF(rect.size()) * ratio).toSize()));
}
// Darkest and mean grey level inside the indicator's border.
std::pair<int, double> greyLevels(const QImage& indicator) {
    int darkest = 255;
    double sum = 0;
    int count = 0;
    for (int y = 2; y < indicator.height() - 2; ++y)
        for (int x = 2; x < indicator.width() - 2; ++x) { const int grey = qGray(indicator.pixel(x, y)); darkest = std::min(darkest, grey); sum += grey; ++count; }
    return {darkest, count ? sum / count : 0};
}
}

class BarPlacement : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() {
        // ctest names the layout it loaded into the offscreen platform; a
        // config the platform ignored would silently test one default screen.
        const int expected = qEnvironmentVariableIntValue("PUTGD_TEST_SCREENS");
        if (expected > 0) QCOMPARE(int(QGuiApplication::screens().size()), expected);
    }
    void requestedAudioSurvivesSilentFormats() {
        putgd::RecordingConfig config; config.audio.system = true;
        ControlBar bar(barOptions(), config); bar.show();
        auto* audio = bar.findChild<QCheckBox*>();
        auto* formats = bar.findChild<QComboBox*>("format");
        QVERIFY(audio && formats);
        QSignalSpy changes(&bar, &ControlBar::configChanged);
        const auto choose = [&](const QString& format) {
            for (int i = 0; i < formats->count(); ++i)
                if (formats->itemData(i).toStringList().first() == format) { formats->setCurrentIndex(i); return; }
        };
        for (const auto& format : {QString("gif"), QString("webp")}) {
            choose(format);
            QVERIFY(bar.requestedAudio()); QVERIFY(!bar.effectiveAudio());
            QCOMPARE(bar.audioState(), QJsonObject({{"requested", true}, {"effective", false}}));
            QVERIFY(audio->isChecked() && !audio->isEnabled());
            QVERIFY(audio->text().contains("Unavailable in " + format.toUpper()));
            QVERIFY(!bar.config().audio.system);
            choose("mp4");
            QVERIFY(bar.requestedAudio() && bar.effectiveAudio());
            QVERIFY(audio->isChecked() && audio->isEnabled());
            QVERIFY(bar.config().audio.system);
        }
        QCOMPARE(changes.size(), 4);
        audio->click(); QVERIFY(!bar.requestedAudio() && !bar.effectiveAudio());
        choose("gif"); choose("mp4");
        QVERIFY(!audio->isChecked() && !bar.effectiveAudio());
    }
    void unavailableAudioRemainsReadableInBothSchemes() {
        const auto previous = putgd::Theme::scheme();
        const auto restore = qScopeGuard([previous] { putgd::Theme::setScheme(previous); });
        for (const auto scheme : {putgd::Theme::Scheme::Dark, putgd::Theme::Scheme::Light}) {
            putgd::Theme::setScheme(scheme);
            putgd::RecordingConfig config; config.audio.system = true;
            ControlBar bar(barOptions(), config); bar.show();
            auto* audio = bar.findChild<QCheckBox*>();
            auto* formats = bar.findChild<QComboBox*>("format");
            QVERIFY(audio && formats);
            for (int index = 0; index < formats->count(); ++index) {
                formats->setCurrentIndex(index);
                const bool silent = formats->itemData(index).toStringList().first() == "gif" || formats->itemData(index).toStringList().first() == "webp";
                QCOMPARE(putgd::Graphite::isLocked(audio), silent);
                QVERIFY(audio->isChecked() && bar.requestedAudio());
                if (silent) {
                    QCOMPARE(audio->palette().color(QPalette::Disabled, QPalette::WindowText), putgd::Theme::current().textMuted);
                    QVERIFY(putgd::Theme::contrast(audio->palette().color(QPalette::Disabled, QPalette::WindowText), putgd::Theme::current().bg) >= 4.5);
                }
            }
        }
    }
    void recordingSelectorCyclesActualFixtureDisplays() {
        const auto screens = QGuiApplication::screens();
        SelectionOverlay selector; selector.start();
        QWidget* surface = nullptr;
        for (auto* w : QApplication::topLevelWidgets()) if (w->isVisible() && w->objectName() == "putgd-record-select" && w->screen() == screens[0]) surface = w;
        QVERIFY(surface);
        QSignalSpy selected(&selector, &SelectionOverlay::selected);
        QTest::keyClick(surface, Qt::Key_Tab);
        const int next = screens.size() > 1 ? 1 : 0;
        QVERIFY(surface->accessibleDescription().contains(screens[next]->name()));
        QVERIFY(surface->accessibleDescription().contains("Drag to select"));
        QTest::keyClick(surface, Qt::Key_Right);
        QRectF expected(QPointF(screens[next]->geometry().topLeft()) + QPointF(screens[next]->geometry().width()/4.0, screens[next]->geometry().height()/4.0), QSizeF(screens[next]->geometry().size())/2);
        QList<QRectF> logical;for(auto* output:screens)logical.append(output->geometry());int current=next;
        putgd::switchRegionDisplay(expected,logical,current,Qt::Key_Backtab);
        QTest::keyClick(surface, Qt::Key_Backtab);
        QVERIFY(surface->accessibleDescription().contains(screens[0]->name()));
        QTest::keyClick(surface, Qt::Key_Return);
        QCOMPARE(selected.size(), 1);
        QVERIFY(expected.toAlignedRect().width() > 0);
        // Fixture DPR is one; root pixels and logical coordinates agree.
        QCOMPARE(qvariant_cast<QRect>(selected.first()[0]), expected.toAlignedRect());
    }
    void fullMonitorRegionGoesToAnotherMonitor() {
        // The region from the issue: 2560x1438 on the first monitor.
        const QRect recorded(0, 0, 2560, 1438);
        const auto spot = placeBar(readyBar, recorded, threeMonitors);
        QVERIFY(spot.outside);
        QVERIFY(spot.screen == 1 || spot.screen == 2);
        QVERIFY(onItsScreen(spot, threeMonitors));
        QVERIFY(!spot.rect.intersects(recorded));
        // The same for the middle monitor and for a monitor target on the small one.
        for (int i = 0; i < threeMonitors.size(); ++i) {
            const auto other = placeBar(readyBar, threeMonitors[i], threeMonitors);
            QVERIFY(other.outside);
            QVERIFY(other.screen != i);
            QVERIFY(onItsScreen(other, threeMonitors));
        }
    }
    void belowThenAboveOnTheHomeScreen() {
        const QRect middle(400, 300, 800, 400);
        auto spot = placeBar(readyBar, middle, threeMonitors);
        QVERIFY(spot.outside);
        QCOMPARE(spot.screen, 0);
        QCOMPARE(spot.rect.top(), middle.bottom() + 1 + 10);
        const QRect low(400, 1000, 800, 400);  // no room below on that monitor
        spot = placeBar(readyBar, low, threeMonitors);
        QCOMPARE(spot.screen, 0);
        QCOMPARE(spot.rect.bottom(), low.top() - 10 - 1);
        // Near the right edge the bar moves left to stay on the screen.
        const QRect right(2400, 300, 160, 100);
        spot = placeBar(readyBar, right, threeMonitors);
        QCOMPARE(spot.screen, 0);
        QCOMPARE(spot.rect.right(), threeMonitors[0].right());
    }
    void stackedMonitorsUseTheOneBelow() {
        const QList<QRect> stacked{QRect(0, 0, 1920, 1080), QRect(0, 1080, 1920, 1080)};
        const auto spot = placeBar(readyBar, stacked[0], stacked);
        QVERIFY(spot.outside);
        QCOMPARE(spot.screen, 1);
        QVERIFY(onItsScreen(spot, stacked));
    }
    void noRoomAnywhereSitsInsideTheCorner() {
        const QList<QRect> single{QRect(0, 0, 1920, 1080)};
        auto spot = placeBar(readyBar, single[0], single);
        QVERIFY(!spot.outside);
        QCOMPARE(spot.screen, 0);
        QVERIFY(single[0].contains(spot.rect));
        QCOMPARE(spot.rect.bottomRight(), QPoint(1919 - 10, 1079 - 10));
        // The whole desktop covers every monitor.
        QRect desktop;
        for (const auto& screen : threeMonitors) desktop |= screen;
        spot = placeBar(readyBar, desktop, threeMonitors);
        QVERIFY(!spot.outside);
        QVERIFY(onItsScreen(spot, threeMonitors));
        // A bar that was placed inside stays inside when it shrinks.
        spot = placeBar(QSize(80, 30), QRect(0, 0, 1920, 1040), single, false);
        QVERIFY(!spot.outside);
        QCOMPARE(spot.rect.bottomRight(), QPoint(1919 - 10, 1039 - 10));
    }
    void neverLeavesItsScreen() {
        // Random regions over several layouts, with the ready, recording and handle sizes.
        const QList<QList<QRect>> layouts{threeMonitors, {QRect(0, 0, 1366, 768)}, {QRect(0, 0, 1920, 1080), QRect(0, 1080, 1920, 1080)},
            {QRect(0, 200, 1280, 1024), QRect(1280, 0, 2560, 1440)}};
        QRandomGenerator random(108);
        for (const auto& screens : layouts) {
            for (int n = 0; n < 500; ++n) {
                const QRect& host = screens[random.bounded(int(screens.size()))];
                const int x = host.left() + random.bounded(host.width() - 8), y = host.top() + random.bounded(host.height() - 8);
                const QRect recorded(x, y, 8 + random.bounded(host.right() - x), 8 + random.bounded(host.bottom() - y));
                for (const QSize size : {readyBar, QSize(900, 44), QSize(80, 30)}) {
                    const auto spot = placeBar(size, recorded, screens);
                    QVERIFY2(onItsScreen(spot, screens), qPrintable(QStringLiteral("%1,%2 %3x%4").arg(recorded.x()).arg(recorded.y()).arg(recorded.width()).arg(recorded.height())));
                    if (spot.outside) QVERIFY(!spot.rect.intersects(recorded));
                }
            }
        }
    }

    // Locked capture values remain readable information in both schemes.
    void lockedValuesAreReadableLabels() {
        const auto restore = qScopeGuard([] { putgd::Theme::setScheme(putgd::Theme::Scheme::Dark); });
        for (const auto scheme : {putgd::Theme::Scheme::Dark, putgd::Theme::Scheme::Light}) {
            putgd::Theme::setScheme(scheme);
            for (const bool sound : {false, true}) {
                putgd::RecordingConfig config;
                config.audio.system = sound;
                ControlBar bar(barOptions(), config);
                bar.show();
                auto* audio = bar.findChild<QCheckBox*>();
                auto* format = bar.findChild<QComboBox*>("format");
                auto* audioValue = bar.findChild<QLabel*>("lockedAudio");
                auto* formatValue = bar.findChild<QLabel*>("lockedFormat");
                QVERIFY(audio && format && audioValue && formatValue);
                QVERIFY(audio->isVisible() && format->isVisible());
                QVERIFY(!audioValue->isVisible() && !formatValue->isVisible());
                for (const char* state : {"recording", "paused", "stopping"}) {
                    bar.setState(QString::fromLatin1(state));
                    QVERIFY(!audio->isVisible() && !format->isVisible());
                    QCOMPARE(audio->isChecked(), sound);
                    QCOMPARE(audioValue->text(), sound ? QStringLiteral("With system audio") : QStringLiteral("No audio"));
                    QCOMPARE(formatValue->text(), format->currentText());
                    auto* facts = bar.findChild<QLabel*>("recordingFacts");
                    QVERIFY(facts);
                    const auto expected = QStringList{barOptions().targetLabel, audioValue->text(), formatValue->text()}.join(" · ");
                    QCOMPARE(facts->text(), expected);
                    QVERIFY(!bar.findChild<QPushButton*>("stop")->icon().isNull());
                    for (auto* label : {facts}) {
                        QVERIFY(label && label->isVisible() && label->isEnabled());
                        QCOMPARE(label->palette().color(QPalette::WindowText), putgd::Theme::current().textMuted);
                        QVERIFY(putgd::Theme::contrast(label->palette().color(QPalette::WindowText), putgd::Theme::current().bg) >= 4.5);
                    }
                }
                bar.setState("ready");
                QVERIFY(audio->isVisible() && format->isVisible());
                QVERIFY(!audioValue->isVisible() && !formatValue->isVisible());
            }
        }
    }
    // #198: the bar uses the interface's scheme, from the same tokens, and a
    // scheme change restyles a bar that is already open.
    void barFollowsTheScheme() {
        using putgd::Theme::Scheme;
        const auto restore = qScopeGuard([] { putgd::Theme::setScheme(Scheme::Dark); });
        ControlBar bar(barOptions(), putgd::RecordingConfig{});
        for (const auto scheme : {Scheme::Light, Scheme::Dark, Scheme::Light}) {
            putgd::Theme::setScheme(scheme);
            const auto& p = putgd::Theme::palette(scheme);
            // Graphite: painted from the tokens, no stylesheet.
            QVERIFY(bar.styleSheet().isEmpty());
            QCOMPARE(bar.palette().color(QPalette::Window), p.bg);
            QCOMPARE(bar.property("themeScheme").toString(), putgd::Theme::name(scheme));
            // Painted: most of the bar is the scheme's background.
            const QImage image = bar.grab().toImage();
            int matching = 0;
            for (int y = 0; y < image.height(); ++y)
                for (int x = 0; x < image.width(); ++x) {
                    const QColor c = image.pixelColor(x, y);
                    if (std::abs(c.red() - p.bg.red()) + std::abs(c.green() - p.bg.green()) + std::abs(c.blue() - p.bg.blue()) < 12) ++matching;
                }
            QVERIFY2(matching * 3 > image.width() * image.height(), qPrintable(QStringLiteral("%1: %2 of %3 pixels are bg").arg(putgd::Theme::name(scheme)).arg(matching).arg(image.width() * image.height())));
        }
    }
    // Over any content: the text reads on the bar, and the bar's outline
    // (3:1 against black and white) shows where it ends, in both schemes.
    void barReadsOverAnyContent() {
        for (const auto scheme : {putgd::Theme::Scheme::Dark, putgd::Theme::Scheme::Light}) {
            const auto& p = putgd::Theme::palette(scheme);
            QVERIFY(putgd::Theme::contrast(p.text, p.bg) >= 4.5);
            QVERIFY(putgd::Theme::contrast(p.text, p.surfaceRaised) >= 4.5);
            QVERIFY(putgd::Theme::contrast(p.textMuted, p.bg) >= 4.5);
            QVERIFY(putgd::Theme::contrast(p.textDisabled, Qt::black) >= 3.0);
            QVERIFY(putgd::Theme::contrast(p.textDisabled, Qt::white) >= 3.0);
            // Record and Stop keep white on green and red.
            QVERIFY(putgd::Theme::contrast(Qt::white, QColor("#2e7d32")) >= 4.5);
            QVERIFY(putgd::Theme::contrast(Qt::white, QColor("#c62828")) >= 4.5);
        }
    }
    // The light System audio box: a dark accent fill with a light tick, against
    // an empty light box, before Record and locked.
    void lightAudioBoxShowsTheRecordedSound() {
        const auto restore = qScopeGuard([] { putgd::Theme::setScheme(putgd::Theme::Scheme::Dark); });
        putgd::Theme::setScheme(putgd::Theme::Scheme::Light);
        auto barWith = [](bool sound) { putgd::RecordingConfig config; config.audio.system = sound; return std::make_unique<ControlBar>(barOptions(), config); };
        const auto withSound = barWith(true), silent = barWith(false);
        auto* on = withSound->findChild<QCheckBox*>();
        auto* off = silent->findChild<QCheckBox*>();
        QVERIFY(on && off);
        auto brightest = [](const QImage& image) { int value = 0; for (int y = 2; y < image.height() - 2; ++y) for (int x = 2; x < image.width() - 2; ++x) value = std::max(value, qGray(image.pixel(x, y))); return value; };
        for (const char* state : {"ready"}) {
            withSound->setState(QString::fromLatin1(state));
            silent->setState(QString::fromLatin1(state));
            const QImage ticked = audioIndicator(on), empty = audioIndicator(off);
            const auto [tickedDarkest, tickedMean] = greyLevels(ticked);
            const auto [emptyDarkest, emptyMean] = greyLevels(empty);
            Q_UNUSED(tickedDarkest);
            Q_UNUSED(emptyDarkest);
            QVERIFY2(emptyMean - tickedMean > 40, qPrintable(QStringLiteral("%1: mean %2 vs %3").arg(state).arg(tickedMean).arg(emptyMean)));
            QVERIFY2(brightest(ticked) > 200, qPrintable(QStringLiteral("%1: no light tick, brightest %2").arg(state).arg(brightest(ticked))));
        }
    }
    // The real bar, on the offscreen platform's screens (three monitors or one, from ctest).
    void barStaysOnItsScreenAfterRecord() {
        const auto screens = qtScreens();
        QVERIFY(!screens.isEmpty());
        // Room below, near the right edge: the wider recording bar must not run off.
        const QRect& first = screens[0];
        const QRect region(first.right() - 200, first.top() + 100, 200, 120);
        ControlBar bar(barOptions(), putgd::RecordingConfig{});
        QVERIFY(bar.placeAround(region));
        bar.show();
        for (const char* state : {"ready", "recording", "paused", "recording", "stopping"}) {
            bar.setState(QString::fromLatin1(state));
            QVERIFY2(screenHolding(bar.geometry()) >= 0, state);
            QVERIFY2(!bar.geometry().intersects(region), state);
        }
    }
    void fullMonitorBarOnAnotherMonitorOrAHandle() {
        const auto screens = qtScreens();
        ControlBar bar(barOptions(), putgd::RecordingConfig{});
        const bool outside = bar.placeAround(screens[0]);
        bar.setHandleWhileRecording(!outside);
        bar.show();
        QCOMPARE(outside, screens.size() > 1);
        const QSize ready = bar.size();
        bar.setState("recording");
        const QSize recording = bar.size();
        QVERIFY(screenHolding(bar.geometry()) >= 0);
        if (outside) {
            QVERIFY(screenHolding(bar.geometry()) != 0);
            QVERIFY(!bar.geometry().intersects(screens[0]));
        } else {
            // One monitor: the handle is the dot and Stop, in the corner.
            QVERIFY(recording.width() < ready.width() / 3);
            QVERIFY(recording.height() <= ready.height());
            QCOMPARE(bar.geometry().right(), screens[0].right() - 10);
            QCOMPARE(bar.geometry().bottom(), screens[0].bottom() - 10);
            int visibleButtons = 0;
            for (auto* button : bar.findChildren<QPushButton*>()) if (button->isVisible()) { ++visibleButtons; QCOMPARE(button->text(), QString("Stop recording")); }
            QCOMPARE(visibleButtons, 1);
            bar.setState("paused");
            QVERIFY(screens[0].contains(bar.geometry()));
        }
    }
    // Cancel before Record closes at once; once frames exist it asks in the
    // bar, Keep recording first and on Escape, and the bar stays out of the clip.
    void cancelAsksOnlyOnceRecording() {
        const auto screens = qtScreens();
        const QRect region(screens[0].left() + 200, screens[0].top() + 200, 600, 300);
        ControlBar bar(barOptions(), putgd::RecordingConfig{});
        QSignalSpy cancelled(&bar, &ControlBar::cancelRequested);
        QVERIFY(bar.placeAround(region));
        bar.show();
        auto* cancel = bar.findChild<QPushButton*>("cancel");
        auto* keep = bar.findChild<QPushButton*>("keepRecording");
        auto* discard = bar.findChild<QPushButton*>("discardRecording");
        QVERIFY(cancel && keep && discard);
        QCOMPARE(cancel->text(), QStringLiteral("Cancel"));
        cancel->click();
        QCOMPARE(cancelled.size(), 1);
        QSignalSpy resumed(&bar, &ControlBar::resumeRequested);
        for (const char* state : {"recording", "paused"}) {
            bar.setState(QString::fromLatin1(state));
            QCOMPARE(cancel->text(), QStringLiteral("Discard…"));
            cancel->click();
            QCOMPARE(keep->text(), QString::fromLatin1(state) == "paused" ? QStringLiteral("Keep paused") : QStringLiteral("Keep recording"));
            QCOMPARE(cancelled.size(), 1);
            QVERIFY(bar.confirming());
            QVERIFY(keep->isVisible() && discard->isVisible() && !cancel->isVisible());
            QVERIFY(keep->x() < discard->x());QCOMPARE(bar.focusWidget(),keep);
            QVERIFY(!bar.findChild<QPushButton*>("stop")->isVisible());
            QVERIFY(bar.findChild<QLabel*>("discardQuestion")->isVisible());
            QVERIFY2(!bar.geometry().intersects(region) && screenHolding(bar.geometry()) >= 0, state);
            QTest::keyClick(&bar, Qt::Key_Escape);
            QVERIFY(!bar.confirming());
            QVERIFY(bar.findChild<QPushButton*>("stop")->isVisible());
            QCOMPARE(cancelled.size(), 1);
        }
        cancel->click();
        keep->click();
        QVERIFY(!bar.confirming());
        QCOMPARE(bar.findChild<QLabel*>("state")->text(), QStringLiteral("Paused"));
        QCOMPARE(resumed.size(), 0);
        cancel->click();
        discard->click();
        QCOMPARE(cancelled.size(), 2);
        // A stop elsewhere drops the question.
        cancel->click();
        bar.setState("stopping");
        QVERIFY(!bar.confirming());
    }
    // A narrow screen (or large text) takes two rows: Start/Pause/Stop on the
    // first, target, audio and format together on the second.
    void narrowScreensTakeTwoRows() {
        ControlBar bar(barOptions(), putgd::RecordingConfig{});
        auto* format = bar.findChild<QComboBox*>("format");
        QVERIFY(format);
        QCOMPARE(format->accessibleName(), QString("Recording format"));
        bar.fitTo(QRect(0, 0, 2560, 1440));
        QVERIFY(!bar.twoRows());
        bar.show();
        const QSize wide = bar.size();
        bar.fitTo(QRect(0, 0, 1024, 768));
        QVERIFY(bar.twoRows());
        QVERIFY(bar.width() < wide.width());
        QVERIFY(bar.width() <= 1024 * 7 / 10);
        auto* record = bar.findChild<QPushButton*>("record");
        auto* target = bar.findChild<QLabel*>("target");
        auto* audio = bar.findChild<QCheckBox*>();
        auto rowOf = [&](QWidget* w) { return w->mapTo(&bar, QPoint(0, w->height() / 2)).y(); };
        QVERIFY(record->isVisible() && rowOf(record) < rowOf(format));
        QCOMPARE(rowOf(target), rowOf(audio));
        QVERIFY(std::abs(rowOf(audio) - rowOf(format)) <= 4);
        bar.setState("recording");
        auto* stop = bar.findChild<QPushButton*>("stop");
        QVERIFY(stop->isVisible() && rowOf(stop) < rowOf(format));
        // Placement decides from the recorded area's own screen.
        const auto screens = qtScreens();
        const QRect region(screens[0].left() + 200, screens[0].top() + 200, 600, 300);
        bar.setState("ready");
        QVERIFY(bar.placeAround(region));
        QCOMPARE(bar.twoRows(), wide.width() > screens[0].width() * 7 / 10);
        QVERIFY(!bar.geometry().intersects(region));
    }
    void dragKeepsTheBarOutOfTheClip() {
        const auto screens = qtScreens();
        const QRect region(screens[0].left() + 200, screens[0].top() + 200, 600, 300);
        ControlBar bar(barOptions(), putgd::RecordingConfig{});
        QVERIFY(bar.placeAround(region));
        bar.show();
        const QRect placed = bar.geometry();
        // A drag into the region is refused; one beside it is kept, also after Record widens the bar.
        const QPoint grab = placed.topLeft() + QPoint(20, placed.height() / 2);
        QTest::mousePress(&bar, Qt::LeftButton, {}, grab - placed.topLeft());
        auto moveTo = [&](QPoint global) {
            QMouseEvent move(QEvent::MouseMove, bar.mapFromGlobal(global), global, Qt::NoButton, Qt::LeftButton, {});
            QApplication::sendEvent(&bar, &move);
        };
        moveTo(region.center());
        QCOMPARE(bar.geometry(), placed);
        const QPoint below(grab.x(), grab.y() + 120);
        moveTo(below);
        QTest::mouseRelease(&bar, Qt::LeftButton, {}, bar.mapFromGlobal(below));
        const QRect dropped = bar.geometry();
        QVERIFY(dropped != placed);
        QVERIFY(!dropped.intersects(region));
        bar.setState("recording");
        QCOMPARE(bar.geometry().right(), dropped.right());
        QVERIFY(screenHolding(bar.geometry()) >= 0);
    }
};
QTEST_MAIN(BarPlacement)
#include "test_bar_placement.moc"
