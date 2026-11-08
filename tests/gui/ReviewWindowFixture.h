#pragma once
// Offscreen widget contract for the review window (#283): the video takes the
// window, one timeline sets the trim and mask times, mask controls show only
// when they apply, and recording.review drives the same controls. Not desktop
// proof; the lab captures are in the docs.
#include <QSet>
#include <algorithm>
#include "record/ReviewWindow.h"
#include "record/Duration.h"
#include "record/MaskBox.h"
#include "record/FormatBox.h"
#include "record/Overlays.h"
#include "ui/RegionOverlay.h"
#include <QAccessible>
#include <QAbstractItemView>
#include <QScrollBar>
#include "ui/Ui.h"
#include "ui/Fonts.h"
#include "ui/Theme.h"
#include <QLabel>
#include <QApplication>
#include <QPainter>
#include <QGridLayout>
#include <QVBoxLayout>
#include <QClipboard>
#include <QWheelEvent>
#include "platform/HostOpen.h"
#include <QFontInfo>
#include <QComboBox>
#include <QStyleOptionComboBox>
#include <QStyleOptionSpinBox>
#include <QFileDialog>
#include <QFile>
#include <QDir>
#include <QJsonDocument>
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
#include "fixtures/ReviewLayout.h"
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

class ReviewWindowFixture : public QObject {
    Q_OBJECT
protected:
    QFont baselineFont_;
    Theme::Scheme baselineScheme_;
    // One pixel of the timeline is duration / track width; edges land within two.
    static qint64 slack(const ReviewTimeline& t) { return 2 * duration / t.trackRect().width() + 1; }
    static ReviewTimeline* timeline(ReviewWindow& w) { return w.findChild<ReviewTimeline*>("timeline"); }
private slots:
    void initTestCase() {
        // A case must never restore Qt's pre-install font after the app style's lazy
        // install: later data rows would then measure smaller text.
        Fonts::load();
        Ui::install(*qApp);
        baselineFont_ = QApplication::font();
        baselineScheme_ = Theme::scheme();
    }
    void init() {
        QApplication::setFont(baselineFont_);
        Theme::setScheme(baselineScheme_);
        Ui::WindowHeader::setCustomFrameOverride(1);
    }
    void cleanup() {
        QApplication::setFont(baselineFont_);
        Theme::setScheme(baselineScheme_);
        Ui::WindowHeader::setCustomFrameOverride(1);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
    void cleanupTestCase() { Ui::WindowHeader::setCustomFrameOverride(0); }
};
