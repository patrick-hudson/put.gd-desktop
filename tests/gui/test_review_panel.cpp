// #1613: the review window's right panel and its folded Edits overlay. The
// panel only proposes: each control must emit its signal with the right id, the
// top part must follow the selection, and the overlay must lend the one panel
// out and give it back. Offscreen: widget behavior, not desktop proof.
#include "record/ReviewPanel.h"
#include "ui/Fonts.h"
#include "ui/Theme.h"
#include "ui/Ui.h"
#include "fixtures/Waits.h"
#include <QAbstractButton>
#include <QApplication>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalSpy>
#include <QSpinBox>
#include <QTest>
#include <QVBoxLayout>

using namespace putgd;
using namespace putgd::record;

namespace {
constexpr qint64 second = 1000000;
constexpr qint64 duration = 32 * second;

RecordingMask cover(const QString& id, const QString& kind, qint64 start, qint64 end) {
    RecordingMask m;
    m.id = id;
    m.kind = kind;
    m.rect = QRect(10, 10, 100, 40);
    m.start = start;
    m.end = end;
    return m;
}

ReviewPanel::State edited() {
    ReviewPanel::State s;
    s.durationUs = duration;
    s.fps = 30;
    s.edit.trimStart = 2600000;
    s.edit.trimEnd = 29400000;
    s.edit.masks = {cover("m1", "solid", 8200000, 12600000), cover("m2", "blur", 22100000, 24900000)};
    RecordingCut cut;
    cut.id = "c1";
    cut.start = 13 * second;
    cut.end = 19500000;
    s.edit.cuts = {cut};
    return s;
}

QAbstractButton* button(QWidget& panel, const QString& name) {
    for (auto* b : panel.findChildren<QAbstractButton*>())
        if (b->accessibleName() == name && b->isVisibleTo(&panel)) return b;
    return nullptr;
}
QWidget* row(QWidget& panel, const QString& key) {
    for (auto* w : panel.findChildren<QWidget*>("reviewPanelRow"))
        if (w->property("rowKey").toString() == key) return w;
    return nullptr;
}
QStringList names(const QJsonObject& inspected) {
    QStringList result;
    for (const auto& b : inspected["buttons"].toArray()) result << b.toObject()["name"].toString();
    return result;
}
bool enabled(const QJsonObject& inspected, const QString& name) {
    for (const auto& b : inspected["buttons"].toArray())
        if (b.toObject()["name"].toString() == name) return b.toObject()["enabled"].toBool();
    return false;
}
}  // namespace

