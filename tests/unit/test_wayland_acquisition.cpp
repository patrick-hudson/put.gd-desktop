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
    void keepsFrameTimesIncreasing() {
        const qint64 ms = 1'000'000, mono = 5'000'000 * ms, running = 2'000 * ms;
        // A resent keepalive frame stamped at arrival, then a real frame painted earlier.
        const auto keepalive = pipewireFrameTime(-1, mono, running, -1);
        const auto late = pipewireFrameTime(mono - 20 * ms, mono + 1 * ms, running + 1 * ms, keepalive.running);
        QCOMPARE(late.running, keepalive.running + 1);
        QCOMPARE(late.ageNs, 21 * ms);
    }
};
QTEST_GUILESS_MAIN(WaylandAcquisition)
#include "test_wayland_acquisition.moc"
