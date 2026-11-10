#include "record/WaylandAcquisition.h"
#include <QtTest>
#include <cstdlib>
using namespace putgd::record;

// The compositor-native Wayland paths (KWin screencast, Mutter screen cast)
// name a node on the session PipeWire daemon, so the probe only prefers them
// when the daemon's socket is where `pipewiresrc` will look for it.
class WaylandAcquisition : public QObject {
    Q_OBJECT
private slots:
    void findsTheSessionSocketLikePipewiresrc() {
        QTemporaryDir runtime, pipewireDir;
        QVERIFY(runtime.isValid() && pipewireDir.isValid());
        QProcessEnvironment env;
        env.insert("XDG_RUNTIME_DIR", runtime.path());
        QCOMPARE(pipewireSocket(env), QString());  // no daemon socket: the portal stays in charge
        QFile socket(runtime.filePath("pipewire-0")); QVERIFY(socket.open(QIODevice::WriteOnly)); socket.close();
        QCOMPARE(pipewireSocket(env), runtime.filePath("pipewire-0"));
        // PIPEWIRE_RUNTIME_DIR is looked at first, as PipeWire does; an empty one falls through.
        env.insert("PIPEWIRE_RUNTIME_DIR", pipewireDir.path());
        QCOMPARE(pipewireSocket(env), runtime.filePath("pipewire-0"));
        QFile other(pipewireDir.filePath("pipewire-0")); QVERIFY(other.open(QIODevice::WriteOnly)); other.close();
        QCOMPARE(pipewireSocket(env), pipewireDir.filePath("pipewire-0"));
        // PIPEWIRE_REMOTE names a different socket.
        env.insert("PIPEWIRE_REMOTE", "pipewire-1");
        QCOMPARE(pipewireSocket(env), QString());
        QFile named(runtime.filePath("pipewire-1")); QVERIFY(named.open(QIODevice::WriteOnly)); named.close();
        QCOMPARE(pipewireSocket(env), runtime.filePath("pipewire-1"));
    }
    // #137: the source leaves the buffer count to the compositor. KWin offers
    // 2-4 buffers; the AppImage's PipeWire 1.0 plugin asked for 8 or more by
    // default, and PipeWire 1.5.81+ refuses to link ranges that don't overlap.
    void letsTheCompositorChooseTheBufferCount() {
        QCOMPARE(pipewireSourceDescription("putgdpipewiresrc", 42, -1, false),
                 QStringLiteral("putgdpipewiresrc name=src path=42 min-buffers=1 keepalive-time=1000 resend-last=true"));
        QCOMPARE(pipewireSourceDescription("pipewiresrc", 7, 31, true),
                 QStringLiteral("pipewiresrc name=src fd=31 path=7 min-buffers=1 do-timestamp=true keepalive-time=1000 resend-last=true"));
    }
    // #86: a frame keeps the compositor's time, mapped through one reading of
    // both clocks, instead of the time it reached the pipeline.
    void mapsCompositorFrameTimesToRunningTime() {
        const qint64 ms = 1'000'000, mono = 5'000'000 * ms, running = 2'000 * ms;
        // Painted 40 ms before it arrived: 40 ms before now in running time.
        auto frame = pipewireFrameTime(mono - 40 * ms, mono, running, -1);
        QCOMPARE(frame.running, running - 40 * ms);
        QCOMPARE(frame.ageNs, 40 * ms);
        // Each frame gets its own age; there is no offset carried from the first one.
        frame = pipewireFrameTime(mono + 33 * ms - 2 * ms, mono + 33 * ms, running + 33 * ms, frame.running);
        QCOMPARE(frame.running, running + 31 * ms);
        QCOMPARE(frame.ageNs, 2 * ms);
        // A time slightly ahead of this reading is now.
        frame = pipewireFrameTime(mono + 2 * ms, mono, running + 40 * ms, -1);
        QCOMPARE(frame.running, running + 40 * ms);
        QCOMPARE(frame.ageNs, qint64(0));
    }
    void fallsBackToArrivalForMissingOrStaleTimes() {
        const qint64 ms = 1'000'000, mono = 5'000'000 * ms, running = 2'000 * ms;
        // No header PTS, a stale KWin presentation time after an idle output, a time from the future.
        for (const qint64 pts : {qint64(-1), mono - kMaxFrameAgeNs - 1, mono + kFutureSlackNs + 1}) {
            const auto frame = pipewireFrameTime(pts, mono, running, -1);
            QCOMPARE(frame.running, running);
            QCOMPARE(frame.ageNs, qint64(-1));
        }
        // Just inside the window still counts as a compositor time.
        QCOMPARE(pipewireFrameTime(mono - kMaxFrameAgeNs, mono, running, -1).running, running - kMaxFrameAgeNs);
        // Early in the pipeline's life a time before running time zero clamps to zero.
        QCOMPARE(pipewireFrameTime(mono - 30 * ms, mono, 10 * ms, -1).running, qint64(0));
    }
    void locksTheCadenceOfJitteredArrivals() {
        // #6: arrivals jitter by up to +-8 ms around 16.7 ms; each new frame takes
        // the next slot, so videorate sees one frame per slot.
        const qint64 ms = 1'000'000;
        const auto lock = frameLock(60);
        QCOMPARE(lock.periodNs, qint64(16'666'666));
        qint64 previous = -1;
        const qint64 jitter[] = {0, 8, -8, 3, -6, 5, -7, 1, -1, 7, 4, -4};
        for (int i = 0; i < 12; ++i) {
            const qint64 arrival = 100 * ms + i * lock.periodNs + jitter[i] * ms;
            const qint64 stamp = lockedFrameTime(arrival, previous, lock);
            QVERIFY(stamp >= 0);
            if (previous >= 0) QCOMPARE(stamp - previous, lock.periodNs);
            previous = stamp;
        }
        // An extra frame well inside the current slot (a cursor move, a second
        // render) is dropped; a frame after a gap resynchronises to its arrival.
        QCOMPARE(lockedFrameTime(previous + 3 * ms, previous, lock), qint64(-1));
        QCOMPARE(lockedFrameTime(previous + 500 * ms, previous, lock), previous + 500 * ms);
        // 120 Hz input: every other frame is early and dropped.
        qint64 fast = lockedFrameTime(0, -1, lock);
        int kept = 0, dropped = 0;
        for (int i = 1; i <= 20; ++i) { const qint64 stamp = lockedFrameTime(i * lock.periodNs / 2, fast, lock); if (stamp < 0) ++dropped; else { ++kept; fast = stamp; } }
        QCOMPARE(kept, 10); QCOMPARE(dropped, 10);
        // Without a period the arrival is used, and times never go back.
        QCOMPARE(lockedFrameTime(100 * ms, -1, FrameLock{}), 100 * ms);
        QCOMPARE(lockedFrameTime(50 * ms, 100 * ms, FrameLock{}), 100 * ms + 1);
        QCOMPARE(pipewireUnthrottledCaps(), QString(" ! video/x-raw,max-framerate=0/1"));
    }
    void aCursorOnlyFrameKeepsOutOfTheNextChangesSlot() {
        // #1364: with its own cursor the image-copy source pushes a cursor-only frame every 16 ms while
        // the pointer moves. Here the screen itself changes every 137 ms, so the changes meet the grid
        // at every phase; a change's lateness is the stamp of the first kept frame that shows it minus
        // its arrival.
        const qint64 ms = 1'000'000, cursor = 16 * ms;
        struct Run { qint64 mean = 0, worst = 0; int kept = 0, gaps = 0; };
        const auto run = [&](int fps, bool marked) {
            const auto lock = frameLock(fps, cursor);
            Run result;
            qint64 previous = -1, lastPush = -1000 * ms, pending = -1, nextChange = 200 * ms, total = 0;
            int changes = 0;
            for (qint64 now = 0; now < 30'000 * ms; now += ms / 4) {
                bool own = false;
                if (now >= nextChange) { if (pending < 0) pending = now; nextChange += 137 * ms; }
                else if (now - lastPush >= cursor) own = true;
                else continue;
                lastPush = now;
                const qint64 stamp = lockedFrameTime(now, previous, lock, marked && own);
                if (stamp < 0) continue;
                if (previous >= 0 && stamp - previous > lock.periodNs) ++result.gaps;
                previous = stamp; ++result.kept;
                if (pending >= 0) { total += stamp - pending; result.worst = std::max(result.worst, stamp - pending); ++changes; pending = -1; }
            }
            result.mean = changes ? total / changes : -1;
            return result;
        };
        // Taken for captured frames, the cursor's frames hold every slot `tolerance` early, each change
        // arrives "early" for the slot after and shows about a frame late: the picture runs behind the
        // sound for as long as the pointer moves.
        const auto unmarked = run(30, false);
        QVERIFY2(unmarked.mean > 25 * ms, qPrintable(QString::number(unmarked.mean / 1e6)));
        // As the recorder's own frames they wait for the slot's time and the change keeps its slot:
        // no later than a still screen's, never a whole period, and still one frame in every slot.
        const auto marked = run(30, true);
        QVERIFY2(marked.mean < 12 * ms, qPrintable(QString::number(marked.mean / 1e6)));
        QVERIFY2(marked.worst < frameLock(30).periodNs, qPrintable(QString::number(marked.worst / 1e6)));
        QCOMPARE(marked.gaps, 0);
        QVERIFY2(qAbs(marked.kept - 30 * 30) <= 2, qPrintable(QString::number(marked.kept)));
        QCOMPARE(frameLock(30, cursor).ownEarlyNs, qint64(0));
        // At 50 and 60 a second the slot is no longer than the cursor's pace, so its frames get just
        // enough room before the slot that one always lands in it: the cadence holds there too.
        for (const int fps : {50, 60}) {
            const auto fast = run(fps, true);
            QCOMPARE(fast.gaps, 0);
            QVERIFY2(qAbs(fast.kept - 30 * fps) <= 2, qPrintable(QString::number(fast.kept)));
            QVERIFY(fast.mean <= run(fps, false).mean);
            QVERIFY(frameLock(fps, cursor).ownEarlyNs > 0 && frameLock(fps, cursor).ownEarlyNs < frameLock(fps).toleranceNs);
        }
        // A frame that is not the recorder's own is judged as before, whatever the pace given.
        const auto lock = frameLock(30, cursor);
        QCOMPARE(lockedFrameTime(100 * ms + lock.periodNs - 20 * ms, 100 * ms, lock), 100 * ms + lock.periodNs);
        QCOMPARE(lockedFrameTime(100 * ms + lock.periodNs - 20 * ms, 100 * ms, lock, true), qint64(-1));
        QCOMPARE(lockedFrameTime(100 * ms + lock.periodNs + 5 * ms, 100 * ms, lock, true), 100 * ms + lock.periodNs);
    }
    void keepsFrameTimesIncreasing() {
        const qint64 ms = 1'000'000, mono = 5'000'000 * ms, running = 2'000 * ms;
        // A resent keepalive frame stamped at arrival, then a real frame painted earlier.
        const auto keepalive = pipewireFrameTime(-1, mono, running, -1);
        const auto late = pipewireFrameTime(mono - 20 * ms, mono + 1 * ms, running + 1 * ms, keepalive.running);
        QCOMPARE(late.running, keepalive.running + 1);
        QCOMPARE(late.ageNs, 21 * ms);
    }
    // #1278: Mutter draws a recorded window into a larger buffer and names its
    // place in each frame's crop region; videocrop takes it as margins.
    void cutsTheWindowOutOfItsBuffer() {
        QCOMPARE(cropMargins(QSize(1920, 1080), QRect(0, 0, 640, 480)), (CropMargins{0, 0, 1280, 600}));
        QCOMPARE(cropMargins(QSize(1920, 1080), QRect(10, 20, 960, 640)), (CropMargins{10, 20, 950, 420}));
        QCOMPARE(cropMargins(QSize(640, 480), QRect(0, 0, 640, 480)), CropMargins{});
        // Nothing sensible to cut: crop nothing rather than an invalid margin.
        QCOMPARE(cropMargins(QSize(640, 480), QRect()), CropMargins{});
        QCOMPARE(cropMargins(QSize(640, 480), QRect(600, 0, 100, 100)), CropMargins{});
        QCOMPARE(cropMargins(QSize(), QRect(0, 0, 10, 10)), CropMargins{});
    }
    // The canvas a window recording keeps: its frame at the output scale, even.
    void sizesTheWindowCanvas() {
        QCOMPARE(windowCanvas(QSize(641, 481), 1.0), QSize(640, 480));
        QCOMPARE(windowCanvas(QSize(640, 480), 2.0), QSize(1280, 960));
        QCOMPARE(windowCanvas(QSize(333, 201), 1.5), QSize(500, 302));
        QCOMPARE(windowCanvas(QSize(1, 1), 1.0), QSize(2, 2));
        QCOMPARE(windowCanvas(QSize(640, 480), 0), QSize(640, 480));
        QVERIFY(windowCanvas(QSize(), 1.0).isEmpty());
    }
};
QTEST_GUILESS_MAIN(WaylandAcquisition)
#include "test_wayland_acquisition.moc"
