#include "ReviewWindowFixture.h"

class ReviewWindowTest : public ReviewWindowFixture {
    Q_OBJECT
private slots:
    void liveSelectorsSupportKeyboard_data() {
        QTest::addColumn<bool>("recording");
        QTest::newRow("screenshot") << false;
        QTest::newRow("recording") << true;
    }
    void liveSelectorsSupportKeyboard() {
        QFETCH(bool, recording);
        for (int mode = 0; mode < 5; ++mode) {
            const auto screen = QGuiApplication::primaryScreen()->geometry();
            CaptureFrame frame{"fixture", screen, screen, 1, 0, {}};
            RegionOverlay screenshot({frame}, nullptr, "Uploads immediately to Proton Drive");
            SelectionOverlay record;
            QRectF selected;
            int cancelled = 0, accepted = 0;
            connect(&screenshot, &RegionSelector::selected, this, [&](const QRectF& r) { selected = r; ++accepted; });
            connect(&record, &SelectionOverlay::selected, this, [&](const QRect& r) { selected = r; ++accepted; });
            connect(&screenshot, &RegionSelector::cancelled, this, [&](const Error&) { ++cancelled; });
            connect(&record, &SelectionOverlay::cancelled, this, [&](const Error&) { ++cancelled; });
            if (recording) record.start(); else screenshot.start();
            QWidget* surface = nullptr;
            for (auto* w : QApplication::topLevelWidgets())
                if (w->isVisible() && w->objectName() == (recording ? "putgd-record-select" : "putgd-region-overlay")) surface = w;
            QVERIFY(surface);
            auto* accessible = QAccessible::queryAccessibleInterface(surface);
            QVERIFY(accessible);
            QCOMPARE(accessible->text(QAccessible::Description), surface->accessibleDescription());
            QVERIFY(surface->accessibleDescription().contains("Drag to select"));
            if (!recording) QVERIFY(surface->accessibleDescription().endsWith("Uploads immediately to Proton Drive"));
            if (mode == 0) {
                QTest::keyClick(surface, Qt::Key_Return);
                QCOMPARE(selected, QRectF(screen));
            } else if (mode == 4) {
                QTest::mousePress(surface, Qt::LeftButton, {}, {100, 100});
                QTest::mouseMove(surface, {300, 280});
                QVERIFY(surface->accessibleDescription().contains(recording ? "Release to choose recording area" : "Release to capture"));
                QVERIFY(!surface->accessibleDescription().contains("Enter to"));
                QTest::mouseRelease(surface, Qt::LeftButton, {}, {300, 280});
                QCOMPARE(selected, QRectF(QPointF(screen.topLeft()) + QPointF(100, 100), QSizeF(200, 180)));
            } else {
                QTest::keyClick(surface, Qt::Key_Right);
                QVERIFY(surface->accessibleDescription().contains(recording ? "Enter to choose" : "Enter to capture"));
                QVERIFY(!surface->accessibleDescription().contains("logical pixels"));
                QRectF expected(QPointF(screen.topLeft()) + QPointF(screen.width() / 4.0, screen.height() / 4.0), QSizeF(screen.size()) / 2);
                QVERIFY(accessible->text(QAccessible::Description).contains(QString::number(expected.width())));
                QTest::keyClick(surface, Qt::Key_Right);
                expected.translate(1, 0);
                QTest::keyClick(surface, Qt::Key_Down, Qt::ControlModifier);
                expected.translate(0, 10);
                QTest::keyClick(surface, Qt::Key_Right, Qt::ShiftModifier);
                expected.setWidth(expected.width() + 1);
                QTest::keyClick(surface, Qt::Key_Up, Qt::ShiftModifier | Qt::ControlModifier);
                expected.setHeight(expected.height() - 10);
                if (mode == 1) {
                    QTest::keyClick(surface, Qt::Key_Enter);
                    QCOMPARE(selected, recording ? QRectF(expected.toAlignedRect()) : expected);
                } else if (mode == 2) {
                    QTest::keyClick(surface, Qt::Key_Escape);
                    QCOMPARE(cancelled, 1);
                    QCOMPARE(accepted, 0);
                } else {
                    // Oversized keyboard moves and resizes stay on the active screen.
                    for (int i = 0; i < 200; ++i) QTest::keyClick(surface, Qt::Key_Right, Qt::ControlModifier);
                    for (int i = 0; i < 200; ++i) QTest::keyClick(surface, Qt::Key_Down, Qt::ShiftModifier | Qt::ControlModifier);
                    QTest::keyClick(surface, Qt::Key_Return);
                    QVERIFY(QRectF(screen).contains(selected));
                    QVERIFY(selected.width() > 0 && selected.height() > 0);
                }
            }
            QVERIFY(!surface->isVisible());
            QCOMPARE(accepted + cancelled, 1);
        }
    }
    void screenshotSelectorCyclesSyntheticDisplays() {
        const QList<CaptureFrame> frames{{"Left",QRectF(-800,0,800,600),QRect(-800,0,800,600),1,0,{}},{"Right",QRectF(0,100,320,240),QRect(0,100,320,240),1,0,{}}};
        RegionOverlay selector(frames);selector.start();QSignalSpy selected(&selector,&RegionSelector::selected);
        QWidget* surface=nullptr;
        for(auto* w:QApplication::topLevelWidgets())if(w->isVisible()&&w->objectName()=="putgd-region-overlay"&&w->width()==800)surface=w;
        QVERIFY(surface);
        QTest::keyClick(surface,Qt::Key_Tab);
        QVERIFY(surface->accessibleDescription().contains("Display 2 · Entire display · 320 × 240 px · "));
        QTest::keyClick(surface,Qt::Key_Right);
        QTest::keyClick(surface,Qt::Key_Backtab);
        QVERIFY(surface->accessibleDescription().contains("Display 1 · "));
        QTest::keyClick(surface,Qt::Key_Return);QCOMPARE(selected.size(),1);
        QCOMPARE(qvariant_cast<QRectF>(selected.first()[0]),QRectF(-720,60,160,120));
    }
    void accessiblePlayheadAndLiveMaskGeometry() {
        ReviewWindow window(options(), config());
        window.setEdit(twoMasks()); window.show();
        QVERIFY(putgd::test::waitForWindowExposed(&window));
        auto* t = timeline(window);
        auto* accessible = QAccessible::queryAccessibleInterface(t);
        QVERIFY(accessible && accessible->valueInterface());
        auto* value = accessible->valueInterface();
        QCOMPARE(value->minimumValue().toDouble(), 0.0);
        QCOMPARE(value->maximumValue().toDouble(), double(duration - 1) / 1000000);
        QCOMPARE(value->minimumStepSize().toDouble(), 0.1);
        value->setCurrentValue(2.3);
        QCOMPARE(value->currentValue().toDouble(), 2.3);
        QCOMPARE(accessible->text(QAccessible::Value), QString("2.3 seconds of 12 seconds"));
        QCOMPARE(window.inspect()["playheadUs"].toDouble(), 2300000);
        auto* frame = window.findChild<FrameView*>();
        QVERIFY(frame->accessibleDescription().contains("Redaction 2 selected: 320 × 120 at 100, 80"));
        QVERIFY(frame->accessibleDescription().contains("Applies from 0:04.0 to 0:10.0"));
        QTest::keyClick(frame, Qt::Key_Right);
        QVERIFY(frame->accessibleDescription().contains("at 101, 80"));
        frame->commitNudge();
        auto edit = twoMasks(4); edit.masks[1].end = 11000000; window.setEdit(edit);
        QVERIFY(frame->accessibleDescription().contains("to 0:11.0"));
    }
    void concurrentMasksHaveDistinctScrollableLanes() {
        ReviewTimeline t; t.setDuration(duration);
        QList<RecordingMask> masks;
        for (int i = 0; i < 8; ++i) masks.append(mask(QString::number(i), "solid", 1000000, 10000000));
        t.setMasks(masks, "0"); t.resize(600, t.sizeHint().height()); t.show();
        QCOMPARE(t.lanes(), 8);
        auto* scroll = t.findChild<QScrollBar*>("maskLaneScroll");
        QVERIFY(scroll && !scroll->isVisible()); QCOMPARE(scroll->maximum(), 6);
        QVERIFY(t.maskHitRect("0").center().y() < t.maskHitRect("1").center().y());
        QVERIFY(t.maskRect("7").isEmpty());
        const int boundedHeight = t.sizeHint().height();
        t.setMasks(masks, "7"); QCOMPARE(scroll->value(), 6);
        QVERIFY(!t.maskRect("7").isEmpty()); QVERIFY(t.maskRect("0").isEmpty());
        QCOMPARE(t.sizeHint().height(), boundedHeight);
        QSignalSpy selected(&t, &ReviewTimeline::maskSelected);
        scroll->setValue(6);
        QTest::mouseClick(&t, Qt::LeftButton, {}, t.maskRect("6").center());
        QCOMPARE(selected.size(), 1); QCOMPARE(selected.first()[0].toString(), QString("6"));
        // A lane can hold several non-overlapping masks; count hidden masks.
        t.setMasks({mask("a", "solid", 0, 5000000), mask("b", "solid", 0, 5000000), mask("c", "solid", 0, 5000000), mask("d", "solid", 6000000, 10000000)}, "a");
        QCOMPARE(t.findChild<QToolButton*>("showMoreMasks")->accessibleName(), QString("Next mask lanes"));
        scroll->setValue(1);
        QCOMPARE(t.findChild<QToolButton*>("showMoreMasks")->accessibleName(), QString("Next mask lanes"));
    }
    void previewRecoveryStacksAndMeasuresWrappedText() {
        FrameView view; auto large = view.font(); large.setPixelSize(21); view.setFont(large); view.setCanvas({1280,720}); view.resize(360,320); view.show();
        view.setPreviewError("The decoder could not refresh the selected frame. Try again after closing other applications.");
        auto* label = view.findChild<QLabel*>("previewErrorText");
        auto* retry = view.findChild<QPushButton*>("retryPreview");
        QApplication::processEvents();
        QVERIFY(label && retry);
        QVERIFY(label->height() >= label->heightForWidth(label->width()));
        QVERIFY(retry->y() >= label->y() + label->height());
        view.resize(900,600); QApplication::processEvents();
        QVERIFY(label->height() >= label->heightForWidth(label->width()));
        QVERIFY(retry->x() >= label->x() + label->width());
    }
    void destinationAndMutedPreviewRemainVisible() {
        auto o = options();
        o.audioRecorded = true;
        o.destination = "Project archive for customer support in the European Union";
        o.uploadSummary = "Upload to " + o.destination + " · Shared link";
        ReviewWindow window(o, config());
        window.show();
        QVERIFY(putgd::test::waitForWindowExposed(&window));
        window.resize(760, 420);
        QApplication::processEvents();
        const auto state = window.inspect();
        QCOMPARE(state["destination"].toString(), o.destination);
        QCOMPARE(state["sharingMode"].toString(), QString("share"));
        QCOMPARE(state["uploadSummary"].toString(), o.uploadSummary);
        QVERIFY(state["previewMuted"].toBool());
        auto* target = window.findChild<QToolButton*>("uploadDestinationMenu");
        auto* upload = window.findChild<QPushButton*>("upload");
        auto* audio = window.findChild<QLabel*>("exportAudioSummary");
        QVERIFY(target && target->isVisible() && target->toolTip().contains(o.uploadSummary));
        auto* grid = qobject_cast<QGridLayout*>(window.findChild<QWidget*>("reviewActions")->layout());
        QVERIFY(grid);
        QVERIFY(target->text().contains("Shared link"));
        if (grid->property("singleRow").toBool() && !target->property("headerSummary").toBool()) QCOMPARE(target->mapTo(&window, target->rect().center()).y(), upload->mapTo(&window, upload->rect().center()).y());
        else QVERIFY(target->mapTo(&window, target->rect().bottomLeft()).y() < upload->mapTo(&window, QPoint()).y());
        QVERIFY(window.rect().contains(QRect(target->mapTo(&window, QPoint()), target->size())));
        auto* full = window.findChild<QLabel*>("uploadDestinationWrapped");
        QVERIFY(full && !full->isVisible());
        target->menu()->popup(target->mapToGlobal(QPoint(0, target->height())));
        QApplication::processEvents();
        QVERIFY(full->isVisible());
        QVERIFY(full->text().contains(o.destination));
        target->menu()->close();
        QVERIFY(audio->isVisible());
        QVERIFY(!QRect(audio->mapTo(&window, QPoint()), audio->size()).intersects(QRect(target->mapTo(&window, QPoint()), target->size())));
        QVERIFY(audio->mapTo(&window, audio->rect().topRight()).x() <= window.width() - 8);
        QVERIFY(state["audio"].toString().endsWith("Silent preview"));
        QVERIFY(window.findChild<QLabel*>("exportAudioSummary")->isVisible());
    }
    void recordingBarKeepsTheTargetAndFormatBeforeStart_data() {
        QTest::addColumn<double>("scale");
        QTest::newRow("standard") << 1.0;
        QTest::newRow("150%") << 1.5;
    }
    void recordingBarKeepsTheTargetAndFormatBeforeStart() {
        QFETCH(double, scale);
        const auto original = QApplication::font();
        const auto restore = qScopeGuard([&] { QApplication::setFont(original); });
        auto font = original; font.setPixelSize(qRound(14 * scale)); QApplication::setFont(font);
        ControlBar::Options o; o.targetLabel = "Region · 960 × 600"; o.audioSupported = true;
        o.formats = options().formats; o.codecs = options().codecs;
        ControlBar bar(o, config()); bar.fitTo(QRect(0, 0, 800, 720)); bar.show();
        QApplication::processEvents();
        auto* format = bar.findChild<QComboBox*>("format");
        QVERIFY(format && format->isVisible());
        for (int i = 0; i < format->count(); ++i) {
            format->setCurrentIndex(i); QApplication::processEvents();
            const auto purpose = format->currentText().section(QStringLiteral(" — "), 0, 0);
            QVERIFY(format->width() >= format->fontMetrics().horizontalAdvance(purpose) + 44);
            QVERIFY(format->toolTip().contains(o.targetLabel));
        }
        auto* target = bar.findChild<QLabel*>("target");
        QVERIFY(target->isVisible());
        QCOMPARE(QString(target->text()).replace(QChar(0x00a0), ' '), o.targetLabel);
        QVERIFY(target->width() >= target->fontMetrics().horizontalAdvance("960 × 600"));
        QVERIFY(target->height() >= target->heightForWidth(target->width()));
        QVERIFY(bar.rect().contains(QRect(target->mapTo(&bar, QPoint()), target->size())));
        QVERIFY(bar.findChild<QPushButton*>("record")->isVisible());
        for (const char* state : {"recording", "paused", "recording", "ready"}) bar.setState(state);
        bar.fitTo(QRect(0, 0, 1024, 768)); QApplication::processEvents();
        QVERIFY(target->isVisible());
        QVERIFY(target->width() > target->fontMetrics().horizontalAdvance("960 × 600"));
        QVERIFY(target->height() >= target->heightForWidth(target->width()));
    }
    void recordingBarMeasuresEveryCurrentSummary_data() {
        QTest::addColumn<int>("pixels");
        QTest::addColumn<int>("screenWidth");
        for (int pixels : {14, 21}) for (int width : {800, 1366, 2400})
            QTest::newRow(qPrintable(QString("%1px-%2").arg(pixels).arg(width))) << pixels << width;
    }
    void recordingBarMeasuresEveryCurrentSummary() {
        QFETCH(int, pixels); QFETCH(int, screenWidth);
        auto font = qApp->font(); font.setPixelSize(pixels); qApp->setFont(font);
        ControlBar::Options o; o.targetLabel = "Region · 960 × 600"; o.audioSupported = true;
        o.formats = options().formats; o.codecs = options().codecs;
        auto c = config(); c.audio.system = true;
        ControlBar bar(o, c); bar.fitTo(QRect(0, 0, screenWidth, 768)); bar.show();
        auto* format = bar.findChild<QComboBox*>("format");
        auto* facts = bar.findChild<QLabel*>("recordingFacts");
        auto* target = bar.findChild<QLabel*>("target");
        for (const char* state : {"ready", "recording", "paused", "ready", "recording", "paused"}) {
            if (QString::fromLatin1(state) == "ready") {
                format->setCurrentIndex(format->findData(QStringList{"gif", QString()}));
                format->setCurrentIndex(format->findData(QStringList{"mp4", "h264"}));
            }
            bar.setState(state); QApplication::processEvents();
            auto* label = QString::fromLatin1(state) == "ready" ? target : facts;
            QVERIFY(label->isVisible());
            if (label == facts) QCOMPARE(facts->text(), QStringLiteral("Region · 960 × 600 · With system audio · ") + format->currentText().section(QStringLiteral(" — "), 0, 0));
            const int required = label->wordWrap() ? label->heightForWidth(label->width()) : label->fontMetrics().height();
            QVERIFY2(label->height() >= required, qPrintable(QString("%1: %2 < %3").arg(state).arg(label->height()).arg(required)));
            QVERIFY(bar.rect().contains(QRect(label->mapTo(&bar, QPoint()), label->size())));
            if (!label->wordWrap()) QVERIFY(label->width() >= label->fontMetrics().horizontalAdvance(label->text()));
            // A later fit must measure the unwrapped text again.
            bar.fitTo(QRect(0, 0, screenWidth, 768)); QApplication::processEvents();
            QVERIFY(label->height() >= (label->wordWrap() ? label->heightForWidth(label->width()) : label->fontMetrics().height()));
        }
        bar.fitTo(QRect(0, 0, 4000, 768)); QApplication::processEvents();
        QVERIFY(!bar.twoRows());
        QVERIFY(!facts->wordWrap());
        QVERIFY(facts->width() >= facts->fontMetrics().horizontalAdvance(facts->text()));
    }
    void stoppingKeepsTheClipAndDiscardStillAsks() {
        ControlBar::Options o;
        o.targetLabel = "Region"; o.formats = {"mp4"}; o.codecs = {{"mp4", QJsonArray{"h264"}}};
        ControlBar bar(o, config());
        bar.setState("recording"); bar.show();
        auto* stop = bar.findChild<QPushButton*>("stop");
        auto* discard = bar.findChild<QPushButton*>("cancel");
        QVERIFY(stop && discard);
        QCOMPARE(stop->property("visualRole").toString(), QString("primary"));
        QCOMPARE(discard->property("visualRole").toString(), QString("quiet"));
        QSignalSpy stopped(&bar, &ControlBar::stopRequested), cancelled(&bar, &ControlBar::cancelRequested);
        stop->click(); QCOMPARE(stopped.size(), 1); QCOMPARE(cancelled.size(), 0);
        discard->click(); QVERIFY(bar.confirming()); QCOMPARE(cancelled.size(), 0);
        bar.findChild<QPushButton*>("keepRecording")->click(); QVERIFY(!bar.confirming());
    }
    void compactFormatsUseCompletePurposeOrFormatNames() {
        FormatBox box;
        box.addItem("Compatible video — MP4 (H.264) · Recommended", QStringList{"mp4", "h264"});
        box.setItemData(0, "Recommended for browser sharing", Qt::ToolTipRole);
        box.resize(500, box.sizeHint().height());
        QCOMPARE(box.displayedText(), QStringLiteral("Compatible video"));
        box.resize(box.minimumSizeHint().width(), box.height());
        QCOMPARE(box.displayedText(), QStringLiteral("H.264"));
        QVERIFY(box.accessibleDescription().contains("H.264"));
        QVERIFY(box.accessibleDescription().contains("Recommended"));
        box.addItem("Smaller video — MP4 (H.265)", QStringList{"mp4", "h265"});
        QVERIFY(FormatDelegate::tradeoff(box.model()->index(1, 0)).contains("Limited browser compatibility"));
        QStyleOptionViewItem option;
        QVERIFY(box.itemDelegate()->sizeHint(option, box.model()->index(1, 0)).height() > 2 * box.fontMetrics().height());
    }
    void compactReviewDistinguishesAvcAndHevcAtLargeText() {
        const auto original = qApp->font(); auto restore = qScopeGuard([&] { qApp->setFont(original); });
        auto font = original; font.setPixelSize(21); qApp->setFont(font);
        ReviewWindow window(options(), config()); window.setEdit(twoMasks()); window.show();
        putgd::test::settle(20); window.resize(640, 720); putgd::test::settle(20);
        FormatBox* format = nullptr;
        for (auto* box : window.findChildren<QComboBox*>()) if (box->accessibleName() == "Output format") format = static_cast<FormatBox*>(box);
        QVERIFY(format);
        // Exercise the compact allocation used when the footer gives its space to actions.
        format->setFixedWidth(format->minimumSizeHint().width());
        QCOMPARE(format->displayedText(), QString("H.264")); const auto avc = format->grab().toImage();
        window.drive({{"action", "format"}, {"format", "mp4"}, {"codec", "h265"}});
        QCOMPARE(format->displayedText(), QString("H.265")); QVERIFY(format->grab().toImage() != avc);
        QVERIFY(format->toolTip().contains("Smaller video")); QVERIFY(format->accessibleDescription().contains("H.265"));
        QVERIFY(format->minimumSizeHint().width() >= format->fontMetrics().horizontalAdvance("H.265") + Ui::scaled(44));
    }
    void maskChooserReservesScaledControlAndPopupSpace() {
        for (int pixels : {14, 21}) {
            auto font = qApp->font(); font.setPixelSize(pixels); qApp->setFont(font);
            MaskBox box; box.addItems({"Redact 1", "Blur 1"}); box.show();
            const int text = std::max(box.fontMetrics().horizontalAdvance("Redact 1"), box.fontMetrics().horizontalAdvance("Blur 1"));
            QVERIFY(box.sizeHint().width() >= text + Ui::scaled(44));
            box.showPopup();
            QVERIFY(box.view()->minimumWidth() >= text + Ui::scaled(40));
            box.hidePopup();
        }
    }
    void timelineIntervalsAndBlurWarningsHaveContrast() {
        for (const auto scheme : {Theme::Scheme::Dark, Theme::Scheme::Light}) {
            Theme::setScheme(scheme);
            const auto& colors = Theme::current();
            ReviewTimeline t;
            t.setDuration(duration); t.setTrim(1000000, 11000000);
            t.setMasks({mask("blur", "blur", 2000000, 10000000)}, {});
            t.resize(900, t.sizeHint().height()); t.show();
            QApplication::processEvents();
            const auto image = t.grab().toImage();
            const auto track = t.trackRect();
            const auto retained = image.pixelColor(t.xFor(6000000), track.center().y());
            QVERIFY(Theme::contrast(retained, colors.surfaceRaised) >= 3.0);
            const auto bar = t.maskRect("blur");
            const auto fill = image.pixelColor(bar.right() - 12, bar.center().y());
            const auto contrastIn = [&](const QRect& area) {
                double contrast = 1;
                for (int y = area.top(); y <= area.bottom(); ++y)
                    for (int x = area.left(); x <= area.right(); ++x)
                        contrast = std::max(contrast, Theme::contrast(image.pixelColor(x, y), fill));
                return contrast;
            };
            const int side = std::min(bar.height() - 2, Ui::scaled(14));
            QVERIFY(contrastIn(QRect(bar.left() + 4, bar.center().y() - side / 2, side, side)) >= 4.5);
            QVERIFY(contrastIn(QRect(bar.right() - 40, bar.bottom() - 4, 24, 2)) >= 3.0);
            t.setMasks({mask("solid", "solid", 2000000, 10000000)}, "solid");
            const auto solid = t.maskRect("solid");
            const auto selected = t.grab().toImage();
            const auto selectedFill = selected.pixelColor(solid.right() - 20, solid.center().y());
            QVERIFY(Theme::contrast(colors.text, selectedFill) >= 4.5);
        }
    }
    void timelinePlayheadKeepsMaskNamesReadable() {
        ReviewTimeline timeline; timeline.setDuration(duration); timeline.setTrim(0, duration);
        timeline.setMasks({mask("solid", "solid", 2000000, 10000000)}, "solid");
        timeline.resize(900, timeline.sizeHint().height()); timeline.show(); QApplication::processEvents();
        const auto bar = timeline.maskRect("solid");
        const QRect name(bar.left() + 7, bar.top() + 1, QFontMetrics(Ui::font(Ui::Type::caption)).horizontalAdvance("Redaction 1") - 2, bar.height() - 2);
        timeline.setPlayhead(11000000); const auto before = timeline.grab().toImage().copy(name);
        const qint64 at = 2000000 + qRound64(double(name.center().x() - timeline.xFor(2000000)) * duration / (timeline.xFor(duration) - timeline.xFor(0)));
        timeline.setPlayhead(at); const auto after = timeline.grab().toImage().copy(name);
        QCOMPARE(after, before);
    }
    void timelineTargetsAre32PixelsAndNearestHandleWins_data() {
        QTest::addColumn<double>("scale");
        QTest::newRow("standard") << 1.0;
        QTest::newRow("150%") << 1.5;
    }
    void timelineTargetsAre32PixelsAndNearestHandleWins() {
        QFETCH(double, scale);
        const auto original = QApplication::font();
        const auto restore = qScopeGuard([&] { QApplication::setFont(original); });
        auto font = original;
        if (font.pointSizeF() > 0) font.setPointSizeF(font.pointSizeF() * scale);
        else font.setPixelSize(qRound(font.pixelSize() * scale));
        QApplication::setFont(font);
        ReviewTimeline t;
        t.setDuration(duration);
        t.setTrim(5000000, 5200000);
        t.setMasks({mask("one", "solid", 1000000, 2000000), mask("two", "solid", 1500000, 1900000)}, "one");
        t.resize(600, t.sizeHint().height());
        t.show();
        const auto first = t.maskHitRect("one"), second = t.maskHitRect("two");
        const auto firstBar = t.maskRect("one"), secondBar = t.maskRect("two");
        auto laneFont = Ui::font(Ui::Type::caption); laneFont.setWeight(QFont::Medium);
        QVERIFY(firstBar.height() >= 16);
        QVERIFY(firstBar.height() >= QFontMetrics(laneFont).tightBoundingRect(QStringLiteral("Redaction 12")).height() + 6);
        QCOMPARE(secondBar.top() - firstBar.top(), firstBar.height() + 2);
        QVERIFY(first.contains(firstBar));
        QVERIFY(second.contains(secondBar));
        QVERIFY(first.height() >= 40);
        QVERIFY(second.height() >= 40);
        QVERIFY(t.rect().contains(first));
        QVERIFY(t.rect().contains(second));
        // Compact lanes retain 32 px targets; the nearest lane wins where they overlap.
        QSignalSpy selected(&t, &ReviewTimeline::maskSelected);
        QTest::mouseClick(&t, Qt::LeftButton, {}, second.center());
        QCOMPARE(selected.size(), 1);
        QCOMPARE(selected.last().first().toString(), QString("two"));
        const auto overlap = first.intersected(second);
        QVERIFY(!overlap.isEmpty());
        QTest::mouseClick(&t, Qt::LeftButton, {}, QPoint(overlap.center().x(), overlap.top()));
        QCOMPARE(selected.last().first().toString(), QString("one"));
        QTest::mouseClick(&t, Qt::LeftButton, {}, QPoint(overlap.center().x(), overlap.bottom()));
        QCOMPARE(selected.last().first().toString(), QString("two"));
        auto* scroll = t.findChild<QScrollBar*>("maskLaneScroll");
        QVERIFY(scroll->isHidden());
        const auto secondTarget = t.maskHitRect("two");
        QVERIFY(secondTarget.height() >= 40);
        QVERIFY(!t.maskHitRect("one").isEmpty());
        QVERIFY(secondTarget.top() > t.trackRect().center().y());
        scroll->setValue(0);
        QVERIFY(first.top() >= 0);
        QSignalSpy trim(&t, &ReviewTimeline::trimChanged);
        drag(&t, QPoint(t.xFor(5200000) + 12, t.trackRect().center().y()), QPoint(t.xFor(7000000), t.trackRect().center().y()));
        QCOMPARE(trim.size(), 1);
        QCOMPARE(trim.last()[0].toLongLong(), 5000000);
        QVERIFY(trim.last()[1].toLongLong() > 6000000);

        t.setMasks({mask("one", "solid", 1000000, 2000000), mask("two", "solid", 1500000, 1900000)}, "one");
        QSignalSpy retimed(&t, &ReviewTimeline::maskRetimed);
        const auto bar = t.maskRect("one");
        drag(&t, QPoint(bar.left() - 12, bar.center().y()), QPoint(t.xFor(500000), bar.center().y()));
        QCOMPARE(retimed.size(), 1);
        QCOMPARE(retimed.last()[0].toString(), QString("one"));
    }
    void destinationCanBeRevealedByKeyboardAndClick() {
        auto o = options(); o.destination = QString("Long project archive ").repeated(12); o.sharingMode = "private";
        o.uploadSummary = "Private upload to " + o.destination;
        ReviewWindow window(o, config()); window.show();
        QVERIFY(putgd::test::waitForWindowExposed(&window));
        auto* button = window.findChild<QToolButton*>("uploadDestinationMenu");
        QVERIFY(button && button->focusPolicy() & Qt::TabFocus);
        bool keyboardOpened = false;
        QTimer::singleShot(0, button, [&] { keyboardOpened = button->menu()->isVisible(); QTest::keyClick(button->menu(), Qt::Key_Escape); });
        button->setFocus(Qt::TabFocusReason); QTest::keyClick(button, Qt::Key_Space);
        QVERIFY(keyboardOpened);
        auto* value = window.findChild<QLabel*>("uploadDestinationWrapped");
        QVERIFY(value->text().startsWith("Destination: " + o.destination + "\nSharing: Private upload"));
        QVERIFY(value->textInteractionFlags() & Qt::TextSelectableByMouse);
        QVERIFY(value->textInteractionFlags() & Qt::TextSelectableByKeyboard);
        QTest::keyClick(button->menu(), Qt::Key_Escape);
        QVERIFY(!button->menu()->isVisible());
        button->menu()->popup(button->mapToGlobal(QPoint(0, button->height())));
        PUTGD_TRY_VERIFY(button->menu()->isVisible());
        button->menu()->close();

    }
    void compactHeaderIsOneLineNamedForTheWindow() {
        ReviewWindow window(options(), config());
        window.show();
        QVERIFY(putgd::test::waitForWindowExposed(&window));
        auto* header = window.findChild<Ui::WindowHeader*>("windowHeader");
        QVERIFY(header && header->isVisible());
        QCOMPARE(header->density(), Ui::WindowHeader::Density::Compact);
        QVERIFY(window.windowFlags().testFlag(Qt::FramelessWindowHint));
        // Its height keeps the full title line and the 32 px control targets.
        QCOMPARE(header->height(), header->compactHeight());
        QVERIFY2(header->height() >= 32 && header->height() <= 40, qPrintable(QString::number(header->height())));
        QCOMPARE(window.inspect()["header"].toObject()["rect"].toArray()[3].toInt(), header->height());
        // Playback reuses the compact header; the window retains its name.
        QCOMPARE(window.windowTitle(), QStringLiteral("Review recording"));
        QCOMPARE(header->accessibleName(), QStringLiteral("Review recording"));
        QVERIFY(header->isAncestorOf(window.findChild<QWidget*>("reviewTransport")));
        auto* state = header->findChild<QLabel*>("documentState");
        QVERIFY(!state || !state->isVisible());
        auto* draft = window.findChild<QLabel*>("draftState");
        QVERIFY(draft && draft->isVisible() && draft->text() == QStringLiteral("Not uploaded"));
        QCOMPARE(header->isAncestorOf(draft), window.findChild<QToolButton*>("uploadDestinationMenu")->property("headerSummary").toBool());
        // The shared window controls, each at least a 32 px target.
        for (const auto* name : {"windowMinimize", "windowMaximize", "windowClose"}) {
            auto* button = header->findChild<QToolButton*>(name);
            QVERIFY2(button && button->isVisible(), name);
            QVERIFY(button->width() >= 32 && button->height() >= 32);
            QVERIFY(button->y() >= 0 && button->y() + button->height() <= header->height());
        }
        // Double-click on the band maximizes and restores, as on every put.gd window.
        const QPoint empty(header->width() / 4, header->height() / 2);
        QTest::mouseDClick(header, Qt::LeftButton, {}, empty);
        PUTGD_TRY_VERIFY(window.isMaximized());
        QTest::mouseDClick(header, Qt::LeftButton, {}, empty);
        PUTGD_TRY_VERIFY(!window.isMaximized());
    }
    void compactHeaderShowsOnlyCompleteNames_data() {
        QTest::addColumn<double>("scale");
        QTest::newRow("standard") << 1.0;
        QTest::newRow("150%") << 1.5;
    }
    void compactHeaderShowsOnlyCompleteNames() {
        QFETCH(double, scale);
        const auto original = QApplication::font();
        const auto restore = qScopeGuard([&] { QApplication::setFont(original); });
        auto font = original;
        if (font.pointSizeF() > 0) font.setPointSizeF(font.pointSizeF() * scale);
        else font.setPixelSize(qRound(font.pixelSize() * scale));
        QApplication::setFont(font);
        ReviewWindow window(options(), config()); window.show();
        QVERIFY(putgd::test::waitForWindowExposed(&window));
        auto* title = window.findChild<Ui::ElidedLabel*>("brandName");
        for (const int width : {1366, 720, 1060, 800, 1366}) {
            window.resize(width, 720); PUTGD_VERIFY_STEADY(title->isHidden() || !title->elided(),10);
            QVERIFY(window.findChild<QWidget*>("reviewTransport")->isVisible());
            QCOMPARE(window.windowTitle(), QString("Review recording"));
        }
        auto* header = window.findChild<Ui::WindowHeader*>();
        QWidget documentWindow; QVBoxLayout layout(&documentWindow);
        auto* documentHeader = Ui::WindowHeader::install(&documentWindow, &layout, nullptr, Ui::WindowHeader::Density::Compact);
        documentHeader->setDocument(QString(200, 'W'));
        documentWindow.resize(680, 200); documentWindow.show(); putgd::test::settle(10);
        auto* document = documentHeader->findChild<Ui::ElidedLabel*>("documentName");
        QVERIFY(document->isHidden());
        documentHeader->setDocument("Clip"); PUTGD_VERIFY_STEADY(document->isVisible(),10); QVERIFY(!document->elided());
        QVERIFY(header->height() >= 32);
    }
    void desktopDecorationsHideTheCompactHeader() {
        Ui::WindowHeader::setCustomFrameOverride(-1);
        {
            ReviewWindow window(options(), config());
            window.show();
            QVERIFY(putgd::test::waitForWindowExposed(&window));
            auto* header = window.findChild<Ui::WindowHeader*>("windowHeader");
            QVERIFY(header && !header->isVisible());
            QVERIFY(!window.windowFlags().testFlag(Qt::FramelessWindowHint));
            QCOMPARE(window.windowTitle(), QStringLiteral("Review recording"));
            QVERIFY(!window.inspect()["header"].toObject()["visible"].toBool());
        }
        Ui::WindowHeader::setCustomFrameOverride(1);
    }
    void defaultSizeFitsAndTheVideoTakesMostOfTheWindow() {
        ReviewWindow window(options(), config());
        // The one-monitor fixture is 1366×768: the default fits with room for a panel and title bar.
        // Offscreen the fixture is 1366×768; a native run (real compositor) checks
        // the same shares on the desktop's own screen.
        if (QGuiApplication::platformName() == QLatin1String("offscreen")) QCOMPARE(QGuiApplication::primaryScreen()->geometry().size(), QSize(1366, 768));
        QVERIFY(ReviewWindow::defaultSize().width() <= 1366 - 24 && ReviewWindow::defaultSize().height() <= 768 - 48);
        window.show();
        QVERIFY(putgd::test::waitForWindowExposed(&window));
        QVERIFY(window.width() <= 1342 && window.height() <= 720);
        QVERIFY(window.width() >= window.minimumSizeHint().width());
        auto state = window.inspect();
        qInfo("review default %dx%d header %d share %.3f", window.width(), window.height(), state["header"].toObject()["rect"].toArray()[3].toInt(), state["video"].toObject()["share"].toDouble());
        // Before #283 the frame had about 30% of a 900×700 window; now most of it.
        QVERIFY2(state["video"].toObject()["share"].toDouble() >= 0.6, qPrintable(QString::number(state["video"].toObject()["share"].toDouble())));
        window.setEdit(twoMasks());
        QApplication::processEvents();
        state = window.inspect();
        QCOMPARE(state["timeline"].toObject()["lanes"].toInt(), 2);
        qInfo("review two masks share %.3f timeline %d", state["video"].toObject()["share"].toDouble(), state["timeline"].toObject()["rect"].toArray()[3].toInt());
        QVERIFY2(state["video"].toObject()["share"].toDouble() >= 0.6, qPrintable(QString::number(state["video"].toObject()["share"].toDouble())));
        // Transient guidance sits below the frame, leaving the video unobscured.
        QVERIFY(window.drive({{"action", "addMask"}, {"kind", "blur"}}).code.isEmpty());
        window.drive({{"action", "selectMask"}, {"maskId", "m-blur"}});
        QApplication::processEvents();
        QVERIFY(!window.findChild<QWidget*>("statusRow")->isVisible());
        qInfo("review with in-row guidance share %.3f", window.inspect()["video"].toObject()["share"].toDouble());
        QVERIFY2(window.inspect()["video"].toObject()["share"].toDouble() >= 0.53, qPrintable(QString::number(window.inspect()["video"].toObject()["share"].toDouble())));
        auto* row = window.findChild<QWidget*>("statusRow");
        auto* frame = window.findChild<FrameView*>();
        QVERIFY(!row->isVisible());
        QVERIFY(window.findChild<QWidget*>("maskControls")->mapTo(&window, QPoint()).y() >= frame->geometry().bottom());
        // Got it hides the guidance for the rest of the review.
        QVERIFY(window.drive({{"action", "dismissBlurTip"}}).code.isEmpty());
        QApplication::processEvents();
        QVERIFY(!window.findChild<QWidget*>("statusRow")->isVisible());
        window.drive({{"action", "selectMask"}, {"maskId", "m-solid"}});
        QApplication::processEvents();
        QVERIFY(window.inspect()["video"].toObject()["share"].toDouble() >= 0.6);
        // The minimum leaves the frame usable and fits a small laptop screen.
        qInfo("review minimum %dx%d", window.minimumSizeHint().width(), window.minimumSizeHint().height());
        window.resize(window.minimumSizeHint());
        QApplication::processEvents();
        QVERIFY(window.findChild<FrameView*>()->height() >= window.findChild<FrameView*>()->minimumHeight());
        // The video scales with the window.
        const auto before = state["video"].toObject()["rect"].toArray();
        window.resize(1280, 760);
        PUTGD_TRY_VERIFY(window.inspect()["video"].toObject()["rect"].toArray()[2].toInt() > before[2].toInt());
    }
    void largeTextControlsKeepTextPaddingAndOneLineRows() {
        const auto original = qApp->font();
        const auto restore = qScopeGuard([&] { qApp->setFont(original); Ui::WindowHeader::setCustomFrameOverride(0); });
        Ui::WindowHeader::setCustomFrameOverride(1);
        auto font = original; font.setPointSizeF(10.5 * 1.5); qApp->setFont(font);
        auto o = options(); o.canvas = QSize(960, 600); o.durationUs = 32000000; o.availableSize = QSize(1366, 680); o.audioRecorded = true;
        o.destination = QStringLiteral("Project archive for customer support in the European Union");
        o.uploadSummary = "Upload to " + o.destination + " · Shared link";
        RecordingEdit edit; edit.masks = {mask("solid", "solid", 8000000, 21000000), mask("blur", "blur", 3000000, 12000000)};
        ReviewWindow window(o, config()); window.setEdit(edit); window.show();
        QVERIFY(putgd::test::waitForWindowExposed(&window));
        window.drive({{"action", "selectMask"}, {"maskId", "blur"}});
        putgd::test::settle(20);
        // Every framed text control keeps 3 px above and below its full text line.
        for (const auto* name : {"trimControls", "maskControls", "reviewActions", "reviewTransport"})
            for (auto* control : window.findChild<QWidget*>(name)->findChildren<QWidget*>()) {
                if (!control->isVisible() || control->property("uiFlush").toBool()) continue;
                auto* button = qobject_cast<QAbstractButton*>(control);
                if (!(button && !button->text().isEmpty()) && !qobject_cast<QComboBox*>(control)) continue;
                QVERIFY2(control->height() >= control->fontMetrics().height() + 6, qPrintable(QString("%1 '%2' %3 px for a %4 px line").arg(control->metaObject()->className(), button ? button->text() : control->objectName()).arg(control->height()).arg(control->fontMetrics().height())));
            }
        // Media chrome follows its contents rather than a scaled pointer target.
        auto* header = window.findChild<Ui::WindowHeader*>("windowHeader");
        auto* transport = window.findChild<QWidget*>("reviewTransport");
        QVERIFY(header->height() < Ui::scaled(Ui::Metric::iconTarget));
        QVERIFY(header->rect().contains(transport->geometry()));
        for (auto* button : header->findChildren<QToolButton*>()) if (button->isVisible() && button->property("uiChrome").toBool()) {
            QVERIFY(header->rect().contains(button->geometry().translated(button->parentWidget()->mapTo(header, QPoint()))));
            QVERIFY(button->height() >= button->iconSize().height() + 8);
        }
        // Trim and Mask share one name column and stay single lines when the lanes' gutter can't.
        auto* trimName = window.findChild<QLabel*>("trimGroup");
        auto* maskName = window.findChild<QLabel*>("maskGroup");
        QCOMPARE(trimName->width(), maskName->width());
        QVERIFY(trimName->width() < ReviewTimeline::gutterWidth());
        QVERIFY(trimName->width() >= trimName->fontMetrics().horizontalAdvance(trimName->text()));
        auto* masks = window.findChild<QComboBox*>("masks");
        QCOMPARE(masks->mapTo(&window, QPoint()).x(), window.findChild<QWidget*>("trimTimingPair")->mapTo(&window, QPoint()).x());
        QCOMPARE(window.findChild<QWidget*>("trimControls")->height(), window.findChild<QToolButton*>("addMask")->height());
        // Sharing stays whole and the name retains a useful prefix at large text.
        auto* output = window.findChild<QToolButton*>("uploadDestinationMenu");
        QVERIFY(output->property("elideDestination").toBool());
        QCOMPARE(output->height(), output->fontMetrics().height());
        QVERIFY(!window.findChild<QWidget*>("reviewDestinationLine")->isVisible());
        QVERIFY(output->toolTip().contains(o.destination));
        window.resize(640, 648); putgd::test::settle(20);
        const auto suffix = output->property("sharingSuffix").toString();
        const int nameWidth = output->width() - output->fontMetrics().horizontalAdvance(suffix) - (6 + Ui::scaled(8) + 2);
        QVERIFY(nameWidth >= Ui::scaled(96));
        QVERIFY(output->fontMetrics().elidedText(o.destination, Qt::ElideRight, nameWidth).startsWith(o.destination.left(10)));
        auto* summary = window.findChild<QWidget*>("reviewDestinationSummary");
        auto* metadata = window.findChild<QWidget*>("reviewMetadata");
        const auto summaryRect = QRect(summary->mapTo(&window, QPoint()), summary->size());
        const auto metadataRect = QRect(metadata->mapTo(&window, QPoint()), metadata->size());
        QVERIFY(metadataRect.top() > summaryRect.bottom() || metadataRect.left() - summaryRect.right() - 1 >= Ui::space(Ui::Space::l));
        QVERIFY2(putgd::test::reviewFooterProblem(window).isEmpty(), qPrintable(putgd::test::reviewFooterProblem(window)));
        // Wide windows keep the rows on the lanes' gutter.
        window.resize(1600, window.height()); PUTGD_COMPARE_STEADY(trimName->width(), ReviewTimeline::gutterWidth(),20);
        QCOMPARE(maskName->width(), ReviewTimeline::gutterWidth());
    }
    void largeTextStillFitsA1366x768Desktop() {
        const auto font = QApplication::font();
        const auto scheme = Theme::scheme();
        const auto restore = qScopeGuard([&] { QApplication::setFont(font); Theme::setScheme(scheme); });
        auto large = font;
        if (font.pointSizeF() > 0) large.setPointSizeF(font.pointSizeF() * 1.5);
        else large.setPixelSize(font.pixelSize() * 3 / 2);
        QApplication::setFont(large);
        {
            ReviewWindow window(options(), config());
            // Constructing the window keeps the enlarged text (the shared style
            // installs once and never resets the application font).
            QVERIFY2(QFontInfo(QApplication::font()).pixelSize() >= QFontInfo(large).pixelSize() - 1, qPrintable(QString("%1 < %2").arg(QFontInfo(QApplication::font()).pixelSize()).arg(QFontInfo(large).pixelSize())));
            window.setEdit(twoMasks());
            window.show();
            QVERIFY(putgd::test::waitForWindowExposed(&window));
            QVERIFY(QFontInfo(window.font()).pixelSize() >= QFontInfo(large).pixelSize() - 1);
            const auto minimum = window.minimumSizeHint();
            QVERIFY2(minimum.width() <= 1366 - 24 && minimum.height() <= 768 - 120, qPrintable(QStringLiteral("%1x%2").arg(minimum.width()).arg(minimum.height())));
            QVERIFY(window.width() <= 1366 && window.height() <= 768);
            qInfo("review 150%% text %dx%d minimum %dx%d share %.3f", window.width(), window.height(), minimum.width(), minimum.height(), window.inspect()["video"].toObject()["share"].toDouble());
            const auto userGeometry = window.geometry();
            QVERIFY2(window.inspect()["video"].toObject()["share"].toDouble() >= 0.60, qPrintable(QString::number(window.inspect()["video"].toObject()["share"].toDouble())));
            QVERIFY(window.drive({{"action", "addMask"}, {"kind", "blur"}}).code.isEmpty());
            window.drive({{"action", "selectMask"}, {"maskId", "m-blur"}});
            QApplication::processEvents();
            qInfo("review 150%% text transient %dx%d share %.3f", window.width(), window.height(), window.inspect()["video"].toObject()["share"].toDouble());
            for (auto* label : window.findChild<QWidget*>("statusRow")->findChildren<QLabel*>("status")) if (label->isVisible())
                QVERIFY2(label->contentsRect().width() >= label->fontMetrics().horizontalAdvance(label->text()), qPrintable(QString("Blur guidance wraps: %1 < %2").arg(label->contentsRect().width()).arg(label->fontMetrics().horizontalAdvance(label->text()))));
            QCOMPARE(window.geometry(), userGeometry);
            QVERIFY2(window.inspect()["video"].toObject()["share"].toDouble() >= 0.53, qPrintable(QString::number(window.inspect()["video"].toObject()["share"].toDouble())));
            auto* status = window.findChild<QWidget*>("statusRow");
            QVERIFY(!status->isVisible());
            QVERIFY(window.inspect()["blurGuidance"].toObject()["visible"].toBool());
            QVERIFY(window.drive({{"action", "dismissBlurTip"}}).code.isEmpty());
            QApplication::processEvents();
            QVERIFY(window.inspect()["video"].toObject()["share"].toDouble() >= 0.60);
            // The compact header grows with the text instead of clipping it.
            auto* header = window.findChild<Ui::WindowHeader*>("windowHeader");
            QVERIFY(header && header->isVisible() && header->height() == header->compactHeight());
            auto* transport = header->findChild<QWidget*>("reviewTransport");
            QVERIFY(transport && transport->height() >= 28 && header->rect().contains(transport->geometry()));
            for (const auto& text : {QString("Save a copy…"), QString("Discard…"), QString("Upload")}) {
                auto* action = buttonNamed(window, text); QVERIFY(action);
                const auto font = action->font(); QCOMPARE(font.weight(), QFont::Normal);
                const int icon = action->icon().isNull() ? 0 : action->iconSize().width() + 8;
                QVERIFY(action->width() >= QFontMetrics(font).horizontalAdvance(text) + icon + 24);
            }
            // Every control is inside the window.
            for (auto* w : window.findChildren<QWidget*>()) if (w->isVisible() && w->isWindow() == false) QVERIFY2(window.rect().contains(QRect(w->mapTo(&window, QPoint()), w->size())), qPrintable(w->accessibleName() + w->objectName()));
        }
    }

