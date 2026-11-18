#include "record/Filmstrip.h"
#include <QElapsedTimer>
#include <QSignalSpy>
#include <QtTest>
#include "fixtures/GopClip.h"
#include "fixtures/Waits.h"
#include <algorithm>
using namespace putgd;
using namespace putgd::record;

// #1613: the review filmstrip's thumbnail worker and skim-frame decoder, on
// the GOP clip (tests/fixtures/GopClip.h): a keyframe tile and the exact frame
// in the same GOP differ visibly.
namespace {
constexpr int fps = test::gopclip::fps, gop = test::gopclip::gop, width = 128, height = 72;
constexpr qint64 second = 1'000'000;
using test::gopclip::grayOf;
using test::gopclip::levelOf;
bool writeGopClip(const QString& path, int seconds) { return test::gopclip::write(path, seconds, width, height); }
}

class FilmstripTest : public QObject {
    Q_OBJECT
    QTemporaryDir dir_;
    QString clip_;
    static constexpr int seconds_ = 90;
    static qint64 mid(int g) { return g * second + second / 2; }
private slots:
    void initTestCase() {
        QVERIFY(dir_.isValid());
        clip_ = dir_.filePath("gops.mkv");
        QVERIFY(writeGopClip(clip_, seconds_));
    }
    void tilesArriveAtTheRequestedSizeFromTheirKeyframe() {
        Filmstrip strip;
        QSignalSpy ready(&strip, &Filmstrip::tileReady);
        strip.open(clip_, seconds_ * second, {width, height});
        const QList<qint64> times{mid(5), mid(0), mid(3)};
        strip.request(times, {32, 18});
        PUTGD_TRY_COMPARE_WITH_TIMEOUT(ready.count(), 3, 10000);
        for (const auto& args : ready) {
            const qint64 requested = args[0].toLongLong(), keyframe = args[1].toLongLong();
            const auto tile = args[2].value<QImage>();
            QVERIFY(times.contains(requested));
            QCOMPARE(keyframe, requested / second * second);
            QCOMPARE(tile.size(), QSize(32, 18));
            const int g = int(requested / second);
            QVERIFY2(std::abs(qGray(tile.pixel(16, 9)) - grayOf(levelOf(g * gop))) <= 6, qPrintable(QStringLiteral("gop %1 grey %2").arg(g).arg(qGray(tile.pixel(16, 9)))));
        }
        QCOMPARE(strip.keyframeAtOrBefore(mid(3)), 3 * second);
        QVERIFY(!strip.tile(mid(5)).isNull());
        QVERIFY(strip.tile(mid(40)).isNull());
    }
    void timesInOneGopDecodeOnce() {
        Filmstrip strip;
        QSignalSpy ready(&strip, &Filmstrip::tileReady);
        strip.open(clip_, seconds_ * second, {width, height});
        strip.request({second + 200'000, second + 800'000}, {32, 18});
        PUTGD_TRY_COMPARE_WITH_TIMEOUT(ready.count(), 2, 10000);
        QCOMPARE(strip.stats().decoded, 1);
        // Asking again answers from the cache, queued, without another decode.
        strip.request({second + 500'000}, {32, 18});
        PUTGD_TRY_COMPARE_WITH_TIMEOUT(ready.count(), 3, 10000);
        QCOMPARE(strip.stats().decoded, 1);
        QVERIFY(strip.stats().cached >= 1);
    }
    void aReplacingRequestDropsTheQueuedTimes() {
        Filmstrip strip;
        QSignalSpy ready(&strip, &Filmstrip::tileReady);
        strip.open(clip_, seconds_ * second, {width, height});
        QList<qint64> first;
        for (int g = 0; g < 48; ++g) first.append(mid(g));
        strip.request(first, {32, 18});
        strip.request({mid(80)}, {32, 18});  // no event loop between: the worker is still opening
        QCOMPARE(strip.stats().replaced, 1);
        PUTGD_TRY_VERIFY_WITH_TIMEOUT(!strip.tile(mid(80)).isNull(), 10000);
        QVERIFY(strip.tile(mid(47)).isNull());
        QVERIFY(strip.stats().decoded < 48);
    }
    void cancelAndDestroyReturnPromptly() {
        QElapsedTimer timer;
        auto* strip = new Filmstrip;
        strip->open(clip_, seconds_ * second, {width, height});
        QList<qint64> times;
        for (int g = 0; g < 48; ++g) times.append(mid(g));
        strip->request(times, {32, 18});
        timer.start();
        strip->cancel();
        QVERIFY2(timer.elapsed() < 50, "cancel waited for the worker");
        strip->request(times, {64, 36});
        timer.restart();
        delete strip;
        QVERIFY2(timer.elapsed() < 1000, qPrintable(QStringLiteral("destroying took %1 ms").arg(timer.elapsed())));
        auto* skimmer = new FrameSkimmer;
        skimmer->open(clip_, {width, height});
        skimmer->request(80 * second, {}, 64);
        timer.restart();
        delete skimmer;
        QVERIFY2(timer.elapsed() < 1000, qPrintable(QStringLiteral("destroying the skimmer took %1 ms").arg(timer.elapsed())));
    }
    void aMissingClipIsReportedOnce() {
        Filmstrip strip;
        QSignalSpy failed(&strip, &Filmstrip::failed);
        strip.open(dir_.filePath("absent.mkv"), second, {width, height});
        strip.request({0, second / 2}, {32, 18});
        PUTGD_TRY_COMPARE_WITH_TIMEOUT(failed.count(), 1, 10000);
        PUTGD_COMPARE_STEADY(failed.count(), 1, 100);
    }
    void theSkimmerDecodesTheExactFrameWithCoversApplied() {
        FrameSkimmer skimmer;
        QSignalSpy frames(&skimmer, &FrameSkimmer::frameReady);
        skimmer.open(clip_, {width, height});
        RecordingEdit edit;
        RecordingMask solid;
        solid.id = "m1";
        solid.kind = "solid";
        solid.rect = {0, 0, width / 2, height};
        solid.start = 0;
        solid.end = 3 * second;
        edit.masks.append(solid);
        const qint64 at = second + 500'000;  // frame 45, 15 frames past its keyframe
        skimmer.request(at, edit, 64);
        PUTGD_TRY_COMPARE_WITH_TIMEOUT(frames.count(), 1, 10000);
        const auto args = frames.takeFirst();
        QCOMPARE(args[0].toLongLong(), at);
        QCOMPARE(args[1].toLongLong(), at);
        const auto image = args[2].value<QImage>();
        QCOMPARE(image.size(), QSize(64, 36));
        QVERIFY(qGray(image.pixel(5, 5)) <= 3);  // inside the cover
        const int expected = grayOf(levelOf(45));
        QVERIFY2(std::abs(qGray(image.pixel(60, 5)) - expected) <= 6, qPrintable(QStringLiteral("grey %1, expected %2").arg(qGray(image.pixel(60, 5))).arg(expected)));
        QVERIFY(std::abs(grayOf(levelOf(45)) - grayOf(levelOf(30))) > 12);
        // Past the cover's end the same pixel is not covered.
        skimmer.request(4 * second + 100'000, edit, 64);
        PUTGD_TRY_COMPARE_WITH_TIMEOUT(frames.count(), 1, 10000);
        QVERIFY(qGray(frames.takeFirst()[2].value<QImage>().pixel(5, 5)) > 20);
    }
    // A2: one open skimmer answering a run of times, back and forth across
    // GOPs and within one, gives each time its own frame (timestamp and
    // picture), never the keyframe or another keyframe labelled exact.
    void everySkimAtANonKeyframeIsThatFrame() {
        FrameSkimmer skimmer;
        QSignalSpy frames(&skimmer, &FrameSkimmer::frameReady);
        skimmer.open(clip_, {width, height});
        for (const int index : {45, 47, 59, 31, 13, 76, 75, 1, 89, 44}) {
            const qint64 at = std::llround(index * 1e6 / fps);
            skimmer.request(at, {}, 64);
            PUTGD_TRY_COMPARE_WITH_TIMEOUT(frames.count(), 1, 10000);
            const auto args = frames.takeFirst();
            QCOMPARE(args[0].toLongLong(), at);
            QVERIFY2(std::llabs(args[1].toLongLong() - at) <= 1000, qPrintable(QStringLiteral("frame %1 came back at %2 us").arg(index).arg(args[1].toLongLong())));
            const int grey = qGray(args[2].value<QImage>().pixel(32, 18)), expected = grayOf(levelOf(index));
            QVERIFY2(std::abs(grey - expected) <= 3, qPrintable(QStringLiteral("frame %1: grey %2, expected %3 (its keyframe is %4)").arg(index).arg(grey).arg(expected).arg(grayOf(levelOf(index / gop * gop)))));
        }
    }
    void aCancelledSkimDropsItsAnswer() {
        FrameSkimmer skimmer;
        QSignalSpy frames(&skimmer, &FrameSkimmer::frameReady);
        skimmer.open(clip_, {width, height});
        skimmer.request(60 * second, {}, 64);
        skimmer.cancel();
        PUTGD_COMPARE_STEADY(frames.count(), 0, 400);
        skimmer.request(2 * second, {}, 64);
        PUTGD_TRY_COMPARE_WITH_TIMEOUT(frames.count(), 1, 10000);
    }
};

QTEST_MAIN(FilmstripTest)
#include "test_filmstrip.moc"
