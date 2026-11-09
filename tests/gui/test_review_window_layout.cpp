#include "ReviewWindowFixture.h"

class ReviewWindowTest : public ReviewWindowFixture {
    Q_OBJECT
private slots:

    void readoutsKeepAReservedLastColumn() {
        ReviewWindow window(options(), config()); window.setEdit(twoMasks()); window.show();
        QVERIFY(putgd::test::waitForWindowExposed(&window));
        QVERIFY(window.drive({{"action", "addMask"}, {"kind", "blur"}}).code.isEmpty());
        window.drive({{"action", "selectMask"}, {"maskId", QJsonValue(QJsonValue::Null)}});
        PUTGD_COMPARE_STEADY(window.findChild<QLabel*>("maskActiveInterval")->text(), QString("No mask selected"),10);
    }
    void labelsStayInsideTheirOwnMaskCoverage() {
        FrameView frame;
        frame.setCanvas({320, 180}); frame.resize(640, 360);
        QImage media(320, 180, QImage::Format_RGB32); media.fill(QColor(130, 60, 120));
        frame.setFrame(media, 0);
        auto small = mask("small", "solid", 1000000, 2000000); small.rect = QRect(80, 60, 50, 24);
        frame.setMasks({small}, small.id); frame.show(); QApplication::processEvents();
        const auto image = frame.grab().toImage();
        const QRect permitted(155, 115, 110, 58);
        for (int y = 1; y < image.height() - 1; ++y) for (int x = 1; x < image.width() - 1; ++x)
            if (!permitted.contains(x, y)) QCOMPARE(image.pixelColor(x, y), media.pixelColor(0, 0));
        ReviewTimeline t;
        t.setDuration(duration); t.setTrim(0, duration); t.setPlayhead(duration - 1);
        t.setMasks({mask("long", "solid", 0, duration - 1000000)}, {});
        t.resize(260, t.sizeHint().height()); t.show(); QApplication::processEvents();
        const auto pixels = t.grab().toImage();
        const auto bar = t.maskRect("long").adjusted(4, 4, -4, -4);
        // A name may paint text, but never a background-coloured label box over coverage.
        for (int y = bar.top(); y <= bar.bottom(); ++y) for (int x = bar.left(); x <= bar.right(); ++x)
            QVERIFY(pixels.pixelColor(x, y) != Theme::current().bg);
        QCOMPARE(t.trackRect().height(), 8);
    }
    void maskReadoutKeepsFractionalTimesAndSelectionGeometry_data() {
        QTest::addColumn<int>("pixels");
        QTest::newRow("text100") << 14;
        QTest::newRow("text150") << 21;
    }
    void maskReadoutKeepsFractionalTimesAndSelectionGeometry() {
        QFETCH(int, pixels);
        auto font = qApp->font(); font.setPixelSize(pixels); qApp->setFont(font);
        auto edit = twoMasks(); edit.masks[1].start = 1200000; edit.masks[1].end = 10200000;
        ReviewWindow window(options(), config()); window.setEdit(edit); window.show();
        QVERIFY(putgd::test::waitForWindowExposed(&window));
        window.resize(1200, 720); QApplication::processEvents();
        auto* interval = window.findChild<QLabel*>("maskActiveInterval");
        const auto geometry = interval->geometry();
        for (const auto& id : QStringList{"m-solid", "m-blur", {}}) {
            window.drive({{"action", "selectMask"}, {"maskId", id.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(id)}});
            QApplication::processEvents();
            QCOMPARE(interval->geometry(), geometry);
            if (id == "m-solid") {
                QVERIFY(interval->text().contains("0:01.2–0:10.2"));
                QVERIFY(interval->width() >= interval->fontMetrics().horizontalAdvance(QStringLiteral("0:01.2–0:10.2")));
            }
            const int measured = interval->width();
            interval->setFixedWidth(512);
            const auto reference = interval->grab().toImage();
            const auto background = reference.pixelColor(0, 0);
            for (int y = 0; y < reference.height(); ++y) for (int x = 0; x < reference.width() - measured; ++x)
                QCOMPARE(reference.pixelColor(x, y), background);
            interval->setFixedWidth(measured); QApplication::processEvents();
        }
    }

    void maskLanesShowHandlesOnlyOnTheSelectedInterval() {
        ReviewTimeline track;
        track.setDuration(duration); track.setTrim(0, duration); track.setPlayhead(duration - 1);
        const auto solid = mask("solid", "solid", 1000000, 10000000);
        const auto blur = mask("blur", "blur", 2000000, 9000000);
        track.setMasks({solid, blur}, {});
        track.resize(700, track.sizeHint().height()); track.show();
        for (const auto scheme : {Theme::Scheme::Dark, Theme::Scheme::Light}) {
            Theme::setScheme(scheme); QApplication::processEvents();
            const auto fill = QColor(scheme == Theme::Scheme::Dark ? "#3A3A37" : "#D9D8D2");
            const auto plain = track.grab().toImage();
            for (const auto id : {"solid", "blur"}) {
                const auto bar = track.maskRect(id);
                QCOMPARE(plain.pixelColor(bar.left() + 2, bar.center().y()), fill);
                QCOMPARE(plain.pixelColor(bar.right() - 2, bar.center().y()), fill);
            }
            track.setMasks({solid, blur}, solid.id); QApplication::processEvents();
            const auto selected = track.grab().toImage();
            const auto bar = track.maskRect(solid.id);
            QCOMPARE(selected.pixelColor(bar.left(), bar.center().y()), Theme::current().accent);
            QCOMPARE(selected.pixelColor(bar.right() + 1, bar.center().y()), Theme::current().accent);
            const auto other = track.maskRect(blur.id);
            QCOMPARE(selected.pixelColor(other.left() + 2, other.center().y()), fill);
            track.setMasks({solid, blur}, {});
        }
    }
    void addBlurGuidanceGivesRecoveryRoomAndResumesUntilDismissed() {
        ReviewWindow window(options(), config()); window.setEdit(twoMasks()); window.show();
        QVERIFY(putgd::test::waitForWindowExposed(&window));
        const auto visible = [&] { return window.inspect()["blurGuidance"].toObject()["visible"].toBool(); };
        QVERIFY(!visible());
        QVERIFY(window.drive({{"action", "addMask"}, {"kind", "blur"}}).code.isEmpty());
        QVERIFY(visible());
        const auto frame = window.findChild<FrameView*>()->geometry();
        window.drive({{"action", "selectMask"}, {"maskId", "m-blur"}});
        QApplication::processEvents();
        QCOMPARE(window.findChild<FrameView*>()->geometry(), frame);
        QVERIFY(window.findChild<QLabel*>("maskActiveInterval")->accessibleDescription().contains(Ui::blurWarningText()));
        QVERIFY(window.inspect()["statusRow"].toArray().isEmpty());
        window.showError("Export failed: disk full");
        QCOMPARE(window.inspect()["statusRow"].toArray().size(), 1);
        // Council 17: the error doesn't hide the warning.
        QVERIFY(visible());
        QVERIFY(buttonNamed(window, "Try again"));
        window.drive({{"action", "dismissStatus"}});
        QVERIFY(window.inspect()["statusRow"].toArray().isEmpty());
        QVERIFY(visible());
        window.drive({{"action", "dismissBlurTip"}});
        QVERIFY(!visible());
        window.drive({{"action", "selectMask"}, {"maskId", "m-solid"}});
        window.drive({{"action", "selectMask"}, {"maskId", "m-blur"}});
        QVERIFY(!visible());
    }
    void savedCopyUsesTheBannerDismissAction() {
        ReviewWindow window(options(), config()); window.show();
        window.setSavedCopy("/nonexistent/synthetic.mp4", 0, config());
        putgd::test::settle(10);
        auto* band = window.findChild<Ui::Banner*>("statusLine"); QVERIFY(band);
        auto* dismiss = band->dismissButton(); QVERIFY(dismiss->isVisibleTo(&window));
        QVERIFY(dismiss->text().isEmpty()); QVERIFY(!dismiss->icon().isNull());
        QCOMPARE(dismiss->accessibleName(), QString("Dismiss"));
        QCOMPARE(dismiss->property("driveAction").toJsonObject()["action"].toString(), QString("dismissSavedCopy"));
        dismiss->setFocus(Qt::TabFocusReason); QTest::keyClick(dismiss, Qt::Key_Space);
        QVERIFY(window.inspect()["statusRow"].toArray().isEmpty());
    }