    void diskFullExportErrorOffersRecoveryGuidance() {
        ReviewWindow window(options(), config()); window.setEdit(twoMasks()); window.show();
        QVERIFY(putgd::test::waitForWindowExposed(&window));
        QVERIFY(window.drive({{"action", "addMask"}, {"kind", "blur"}}).code.isEmpty());
        window.showError("Export failed: the encoder stopped after 412 frames because the disk holding the private recording is full; free some space and try again");
        PUTGD_TRY_VERIFY(window.inspect()["status"].toString().contains("Disk is full. Free up space, then try again."));
    }

    void compactPreviewKeepsANonInteractiveMutedIndicator() {
        const auto original = qApp->font(); auto restore = qScopeGuard([&] { qApp->setFont(original); });
        auto font = original; font.setPixelSize(21); qApp->setFont(font);
        auto o = options(); o.audioRecorded = true;
        auto c = config(); c.audio.system = true;
        ReviewWindow window(o, c); window.setEdit(twoMasks()); window.show();
        putgd::test::settle(20); window.resize(640, 720); putgd::test::settle(20);
        auto* text = window.findChild<QLabel*>("playbackStatus");
        auto* indicator = window.findChild<QLabel*>("previewMutedIndicator"); QVERIFY(indicator);
        QVERIFY(!text->isVisibleTo(&window)); QVERIFY(indicator->isVisibleTo(&window));
        QCOMPARE(indicator->focusPolicy(), Qt::NoFocus);
        auto* transport = window.findChild<QWidget*>("reviewTransport");
        // Layout-managed after the speed control, inside the header's measured fit.
        QCOMPARE(indicator->parentWidget(), transport);
        auto* speed = window.findChild<QWidget*>("speed");
        QCOMPARE(indicator->x() + indicator->contentsRect().left() - speed->geometry().right() - 1, int(Ui::Space::m));
        QVERIFY(window.rect().contains(QRect(indicator->mapTo(&window, QPoint()), indicator->size())));
        QVERIFY(indicator->accessibleDescription().contains("preview is silent", Qt::CaseInsensitive));
        QVERIFY(indicator->accessibleDescription().contains("export keeps the recorded system audio"));
        window.drive({{"action", "format"}, {"format", "gif"}});
        QVERIFY(indicator->accessibleDescription().contains("GIF exports are silent"));
    }
    // Round 19 R4: the silent-preview glyph keeps space(xl) clear of the window
    // controls at every width and text size.

