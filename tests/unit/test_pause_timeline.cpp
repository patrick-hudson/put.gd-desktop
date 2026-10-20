#include "record/PauseTimeline.h"
#include <QtTest>
using namespace putgd::record;

namespace {
constexpr std::int64_t ms = 1'000'000;
}

// #130: pause/resume drops and shifts both tracks by the same paused time, so
// the audio-minus-video offset after a resume is the one before it.
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
};
QTEST_GUILESS_MAIN(PauseTimelineContract)
#include "test_pause_timeline.moc"