    void blurChipDistinguishesEnabledAndDisabledActions() {
        ReviewWindow window(options(), config()); window.setEdit(twoMasks()); window.show();
        QVERIFY(putgd::test::waitForWindowExposed(&window));
        QVERIFY(window.drive({{"action", "addMask"}, {"kind", "blur"}}).code.isEmpty());
        auto* chip = window.findChild<QToolButton*>("reviewBlurChip"); QVERIFY(chip);
        for (const auto scheme : {Theme::Scheme::Dark, Theme::Scheme::Light}) {
            Theme::setScheme(scheme);
            window.drive({{"action", "selectMask"}, {"maskId", "m-blur"}}); PUTGD_VERIFY_STEADY(chip->isEnabled(),10);
            const auto size = chip->sizeHint();
            const auto frame = window.findChild<FrameView*>()->geometry();
            const auto enabled = chip->grab().toImage();
            window.drive({{"action", "selectMask"}, {"maskId", "m-solid"}}); PUTGD_VERIFY_STEADY(chip->isEnabled(),10);
            QCOMPARE(chip->toolTip(), QString("Select a blur mask to use Redact"));
            QCOMPARE(chip->accessibleDescription(), chip->toolTip());
            QCOMPARE(chip->text(), QString("Blur may be reversible · Use Redact"));
            putgd::test::settle(10); chip->setFocus(Qt::TabFocusReason); QVERIFY(chip->hasFocus());
            QTest::keyClick(chip, Qt::Key_Backtab); QTest::keyClick(QApplication::focusWidget(), Qt::Key_Tab);
            QCOMPARE(QApplication::focusWidget(), chip);
            chip->clearFocus();
            QCOMPARE(chip->accessibleName(), chip->text());
            QCOMPARE(chip->sizeHint(), size);
            QCOMPARE(window.findChild<FrameView*>()->geometry(), frame);
            const auto& palette = Theme::current();
            const auto ink = Theme::contrast(palette.textDisabled, palette.warningSurface) >= 4.5 ? palette.textDisabled : palette.textSecondary;
            QVERIFY(Theme::contrast(ink, palette.warningSurface) >= 4.5);
            const auto disabled = chip->grab().toImage();
            QVERIFY(disabled != enabled);
            QTest::mouseMove(chip, chip->rect().center()); PUTGD_COMPARE_STEADY(chip->grab().toImage(), disabled,10);
            QSignalSpy proposals(&window, &ReviewWindow::editProposed);
            chip->click(); QCOMPARE(proposals.size(), 0);
            window.drive({{"action", "selectMask"}, {"maskId", "m-blur"}}); PUTGD_VERIFY_STEADY(chip->isEnabled(),10);
            QVERIFY(chip->toolTip() != QString("Select a blur mask to use Redact"));
            chip->click(); QCOMPARE(proposals.size(), 1);
            QCOMPARE(proposed(proposals)["maskId"].toString(), QString("m-blur"));
        }
    }