    void selectedTinyInactiveMaskKeepsItsStateBesideTheMask() {
        const auto original = qApp->font(); auto restore = qScopeGuard([&] { qApp->setFont(original); });
        auto font = original; font.setPixelSize(21); qApp->setFont(font);
        FrameView frame; frame.resize(640, 360); frame.setCanvas({640, 360});
        QImage image(640, 360, QImage::Format_RGB32); image.fill(QColor(120, 120, 120));
        auto m = mask("one", "solid", 1000000, 2000000); m.rect = QRect(100, 80, 40, 14);
        frame.setFrame(image, 0); frame.setMasks({m}, "one"); frame.show(); PUTGD_VERIFY_STEADY(frame.accessibleDescription().contains("Inactive at playhead"),10);
        const auto shot = frame.grab().toImage();
        const QFontMetrics metrics(Ui::font(Ui::Type::maskBadge, true));
        const int width = metrics.horizontalAdvance(QStringLiteral("1 · inactive")) + 8;
        int scrim = 0;
        for (int x = m.rect.right() + 5; x < m.rect.right() + width + 4; ++x)
            scrim += shot.pixelColor(x, m.rect.top() + 2).lightness() < 100;
        QVERIFY2(scrim >= width - 8, "The compact identity and inactive state must fit beside a tiny mask");
    }
    void wideningDoesNotRaiseTheFittedMinimum_data() {
        QTest::addColumn<int>("pixels");
        QTest::newRow("normal") << 14;
        QTest::newRow("large") << 21;
    }
    void wideningDoesNotRaiseTheFittedMinimum() {
        QFETCH(int, pixels);
        const auto original = qApp->font();
        auto restore = qScopeGuard([&] { qApp->setFont(original); });
        auto font = original; font.setPixelSize(pixels); qApp->setFont(font);
        auto o = options(); o.availableSize = QSize(1366, 720); o.audioRecorded = true;
        ReviewWindow window(o, config()); window.setEdit(twoMasks()); window.show();
        QVERIFY(putgd::test::waitForWindowExposed(&window));
        putgd::test::settle(20);
        const auto fitted = window.size();
        const int minimum = window.minimumWidth();
        window.resize(1342, fitted.height()); PUTGD_VERIFY_STEADY(window.minimumWidth() <= minimum,20);QVERIFY2(window.minimumWidth() <= minimum, qPrintable(QString("Minimum ratcheted from %1 to %2").arg(minimum).arg(window.minimumWidth())));
        window.resize(fitted); PUTGD_COMPARE_STEADY(window.size(), fitted,20);
        QVERIFY2(putgd::test::reviewFooterProblem(window).isEmpty(), qPrintable(putgd::test::reviewFooterProblem(window)));
    }
    void compactReviewReportsStagesAndFitsBlurValue_data() {
        QTest::addColumn<QSize>("canvas"); QTest::addColumn<double>("scale");
        for (const auto canvas : {QSize(1280, 720), QSize(960, 600)}) for (double scale : {1.0, 1.25, 1.5})
            QTest::newRow(qPrintable(QString("%1-%2").arg(canvas.width()).arg(scale))) << canvas << scale;
    }
    void compactReviewReportsStagesAndFitsBlurValue() {
        QFETCH(QSize, canvas); QFETCH(double, scale);
        const auto original = qApp->font();
        auto restore = qScopeGuard([&] { qApp->setFont(original); });
        auto font = original; font.setPointSizeF(10.5 * scale); qApp->setFont(font);
        QCOMPARE(Ui::scaled(Ui::Metric::controlCompact), qRound(28 * scale));
        auto o = options(); o.canvas = canvas; o.durationUs = 32000000; o.availableSize = QSize(1366, 680); o.audioRecorded = true;
        auto c = config(); c.audio.system = true;
        RecordingEdit edit; edit.trimStart = 2000000; edit.trimEnd = 29000000;
        edit.masks = {mask("solid", "solid", 8000000, 21000000), mask("blur", "blur", 3000000, 12000000)};
        ReviewWindow window(o, c); window.setEdit(edit); window.show();
        QVERIFY(putgd::test::waitForWindowExposed(&window));
        QJsonObject report{{"canvas", QJsonArray{canvas.width(), canvas.height()}}, {"scale", scale}};
        const auto capture = [&](const QString& stage) {
            QApplication::processEvents();
            const auto state = window.inspect(); const double share = state["video"].toObject()["share"].toDouble();
            report[stage] = state;
            qInfo("compact tier %dx%d %.2f %s %dx%d share %.6f", canvas.width(), canvas.height(), scale, qPrintable(stage), window.width(), window.height(), share);
            const auto output = qEnvironmentVariable("PUTGD_REVIEW_TIER_EVIDENCE");
            if (!output.isEmpty()) { QDir().mkpath(output); window.grab().save(QDir(output).filePath(QString("%1-%2-%3.png").arg(canvas.width()).arg(scale).arg(stage))); }
        };
        window.drive({{"action", "selectMask"}, {"maskId", "solid"}}); capture("steady");
        window.drive({{"action", "addMask"}, {"kind", "blur"}}); window.drive({{"action", "selectMask"}, {"maskId", "blur"}}); capture("transient");
        window.drive({{"action", "format"}, {"format", "mp4"}, {"codec", "h265"}}); window.showError("Export failed: disk full"); capture("recovery");
        auto* strength = window.findChild<QSpinBox*>(); QVERIFY(strength);
        QStyleOptionSpinBox spin; spin.initFrom(strength); spin.frame = strength->hasFrame(); spin.buttonSymbols = strength->buttonSymbols();
        const int editWidth = strength->style()->subControlRect(QStyle::CC_SpinBox, &spin, QStyle::SC_SpinBoxEditField, strength).width();
        QCOMPARE(strength->suffix(), QString(" px"));
        QVERIFY(!window.findChild<QLabel*>("blurRadiusUnit")->isVisible());
        const int valueWidth = strength->fontMetrics().horizontalAdvance(QString::number(strength->maximum()) + strength->suffix());
        report["blurSpin"] = QJsonObject{{"width", strength->width()}, {"height", strength->height()}, {"editWidth", editWidth}, {"valueWidth", valueWidth}};
        const auto output = qEnvironmentVariable("PUTGD_REVIEW_TIER_EVIDENCE");
        if (!output.isEmpty()) { QFile file(QDir(output).filePath(QString("%1-%2.json").arg(canvas.width()).arg(scale))); QVERIFY(file.open(QIODevice::WriteOnly)); file.write(QJsonDocument(report).toJson()); }
        QVERIFY2(editWidth >= valueWidth, "The scaled steppers must leave room for the largest blur value");
    }
    void enlargedPreviewSequenceKeepsFrameGates() {
        const auto original = QApplication::font();
        const auto scheme = Theme::scheme();
        const auto restore = qScopeGuard([&] { QApplication::setFont(original); Theme::setScheme(scheme); });
        // Match the preview's 21 px body, including rounding from the 14 px base.
        auto enlarged = original; enlarged.setPixelSize(21); QApplication::setFont(enlarged);
        for (const auto theme : {Theme::Scheme::Dark, Theme::Scheme::Light}) for (const int height : {720, 680}) {
            Theme::setScheme(theme);
            auto o = options(); o.durationUs = 32000000; o.canvas = QSize(960, 600); o.audioRecorded = true; o.availableSize = QSize(1366, height);
            auto c = config(); c.audio.system = true;
            RecordingEdit edit; edit.trimStart = 2000000; edit.trimEnd = 29000000;
            edit.masks = {mask("solid", "solid", 8000000, 21000000), mask("blur", "blur", 3000000, 12000000)};
            ReviewWindow window(o, c); window.setEdit(edit); window.show();
            QVERIFY(putgd::test::waitForWindowExposed(&window));
            QVERIFY(QFontInfo(window.findChild<QLabel*>("trimGroup")->font()).pixelSize() >= 18);
            const auto geometry = window.geometry();
            window.drive({{"action", "selectMask"}, {"maskId", "solid"}});
            QApplication::processEvents();
            qInfo("sequence steady %dx%d share %.3f frame %d mask %d status %d timeline %d", window.width(), window.height(), window.inspect()["video"].toObject()["share"].toDouble(), window.findChild<FrameView*>()->height(), window.findChild<QWidget*>("maskControls")->height(), window.findChild<QWidget*>("statusRow")->height(), timeline(window)->height());

            QVERIFY2(window.inspect()["video"].toObject()["share"].toDouble() >= 0.60, qPrintable(QString::number(window.inspect()["video"].toObject()["share"].toDouble())));
            QVERIFY(window.drive({{"action", "addMask"}, {"kind", "blur"}}).code.isEmpty());
            window.drive({{"action", "selectMask"}, {"maskId", "blur"}});
            QApplication::processEvents();
            qInfo("sequence transient %dx%d share %.3f frame %d status %d footer %d", window.width(), window.height(), window.inspect()["video"].toObject()["share"].toDouble(), window.findChild<FrameView*>()->height(), window.findChild<QWidget*>("statusRow")->height(), window.findChild<QWidget*>("reviewFooter")->height());
            for (auto* group : window.findChild<QWidget*>("maskControls")->findChildren<QWidget*>(QString(), Qt::FindDirectChildrenOnly)) if (group->isVisible())
                qInfo("mask group %s %dx%d at %d,%d", qPrintable(group->objectName()), group->width(), group->height(), group->x(), group->y());
            qInfo("mask row %d; desired %d", window.findChild<QWidget*>("maskControls")->height(), window.findChild<QWidget*>("maskControls")->heightForWidth(window.findChild<QWidget*>("maskControls")->width()));
            for (auto* label : window.findChild<QWidget*>("statusRow")->findChildren<QLabel*>("status")) if (label->isVisible())
                QVERIFY2(label->contentsRect().width() >= QFontMetricsF(label->font()).horizontalAdvance(label->text()), "Blur guidance must stay on one line in a constrained workspace");
            QVERIFY(window.inspect()["video"].toObject()["share"].toDouble() >= 0.53);
            window.drive({{"action", "format"}, {"format", "mp4"}, {"codec", "h265"}});
            window.showError("Export failed: disk full");
            QApplication::processEvents();
            for (auto* label : window.findChild<QWidget*>("statusRow")->findChildren<QLabel*>("status")) if (label->isVisible())
                QVERIFY2(label->heightForWidth(label->width()) <= label->fontMetrics().height(),
                    qPrintable(QString("'%1' needs %2 px, has %3 px at window %4; details %5 px in %6")
                        .arg(label->text()).arg(label->fontMetrics().horizontalAdvance(label->text())).arg(label->width()).arg(window.width())
                        .arg(window.findChild<QPushButton*>("reviewErrorDetails")->width()).arg(window.findChild<QPushButton*>("reviewErrorDetails")->parentWidget()->objectName())));
            qInfo("enlarged preview sequence %dx%d recovery %.3f", window.width(), window.height(), window.inspect()["video"].toObject()["share"].toDouble());
            QVERIFY(window.inspect()["video"].toObject()["share"].toDouble() >= 0.45);
            QCOMPARE(window.geometry(), geometry);
            for (auto* button : window.findChild<QWidget*>("statusRow")->findChildren<QPushButton*>("statusAction")) {
                QVERIFY(button->property("uiFlush").toBool());
                QVERIFY2(button->width() >= std::ceil(QFontMetricsF(button->font()).horizontalAdvance(button->text())), qPrintable(button->text()));
            }
        }
    }
    void constrainedWorkspaceKeepsWarningsAndActionsReachable_data() {
        QTest::addColumn<QSize>("canvas");
        QTest::newRow("16:9") << QSize(1280, 720);
        QTest::newRow("16:10") << QSize(960, 600);
    }
    // Final read (council 17): an error used to hide the selected blur's warning. With
    // room, the warning line and the error band stack; the footer keeps its facts.
    void blurWarningStaysPaintedWithAnError() {
        auto o = options(); o.audioRecorded = true;
        ReviewWindow window(o, config()); window.setEdit(twoMasks()); window.show();
        QVERIFY(putgd::test::waitForWindowExposed(&window));
        QVERIFY(window.drive({{"action", "addMask"}, {"kind", "blur"}}).code.isEmpty());
        window.drive({{"action", "selectMask"}, {"maskId", "m-blur"}});
        window.showError("Export failed: the disk holding the private recording is full");
        QApplication::processEvents();
        QVERIFY(window.inspect()["blurGuidance"].toObject()["visible"].toBool());
        QVERIFY(window.findChild<QWidget*>("reviewBlurChip")->isVisible());
        QVERIFY(window.findChild<QWidget*>("statusRow")->isVisible());
        QCOMPARE(window.findChild<QLabel*>("playbackStatus")->text(), QString("Silent preview"));
        QVERIFY(window.inspect()["video"].toObject()["share"].toDouble() >= 0.45);
        QVERIFY2(putgd::test::reviewFooterProblem(window).isEmpty(), qPrintable(putgd::test::reviewFooterProblem(window)));
        // Dismissing the error restores the footer's facts.
        window.drive({{"action", "dismissStatus"}});
        QVERIFY(!window.findChild<QWidget*>("statusRow")->isVisible());
        QVERIFY(window.findChild<QWidget*>("reviewBlurChip")->isVisible());
        QCOMPARE(window.findChild<QLabel*>("playbackStatus")->text(), QString("Silent preview"));
    }
    void constrainedWorkspaceKeepsWarningsAndActionsReachable() {
        QFETCH(QSize, canvas);
        const auto font = QApplication::font();
        const auto scheme = Theme::scheme();
        const auto restore = qScopeGuard([&] { QApplication::setFont(font); Theme::setScheme(scheme); });
        auto large = font;
        if (font.pointSizeF() > 0) large.setPointSizeF(font.pointSizeF() * 1.5);
        else large.setPixelSize(font.pixelSize() * 3 / 2);
        QApplication::setFont(large);
        for (const auto scheme : {Theme::Scheme::Dark, Theme::Scheme::Light}) {
            Theme::setScheme(scheme);
            auto o = options(); o.availableSize = QSize(1366, 680); o.canvas = canvas; o.durationUs = 32000000;
            auto edit = twoMasks(); edit.trimStart = 2000000; edit.trimEnd = 29000000;
            ReviewWindow window(o, config()); window.setEdit(edit); window.show();
            QVERIFY(putgd::test::waitForWindowExposed(&window));
            QCOMPARE(window.inspect()["playheadUs"].toDouble(), double(edit.trimStart));
            if (window.height() > 680 || window.inspect()["video"].toObject()["share"].toDouble() < 0.60) {
                const auto render = qEnvironmentVariable("PUTGD_REVIEW_FAILURE_IMAGE");
                if (!render.isEmpty()) window.grab().save(render);
                for (auto* w : window.findChildren<QWidget*>()) if (w->isVisible() && !w->isWindow())
                    qInfo() << w->metaObject()->className() << w->objectName() << w->accessibleName() << w->geometry() << w->minimumSizeHint() << w->sizeHint();
            }
            PUTGD_TRY_VERIFY(window.height() <= 680);
            auto* audio = window.findChild<QLabel*>("exportAudioSummary");
            QCOMPARE(audio->text(), QStringLiteral("No audio recorded"));
            QCOMPARE(audio->accessibleName(), QStringLiteral("No audio recorded · Silent preview"));
            QVERIFY(window.inspect()["previewMuted"].toBool());
            const auto steady = window.inspect()["video"].toObject()["share"].toDouble();
            qInfo("constrained review %dx%d %s window %dx%d steady %.3f", canvas.width(), canvas.height(), qPrintable(Theme::name(scheme)), window.width(), window.height(), steady);
            QVERIFY2(steady >= 0.60, qPrintable(QString::number(steady)));
            QVERIFY(window.drive({{"action", "addMask"}, {"kind", "blur"}}).code.isEmpty());
            window.drive({{"action", "selectMask"}, {"maskId", "m-blur"}});
            window.drive({{"action", "format"}, {"format", "mp4"}, {"codec", "h265"}});
            putgd::test::settle(40);
            const auto transient = window.inspect()["video"].toObject()["share"].toDouble();
            qInfo("constrained review transient %.3f", transient);
            QVERIFY2(transient >= 0.53, qPrintable(QString::number(transient)));
            const QString failure = "Export failed: the encoder stopped after 412 frames because the disk holding the private recording is full; free some space and try again";
            window.showError(failure);
            PUTGD_TRY_VERIFY2(window.height() <= 680 && window.minimumSizeHint().height() <= 680,
                qPrintable(QString("%1x%2 min %3x%4 share %5").arg(window.width()).arg(window.height()).arg(window.minimumSizeHint().width()).arg(window.minimumSizeHint().height()).arg(window.inspect()["video"].toObject()["share"].toDouble())));
            PUTGD_TRY_VERIFY(window.inspect()["status"].toString().contains("Disk is full. Free up space, then try again."));
            qInfo("visible recovery frame share %.3f at %dx%d", window.inspect()["video"].toObject()["share"].toDouble(), window.width(), window.height());
            QVERIFY(window.inspect()["video"].toObject()["share"].toDouble() >= 0.45);
            QVERIFY(!window.inspect()["status"].toString().contains(Ui::blurWarningText()));
            // Final read (council 17): the error doesn't hide the selected blur's warning;
            // both lines stack in the constrained window and stay inside it.
            QVERIFY(window.inspect()["blurGuidance"].toObject()["visible"].toBool());
            auto* chip = window.findChild<QWidget*>("reviewBlurChip");
            QVERIFY(chip && chip->isVisible() && window.rect().contains(QRect(chip->mapTo(&window, QPoint()), chip->size())));
            // Here both don't fit: the error takes the footer's status slot with Details….
            QVERIFY(!window.findChild<QWidget*>("statusRow")->isVisible());
            QCOMPARE(window.findChild<QLabel*>("playbackStatus")->text(), QString("Disk is full. Free up space, then try again."));
            QVERIFY(window.findChild<QWidget*>("reviewFooter")->isAncestorOf(window.findChild<QWidget*>("reviewErrorDetails")));
            QVERIFY2(putgd::test::reviewFooterProblem(window).isEmpty(), qPrintable(putgd::test::reviewFooterProblem(window)));
            QVERIFY(window.inspect()["errorDetails"].toObject()["visible"].toBool());
            QCOMPARE(window.inspect()["errorDetails"].toObject()["message"].toString(), failure);
            for (auto* w : window.findChildren<QWidget*>()) if (w->isVisible() && !w->isWindow())
                QVERIFY2(window.rect().contains(QRect(w->mapTo(&window, QPoint()), w->size())), qPrintable(w->accessibleName() + w->objectName() + QString(" %1,%2 %3x%4 in %5x%6").arg(w->mapTo(&window, QPoint()).x()).arg(w->mapTo(&window, QPoint()).y()).arg(w->width()).arg(w->height()).arg(window.width()).arg(window.height())));
            QVERIFY(window.drive({{"action", "showAdvice"}}).state["adviceDialog"].toObject()["open"].toBool());
            auto* details = window.findChild<QWidget*>("reviewAdviceText");
            QVERIFY(details);
            PUTGD_TRY_VERIFY(details->hasFocus());
            QVERIFY(details->property("plainText").toString().contains(failure));
            QVERIFY(!buttonNamed(window, "Use MP4 (H.264)"));
            QVERIFY(!window.drive({{"action", "showAdvice"}}).state["adviceDialog"].toObject()["open"].toBool());
        }
    }

};

QTEST_MAIN(ReviewWindowTest)
#include "test_review_window_workspace.moc"
