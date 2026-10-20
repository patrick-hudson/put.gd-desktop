#include "record/PauseTimeline.h"
#include <QtTest>
using namespace putgd::record;

namespace {
constexpr std::int64_t ms = 1'000'000;
}

// #130: pause/resume drops and shifts both tracks by the same paused time, so
// the audio-minus-video offset after a resume is the one before it. #129: the
// audio guard keeps a runaway device clock from blocking the muxer.
class PauseTimelineContract : public QObject {
    Q_OBJECT
private slots:
    void withoutPausesBuffersPassUnchanged() {
        PauseTimeline timeline;
        QCOMPARE(timeline.map(0, 0), std::optional<std::int64_t>(0));
        QCOMPARE(timeline.map(1234 * ms, 10 * ms), std::optional<std::int64_t>(1234 * ms));
        QVERIFY(!timeline.paused());
    }
    void anOpenPauseDropsEverythingFromItsStart() {
        PauseTimeline timeline;
        timeline.pause(1000 * ms);
        QVERIFY(timeline.paused());
        QCOMPARE(timeline.map(990 * ms, 10 * ms), std::optional<std::int64_t>(990 * ms));
        // Late buffers captured before the pause still count; one reaching into it does not.
        QVERIFY(!timeline.map(995 * ms, 10 * ms));
        QVERIFY(!timeline.map(1000 * ms, 0));
        QVERIFY(!timeline.map(1500 * ms, 0));
        // A second pause while paused keeps the first start.
        timeline.pause(1200 * ms);
        timeline.resume(1300 * ms);
        QCOMPARE(timeline.pausedTotal(), 300 * ms);
    }
    void aClosedPauseShiftsLaterBuffersByItsLength() {
        PauseTimeline timeline;
        timeline.pause(1000 * ms);
        timeline.resume(4000 * ms);
        QVERIFY(!timeline.paused());
        QVERIFY(!timeline.map(2000 * ms, 0));
        QVERIFY(!timeline.map(3995 * ms, 10 * ms));
        QCOMPARE(timeline.map(4000 * ms, 0), std::optional<std::int64_t>(1000 * ms));
        QCOMPARE(timeline.map(4010 * ms, 10 * ms), std::optional<std::int64_t>(1010 * ms));
        QCOMPARE(timeline.map(500 * ms, 10 * ms), std::optional<std::int64_t>(500 * ms));
    }
    void videoAndAudioKeepTheirOffsetAcrossTheGap() {
        PauseTimeline timeline;
        timeline.pause(8000 * ms);
        timeline.resume(11000 * ms);
        // A 33 ms frame grid and 10 ms audio buffers, 7 ms apart, both after the resume.
        for (std::int64_t video = 11003 * ms; video < 11300 * ms; video += 33 * ms) {
            const std::int64_t audio = video + 7 * ms;
            const auto v = timeline.map(video, 0), a = timeline.map(audio, 10 * ms);
            QVERIFY(v && a);
            QCOMPARE(*a - *v, audio - video);
        }
    }
    void pausesAccumulate() {
        PauseTimeline timeline;
        timeline.pause(1000 * ms);
        timeline.resume(2000 * ms);
        timeline.pause(3000 * ms);
        timeline.resume(3500 * ms);
        QCOMPARE(timeline.map(2500 * ms, 0), std::optional<std::int64_t>(1500 * ms));
        QVERIFY(!timeline.map(3200 * ms, 0));
        QCOMPARE(timeline.map(4000 * ms, 0), std::optional<std::int64_t>(2500 * ms));
        QCOMPARE(timeline.pausedTotal(), 1500 * ms);
    }
    void resumeWithoutPauseAndResetAreHarmless() {
        PauseTimeline timeline;
        timeline.resume(1000 * ms);
        QCOMPARE(timeline.pausedTotal(), std::int64_t(0));
        timeline.pause(1000 * ms);
        timeline.reset();
        QVERIFY(!timeline.paused());
        QCOMPARE(timeline.map(2000 * ms, 0), std::optional<std::int64_t>(2000 * ms));
    }
    void withAFrameRatePausesLastWholeFrames() {
        PauseTimeline timeline;
        timeline.setFrameRate(30);
        // 3.010 s paused is 90.3 frames: the interval lasts 90 frames, exactly 3 s.
        timeline.pause(1000 * ms);
        timeline.resume(4010 * ms);
        QCOMPARE(timeline.pausedTotal(), 3000 * ms);
        // A frame on the 30 fps grid after the resume lands on the same grid.
        const std::int64_t frame = 1'000'000'000 / 30;
        const std::int64_t after = 4000 * ms + 4 * frame;
        QCOMPARE(*timeline.map(after, 0), 1000 * ms + 4 * frame);
        // A pause shorter than half a frame removes nothing.
        timeline.pause(6000 * ms);
        timeline.resume(6010 * ms);
        QCOMPARE(timeline.pausedTotal(), 3000 * ms);
    }
    // #129: pulsesrc on a suspended null sink stamped audio ~2^32 segments ahead.
    void audioStampedInTheFutureIsReanchoredToItsArrival() {
        AudioClockGuard guard;
        // Normal capture: a few ms behind the running time, untouched.
        QCOMPARE(guard.apply(995 * ms, 10 * ms, 1000 * ms), 995 * ms);
        QCOMPARE(guard.corrections(), 0);
        const std::int64_t runaway = std::int64_t(4294967295) * 10 * ms;
        QCOMPARE(guard.apply(runaway, 10 * ms, 2000 * ms), 1990 * ms);
        QCOMPARE(guard.corrections(), 1);
        // The buffers after it keep their spacing from the re-anchored one.
        QCOMPARE(guard.apply(runaway + 10 * ms, 10 * ms, 2011 * ms), 2000 * ms);
        QCOMPARE(guard.apply(runaway + 20 * ms, 10 * ms, 2019 * ms), 2010 * ms);
        QCOMPARE(guard.corrections(), 1);
        // Timestamps that come back to normal drop the correction again.
        QCOMPARE(guard.apply(2025 * ms, 10 * ms, 2030 * ms), 2025 * ms);
        QCOMPARE(guard.correction(), std::int64_t(0));
        // Up to a second ahead is left alone (a clock that runs slightly fast).
        QCOMPARE(guard.apply(2900 * ms, 10 * ms, 2100 * ms), 2900 * ms);
        // Before the pipeline has a clock nothing is decided.
        guard.reset();
        QCOMPARE(guard.apply(runaway, 10 * ms, -1), runaway);
    }
};
QTEST_GUILESS_MAIN(PauseTimelineContract)
#include "test_pause_timeline.moc"
