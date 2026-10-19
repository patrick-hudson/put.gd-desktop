#include "record/BarPlacement.h"
#include "record/Overlays.h"
#include <QApplication>
#include <QJsonArray>
#include <QPushButton>
#include <QRandomGenerator>
#include <QScreen>
#include <QtTest>
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
            for (auto* button : bar.findChildren<QPushButton*>()) if (button->isVisible()) { ++visibleButtons; QCOMPARE(button->text(), QString("Stop")); }
            QCOMPARE(visibleButtons, 1);
            bar.setState("paused");
            QVERIFY(screens[0].contains(bar.geometry()));
        }
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
