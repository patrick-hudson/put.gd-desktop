#include "record/Overlays.h"
#include <QApplication>
#include <QCheckBox>
#include <QJsonArray>
#include <QPushButton>
#include <QSignalSpy>
#include <QtTest>
using namespace putgd::record;

// The recording control bar's two silent and costly mistakes: a recording thrown
// away by one click, and a microphone recorded that nobody asked for.
namespace {
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
    // #1364: what the user asked for decides the microphone. One that is plugged in
    // after the bar opened is offered, never switched on.
    void aMicrophoneIsRecordedOnlyWhenAskedFor() {
        auto options = barOptions();
        options.microphoneUnavailable = "No microphone is connected.";
        ControlBar bar(options, putgd::RecordingConfig{}); bar.show(); QApplication::processEvents();
        auto* microphone = bar.findChild<QCheckBox*>("microphone");
        QVERIFY(microphone);
        QVERIFY(!bar.effectiveMicrophone() && !bar.config().audio.microphone);
        bar.setMicrophone(true, "USB microphone", {});
        QVERIFY(microphone->isEnabled() && !microphone->isChecked());
        QVERIFY(!bar.requestedMicrophone() && !bar.effectiveMicrophone() && !bar.config().audio.microphone);
    }
    // Cancel before Record closes at once; once frames exist one click only asks,
    // and Keep (the focused default) and Escape keep the recording.
    void cancelAsksOnlyOnceRecording() {
        ControlBar bar(barOptions(), putgd::RecordingConfig{});
        QSignalSpy cancelled(&bar, &ControlBar::cancelRequested);
        bar.show();
        auto* cancel = bar.findChild<QPushButton*>("cancel");
        auto* keep = bar.findChild<QPushButton*>("keepRecording");
        auto* discard = bar.findChild<QPushButton*>("discardRecording");
        QVERIFY(cancel && keep && discard);
        cancel->click();
        QCOMPARE(cancelled.size(), 1);
        for (const char* state : {"recording", "paused"}) {
            bar.setState(QString::fromLatin1(state));
            cancel->click();
            QCOMPARE(cancelled.size(), 1);
            QVERIFY(bar.confirming());
            QCOMPARE(bar.focusWidget(),keep);
            QTest::keyClick(&bar, Qt::Key_Escape);
            QVERIFY(!bar.confirming());
            QCOMPARE(cancelled.size(), 1);
        }
        cancel->click();
        keep->click();
        QVERIFY(!bar.confirming());
        QCOMPARE(cancelled.size(), 1);
        cancel->click();
        discard->click();
        QCOMPARE(cancelled.size(), 2);
        // A stop elsewhere drops the question.
        cancel->click();
        bar.setState("stopping");
        QVERIFY(!bar.confirming());
    }
};
QTEST_MAIN(BarPlacement)
#include "test_bar_placement.moc"