    void recoveryStaysWithItsMessageAboveTheFooter() {
        ReviewWindow window(options(), config()); window.setEdit(twoMasks()); window.show();
        QVERIFY(putgd::test::waitForWindowExposed(&window));
        window.showError("Export failed: disk full");
        QApplication::processEvents();
        auto* row = window.findChild<QWidget*>("statusRow");
        auto* footer = window.findChild<QWidget*>("reviewFooter");
        auto* details = window.findChild<QPushButton*>("reviewErrorDetails");
        auto* retry = buttonNamed(window, "Try again");
        auto* dismiss = buttonNamed(window, "Dismiss");
        QVERIFY(row && details && retry && dismiss);
        for (auto* action : QList<QAbstractButton*>{details, retry, dismiss}) {
            QVERIFY(row->isAncestorOf(action) && action->isVisible());
            QVERIFY(row->rect().contains(QRect(action->mapTo(row, QPoint()), action->size())));
        }
        QVERIFY(row->geometry().top() > timeline(window)->geometry().bottom());
        QCOMPARE(row->geometry().bottom() + 1, footer->geometry().top());
        QSignalSpy retried(&window, &ReviewWindow::retryExportRequested);
        retry->click(); QCOMPARE(retried.size(), 1);
        dismiss->click(); QVERIFY(row->isHidden());
    }
    // Round 19 R5: recovery actions keep one order in every placement and after
    // any layout history: secondary first, primary nearest ×; space(l) between
    // actions, space(xs) before ×; Tab follows the same order.
    void statusActionsKeepOneOrderAndSpacing() {
        const auto original = qApp->font(); auto restore = qScopeGuard([&] { qApp->setFont(original); });
        const auto rect = [](QWidget* w, QWidget* to) { return QRect(w->mapTo(to, QPoint()), w->size()); };
        const auto checkRow = [&](ReviewWindow& window, const QStringList& expected) {
            auto* band = window.findChild<Ui::Banner*>("statusLine"); QVERIFY(band);
            QList<QAbstractButton*> shown;
            for (auto* button : band->findChildren<QAbstractButton*>()) if (button->isVisibleTo(&window) && !button->text().isEmpty()) shown.append(button);
            std::sort(shown.begin(), shown.end(), [&](auto* a, auto* b) { return rect(a, &window).left() < rect(b, &window).left(); });
            QStringList labels; for (auto* button : shown) labels.append(button->text());
            QCOMPARE(labels, expected);
            auto* dismiss = band->dismissButton(); QVERIFY(dismiss->isVisibleTo(&window));
            window.activateWindow(); shown.first()->setFocus(Qt::TabFocusReason); QVERIFY(shown.first()->hasFocus());
            for (int i = 1; i < shown.size(); ++i) { QTest::keyClick(QApplication::focusWidget(), Qt::Key_Tab); QCOMPARE(QApplication::focusWidget(), static_cast<QWidget*>(shown[i])); }
            QTest::keyClick(QApplication::focusWidget(), Qt::Key_Tab); QCOMPARE(QApplication::focusWidget(), static_cast<QWidget*>(dismiss));
        };
        for (const int pixels : {14, 21}) for (const bool longDestination : {false, true}) {
            auto font = original; font.setPixelSize(pixels); qApp->setFont(font);
            auto o = options();
            // A long destination stays out of the header and fills the footer, so Details… can take the header slot.
            if (longDestination) { o.destination = QStringLiteral("Project archive for customer support in the European Union"); o.uploadSummary = "Upload to " + o.destination; }
            ReviewWindow window(o, config()); window.setEdit(twoMasks()); window.show();
            QVERIFY(putgd::test::waitForWindowExposed(&window));
            window.showError("Export failed: the encoder stopped writing the stream after 412 frames; check the recordings folder, then try again.");
            auto* details = window.findChild<QPushButton*>("reviewErrorDetails"); QVERIFY(details);
            // Down, up and down again: the order may not depend on the previous placement.
            for (const int width : {1800, 1342, 1060, 900, 820, 714, 640, 714, 820, 900, 1060, 1342, 1800, 820, 640}) {
                window.resize(width, 720); putgd::test::settle(10);
                if (window.width() != width) continue;
                auto* band = window.findChild<Ui::Banner*>("statusLine");
                const bool inRow = band->isAncestorOf(details);
                checkRow(window, inRow ? QStringList{"Details…", "Try again"} : QStringList{"Try again"});
            }
            window.drive({{"action", "dismissStatus"}}); putgd::test::settle(10);
            QTemporaryDir dir; QVERIFY(dir.isValid());
            const QString saved = dir.filePath("review copy.mp4");
            QFile file(saved); QVERIFY(file.open(QIODevice::WriteOnly)); file.close();
            window.setSavedCopy(saved, 3, config()); putgd::test::settle(10);
            for (const int width : {1342, 640}) { window.resize(width, 720); putgd::test::settle(10); checkRow(window, {"Copy path", "Show in folder"}); }
        }
    }
    void blurChipActionUsesTheSharedTextActionLook() {
        ReviewWindow window(options(), config()); window.setEdit(twoMasks()); window.show();
        QVERIFY(putgd::test::waitForWindowExposed(&window));
        QVERIFY(window.drive({{"action", "addMask"}, {"kind", "blur"}}).code.isEmpty());
        window.drive({{"action", "selectMask"}, {"maskId", "m-solid"}});
        auto* chip = window.findChild<QToolButton*>("reviewBlurChip"); QVERIFY(chip);
        putgd::test::settle(10); chip->setFocus(Qt::TabFocusReason); QVERIFY(chip->hasFocus());
    }
    void readoutAndRecordingBarUseTheSameTenths() {
        auto o = options(); o.durationUs = 125678901;
        ReviewWindow window(o, config()); window.show();
        QVERIFY(putgd::test::waitForWindowExposed(&window));
        ControlBar::Options barOptions; barOptions.formats = {"mp4"};
        ControlBar bar(barOptions, config());
        for (const qint64 at : {qint64(0), qint64(999999), qint64(60567890)}) {
            window.drive({{"action", "seek"}, {"time", double(at)}});
            bar.setRetained(at);
            QCOMPARE(window.findChild<QLabel*>("time")->text().section("/", 0, 0).trimmed(), bar.findChild<QLabel*>("time")->text());
        }
        QCOMPARE(window.findChild<QLabel*>("time")->text(), QString("1:00.5 / 2:05.6"));
    }
    void stableMaskNamesSurviveRemoval() {
        ReviewWindow window(options(), config());
        window.setEdit(twoMasks()); window.show();
        QVERIFY(putgd::test::waitForWindowExposed(&window));
        QCOMPARE(window.inspect()["selectedMaskName"].toString(), QString("Redaction 2"));
        auto edit = twoMasks(4); edit.masks.removeFirst(); window.setEdit(edit);
        QCOMPARE(window.findChild<QComboBox*>("masks")->currentText(), QString("Redaction 2"));
        QCOMPARE(window.inspect()["masks"].toArray().first().toObject()["name"].toString(), QString("Redaction 2"));
        QVERIFY(window.findChild<FrameView*>()->accessibleDescription().contains("Redaction 2 selected"));
        edit.revision++; edit.masks.append(mask("third", "blur", 0, duration)); window.setEdit(edit);
        QCOMPARE(window.inspect()["selectedMaskName"].toString(), QString("Blur 3"));
    }
    void maskChooserKeepsFullNamesWhenClosedAndInItsPopup() {
        ReviewWindow window(options(), config());
        RecordingEdit edit;
        for (int i = 0; i < 12; ++i) edit.masks.append(mask(QString::number(i), "solid", 0, duration));
        window.setEdit(edit); window.show();
        QVERIFY(putgd::test::waitForWindowExposed(&window));
        auto* chooser = window.findChild<QComboBox*>("masks");
        QVERIFY(chooser);
        for (int i = 0; i < chooser->count(); ++i)
            QVERIFY(chooser->width() >= chooser->fontMetrics().horizontalAdvance(chooser->itemText(i)) + Ui::Metric::splitZone + 18);
        chooser->showPopup(); QApplication::processEvents();
        for (int i = 0; i < chooser->count(); ++i)
            QVERIFY(chooser->view()->width() >= chooser->fontMetrics().horizontalAdvance(chooser->itemText(i)) + 32);
        chooser->hidePopup();
        QVERIFY(chooser->accessibleDescription().contains(chooser->currentText()));
        auto* metadata = window.findChild<QLabel*>("maskActiveInterval");
        QVERIFY(metadata && metadata->isVisible());
        QVERIFY(metadata->mapTo(&window, QPoint()).y() >= window.findChild<FrameView*>()->geometry().bottom());
        auto* audio = window.findChild<QLabel*>("exportAudioSummary");
        QVERIFY(audio && audio->isVisible());
        QVERIFY(audio->accessibleName().endsWith(" · Silent preview"));
    }
    void stackedToolsKeepTheirScopeAndTrimLabels() {
        auto edit = twoMasks(); edit.trimStart = 2000000; edit.trimEnd = 29000000;
        auto o = options(); o.durationUs = 32000000;
        ReviewWindow window(o, config()); window.setEdit(edit); window.show();
        QVERIFY(putgd::test::waitForWindowExposed(&window));
        window.resize(window.minimumSizeHint().width(), 680); QApplication::processEvents();
        auto* trim = window.findChild<QWidget*>("trimControls");
        auto* masks = window.findChild<QWidget*>("maskControls");
        auto* interval = window.findChild<QLabel*>("maskActiveInterval");
        auto* timing = window.findChild<QWidget*>("maskTimingControls");
        QVERIFY(masks->isAncestorOf(timing) && masks->isAncestorOf(interval)); QVERIFY(!trim->isAncestorOf(interval) && !trim->isAncestorOf(timing));
        QVERIFY(trim->isVisible());
        // Timing pairs retain their words, height and independent actions in both rows.
        const QList<std::tuple<QWidget*, QString, QString>> actions{{trim, "Set trim start at playhead", "Start here"}, {trim, "Set trim end at playhead", "End here"}, {trim, "Reset trim", "Reset"}, {timing, "Set mask start at playhead", "Start here"}, {timing, "Set mask end at playhead", "End here"}};
        for (const auto& [row, name, text] : actions) {
            QToolButton* found = nullptr;
            for (auto* button : row->findChildren<QToolButton*>()) if (button->accessibleName() == name) found = button;
            QVERIFY2(found && found->isVisible(), qPrintable(name)); QCOMPARE(found->toolButtonStyle(), Qt::ToolButtonTextOnly); QCOMPARE(found->text(), text);
            QVERIFY(!found->autoRaise());
            QVERIFY(found->height() >= 28 && found->height() >= found->fontMetrics().height());
            QCOMPARE(found->property("uiDensity").toString(), QString("compact"));
            QVERIFY(found->width() >= QFontMetricsF(found->font()).horizontalAdvance(text) + 24);
        }
        for (auto* pair : {window.findChild<QWidget*>("trimTimingPair"), timing}) {
            QVERIFY(pair);QCOMPARE(pair->layout()->spacing(), 1);
            const auto buttons = pair->findChildren<QToolButton*>(QString{}, Qt::FindDirectChildrenOnly);
            QCOMPARE(buttons.size(), 2);QCOMPARE(buttons[0]->geometry().right() + 2, buttons[1]->geometry().left());
            for (auto* button : buttons) QCOMPARE(button->property("visualRole").toString(), QString("quiet"));
        }
        QCOMPARE(buttonNamed(window, "Reset trim")->property("visualRole").toString(), QString("quiet"));
        for (const auto& name : {"trimGroup", "maskGroup"}) QCOMPARE(window.findChild<QLabel*>(name)->mapTo(&window, window.findChild<QLabel*>(name)->contentsRect().topLeft()).x(), 16);
        QVERIFY(window.findChild<QToolButton*>("uploadDestinationMenu")->property("uiFlush").toBool());
        QVERIFY(!window.findChild<QLabel*>("maskTimingGroup"));
        QVERIFY(window.findChild<QLabel*>("muted")->text().startsWith("Keep 0:02–0:29 · 27 s"));
        auto* add = window.findChild<QToolButton*>("addMask");
        QVERIFY(add && add->menu());
        QCOMPARE(add->menu()->actions().size(), 2);
        QCOMPARE(add->menu()->actions()[0]->text(), QString("Add redaction"));
        QCOMPARE(add->menu()->actions()[1]->text(), QString("Add blur"));
        QVERIFY(timeline(window)->isAncestorOf(add));

    }
    void contextualControlsReclaimFrameAndBlurWarningIsOncePerSession() {
        auto o = options(); o.canvas = {1600, 900};
        ReviewWindow window(o, config()); window.show();
        QVERIFY(putgd::test::waitForWindowExposed(&window));
        window.resize(640, 480); QApplication::processEvents();
        auto* controls = window.findChild<QWidget*>("maskControls");
        auto* frame = window.findChild<FrameView*>();
        QVERIFY(!controls->isVisible());
        QVERIFY(frame->height() >= window.height() / 2);
        auto* add = window.findChild<QToolButton*>("addMask");
        QVERIFY(add && add->isVisible());
        QVERIFY(add->parentWidget()->rect().contains(add->geometry()));
        QVERIFY(window.findChild<QWidget*>("trimControls")->isVisible());
        auto* footer = window.findChild<QWidget*>("reviewFooter");
        QVERIFY(footer->height() <= 92);
        window.setEdit(twoMasks());
        QVERIFY(window.drive({{"action", "selectMask"}, {"maskId", "m-blur"}}).code.isEmpty());
        QVERIFY(controls->isVisible());
        QVERIFY(!window.inspect()["blurGuidance"].toObject()["visible"].toBool());
        QVERIFY(window.drive({{"action", "addMask"}, {"kind", "blur"}}).code.isEmpty());
        QVERIFY(window.inspect()["blurGuidance"].toObject()["visible"].toBool());
        window.drive({{"action", "selectMask"}, {"maskId", "m-solid"}});
        window.drive({{"action", "selectMask"}, {"maskId", "m-blur"}});
        QVERIFY(window.inspect()["blurGuidance"].toObject()["visible"].toBool());
        window.drive({{"action", "dismissBlurTip"}});
        QApplication::processEvents();
        const auto before = frame->geometry();
        QTest::keyClick(window.findChild<FrameView*>(), Qt::Key_Escape);
        QApplication::processEvents();
        QVERIFY(controls->isVisible());
        QVERIFY(!window.findChild<QToolButton*>("maskOptions")->isEnabled());
        QCOMPARE(window.findChild<QLabel*>("maskActiveInterval")->text(), QString("No mask selected"));
        QCOMPARE(frame->geometry(), before);
    }
    void selectingAMaskDuringAPressNeverRefitsTheFrame_data() {
        QTest::addColumn<double>("scale");
        QTest::newRow("standard") << 1.0;
        QTest::newRow("150%") << 1.5;
    }
    void selectingAMaskDuringAPressNeverRefitsTheFrame() {
        QFETCH(double, scale);
        const auto original = QApplication::font();
        const auto restore = qScopeGuard([&] { QApplication::setFont(original); });
        auto font = original;
        if (font.pointSizeF() > 0) font.setPointSizeF(font.pointSizeF() * scale);
        else font.setPixelSize(qRound(font.pixelSize() * scale));
        QApplication::setFont(font);
        ReviewWindow window(options(), config()); window.setEdit(twoMasks()); window.show();
        QVERIFY(putgd::test::waitForWindowExposed(&window));
        window.resize(window.minimumSizeHint()); QApplication::processEvents();
        auto* frame = window.findChild<FrameView*>();
        auto* combo = window.findChild<QComboBox*>("masks");
        auto* interval = window.findChild<QLabel*>("maskActiveInterval");
        auto* group = window.findChild<QWidget*>("maskControls");
        const auto flushEvents = [] { for (int i = 0; i < 3; ++i) QApplication::processEvents(); };
        window.drive({{"action", "selectMask"}, {"maskId", QJsonValue(QJsonValue::Null)}}); flushEvents();
        const auto geometry = frame->geometry(), image = frame->imageRect(), row = group->geometry();
        const int comboWidth = combo->width(), intervalWidth = interval->width();
        QVERIFY(group->isVisible()); QCOMPARE(combo->currentIndex(), -1); QCOMPARE(combo->placeholderText(), QString("Select a mask"));
        QCOMPARE(interval->text(), QString("No mask selected"));
        auto* maskOptions = window.findChild<QToolButton*>("maskOptions");
        QVERIFY(!maskOptions->isEnabled()); QCOMPARE(maskOptions->toolTip(), QString("Select a mask first"));
        for (const auto id : {"m-blur", "m-solid"}) {
            window.drive({{"action", "selectMask"}, {"maskId", id}}); flushEvents();
            QCOMPARE(frame->geometry(), geometry); QCOMPARE(frame->imageRect(), image); QCOMPARE(group->geometry(), row);
            QCOMPARE(combo->width(), comboWidth); QCOMPARE(interval->width(), intervalWidth);
        }
        window.drive({{"action", "showMaskOptions"}});
        window.drive({{"action", "selectMask"}, {"maskId", QJsonValue(QJsonValue::Null)}}); flushEvents();
        QVERIFY(!maskOptions->menu()->isVisible());
        const auto m = twoMasks().masks.first();
        const QPoint point = image.topLeft() + QPoint(qRound(m.rect.center().x() * double(image.width()) / options().canvas.width()), qRound(m.rect.center().y() * double(image.height()) / options().canvas.height()));
        QTest::mousePress(frame, Qt::LeftButton, {}, point); flushEvents();
        QVERIFY(!window.inspect()["selectedMaskId"].isNull());
        QCOMPARE(frame->geometry(), geometry); QCOMPARE(frame->imageRect(), image); QCOMPARE(group->geometry(), row);
        QTest::mouseRelease(frame, Qt::LeftButton, {}, point);
    }
    void bottomArrangementStaysTogetherAcrossReviewWidths() {
        ReviewWindow window(options(), config()); window.setEdit(twoMasks()); window.show();
        QVERIFY(putgd::test::waitForWindowExposed(&window));
        auto* trim = window.findChild<QWidget*>("trimControls");
        auto* masks = window.findChild<QWidget*>("maskControls");
        auto* track = timeline(window);
        for (int width : {720, 820, 1060}) {
            window.resize(width, 720); QApplication::processEvents();
            QVERIFY(track->geometry().bottom() < trim->y());
            QVERIFY(trim->geometry().bottom() < masks->y());
            QVERIFY(!track->maskRect("m-blur").isEmpty());
            QVERIFY(!track->maskRect("m-solid").isEmpty());
            auto* interval = window.findChild<QLabel*>("maskActiveInterval");
            auto* footer = window.findChild<QWidget*>("reviewFooter");
            QVERIFY(interval->mapTo(&window, interval->rect().bottomLeft()).y() < footer->y());
        }
    }
    void lanesSpanReviewAboveControlsAndTransportStaysOnOneLine_data() {
        QTest::addColumn<double>("scale");
        QTest::newRow("standard") << 1.0;
        QTest::newRow("150%") << 1.5;
    }
    void lanesSpanReviewAboveControlsAndTransportStaysOnOneLine() {
        QFETCH(double, scale);
        const auto original = QApplication::font();
        const auto restore = qScopeGuard([&] { QApplication::setFont(original); });
        auto font = original;
        if (font.pointSizeF() > 0) font.setPointSizeF(font.pointSizeF() * scale);
        else font.setPixelSize(qRound(font.pixelSize() * scale));
        QApplication::setFont(font);
        auto edit = twoMasks(); edit.trimEnd = duration - 1000;
        ReviewWindow window(options(), config()); window.setEdit(edit); window.show();
        QVERIFY(putgd::test::waitForWindowExposed(&window));
        auto* t = timeline(window);
        auto* trim = window.findChild<QWidget*>("trimControls");
        for (int width : {720, 820, 1060}) {
            window.resize(width, 720); QApplication::processEvents();
            auto* metadata = window.findChild<QWidget*>("reviewTransport");
            QVERIFY(metadata && window.findChild<Ui::WindowHeader*>()->isAncestorOf(metadata));
            QVERIFY(metadata->mapTo(&window, metadata->rect().bottomLeft()).y() < t->mapTo(&window, t->trackRect().topLeft()).y());
            auto* maskControls = window.findChild<QWidget*>("maskControls");
            QVERIFY(metadata->isAncestorOf(window.findChild<QComboBox*>("speed")));
            auto* add = window.findChild<QToolButton*>("addMask");
            QVERIFY(add && t->isAncestorOf(add) && !maskControls->isAncestorOf(add));
            for (auto* control : window.findChildren<QToolButton*>()) if (control->accessibleName() == "Delete mask") {
                QVERIFY(window.findChild<QMenu*>("maskOptionsMenu")->isAncestorOf(control));
            }
            auto* clip = window.findChild<QLabel*>("clipGroup");
            QVERIFY(clip && !clip->isVisible());
            const auto lane = t->trackRect().translated(t->pos());
            QVERIFY(t->geometry().bottom() < trim->y());
            QCOMPARE(t->x(), int(Ui::Space::m));
            QCOMPARE(t->width(), window.width() - 2 * int(Ui::Space::m));
            QVERIFY(lane.width() >= QFontMetricsF(Ui::font(Ui::Type::metadata)).horizontalAdvance(durationText(duration)) * 2 + 24);
            for (const auto& mask : twoMasks().masks) {
                const auto bar = t->maskRect(mask.id).translated(t->pos());
                QVERIFY(bar.top() > lane.bottom());
                QVERIFY(bar.bottom() < window.findChild<QWidget*>("maskControls")->y());
                QVERIFY(bar.left() >= lane.left() && bar.right() <= lane.right());
            }
            window.drive({{"action", "seek"}, {"time", double(duration - 1)}});
            auto* time = window.findChild<QLabel*>("time");
            auto* status = window.findChild<QLabel*>("playbackStatus");
            auto* playback = window.findChild<QToolButton*>("play");
            auto* speed = window.findChild<QComboBox*>("speed");
            auto* masks = window.findChild<QWidget*>("maskControls");
            QVERIFY(window.findChild<Ui::WindowHeader*>()->isAncestorOf(metadata));
            QVERIFY(speed->width() >= speed->fontMetrics().horizontalAdvance(QStringLiteral("0.5×")) + Ui::Metric::splitZone);
            for (auto* control : std::initializer_list<QWidget*>{playback, speed, time}) {
                QVERIFY(metadata->isAncestorOf(control));
                const QRect bounds(control->mapTo(&window, QPoint()), control->size());
                QVERIFY(window.rect().contains(bounds));
                QVERIFY(!bounds.intersects(QRect(trim->mapTo(&window, QPoint()), trim->size())));
                QVERIFY(!bounds.intersects(masks->geometry()));
                if (control != time) QCOMPARE(control->mapTo(&window, control->rect().center()).y(), playback->mapTo(&window, playback->rect().center()).y());
            }
            QVERIFY(!time->text().contains('\n'));
            auto* audio = window.findChild<QLabel*>("exportAudioSummary");
            QVERIFY(audio->isVisible() && !audio->text().contains(status->text()));
            QVERIFY(window.findChild<QWidget*>("reviewMetadata")->isAncestorOf(status));
            if (!status->isVisible()) QVERIFY(window.findChild<QToolButton*>("uploadDestinationMenu")->toolTip().contains("Silent preview"));
            QVERIFY((window.findChild<QWidget*>("reviewDestinationSummary")->isAncestorOf(audio)
                || window.findChild<QWidget*>("reviewMetadata")->isAncestorOf(audio)));
            QVERIFY(t->trackRect().width() >= QFontMetricsF(Ui::font(Ui::Type::metadata)).horizontalAdvance(durationText(duration)) * 2 + 24);
            for (auto* button : window.findChildren<QToolButton*>()) if (button->isVisible() && button->toolButtonStyle() == Qt::ToolButtonTextOnly && button->objectName() != "uploadDestinationMenu" && button->objectName() != "addMask") {
                QVERIFY(button->width() - (button->objectName() == "useRedact" ? 16 : button->objectName() == "showMoreMasks" ? 8 : 24) >= QFontMetricsF(button->font()).horizontalAdvance(button->text()));
            }
            auto* interval = window.findChild<QLabel*>("maskActiveInterval");
            auto* footer = window.findChild<QWidget*>("reviewFooter");
            const int inkPadding = (interval->height() - interval->fontMetrics().tightBoundingRect(interval->text()).height()) / 2;
            QVERIFY(footer->y() - interval->mapTo(&window, interval->rect().bottomLeft()).y() + inkPadding >= 4);
        }
    }
    void reviewControlsShareTheTimelineGrid_data() {
        QTest::addColumn<double>("scale");
        QTest::newRow("standard") << 1.0;
        QTest::newRow("150%") << 1.5;
    }
    void reviewControlsShareTheTimelineGrid() {
        QFETCH(double, scale);
        const auto original = QApplication::font();
        const auto restore = qScopeGuard([&] { QApplication::setFont(original); });
        auto font = original;
        if (font.pointSizeF() > 0) font.setPointSizeF(font.pointSizeF() * scale);
        else font.setPixelSize(qRound(font.pixelSize() * scale));
        QApplication::setFont(font);
        ReviewWindow window(options(), config()); window.setEdit(twoMasks()); window.show();
        QVERIFY(putgd::test::waitForWindowExposed(&window));
        QVERIFY(window.findChild<QWidget*>("trimControls")->height() >= 28);
        QVERIFY(window.findChild<QWidget*>("maskControls")->height() >= 28);
        window.resize(1200, 720); QApplication::processEvents();
        auto* track = timeline(window);
        auto* transport = window.findChild<QWidget*>("reviewTransport");
        QVERIFY(transport->height() >= 28);
        QCOMPARE(track->trackRect().top(), 17);
        QCOMPARE(track->trackRect().left(), ReviewTimeline::gutterWidth());
        QVERIFY(ReviewTimeline::gutterWidth() >= std::min(112, Ui::scaled(96)));
        QVERIFY(ReviewTimeline::gutterWidth() >= QFontMetrics(Ui::labelFont()).horizontalAdvance(QStringLiteral("+ Add mask")) + 20);
        const int axis = track->mapTo(&window, track->trackRect().topLeft()).x();
        for (const char* name : {"trimTimingControls", "maskKinds"}) {
            auto* group = window.findChild<QWidget*>(name);
            QCOMPARE(group->mapTo(&window, QPoint()).x(), axis);
        }
        auto* time = window.findChild<QLabel*>("time");
        QVERIFY(time->text().contains(" / "));
        QCOMPARE(time->font(), Ui::monoFont(Ui::Type::technical));
        auto* frame = window.findChild<FrameView*>();
        QVERIFY(frame->contentsRect().contains(frame->imageRect()));
        auto* add = window.findChild<QToolButton*>("addMask");
        QVERIFY(add->height() >= 28);
        QCOMPARE(add->text(), QString("+ Add mask"));
        QCOMPARE(add->geometry().center().y(), track->maskRect("m-blur").center().y());
        // Lane names keep 3 px around their glyphs at every scale.
        { auto laneFont = Ui::font(Ui::Type::caption); laneFont.setWeight(QFont::Medium);
          QVERIFY(track->maskRect("m-blur").height() >= QFontMetrics(laneFont).tightBoundingRect(QStringLiteral("Redaction 12")).height() + 6); }
        QCOMPARE(track->maskRect("m-solid").top() - track->maskRect("m-blur").bottom() - 1, 2);
        auto* chip = window.findChild<QToolButton*>("reviewBlurChip");
        QVERIFY(chip && !chip->isVisible());
        QVERIFY(window.drive({{"action", "addMask"}, {"kind", "blur"}}).code.isEmpty());
        QApplication::processEvents();
        QVERIFY(chip->isVisible());
        QCOMPARE(chip->text(), QString("Blur may be reversible · Use Redact"));
        QCOMPARE(chip->accessibleName(), chip->text());
        QVERIFY(chip->isEnabled());
        QVERIFY(chip->height() >= chip->heightForWidth(chip->width()));
        window.drive({{"action", "selectMask"}, {"maskId", "m-blur"}});
        QSignalSpy proposed(&window, &ReviewWindow::editProposed);
        chip->click();
        QCOMPARE(proposed.size(), 1);
        QCOMPARE(proposed.first()[1].toJsonArray().first().toObject()["mask"].toObject()["kind"].toString(), QString("solid"));
    }
    void compactFooterPrioritizesDestinationName_data() {
        QTest::addColumn<double>("scale");
        QTest::newRow("standard") << 1.0;
        QTest::newRow("150%") << 1.5;
    }
    void compactFooterPrioritizesDestinationName() {
        QFETCH(double, scale);
        const auto original = QApplication::font();
        const auto restore = qScopeGuard([&] { QApplication::setFont(original); });
        auto font = original;
        if (font.pointSizeF() > 0) font.setPointSizeF(font.pointSizeF() * scale);
        else font.setPixelSize(qRound(font.pixelSize() * scale));
        QApplication::setFont(font);
        auto o = options(); o.audioRecorded = true; o.destination = "Proton Drive";
        o.uploadSummary = "Upload to Proton Drive · Shared link";
        auto c = config(); c.audio.system = true;
        ReviewWindow window(o, c); window.setEdit(twoMasks()); window.show();
        QVERIFY(putgd::test::waitForWindowExposed(&window));
        auto* destination = window.findChild<Ui::ElidedLabel*>("uploadDestination");
        auto* output = window.findChild<QToolButton*>("uploadDestinationMenu");
        const auto nameFits = [&] {
            return output && output->isVisible() && output->toolTip().contains(o.destination) && output->text().contains("Shared link")
                && output->toolTip().contains(o.destination) && output->menu()
                && output->height() >= output->fontMetrics().height() && window.rect().contains(QRect(output->mapTo(&window, QPoint()), output->size()))
                && output->text().contains(o.destination) && !output->text().contains(QChar(0x2026))
                && (output->property("headerSummary").toBool() || output->height() >= output->heightForWidth(output->width()));
        };
        window.resize(window.minimumSizeHint()); QApplication::processEvents();
        QVERIFY2(nameFits(), qPrintable(destination->text()));
        window.drive({{"action", "selectMask"}, {"maskId", "m-blur"}});
        window.drive({{"action", "format"}, {"format", "mp4"}, {"codec", "h265"}});
        window.showError("Export failed: disk full. Free up space and try again.");
        for (int i = 0; i < 3; ++i) {
            window.resize(window.minimumSizeHint().width() + i, 680);
            QApplication::processEvents();
            QVERIFY2(nameFits(), qPrintable(destination->text()));
        }
    }
    // Every review control lies whole inside the window and its own container,
    // and no two overlap, at the default and minimum sizes with normal and 150%
    // text: steady, with a blur selected (radius and guidance) and with an error.
    void narrowTimelineUsesTheFullRowAndStatusDoesNotShiftIt_data() {
        QTest::addColumn<double>("scale");
        QTest::newRow("standard") << 1.0;
        QTest::newRow("150%") << 1.5;
    }
    void narrowTimelineUsesTheFullRowAndStatusDoesNotShiftIt() {
        QFETCH(double, scale);
        const auto original = QApplication::font();
        const auto restore = qScopeGuard([&] { QApplication::setFont(original); });
        auto font = original;
        if (font.pointSizeF() > 0) font.setPointSizeF(font.pointSizeF() * scale);
        else font.setPixelSize(qRound(font.pixelSize() * scale));
        QApplication::setFont(font);
        auto edit = twoMasks(); edit.trimStart = 2000000;
        ReviewWindow window(options(), config()); window.setEdit(edit); window.show();
        QVERIFY(putgd::test::waitForWindowExposed(&window));
        auto* track = timeline(window);
        auto* transport = window.findChild<QWidget*>("reviewTransport");
        track->setFixedWidth(520); QApplication::processEvents();
        QVERIFY(transport->mapTo(&window, transport->rect().bottomLeft()).y() < track->mapTo(&window, QPoint()).y());
        QCOMPARE(track->trackRect().left(), ReviewTimeline::gutterWidth());
        QVERIFY(track->trackRect().width() >= 360);
        QCOMPARE(track->trackRect().top(), 17);
        const auto axis = track->trackRect();
        const auto before = window.geometry();
        QVERIFY(window.drive({{"action", "seek"}, {"time", 0}}).code.isEmpty()); QApplication::processEvents();
        QVERIFY(window.findChild<QLabel*>("playbackStatus")->isVisible());
        QCOMPARE(window.findChild<QLabel*>("playbackStatus")->text(), QStringLiteral("Silent preview"));
        QVERIFY(window.findChild<FrameView*>()->outsideKeptPart());
        QCOMPARE(track->trackRect(), axis);
        QCOMPARE(window.geometry(), before);
        track->setMinimumWidth(0); track->setMaximumWidth(QWIDGETSIZE_MAX);
        window.resize(1342, 720); QApplication::processEvents();
        QVERIFY(transport->mapTo(&window, transport->rect().bottomLeft()).y() < track->mapTo(&window, QPoint()).y());
        QVERIFY(track->trackRect().width() >= 360);
        QCOMPARE(track->trackRect().top(), 17);
    }
    void purposeFormatChoicesKeepTheCanonicalIds() {
        auto o = options();
        ReviewWindow window(o, config()); window.show();
        QVERIFY(putgd::test::waitForWindowExposed(&window));
        QComboBox* formats = nullptr;
        for (auto* combo : window.findChildren<QComboBox*>()) if (combo->accessibleName() == "Output format") formats = combo;
        QVERIFY(formats);
        const auto choose = [&](QComboBox* combo, const QStringList& id) {
            const int index = combo->findData(id);
            QVERIFY(index >= 0);
            combo->setCurrentIndex(index);
            QCOMPARE(combo->currentData().toStringList(), id);
            QVERIFY(!combo->itemData(index, Qt::ToolTipRole).toString().isEmpty());
            QCOMPARE(combo->toolTip(), combo->itemData(index, Qt::ToolTipRole).toString());
        };
        choose(formats, {"mp4", "h265"});
        QCOMPARE(window.inspect()["format"].toObject()["codec"].toString(), QString("h265"));
        choose(formats, {"mp4", "h264"});
        QVERIFY(formats->currentText().contains("Recommended"));
        ControlBar::Options b; b.formats = o.formats; b.codecs = o.codecs;
        ControlBar bar(b, config());
        QSignalSpy changed(&bar, &ControlBar::configChanged);
        auto* barFormats = bar.findChild<QComboBox*>("format");
        QVERIFY(barFormats);
        choose(barFormats, {"webm", "vp9"});
        QVERIFY(!changed.isEmpty());
        QCOMPARE(qvariant_cast<RecordingConfig>(changed.last().first()).format, QString("webm"));
        QCOMPARE(qvariant_cast<RecordingConfig>(changed.last().first()).codec, QString("vp9"));
    }
    void maskPlacementCommandsArePlainActions() {
        ReviewWindow window(options(), config()); window.show();
        QVERIFY(putgd::test::waitForWindowExposed(&window));
        auto* add = window.findChild<QToolButton*>("addMask");
        const auto entries = add->menu()->actions();
        QCOMPARE(entries.size(), 2);
        for (auto* entry : entries) QVERIFY(!entry->isCheckable());
        entries[0]->trigger();
        QCOMPARE(window.inspect()["armed"].toString(), QString("solid"));
        QVERIFY(window.findChild<FrameView*>()->accessibleDescription().contains("redaction", Qt::CaseInsensitive));
        entries[1]->trigger();
        QCOMPARE(window.inspect()["armed"].toString(), QString("blur"));
        QVERIFY(window.findChild<FrameView*>()->accessibleDescription().contains("blur", Qt::CaseInsensitive));
    }
    void footerMovesTheSameWidgetsAtMeasuredFit_data() {
        QTest::addColumn<bool>("recorded");
        QTest::addColumn<bool>("enabled");
        QTest::addColumn<QString>("format");
        QTest::newRow("system-audio") << true << true << QString("mp4");
        QTest::newRow("no-audio") << false << false << QString("mp4");
        QTest::newRow("audio-off") << true << false << QString("mp4");
        QTest::newRow("silent-format") << true << true << QString("gif");
    }
    void footerMovesTheSameWidgetsAtMeasuredFit() {
        QFETCH(bool, recorded);
        QFETCH(bool, enabled);
        QFETCH(QString, format);
        Ui::WindowHeader::setCustomFrameOverride(-1);
        auto o = options(); o.audioRecorded = recorded;
        auto c = config(); c.audio.system = enabled; c.format = format;
        ReviewWindow window(o, c); window.show();
        QVERIFY(putgd::test::waitForWindowExposed(&window));
        auto* output = window.findChild<QToolButton*>("uploadDestinationMenu");
        auto* audio = window.findChild<QLabel*>("exportAudioSummary");
        auto* save = buttonNamed(window, "Save a copy…");
        auto* discard = window.findChild<QPushButton*>("discard");
        auto* grid = qobject_cast<QGridLayout*>(window.findChild<QWidget*>("reviewActions")->layout());
        auto* separator = window.findChild<QLabel*>("reviewAudioSeparator");
        QVERIFY(output && audio && save && discard && grid && separator);
        for (const int width : {1366, 640, 1366}) {
            window.resize(width, 720);
            for (int i = 0; i < 3; ++i) QApplication::processEvents();
            QCOMPARE(grid->property("singleRow").toBool(), width == 1366);
            QVERIFY2(putgd::test::reviewFooterProblem(window).isEmpty(), qPrintable(putgd::test::reviewFooterProblem(window)));
            QVERIFY(output->text().contains("Shared link"));
            QVERIFY(output->toolTip().contains(options().destination));
            QCOMPARE(save->text(), QString("Save a copy…"));
            QCOMPARE(discard->text(), QString("Discard…"));
            const int outputY = output->mapTo(&window, output->rect().center()).y();
            QCOMPARE(audio->mapTo(&window, audio->rect().center()).y(), outputY);
            QVERIFY(separator->isVisible());
            QCOMPARE(separator->text(), QStringLiteral(" · "));
            QCOMPARE(separator->mapTo(&window, QPoint()).x(), audio->mapTo(&window, QPoint(audio->width(), 0)).x());
            auto* preview = window.findChild<QLabel*>("playbackStatus");
            QCOMPARE(preview->mapTo(&window, QPoint()).x(), separator->mapTo(&window, QPoint(separator->width(), 0)).x());
            QVERIFY(window.findChild<QWidget*>("reviewMetadata")->isAncestorOf(audio));
            QCOMPARE(audio->width(), audio->sizeHint().width());
            if (width == 1366) {
                QCOMPARE(save->mapTo(&window, save->rect().center()).y(), outputY);
                QVERIFY(window.findChild<QWidget*>("reviewFooter")->height() <= 48);
            }
            else QVERIFY(save->mapTo(&window, save->rect().center()).y() > outputY);
        }
    }
    // Final read: at 1060×720 the one-row footer painted its metadata under
    // Save a copy… and Discard…. Every width keeps each footer child apart.

