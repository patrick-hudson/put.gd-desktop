#include "record/AdaptiveRate.h"
#include <QtTest>
#include <unistd.h>
using namespace putgd::record;

// #984: a 1080p60 capture starved CPU- and GPU-bound desktops while its clip
// still held 60 frames per second. The adaptive rate lowers a recording's rate
// after sustained evidence the machine can't keep up, and never otherwise.
class AdaptiveRateContract : public QObject {
    Q_OBJECT
    static AdaptiveRate::Window healthy(int fps) {
        AdaptiveRate::Window w;
        w.durationNs = 1'000'000'000;
        w.sourceFrames = fps;
        w.leaked = 0;
        w.systemBusy = 0.35;
        w.recorderCores = 0.8;
        return w;
    }
    // Feeds windows until the rate drops; returns how many it took (0: never in `limit`).
    static int windowsUntilLowered(AdaptiveRate& rate, const AdaptiveRate::Window& w, int limit = 20) {
        for (int i = 1; i <= limit; ++i) if (rate.observe(w)) return i;
        return 0;
    }
private slots:
    void stepsHalveTheRateDownToTheFloor() {
        QCOMPARE(AdaptiveRate::lower(60), 30);
        QCOMPARE(AdaptiveRate::lower(30), 15);
        QCOMPARE(AdaptiveRate::lower(24), 15);
        QCOMPARE(AdaptiveRate::lower(50), 25);
        QCOMPARE(AdaptiveRate::lower(15), 15);
        QCOMPARE(AdaptiveRate::lower(10), 10);
    }
    void aHealthyRecordingKeepsItsRate() {
        AdaptiveRate rate(60, true, true);
        QCOMPARE(windowsUntilLowered(rate, healthy(60), 60), 0);
        QCOMPARE(rate.current(), 60);
        QVERIFY(!rate.reduced());
        QVERIFY(rate.reason().isEmpty());
        // A compositor stream sends only what changed: a still screen delivers
        // almost nothing, which is no shortfall.
        AdaptiveRate stream(60, true, false);
        auto still = healthy(60);
        still.sourceFrames = 1;
        QCOMPARE(windowsUntilLowered(stream, still, 60), 0);
    }
    void leakedFramesLowerTheRateAfterWarmupAndThreeWindows() {
        AdaptiveRate rate(60, true, false);
        auto w = healthy(60);
        w.leaked = 12;  // 20% of the frames lost at the capture queue
        QCOMPARE(rate.judge(w), QStringLiteral("encoder"));
        QCOMPARE(windowsUntilLowered(rate, w), AdaptiveRate::warmupWindows + AdaptiveRate::badWindows);
        QCOMPARE(rate.current(), 30);
        QCOMPARE(rate.configured(), 60);
        QVERIFY(rate.reduced());
        QCOMPARE(rate.reason(), QStringLiteral("encoder"));
        // A new warm-up and three more windows before the next step, then the floor.
        QCOMPARE(windowsUntilLowered(rate, w), AdaptiveRate::warmupWindows + AdaptiveRate::badWindows);
        QCOMPARE(rate.current(), AdaptiveRate::floorFps);
        QCOMPARE(windowsUntilLowered(rate, w, 60), 0);
        QCOMPARE(rate.current(), AdaptiveRate::floorFps);
    }
    void aFewLeakedFramesAreNoEvidence() {
        AdaptiveRate rate(60, true, false);
        auto w = healthy(60);
        w.leaked = 2;
        QVERIFY(rate.judge(w).isEmpty());
        w.leaked = 3;  // 5% of 60 exactly is not more than 5%
        QVERIFY(rate.judge(w).isEmpty());
        w.leaked = 4;
        QCOMPARE(rate.judge(w), QStringLiteral("encoder"));
    }
    void aGoodWindowRestartsTheCount() {
        AdaptiveRate rate(60, true, false);
        auto bad = healthy(60);
        bad.leaked = 20;
        for (int i = 0; i < AdaptiveRate::warmupWindows; ++i) QVERIFY(!rate.observe(healthy(60)));
        for (int round = 0; round < 10; ++round) {
            for (int i = 1; i < AdaptiveRate::badWindows; ++i) QVERIFY(!rate.observe(bad));
            QVERIFY(!rate.observe(healthy(60)));
        }
        QCOMPARE(rate.current(), 60);
        for (int i = 1; i < AdaptiveRate::badWindows; ++i) QVERIFY(!rate.observe(bad));
        QVERIFY(rate.observe(bad));
        QCOMPARE(rate.current(), 30);
    }
    void lateGrabsCountOnlyForFixedRateSources() {
        // ximagesrc asked for 60 grabs a second and got 40: the X server or
        // the machine is behind. Measured against the configured rate, since
        // the gate after the source keeps the lowered one.
        auto w = healthy(60);
        w.sourceFrames = 40;
        AdaptiveRate x11(60, true, true);
        QCOMPARE(x11.judge(w), QStringLiteral("capture"));
        QCOMPARE(windowsUntilLowered(x11, w), AdaptiveRate::warmupWindows + AdaptiveRate::badWindows);
        QCOMPARE(x11.reason(), QStringLiteral("capture"));
        auto grabbing = healthy(60);  // still 60 grabs a second after the step
        QCOMPARE(windowsUntilLowered(x11, grabbing, 60), 0);
        QCOMPARE(x11.current(), 30);
        AdaptiveRate stream(60, true, false);
        QVERIFY(stream.judge(w).isEmpty());
    }
    void aBusyMachineCountsWhenTheRecorderIsPartOfIt() {
        auto w = healthy(60);
        w.systemBusy = 0.97;
        w.recorderCores = 1.6;
        AdaptiveRate rate(60, true, false);
        QCOMPARE(rate.judge(w), QStringLiteral("cpu"));
        QCOMPARE(windowsUntilLowered(rate, w), AdaptiveRate::warmupWindows + AdaptiveRate::badWindows);
        QCOMPARE(rate.reason(), QStringLiteral("cpu"));
        QCOMPARE(rate.current(), 30);
        // A busy machine alone never takes a recording below the 30 fps default:
        // the load may be another program's (a build) while the recorder keeps up.
        QCOMPARE(windowsUntilLowered(rate, w, 60), 0);
        QCOMPARE(rate.current(), 30);
        AdaptiveRate thirty(30, true, false);
        QVERIFY(thirty.judge(w).isEmpty());
        QCOMPARE(windowsUntilLowered(thirty, w, 60), 0);
        // Someone else's load with an idle recorder is not the recording's to fix.
        w.recorderCores = 0.2;
        QVERIFY(AdaptiveRate(60, true, false).judge(w).isEmpty());
        // Unknown CPU (no /proc) is no evidence either way.
        w.systemBusy = w.recorderCores = -1;
        QVERIFY(AdaptiveRate(60, true, false).judge(w).isEmpty());
        w.systemBusy = 0.85;
        w.recorderCores = 2;
        QVERIFY(AdaptiveRate(60, true, false).judge(w).isEmpty());
    }
    void disabledOrAtTheFloorNeverChanges() {
        auto w = healthy(60);
        w.leaked = 30;
        w.systemBusy = 1;
        w.recorderCores = 4;
        AdaptiveRate off(60, false, true);
        QCOMPARE(windowsUntilLowered(off, w, 60), 0);
        QCOMPARE(off.current(), 60);
        AdaptiveRate low(15, true, true);
        QCOMPARE(windowsUntilLowered(low, w, 60), 0);
        QCOMPARE(low.current(), 15);
        // A 30 fps recording steps to 15 on the same evidence.
        AdaptiveRate thirty(30, true, false);
        QCOMPARE(windowsUntilLowered(thirty, w), AdaptiveRate::warmupWindows + AdaptiveRate::badWindows);
        QCOMPARE(thirty.current(), 15);
    }
    void cpuSharesFromProc() {
        CpuSample before{1000, 600, 100}, after{1400, 640, 300};
        double busy = 0, cores = 0;
        const long ticks = sysconf(_SC_CLK_TCK);
        cpuShares(before, after, 2'000'000'000, busy, cores);
        QCOMPARE(busy, 0.9);
        QCOMPARE(cores, 200.0 / double(ticks) / 2.0);
        cpuShares(CpuSample{}, after, 2'000'000'000, busy, cores);
        QCOMPARE(busy, -1.0);
        QCOMPARE(cores, -1.0);
        // This process and the machine are readable on Linux.
        const auto now = readCpu();
        QVERIFY(now.totalTicks > 0);
        QVERIFY(now.idleTicks >= 0 && now.idleTicks <= now.totalTicks);
        QVERIFY(now.processTicks >= 0);
    }
};
QTEST_GUILESS_MAIN(AdaptiveRateContract)
#include "test_adaptive_rate.moc"