class ReviewPanelTest : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() {
        Fonts::load();
        Ui::install(*qApp);
    }

    // Each state offers exactly its own controls.
    void everyStateShowsItsControls() {
        ReviewPanel panel(std::make_shared<MaskNames>());
        panel.resize(280, 700);
        ReviewPanel::State s;
        s.durationUs = duration;
        panel.setState(s);
        auto info = panel.inspect();
        QCOMPARE(info["mode"].toString(), QString("empty"));
        QCOMPARE(info["title"].toString(), QString("No edits yet"));
        QVERIFY(names(info).isEmpty());
        QVERIFY(info["rows"].toArray().isEmpty());
        QVERIFY(!panel.defaultControl());

        s.timeSelection = recording::Span{8200000, 12600000};
        panel.setState(s);
        info = panel.inspect();
        QCOMPARE(info["mode"].toString(), QString("selection"));
        QCOMPARE(names(info), (QStringList{"Cut", "Redact", "Blur"}));

        s = edited();
        s.selectedKind = "cut";
        s.selectedId = "c1";
        panel.setState(s);
        info = panel.inspect();
        QCOMPARE(info["mode"].toString(), QString("cut"));
        QVERIFY(names(info).contains("Restore") && names(info).contains("Play across"));
        QCOMPARE(panel.defaultControl(), button(panel, "Restore"));
        // The selected cut keeps its span: Restore is above, so its row has no verb button.
        QVERIFY(!names(info).contains("Restore cut"));
        QCOMPARE(info["rows"].toArray().size(), 4);
        // Source order: trim, redaction, cut, blur.
        QStringList order;
        for (const auto& r : info["rows"].toArray()) order << r.toObject()["name"].toString();
        QCOMPARE(order, (QStringList{"Trim", "Redaction 1", "Cut", "Blur 2"}));

        s.selectedKind = "cover";
        s.selectedId = "m1";
        s.coverFirstKept = 8200000;
        s.coverLastKept = 12566667;
        panel.setState(s);
        info = panel.inspect();
        QCOMPARE(info["mode"].toString(), QString("cover"));
        QCOMPARE(info["title"].toString(), QString("Redaction 1"));
        for (const auto& n : {"Shows from", "Until", "Set Shows from to playhead", "Set Until to playhead", "Redact", "Blur", "Remove Redaction 1"})
            QVERIFY2(names(info).contains(n), n);
        QVERIFY(!panel.findChild<QSpinBox*>("reviewPanelStrength"));

        s.selectedId = "m2";
        panel.setState(s);
        QVERIFY(panel.findChild<QSpinBox*>("reviewPanelStrength"));

        s.selectedKind = "trim";
        s.selectedId.clear();
        panel.setState(s);
        info = panel.inspect();
        QCOMPARE(info["mode"].toString(), QString("edits"));
        QVERIFY(names(info).contains("Reset trim"));
    }

    // Every control emits its signal with the right id.
    void buttonsEmitTheirSignals() {
        ReviewPanel panel(std::make_shared<MaskNames>());
        panel.resize(280, 700);
        ReviewPanel::State s;
        s.durationUs = duration;
        s.timeSelection = recording::Span{8200000, 12600000};
        panel.setState(s);
        QSignalSpy cut(&panel, &ReviewPanel::cutRequested), covers(&panel, &ReviewPanel::coverRequested);
        button(panel, "Cut")->click();
        button(panel, "Redact")->click();
        button(panel, "Blur")->click();
        QCOMPARE(cut.size(), 1);
        QCOMPARE(covers.size(), 2);
        QCOMPARE(covers[0][0].toString(), QString("solid"));
        QCOMPARE(covers[1][0].toString(), QString("blur"));

        s = edited();
        s.selectedKind = "cut";
        s.selectedId = "c1";
        panel.setState(s);
        QSignalSpy restore(&panel, &ReviewPanel::restoreCut), across(&panel, &ReviewPanel::playAcross), activated(&panel, &ReviewPanel::editActivated);
        button(panel, "Restore")->click();
        button(panel, "Play across")->click();
        QCOMPARE(restore.size(), 1);
        QCOMPARE(restore[0][0].toString(), QString("c1"));
        QCOMPARE(across.size(), 1);
        QCOMPARE(across[0][0].toString(), QString("c1"));
        QTest::mouseClick(row(panel, "cover/m2"), Qt::LeftButton);
        QCOMPARE(activated.size(), 1);
        QCOMPARE(activated[0][0].toString(), QString("cover"));
        QCOMPARE(activated[0][1].toString(), QString("m2"));
        QTest::keyClick(row(panel, "trim/"), Qt::Key_Return);
        QCOMPARE(activated.size(), 2);
        QCOMPARE(activated[1][0].toString(), QString("trim"));

        s.selectedKind = "cover";
        s.selectedId = "m2";
        s.coverFirstKept = 22100000;
        s.coverLastKept = 24866667;
        panel.setState(s);
        QSignalSpy seek(&panel, &ReviewPanel::seekCoverEdge), toPlayhead(&panel, &ReviewPanel::coverEdgeToPlayhead);
        QSignalSpy kind(&panel, &ReviewPanel::coverKindChanged), strength(&panel, &ReviewPanel::coverStrengthChanged), remove(&panel, &ReviewPanel::removeCover);
        button(panel, "Shows from")->click();
        button(panel, "Until")->click();
        button(panel, "Set Shows from to playhead")->click();
        button(panel, "Set Until to playhead")->click();
        QCOMPARE(seek.size(), 2);
        QCOMPARE(seek[0][1].toBool(), true);
        QCOMPARE(seek[1][1].toBool(), false);
        QCOMPARE(seek[1][0].toString(), QString("m2"));
        QCOMPARE(toPlayhead.size(), 2);
        QCOMPARE(toPlayhead[1][1].toBool(), false);
        QVERIFY(panel.findChild<QWidget*>("reviewPanelCoverKind"));
        QVERIFY(button(panel, "Blur")->isChecked());
        button(panel, "Redact")->click();
        QCOMPARE(kind.size(), 1);
        QCOMPARE(kind[0][0].toString(), QString("m2"));
        QCOMPARE(kind[0][1].toString(), QString("solid"));
        // F2: steps coalesce for 350 ms into one edit.
        panel.findChild<QSpinBox*>("reviewPanelStrength")->setValue(39);
        panel.findChild<QSpinBox*>("reviewPanelStrength")->setValue(40);
        QCOMPARE(strength.size(), 0);
        QTRY_COMPARE(strength.size(), 1);
        QCOMPARE(strength[0][1].toInt(), 40);
        button(panel, "Remove Blur 2")->click();
        QCOMPARE(remove.size(), 1);
        QCOMPARE(remove[0][0].toString(), QString("m2"));

        s.selectedKind = "trim";
        s.selectedId.clear();
        panel.setState(s);
        QSignalSpy reset(&panel, &ReviewPanel::resetTrim);
        button(panel, "Reset trim")->click();
        QCOMPARE(reset.size(), 1);
    }

    // A cover wholly outside the kept footage cannot be jumped to.
    void coverEdgeLinksNeedKeptFrames() {
        ReviewPanel panel(std::make_shared<MaskNames>());
        panel.resize(280, 700);
        auto s = edited();
        s.selectedKind = "cover";
        s.selectedId = "m1";
        panel.setState(s);
        auto info = panel.inspect();
        QVERIFY(!enabled(info, "Shows from"));
        QVERIFY(!enabled(info, "Until"));
        QVERIFY(enabled(info, "Set Shows from to playhead"));
        QVERIFY(panel.findChild<QWidget*>("reviewPanelExcluded"));
        QCOMPARE(button(panel, "Shows from")->toolTip(), QString("Excluded from output"));
        s.coverFirstKept = 8200000;
        s.coverLastKept = 12566667;
        panel.setState(s);
        info = panel.inspect();
        QVERIFY(enabled(info, "Shows from") && enabled(info, "Until"));
        QVERIFY(!panel.findChild<QWidget*>("reviewPanelExcluded"));
    }

    // A typed time lands on the frame grid (30 fps here); text that is not a time changes nothing.
    void fieldEditEmitsAFrameAlignedTime() {
        ReviewPanel panel(std::make_shared<MaskNames>());
        panel.resize(280, 700);
        auto s = edited();
        s.selectedKind = "cover";
        s.selectedId = "m1";
        s.coverFirstKept = 8200000;
        s.coverLastKept = 12566667;
        panel.setState(s);
        QSignalSpy edited(&panel, &ReviewPanel::coverEdgeEdited);
        auto* until = panel.findChild<QLineEdit*>("reviewPanelUntilTime");
        QVERIFY(until);
        QCOMPARE(until->text(), QString("0:12.6"));
        until->setText("0:13.34");  // frame 400.2 -> 400
        until->editingFinished();
        QCOMPARE(edited.size(), 1);
        QCOMPARE(edited[0][0].toString(), QString("m1"));
        QCOMPARE(edited[0][1].toBool(), false);
        QCOMPARE(edited[0][2].toLongLong(), qint64(13333333));
        QCOMPARE(until->toolTip(), QString("Frame 400"));
        until->setText("soon");
        until->editingFinished();
        QCOMPARE(edited.size(), 1);
        QCOMPARE(until->text(), QString("0:13.3"));
        auto* from = panel.findChild<QLineEdit*>("reviewPanelShowsFromTime");
        from->setText("0:08.2");  // unchanged text: no edit
        from->editingFinished();
        QCOMPARE(edited.size(), 1);
        from->setText("99:00.0");  // clamps to the clip
        from->editingFinished();
        QCOMPARE(edited.size(), 2);
        QCOMPARE(edited[1][2].toLongLong(), duration);
    }

    // Delete removes a cover row and never restores a cut.
    void deleteOnRowsOnlyRemovesCovers() {
        ReviewPanel panel(std::make_shared<MaskNames>());
        panel.resize(280, 700);
        auto s = edited();
        panel.setState(s);
        QSignalSpy remove(&panel, &ReviewPanel::removeCover), restore(&panel, &ReviewPanel::restoreCut), reset(&panel, &ReviewPanel::resetTrim);
        QTest::keyClick(row(panel, "cut/c1"), Qt::Key_Delete);
        QTest::keyClick(row(panel, "trim/"), Qt::Key_Delete);
        QCOMPARE(remove.size() + restore.size() + reset.size(), 0);
        QTest::keyClick(row(panel, "cover/m1"), Qt::Key_Delete);
        QCOMPARE(remove.size(), 1);
        QCOMPARE(remove[0][0].toString(), QString("m1"));
        QCOMPARE(restore.size() + reset.size(), 0);
    }

    // U4: with a time selection, Delete on any row cuts the selection (as in the
    // viewer and the timeline) and never removes the row's cover.
    void deleteOnARowCutsATimeSelection() {
        ReviewPanel panel(std::make_shared<MaskNames>());
        panel.resize(280, 900);
        auto s = edited();
        s.timeSelection = recording::Span{3 * second, 5 * second};
        panel.setState(s);
        QSignalSpy remove(&panel, &ReviewPanel::removeCover), cut(&panel, &ReviewPanel::cutRequested);
        QVERIFY(row(panel, "cover/m1"));
        QTest::keyClick(row(panel, "cover/m1"), Qt::Key_Delete);
        QTest::keyClick(row(panel, "cut/c1"), Qt::Key_Backspace);
        QCOMPARE(remove.size(), 0);
        QCOMPARE(cut.size(), 2);
    }

    // The overlay lends the window's one panel and gives it back: same parent,
    // same place in its layout, focus on the toggle.
    void overlayReturnsThePanelAndFocus() {
        QWidget window;
        window.resize(1100, 700);
        auto* layout = new QHBoxLayout(&window);
        auto* toggle = new QPushButton("Edits", &window);
        auto* viewer = new QWidget(&window);
        auto* panel = new ReviewPanel(std::make_shared<MaskNames>(), &window);
        layout->addWidget(toggle);
        layout->addWidget(viewer, 1);
        layout->addWidget(panel);
        panel->setFixedWidth(280);
        auto s = edited();
        s.selectedKind = "cut";
        s.selectedId = "c1";
        panel->setState(s);
        window.show();
        QVERIFY(putgd::test::waitForWindowExposed(&window));
        window.activateWindow();
        QVERIFY(putgd::test::waitForWindowActive(&window));
        const int index = layout->indexOf(panel);

        EditsOverlay overlay(&window);
        QSignalSpy closed(&overlay, &EditsOverlay::closed);
        overlay.openFor(panel, toggle);
        QVERIFY(overlay.isOpen());
        QVERIFY(panel->parentWidget() != &window);
        QCOMPARE(overlay.width(), 300);
        QCOMPARE(QApplication::focusWidget(), panel->defaultControl());
        QCOMPARE(panel->width() <= 300, true);

        QTest::keyClick(QApplication::focusWidget(), Qt::Key_Escape);
        QVERIFY(!overlay.isOpen());
        QCOMPARE(closed.size(), 1);
        QCOMPARE(panel->parentWidget(), &window);
        QCOMPARE(layout->indexOf(panel), index);
        QCOMPARE(panel->maximumWidth(), 280);
        QVERIFY(panel->isVisible());
        QCOMPARE(QApplication::focusWidget(), static_cast<QWidget*>(toggle));

        // A click outside closes it the same way.
        overlay.openFor(panel, toggle);
        QVERIFY(overlay.isOpen());
        QTest::mouseClick(viewer, Qt::LeftButton);
        QVERIFY(!overlay.isOpen());
        QCOMPARE(layout->indexOf(panel), index);
        QCOMPARE(closed.size(), 2);
    }
};

QTEST_MAIN(ReviewPanelTest)
#include "test_review_panel.moc"