    void destinationMovesToTheHeaderOnlyWhenItFits_data() {
        QTest::addColumn<int>("pixels");
        QTest::addColumn<bool>("privateUpload");
        QTest::addColumn<bool>("light");
        for (int pixels : {14, 21}) for (bool privateUpload : {false, true}) for (bool light : {false, true})
            QTest::newRow(qPrintable(QString("%1px-%2-%3").arg(pixels).arg(privateUpload ? "private" : "shared").arg(light ? "light" : "dark"))) << pixels << privateUpload << light;
    }
    void destinationMovesToTheHeaderOnlyWhenItFits() {
        QFETCH(int, pixels); QFETCH(bool, privateUpload); QFETCH(bool, light);
        Theme::setScheme(light ? Theme::Scheme::Light : Theme::Scheme::Dark);
        auto font = qApp->font(); font.setPixelSize(pixels); qApp->setFont(font);
        auto o = options(); o.audioRecorded = true;
        o.destination = QStringLiteral("Project archive for customer support in the European Union");
        o.sharingMode = privateUpload ? "private" : "share";
        o.uploadSummary = (privateUpload ? "Private upload to " : "Upload to ") + o.destination;
        auto c = config(); c.audio.system = true;
        ReviewWindow window(o, c); window.setEdit(twoMasks()); window.show();
        QVERIFY(putgd::test::waitForWindowExposed(&window));
        auto* header = window.findChild<Ui::WindowHeader*>();
        auto* output = window.findChild<QToolButton*>("uploadDestinationMenu");
        auto* menu = output->menu();
        auto* draft = window.findChild<QLabel*>("draftState");
        auto* audio = window.findChild<QLabel*>("exportAudioSummary");
        auto* transport = window.findChild<QWidget*>("reviewTransport");
        const auto bounds = [&](QWidget* w) { return QRect(w->mapTo(&window, QPoint()), w->size()); };
        for (const int width : {1600, 1340, 1060, 640, 1600}) {
            window.resize(width, 720);
            for (int i = 0; i < 4; ++i) QApplication::processEvents();
            QCOMPARE(output, window.findChild<QToolButton*>("uploadDestinationMenu"));
            QCOMPARE(menu, output->menu());
            const bool inHeader = output->property("headerSummary").toBool();
            if (width == 1600 || width == 640) QCOMPARE(inHeader, width == 1600);
            QVERIFY2(putgd::test::reviewFooterProblem(window).isEmpty(), qPrintable(putgd::test::reviewFooterProblem(window)));
            QCOMPARE(header->isAncestorOf(output), inHeader);
            QCOMPARE(header->isAncestorOf(draft), inHeader);
            QCOMPARE(header->isAncestorOf(audio), inHeader);
            QVERIFY(output->isVisible() && draft->isVisible() && audio->isVisible());
            QVERIFY(output->toolTip().contains(o.destination));
            QVERIFY(output->accessibleDescription().contains(o.destination));
            QVERIFY(output->accessibleDescription().contains(privateUpload ? "Private upload" : "Shared link"));
            for (auto* widget : std::initializer_list<QWidget*>{output, draft, audio, transport}) QVERIFY(window.rect().contains(bounds(widget)));
            if (inHeader) {
                const auto* view = window.findChild<FrameView*>();
                const qreal frameCentre = view->mapTo(&window, QPointF(view->imageRect().center())).x();
                const qreal transportCentre = transport->mapTo(&window, QPointF(transport->width() / 2.0, 0)).x();
                QVERIFY2(qAbs(transportCentre - frameCentre) <= 2, qPrintable(QString("transport %1, frame %2").arg(transportCentre).arg(frameCentre)));
                QVERIFY(output->maximumWidth() <= output->fontMetrics().horizontalAdvance(output->text()) + Ui::space(Ui::Space::l));
                QTest::mouseMove(&window, QPoint(5, window.height() - 5));
                output->clearFocus(); QApplication::processEvents();
                const auto image = header->grab().toImage();
                QCOMPARE(image.pixelColor(header->width() / 3, 3), Theme::current().bg);
                const auto menuImage = output->grab().toImage();
                QCOMPARE(menuImage.pixelColor(output->width() / 2, 3), Theme::current().bg);
                QVERIFY(bounds(output).right() < bounds(transport).left());
                QVERIFY(bounds(transport).right() < bounds(audio).left());
                QVERIFY(bounds(audio).right() < bounds(draft).left());
                for (const auto* name : {"windowMinimize", "windowMaximize", "windowClose"}) {
                    const auto button = bounds(window.findChild<QToolButton*>(name));
                    QVERIFY(!button.intersects(bounds(audio)));
                    if (button.left() > bounds(draft).right()) QVERIFY(button.left() - bounds(draft).right() - 1 >= Ui::space(Ui::Space::l));
                }
                const auto suffix = output->property("sharingSuffix").toString();
                QVERIFY(output->width() >= output->fontMetrics().horizontalAdvance(suffix + QStringLiteral("…")) + 24);
                QVERIFY(!window.findChild<QLabel*>("brandName")->isVisible());
            } else QVERIFY(window.findChild<QWidget*>("reviewFooter")->isAncestorOf(output));
            auto* speed = window.findChild<QComboBox*>("speed");
            const int speedWidth = speed->width();
            for (int index = 0; index < speed->count(); ++index) {
                speed->setCurrentIndex(index);
                QStyleOptionComboBox option; option.initFrom(speed); option.frame = false; option.currentText = speed->currentText();
                const auto text = speed->style()->subControlRect(QStyle::CC_ComboBox, &option, QStyle::SC_ComboBoxEditField, speed);
                const auto arrow = speed->style()->subControlRect(QStyle::CC_ComboBox, &option, QStyle::SC_ComboBoxArrow, speed);
                QVERIFY2(qAbs(text.width() - QFontMetricsF(speed->font()).horizontalAdvance(speed->currentText())) <= 1,
                    qPrintable(QString("speed '%1': text %2, advance %3, control %4, font %5; painted %6").arg(speed->currentText()).arg(text.width()).arg(QFontMetricsF(speed->font()).horizontalAdvance(speed->currentText())).arg(speed->width()).arg(speed->font().toString(), Ui::font(Ui::Type::technical).toString())));
                QCOMPARE(arrow.left() - text.right() - 1, Ui::space(Ui::Space::xs));
                QVERIFY(speed->rect().contains(arrow));
                QCOMPARE(speed->width(), speedWidth);
            }
            bool opened = false;
            QTimer::singleShot(0, output, [&] { opened = menu->isVisible(); menu->close(); });
            output->setFocus(Qt::TabFocusReason); QTest::keyClick(output, Qt::Key_Space);
            QVERIFY(opened);
        }
    }
    void maskLanePagingPreservesSelectionAndDoesNotWrap() {
        ReviewWindow window(options(), config());
        RecordingEdit edit;
        for (int i = 0; i < 8; ++i) edit.masks.append(mask(QString::number(i), "solid", 0, duration));
        window.setEdit(edit); window.show();
        QVERIFY(putgd::test::waitForWindowExposed(&window));
        auto* next = window.findChild<QToolButton*>("showMoreMasks");
        auto* previous = window.findChild<QToolButton*>("previousMaskLanes");
        auto* scroll = window.findChild<QScrollBar*>("maskLaneScroll");
        QVERIFY(next && next->isVisible() && previous && previous->isVisible());
        QVERIFY(scroll && !scroll->isVisible());
        QVERIFY(window.drive({{"action", "selectMask"}, {"maskId", "0"}}).code.isEmpty());
        QCOMPARE(window.inspect()["timeline"].toObject()["laneSummary"].toString(), QString("Lanes 1–2 of 8"));
        QVERIFY(!previous->isEnabled());
        for (const int first : {2, 4, 6, 6}) {
            QVERIFY(window.drive({{"action", "nextMaskLanes"}}).code.isEmpty());
            QCOMPARE(scroll->value(), first);
            QCOMPARE(window.inspect()["timeline"].toObject()["laneSummary"].toString(), QString("Lanes %1–%2 of 8").arg(first + 1).arg(first + 2));
            QCOMPARE(window.inspect()["selectedMaskId"].toString(), QString("0"));
            window.drive({{"action", "seek"}, {"time", double(first * 100000)}});
            QCOMPARE(scroll->value(), first);
        }
        QVERIFY(!next->isEnabled());
        previous->click(); QCOMPARE(scroll->value(), 4);
        QVERIFY(window.drive({{"action", "previousMaskLanes"}}).code.isEmpty()); QCOMPARE(scroll->value(), 2);
        QCOMPARE(window.inspect()["selectedMaskId"].toString(), QString("0"));
        // Selecting a different mask still reveals its lane through session/CLI/MCP.
        QVERIFY(window.drive({{"action", "selectMask"}, {"maskId", "7"}}).code.isEmpty());
        QVERIFY(!timeline(window)->maskRect("7").isEmpty());
        QVERIFY(timeline(window)->maskRect("7").height() >= Ui::scaled(16));
    }
    void maskOptionsExposeRadiusAndDeletionThroughTheSameDrive_data() {
        QTest::addColumn<double>("scale");
        QTest::newRow("standard") << 1.0;
        QTest::newRow("150%") << 1.5;
    }
    void maskOptionsExposeRadiusAndDeletionThroughTheSameDrive() {
        QFETCH(double, scale);
        const auto original = QApplication::font();
        const auto restore = qScopeGuard([&] { QApplication::setFont(original); });
        auto font = original;
        if (font.pointSizeF() > 0) font.setPointSizeF(font.pointSizeF() * scale);
        else font.setPixelSize(qRound(font.pixelSize() * scale));
        QApplication::setFont(font);
        ReviewWindow window(options(), config()); window.setEdit(twoMasks()); window.show();
        QVERIFY(putgd::test::waitForWindowExposed(&window));
        const auto geometry = window.geometry();
        QVERIFY(window.drive({{"action", "selectMask"}, {"maskId", "m-blur"}}).code.isEmpty());
        QVERIFY(window.drive({{"action", "showMaskOptions"}}).state["maskOptions"].toObject()["open"].toBool());
        auto* menu = window.findChild<QMenu*>("maskOptionsMenu");
        QVERIFY(menu && menu->isVisible());
        auto* radius = menu->findChild<QSpinBox*>(); QVERIFY(radius && radius->isVisible());
        QToolButton* remove = nullptr;
        for (auto* button : menu->findChildren<QToolButton*>()) if (button->accessibleName() == "Delete mask") remove = button;
        QVERIFY(remove && remove->isVisible());
        QVERIFY(remove->width() - remove->iconSize().width() - 32 >= std::ceil(QFontMetricsF(remove->font()).horizontalAdvance(remove->text())));
        QSignalSpy edits(&window, &ReviewWindow::editProposed);
        QVERIFY(window.drive({{"action", "strength"}, {"strength", 24}}).code.isEmpty());
        QCOMPARE(edits.size(), 1);
        QVERIFY(!window.drive({{"action", "closeMaskOptions"}}).state["maskOptions"].toObject()["open"].toBool());
        QCOMPARE(window.geometry(), geometry);
    }
    void maskBadgesFollowHoverSelectionAndPlayback() {
        FrameView frame;
        frame.setCanvas({640, 360}); frame.resize(640, 360);
        QImage media(640, 360, QImage::Format_RGB32); media.fill(QColor(130, 60, 120));
        frame.setFrame(media, 0);
        auto first = mask("first", "solid", 0, duration); first.rect = QRect(30, 60, 230, 140);
        auto second = first; second.id = "second"; second.rect.moveLeft(350);
        frame.setMasks({first, second}, first.id); frame.show(); QApplication::processEvents();
        QTest::mouseMove(&frame, QPoint(10, 10));
        const auto idle = frame.grab().toImage().convertToFormat(media.format());
        const QRect afterNumber(390, 66, 60, 10);
        QCOMPARE(idle.copy(afterNumber), media.copy(afterNumber));
        QTest::mouseMove(&frame, second.rect.center());
        const auto hovered = frame.grab().toImage().convertToFormat(media.format());
        QVERIFY(hovered.copy(afterNumber) != idle.copy(afterNumber));
        frame.setPlaying(true);
        QCOMPARE(frame.grab().toImage().convertToFormat(media.format()).copy(media.rect().adjusted(1,1,-1,-1)), media.copy(media.rect().adjusted(1,1,-1,-1)));
        frame.setPlaying(false);
        QVERIFY(frame.grab().toImage().convertToFormat(media.format()) != media);
    }
    void controlsStayInsideTheWindowAndApart_data() {
        QTest::addColumn<double>("scale");
        QTest::newRow("standard") << 1.0;
        QTest::newRow("150%") << 1.5;
    }
    void controlsStayInsideTheWindowAndApart() {
        QFETCH(double, scale);
        const auto original = QApplication::font();
        const auto restore = qScopeGuard([&] { QApplication::setFont(original); });
        auto font = original;
        if (font.pointSizeF() > 0) font.setPointSizeF(font.pointSizeF() * scale);
        else font.setPixelSize(qRound(font.pixelSize() * scale));
        QApplication::setFont(font);
        auto o = options(); o.audioRecorded = true; o.durationUs = 32000000;
        auto edit = twoMasks(); edit.trimStart = 2000000; edit.trimEnd = 29000000;
        ReviewWindow window(o, config()); window.setEdit(edit); window.show();
        QVERIFY(putgd::test::waitForWindowExposed(&window));
        const auto flushEvents = [&] { for (int i = 0; i < 3; ++i) { QApplication::processEvents(); QApplication::sendPostedEvents(); } };
        const auto check = [&](const QString& state) {
            flushEvents();
            const auto where = [&](QWidget* w) { return QStringLiteral("(min %1x%2 hint %3x%4) ").arg(window.minimumWidth()).arg(window.minimumHeight()).arg(window.minimumSizeHint().width()).arg(window.minimumSizeHint().height()) + QStringLiteral("%1 %2 %3 at %4x%5: %6,%7 %8x%9").arg(state, QString::fromLatin1(w->metaObject()->className()), w->objectName() + "/" + w->accessibleName()).arg(window.width()).arg(window.height()).arg(w->mapTo(&window, QPoint()).x()).arg(w->mapTo(&window, QPoint()).y()).arg(w->width()).arg(w->height()); };
            // The frameless window's resize edges lie over its border by design.
            const auto edge = [&](QWidget* w) { return w->parentWidget() == &window && QList<Qt::CursorShape>{Qt::SizeHorCursor, Qt::SizeVerCursor, Qt::SizeFDiagCursor, Qt::SizeBDiagCursor}.contains(w->cursor().shape()); };
            QList<QWidget*> shown;
            for (auto* w : window.findChildren<QWidget*>()) if (w->isVisible() && !w->isWindow() && !edge(w)) shown.append(w);
            for (auto* w : shown) {
                QVERIFY2(window.contentsRect().contains(QRect(w->mapTo(&window, QPoint()), w->size())), qPrintable(where(w)));
                QVERIFY2(w->parentWidget()->rect().contains(w->geometry()), qPrintable(where(w)));
            }
            for (int i = 0; i < shown.size(); ++i) for (int j = i + 1; j < shown.size(); ++j) {
                auto *a = shown[i], *b = shown[j];
                if (a->isAncestorOf(b) || b->isAncestorOf(a)) continue;
                const QRect first(a->mapTo(&window, QPoint()), a->size()), second(b->mapTo(&window, QPoint()), b->size());
                QVERIFY2(!first.intersects(second), qPrintable(where(a) + " overlaps " + where(b)));
            }
        };
        for (const bool minimum : {false, true}) {
            if (minimum) { for (int i = 0; i < 3; ++i) { window.resize(window.minimumSizeHint()); flushEvents(); } }
            const QString size = minimum ? QStringLiteral("minimum") : QStringLiteral("default");
            window.drive({{"action", "selectMask"}, {"maskId", "m-solid"}});
            check(size + " steady");
            window.drive({{"action", "selectMask"}, {"maskId", "m-blur"}});
            check(size + " blur");
            window.drive({{"action", "format"}, {"format", "mp4"}, {"codec", "h265"}});
            window.showError("Export failed: the encoder stopped after 412 frames because the disk holding the private recording is full; free some space and try again");
            check(size + " error");
            window.drive({{"action", "dismissStatus"}});
            window.drive({{"action", "format"}, {"format", "mp4"}, {"codec", "h264"}});
        }
    }
    void formatAdviceSitsBesideTheFormatAndRecoveryIsBordered() {
        ReviewWindow window(options(), config()); window.show();
        QVERIFY(putgd::test::waitForWindowExposed(&window));
        window.drive({{"action", "format"}, {"format", "mp4"}, {"codec", "h265"}});
        auto* advice = window.findChild<QToolButton*>("formatAdvice");
        QComboBox* format = nullptr;
        for (auto* combo : window.findChildren<QComboBox*>()) if (combo->accessibleName() == "Output format") format = combo;
        QVERIFY(advice && format);
        QVERIFY(advice->isVisible());
        QCOMPARE(advice->mapTo(&window, advice->rect().center()).y(), format->mapTo(&window, format->rect().center()).y());
        QVERIFY(advice->mapTo(&window, QPoint()).x() > format->mapTo(&window, format->rect().topRight()).x());
        window.showError("No space left on device");
        auto* retry = buttonNamed(window, "Try again");
        QVERIFY(retry && retry->isVisible());
        QVERIFY(retry->property("visualRole").toString() != "link");
        QSignalSpy retried(&window, &ReviewWindow::retryExportRequested);
        retry->click(); QCOMPARE(retried.size(), 1);
    }
    void closedChoicesIgnoreWheel() {
        ReviewWindow window(options(), config()); window.setEdit(twoMasks()); window.show();
        QVERIFY(putgd::test::waitForWindowExposed(&window));
        for (auto* combo : window.findChildren<QComboBox*>()) {
            const int index = combo->currentIndex();
            QWheelEvent event(QPointF(5, 5), combo->mapToGlobal(QPoint(5, 5)), {}, QPoint(0, -120), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
            QApplication::sendEvent(combo, &event);
            QCOMPARE(combo->currentIndex(), index);
            QVERIFY(!event.isAccepted());
            QTest::keyClick(combo, Qt::Key_Down);
            QVERIFY(combo->currentIndex() != index || index == combo->count() - 1);
        }
    }
    void savedCopyIdentifiesFileAndBecomesEarlier() {
        ReviewWindow window(options(), config()); window.setEdit(twoMasks()); window.show();
        QVERIFY(putgd::test::waitForWindowExposed(&window));
        QCOMPARE(window.drive({{"action", "copyPath"}}).code, QString("invalid_request"));
        QTemporaryDir dir; QVERIFY(dir.isValid());
        const QString saved = dir.filePath("review copy.mp4");
        QFile file(saved); QVERIFY(file.open(QIODevice::WriteOnly)); file.close();
        window.setSavedCopy(saved, 3, config());
        QVERIFY(window.inspect()["status"].toString().contains("Saved review copy.mp4"));
        QVERIFY(window.inspect()["savedCopy"].toObject()["current"].toBool());
        QVERIFY(window.drive({{"action", "copyPath"}}).code.isEmpty());
        QCOMPARE(QApplication::clipboard()->text(), saved);
        QUrl opened; host::setTestOpener([&](const QUrl& url) { opened = url; });
        const auto restore = qScopeGuard([] { host::setTestOpener({}); });
        QVERIFY(window.drive({{"action", "showInFolder"}}).code.isEmpty());
        QCOMPARE(opened.toLocalFile(), dir.path());
        window.setEdit(twoMasks(4));
        QVERIFY(window.inspect()["status"].toString().contains("Earlier copy saved: review copy.mp4"));
        QVERIFY(!window.inspect()["savedCopy"].toObject()["current"].toBool());
        window.setSavedCopy("/tmp/new.mp4", 4, config());
        window.drive({{"action", "format"}, {"format", "webm"}, {"codec", "vp9"}});
        QVERIFY(!window.inspect()["savedCopy"].toObject()["current"].toBool());
    }
    void resizeCornerUsesFullTargetAndNearestMask() {
        FrameView frame; frame.resize(640, 360); frame.setCanvas({640, 360});
        auto one = mask("one", "solid", 0, duration); one.rect = QRect(20, 20, 100, 100);
        auto two = one; two.id = "two"; two.rect.translate(20, 0);
        frame.setMasks({one, two}, "one"); frame.show();
        QSignalSpy moved(&frame, &FrameView::maskMoved);
        // Twelve pixels beyond the painted corner is still in its 32 px target.
        drag(&frame, QPoint(148, 116), QPoint(158, 126));
        QCOMPARE(moved.size(), 1);
        QCOMPARE(moved.first()[0].toString(), QString("two"));
        QCOMPARE(moved.first()[1].toRect().size(), QSize(110, 110));
    }
    // put.gd's own frame everywhere below, also offscreen: the frame share,
    // minimum size and fit include the compact header's pixels.

};

QTEST_MAIN(ReviewWindowTest)
#include "test_review_window_layout.moc"
