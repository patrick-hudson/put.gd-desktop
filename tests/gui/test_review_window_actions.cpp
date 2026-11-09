#include "ReviewWindowFixture.h"

class ReviewWindowTest : public ReviewWindowFixture {
    Q_OBJECT
private slots:
    void audioSurvivesSilentFormatRoundTrips_data() {
        QTest::addColumn<bool>("recorded");
        QTest::newRow("recorded") << true;
        QTest::newRow("silent-capture") << false;
    }
    void audioSurvivesSilentFormatRoundTrips() {
        QFETCH(bool, recorded);
        auto o = options(); o.audioRecorded = recorded; o.formats.append("webp");
        auto c = config(); c.audio.system = recorded;
        ReviewWindow window(o, c); window.show(); QVERIFY(putgd::test::waitForWindowExposed(&window));
        QSignalSpy changed(&window, &ReviewWindow::configChanged);
        for (const auto& format : {"gif", "webp"}) {
            QVERIFY(window.drive({{"action", "format"}, {"format", format}}).code.isEmpty());
            QVERIFY(!qvariant_cast<RecordingConfig>(changed.last().first()).audio.system);
            QCOMPARE(window.findChild<QLabel*>("exportAudioSummary")->accessibleName(), recorded ? QString("No audio in %1 · Silent preview").arg(QString(format).toUpper()) : QString("No audio recorded · Silent preview"));
            QVERIFY(!window.inspect()["audioEffective"].toBool());
            QVERIFY(window.drive({{"action", "format"}, {"format", "mp4"}, {"codec", "h264"}}).code.isEmpty());
            QCOMPARE(qvariant_cast<RecordingConfig>(changed.last().first()).audio.system, recorded);
            QCOMPARE(window.findChild<QLabel*>("exportAudioSummary")->accessibleName(), recorded ? QString("System audio included · Silent preview") : QString("No audio recorded · Silent preview"));
            QCOMPARE(window.inspect()["audioEffective"].toBool(), recorded);
        }
    }
    void controlsAndStatusKeepTheUsersGeometry() {
        ReviewWindow window(options(), config()); window.show(); QVERIFY(putgd::test::waitForWindowExposed(&window));
        QApplication::processEvents();window.resize(780,600);QApplication::processEvents();const auto geometry=window.geometry();
        window.setEdit(twoMasks());window.drive({{"action","selectMask"},{"maskId","m-blur"}});
        window.showError("Export failed: free some space and try again");QApplication::processEvents();QCOMPARE(window.geometry(),geometry);
        window.drive({{"action","dismissBlurTip"}});window.drive({{"action","dismissStatus"}});QApplication::processEvents();QCOMPARE(window.geometry(),geometry);
    }
    void selectedMaskTimingStaysVisibleAfterEdits() {
        auto o = options(); o.durationUs = 32000000;
        ReviewWindow window(o, config()); auto edit = twoMasks(); window.setEdit(edit); window.show();
        window.drive({{"action", "selectMask"}, {"maskId", "m-blur"}});
        auto* label = window.findChild<QLabel*>("maskActiveInterval");
        QVERIFY(label && label->isVisible());
        edit.revision++; edit.masks[0].start = 5000000; edit.masks[0].end = 20000000; window.setEdit(edit);
        // Before the mask: when it starts; inside it: the interval alone; after it: not at the playhead.
        QCOMPARE(label->text(), QString("0:05.0–0:20.0 · Starts at 0:05.0"));
        QCOMPARE(window.inspect()["selectedMaskInterval"].toObject()["text"].toString(), label->text());
        const auto geometry = label->geometry();
        window.drive({{"action", "seek"}, {"time", 8000000}});
        QCOMPARE(label->text(), QString("0:05.0–0:20.0"));
        QCOMPARE(label->geometry(), geometry);
        window.drive({{"action", "seek"}, {"time", 25000000}});
        QCOMPARE(label->text(), QString("0:05.0–0:20.0 · Not at playhead"));
        QCOMPARE(label->geometry(), geometry);
        QVERIFY(!label->text().contains("Inactive"));
        window.drive({{"action", "seek"}, {"time", 0}});
        const auto position = label->pos();
        QTest::keyClick(window.findChild<FrameView*>(), Qt::Key_Right, Qt::ControlModifier);
        QCOMPARE(label->pos(), position);
        QVERIFY(!window.findChild<FrameView*>()->isAncestorOf(label));
        QCOMPARE(label->text(), QString("0:05.0–0:20.0 · Starts at 0:05.0"));
        QCOMPARE(QFontInfo(label->font()).family(), QFontInfo(Ui::monoFont(Ui::Type::technical)).family());
        edit.revision++; edit.trimStart = 2000000; edit.trimEnd = 29000000; window.setEdit(edit);
        window.drive({{"action", "seek"}, {"time", 0}});
        QCOMPARE(window.inspect()["previewAudioStatus"].toString(), QStringLiteral("Silent preview"));
        QVERIFY(window.inspect()["outsideKeptPart"].toBool());
        QCOMPARE(window.findChild<QLabel*>("muted")->text(), QString("Keep 0:02–0:29 · 27 s"));
    }
    void adviceWidthScalesWithinTheAvailableScreen() {
        auto font = qApp->font(); font.setPixelSize(21); qApp->setFont(font);
        for (const auto available : {QSize(1200, 680), QSize(700, 680)}) {
            auto o = options(); o.availableSize = available;
            ReviewWindow window(o, config()); window.show();
            window.drive({{"action", "showAdvice"}});
            auto* dialog = window.findChild<QDialog*>("reviewAdvice");
            QVERIFY(dialog && dialog->isVisible());
            QCOMPARE(dialog->width(), std::min(Ui::scaled(640), std::min(available.width(), dialog->screen()->availableGeometry().width())));
            QVERIFY(dialog->height() <= available.height());
        }
    }
    void adviceOnlyOffersAvailableFormats() {
        auto o = options(); o.codecs["mp4"] = QJsonArray{"h265"};
        auto c = config(); c.codec = "h265";
        ReviewWindow window(o, c); window.show();
        QVERIFY(putgd::test::waitForWindowExposed(&window));
        QVERIFY(window.drive({{"action", "showAdvice"}}).state["adviceDialog"].toObject()["open"].toBool());
        QVERIFY(!buttonNamed(window, "Use MP4 (H.264)"));
    }
    void emptyMaskLanesHaveATruthfulSummary() {
        ReviewWindow window(options(), config()); window.show();
        QApplication::processEvents();
        auto* summary = window.findChild<QLabel*>("maskLaneSummary");
        QVERIFY(summary);
        QCOMPARE(summary->text(), QStringLiteral("No masks"));
        QCOMPARE(summary->toolTip(), summary->text());
        QCOMPARE(summary->accessibleDescription(), summary->text());
        QCOMPARE(window.inspect()["timeline"].toObject()["laneSummary"].toString(), summary->text());
        window.setEdit(twoMasks()); window.setEdit({});
        QCOMPARE(summary->text(), QStringLiteral("No masks"));
    }
    void maskControlsShowOnlyWhenTheyApply() {
        ReviewWindow window(options(), config());
        window.show();
        QVERIFY(putgd::test::waitForWindowExposed(&window));
        auto controls = [&] { return window.inspect()["maskControls"].toObject(); };
        QCOMPARE(controls(), QJsonObject({{"list", false}, {"times", false}, {"strength", false}, {"remove", false}}));
        // Two masks arrive: the list and timing stay inline; secondary controls live in Mask options.
        window.setEdit(twoMasks());
        QCOMPARE(window.inspect()["selectedMaskId"].toString(), QString("m-solid"));
        QCOMPARE(controls(), QJsonObject({{"list", true}, {"times", true}, {"strength", false}, {"remove", false}}));
        auto* list = window.findChild<QComboBox*>("masks");
        QVERIFY(list);
        QCOMPARE(list->count(), 2);
        QCOMPARE(list->itemText(0), QString("Blur 1"));
        list->setCurrentIndex(0);
        QCOMPARE(window.inspect()["selectedMaskId"].toString(), QString("m-blur"));
        QCOMPARE(controls()["strength"].toBool(), false);
        QVERIFY(window.inspect()["maskOptions"].toObject()["blurRadiusAvailable"].toBool());
        QVERIFY(window.drive({{"action", "showMaskOptions"}}).code.isEmpty());
        QCOMPARE(controls()["strength"].toBool(), true);
        QVERIFY(window.drive({{"action", "closeMaskOptions"}}).code.isEmpty());
        // A click on a mask's bar on the timeline selects it.
        auto* t = timeline(window);
        t->findChild<QScrollBar*>("maskLaneScroll")->setValue(1);
        QTest::mouseClick(t, Qt::LeftButton, {}, t->maskRect("m-solid").center());
        QCOMPARE(window.inspect()["selectedMaskId"].toString(), QString("m-solid"));
        // A click outside every mask clears the selection and hides its controls.
        auto* frame = window.findChild<FrameView*>();
        QTest::mouseClick(frame, Qt::LeftButton, {}, frame->imageRect().bottomRight() - QPoint(4, 4));
        QVERIFY(window.inspect()["selectedMaskId"].isNull());
        QCOMPARE(controls(), QJsonObject({{"list", true}, {"times", true}, {"strength", false}, {"remove", false}}));
        // Later edits keep "nothing selected"; removing every mask hides the group.
        auto edit = twoMasks(4);
        edit.trimStart = 2000000;
        window.setEdit(edit);
        QVERIFY(window.inspect()["selectedMaskId"].isNull());
        RecordingEdit empty;
        empty.revision = 5;
        window.setEdit(empty);
        QCOMPARE(controls()["list"].toBool(), false);
    }
    void timelineSetsTrimAndMaskTimes() {
        ReviewWindow window(options(), config());
        QSignalSpy proposals(&window, &ReviewWindow::editProposed);
        window.show();
        QVERIFY(putgd::test::waitForWindowExposed(&window));
        window.activateWindow();
        QVERIFY(putgd::test::waitForWindowActive(&window));
        auto* t = timeline(window);
        QVERIFY(t);
        const int y = t->trackRect().center().y();
        // A click on the track moves the playhead; I sets the trim start there.
        QTest::mouseClick(t, Qt::LeftButton, {}, QPoint(t->xFor(3000000), y));
        QVERIFY(std::abs(t->playhead() - 3000000) <= slack(*t));
        QTest::keyClick(t, Qt::Key_I);
        QCOMPARE(proposals.size(), 1);
        QCOMPARE(proposed(proposals)["op"].toString(), QString("setTrim"));
        QCOMPARE(qint64(proposed(proposals)["start"].toDouble()), t->playhead());
        QVERIFY(proposed(proposals)["end"].isNull());
        // O sets the trim end at the playhead.
        QTest::mouseClick(t, Qt::LeftButton, {}, QPoint(t->xFor(9000000), y));
        QTest::keyClick(t, Qt::Key_O);
        QCOMPARE(proposals.size(), 2);
        QCOMPARE(qint64(proposed(proposals)["end"].toDouble()), t->playhead());
        // The resident's answer shows on the timeline; dragging its start handle retimes it.
        RecordingEdit edit;
        edit.revision = 3;
        edit.trimStart = 3000000;
        edit.trimEnd = 9000000;
        window.setEdit(edit);
        drag(t, QPoint(t->xFor(3000000), y), QPoint(t->xFor(5000000), y));
        QCOMPARE(proposals.size(), 3);
        QCOMPARE(proposals.last().at(0).toULongLong(), quint64(3));
        QVERIFY(std::abs(qint64(proposed(proposals)["start"].toDouble()) - 5000000) <= slack(*t));
        QCOMPARE(proposed(proposals)["end"].toDouble(), 9000000.0);
        // Dragging the end handle past the clip end leaves the end open.
        drag(t, QPoint(t->xFor(9000000), y), QPoint(t->width() + 40, y));
        QVERIFY(proposed(proposals)["end"].isNull());
        // A selected mask: drag its end on its lane, then Shift+I and Delete.
        edit.revision = 4;
        edit.masks = {mask("m-blur", "blur", 4000000, 8000000)};
        window.setEdit(edit);
        QCOMPARE(window.inspect()["selectedMaskId"].toString(), QString("m-blur"));
        const auto bar = t->maskRect("m-blur");
        drag(t, QPoint(bar.left() + bar.width(), bar.center().y()), QPoint(t->xFor(10000000), bar.center().y()));
        QCOMPARE(proposed(proposals)["op"].toString(), QString("updateMask"));
        QCOMPARE(proposed(proposals)["maskId"].toString(), QString("m-blur"));
        QCOMPARE(proposed(proposals)["mask"].toObject()["start"].toDouble(), 4000000.0);
        QVERIFY(std::abs(qint64(proposed(proposals)["mask"].toObject()["end"].toDouble()) - 10000000) <= slack(*t));
        QTest::mouseClick(t, Qt::LeftButton, {}, QPoint(t->xFor(5000000), y));
        QTest::keyClick(t, Qt::Key_I, Qt::ShiftModifier);
        QCOMPARE(qint64(proposed(proposals)["mask"].toObject()["start"].toDouble()), t->playhead());
        QCOMPARE(proposed(proposals)["mask"].toObject()["end"].toDouble(), 8000000.0);
        QTest::keyClick(t, Qt::Key_O, Qt::ShiftModifier);
        QCOMPARE(qint64(proposed(proposals)["mask"].toObject()["end"].toDouble()), t->playhead());
        QTest::keyClick(t, Qt::Key_Delete);
        QCOMPARE(proposed(proposals), QJsonObject({{"op", "removeMask"}, {"maskId", "m-blur"}}));
        // Arrows step the playhead; Home goes to the trim start.
        const auto at = t->playhead();
        QTest::keyClick(t, Qt::Key_Right);
        QCOMPARE(t->playhead(), at + 100000);
        QTest::keyClick(t, Qt::Key_Home);
        QCOMPARE(t->playhead(), qint64(3000000));
    }
    void draggingAMaskBarMovesItInTime() {
        ReviewWindow window(options(), config());
        QSignalSpy proposals(&window, &ReviewWindow::editProposed);
        window.setEdit(twoMasks());
        window.show();
        QVERIFY(putgd::test::waitForWindowExposed(&window));
        auto* t = timeline(window);
        QVERIFY(t);
        // Press inside the unselected redaction (4 s to 10 s) and drag it 3 s
        // earlier: it is selected and moves as a whole, keeping its 6 s length.
        const auto bar = t->maskRect("m-solid");
        const QPoint from(t->xFor(7000000), bar.center().y());
        drag(t, from, QPoint(t->xFor(4000000), bar.center().y()));
        QCOMPARE(window.inspect()["selectedMaskId"].toString(), QString("m-solid"));
        QCOMPARE(proposed(proposals)["op"].toString(), QString("updateMask"));
        QCOMPARE(proposed(proposals)["maskId"].toString(), QString("m-solid"));
        const auto moved = proposed(proposals)["mask"].toObject();
        QVERIFY(std::abs(qint64(moved["start"].toDouble()) - 1000000) <= slack(*t));
        QCOMPARE(qint64(moved["end"].toDouble() - moved["start"].toDouble()), qint64(6000000));
        // Past the clip's end it stops there, still 6 s long.
        RecordingEdit edit = twoMasks(4);
        window.setEdit(edit);
        const auto again = t->maskRect("m-solid");
        drag(t, QPoint(again.center().x(), again.center().y()), QPoint(t->width() + 80, again.center().y()));
        const auto clamped = proposed(proposals)["mask"].toObject();
        QCOMPARE(qint64(clamped["end"].toDouble()), t->duration());
        QCOMPARE(qint64(clamped["end"].toDouble() - clamped["start"].toDouble()), qint64(6000000));
    }
    // A click that wobbles less than the drag distance only selects the mask:
    // no retime (no edit, no undo step) and the playhead stays where it was.
    void clickingAMaskBarWithJitterOnlySelectsIt() {
        ReviewWindow window(options(), config());
        QSignalSpy proposals(&window, &ReviewWindow::editProposed);
        window.setEdit(twoMasks());
        window.show();
        QVERIFY(putgd::test::waitForWindowExposed(&window));
        auto* t = timeline(window);
        QVERIFY(t);
        const auto before = proposals.size();
        const auto playhead = t->playhead();
        const auto bar = t->maskRect("m-solid");
        const QPoint from(t->xFor(7000000), bar.center().y());
        drag(t, from, from + QPoint(QApplication::startDragDistance() - 1, 0));
        QCOMPARE(window.inspect()["selectedMaskId"].toString(), QString("m-solid"));
        QCOMPARE(proposals.size(), before);
        QCOMPARE(t->playhead(), playhead);
    }
    void keyboardAndNamedIconControlsInBothSchemes() {
        for (const auto scheme : {Theme::Scheme::Dark, Theme::Scheme::Light}) {
            Theme::setScheme(scheme);
            ReviewWindow window(options(), config());
            window.setEdit(twoMasks());
            window.show();
            QVERIFY(putgd::test::waitForWindowExposed(&window));
            // No two shortcuts share a key; ambiguous shortcuts never fire.
            QList<QKeySequence> keys;
            for (auto* s : window.findChildren<QShortcut*>()) { QVERIFY2(!keys.contains(s->key()), qPrintable(s->key().toString())); keys.append(s->key()); }
            for (const auto& key : {QKeySequence(Qt::Key_I), QKeySequence(Qt::Key_O), QKeySequence(Qt::SHIFT | Qt::Key_I), QKeySequence(Qt::SHIFT | Qt::Key_O), QKeySequence(Qt::Key_Delete)}) QVERIFY2(keys.contains(key), qPrintable(key.toString()));
            // Timing actions use words; the remaining tools retain their named glyphs.
            const auto tools = window.findChildren<QToolButton*>();
            QVERIFY(tools.size() >= 9);
            for (auto* b : tools) {
                QVERIFY2(b->objectName() == "reviewBlurChip" || (b->toolButtonStyle() == Qt::ToolButtonTextOnly && (QStringList{"Set trim start at playhead", "Set trim end at playhead", "Reset trim", "Set mask start at playhead", "Set mask end at playhead", "Add mask"}.contains(b->accessibleName()) || QStringList{"showMoreMasks", "previousMaskLanes", "uploadDestinationMenu"}.contains(b->objectName()))) || ((!b->icon().isNull() || b->arrowType() != Qt::NoArrow) && (b->toolButtonStyle() == Qt::ToolButtonIconOnly || (b->toolButtonStyle() == Qt::ToolButtonTextBesideIcon && QStringList{"Start", "End", "Delete mask"}.contains(b->text())))), qPrintable(b->accessibleName()));
                QVERIFY2(!b->accessibleName().isEmpty() && !b->toolTip().isEmpty(), qPrintable(b->objectName()));
                if(b->isVisible()&&b->objectName()!="reviewBlurChip"&&b->toolButtonStyle()==Qt::ToolButtonTextBesideIcon)QVERIFY2(b->width()>=b->fontMetrics().horizontalAdvance(b->text())+b->iconSize().width()+16,qPrintable(b->accessibleName()));
            }
            // The window background is the scheme's.
            const auto image = window.grab().toImage();
            QCOMPARE(image.pixelColor(2, 2).rgb(), Theme::palette(scheme).bg.rgb());
            QCOMPARE(window.inspect()["scheme"].toString(), Theme::name(scheme));
            // Space starts the preview (the clip is missing, so it stops again).
            window.activateWindow();
            QVERIFY(putgd::test::waitForWindowActive(&window));
            QTest::keyClick(timeline(window), Qt::Key_Space);
            QVERIFY(window.inspect()["playing"].toBool());
            PUTGD_TRY_VERIFY(!window.inspect()["playing"].toBool());
        }
        Theme::setScheme(Theme::Scheme::Dark);
    }
    void reviewMethodDrivesTheSameControls() {
        ReviewWindow window(options(), config());
        QSignalSpy proposals(&window, &ReviewWindow::editProposed);
        QSignalSpy configs(&window, &ReviewWindow::configChanged);
        window.show();
        QVERIFY(putgd::test::waitForWindowExposed(&window));
        auto drive = [&](const QJsonObject& params) { return window.drive(params); };
        auto seek = drive({{"action", "seek"}, {"time", 2000000}});
        QVERIFY(seek.code.isEmpty());
        QCOMPARE(seek.state["playheadUs"].toDouble(), 2000000.0);
        QCOMPARE(drive({{"action", "seek"}, {"time", double(duration)}}).code, QString("invalid_request"));
        QVERIFY(drive({{"action", "setIn"}}).code.isEmpty());
        QCOMPARE(proposed(proposals), QJsonObject({{"op", "setTrim"}, {"start", 2000000.0}, {"end", QJsonValue(QJsonValue::Null)}}));
        // setOut at or before the trim start is refused like the control (and shown).
        RecordingEdit edit;
        edit.revision = 2;
        edit.trimStart = 2000000;
        window.setEdit(edit);
        const auto refused = drive({{"action", "setOut"}});
        QCOMPARE(refused.code, QString("invalid_request"));
        QCOMPARE(window.inspect()["status"].toString(), refused.message);
        QVERIFY(drive({{"action", "addMask"}, {"kind", "blur"}, {"rect", QJsonArray{10, 10, 200, 100}}}).code.isEmpty());
        const auto added = proposed(proposals);
        QCOMPARE(added["op"].toString(), QString("addMask"));
        QCOMPARE(added["mask"].toObject()["start"].toDouble(), 2000000.0);
        QCOMPARE(added["mask"].toObject()["end"].toDouble(), double(duration));
        QCOMPARE(added["mask"].toObject()["strength"].toInt(), 16);
        QCOMPARE(drive({{"action", "addMask"}, {"kind", "blur"}, {"rect", QJsonArray{1200, 10, 200, 100}}}).code, QString("invalid_request"));
        QCOMPARE(drive({{"action", "selectMask"}, {"maskId", "nope"}}).code, QString("invalid_request"));
        QCOMPARE(drive({{"action", "strength"}, {"strength", 20}}).code, QString("invalid_request"));
        edit.revision = 3;
        edit.masks = {mask("m-blur", "blur", 2000000, duration)};
        window.setEdit(edit);
        auto selected = drive({{"action", "selectMask"}, {"maskId", "m-blur"}});
        QCOMPARE(selected.state["maskControls"].toObject()["strength"].toBool(), false);
        QVERIFY(selected.state["maskOptions"].toObject()["blurRadiusAvailable"].toBool());
        QVERIFY(drive({{"action", "strength"}, {"strength", 30}}).code.isEmpty());
        QCOMPARE(proposed(proposals)["mask"].toObject()["strength"].toInt(), 30);
        QVERIFY(drive({{"action", "seek"}, {"time", 7000000}}).code.isEmpty());
        QVERIFY(drive({{"action", "setMaskOut"}}).code.isEmpty());
        QCOMPARE(proposed(proposals)["mask"].toObject()["end"].toDouble(), 7000000.0);
        QVERIFY(drive({{"action", "removeMask"}}).code.isEmpty());
        QCOMPARE(proposed(proposals), QJsonObject({{"op", "removeMask"}, {"maskId", "m-blur"}}));
        QVERIFY(drive({{"action", "speed"}, {"speed", 2}}).state["speed"].toDouble() == 2.0);
        QCOMPARE(drive({{"action", "speed"}, {"speed", 3}}).code, QString("invalid_request"));
        QVERIFY(drive({{"action", "format"}, {"format", "webm"}, {"codec", "vp9"}}).code.isEmpty());
        QCOMPARE(configs.size(), 1);
        QCOMPARE(drive({{"action", "format"}, {"format", "webp"}, {"codec", ""}}).code, QString("invalid_request"));
        QCOMPARE(drive({{"action", "explode"}}).code, QString("invalid_request"));
        // While an export runs only inspect answers.
        const auto count = proposals.size();
        window.setExporting(true, "Exporting MP4…");
        QCOMPARE(drive({{"action", "setIn"}}).code, QString("recording_state"));
        QVERIFY(drive({{"action", "inspect"}}).state["exporting"].toBool());
        QCOMPARE(proposals.size(), count);
    }
    // Discard…, the close button and drive's discard ask before the recording
    // and its edits go; the safe answer is the default and Escape.
    void discardAsksBeforeTheRecordingGoes() {
        ReviewWindow window(options(), config());
        QSignalSpy cancelled(&window, &ReviewWindow::cancelRequested);
        window.show();
        QVERIFY(putgd::test::waitForWindowExposed(&window));
        auto* discard = window.findChild<QPushButton*>("discard");
        QVERIFY(discard && discard->text() == QString::fromUtf8("Discard…"));
        discard->click();
        QMessageBox* box = nullptr;
        PUTGD_TRY_VERIFY((box = window.findChild<QMessageBox*>("confirmAction")) && box->isVisible());
        QCOMPARE(cancelled.size(), 0);
        QVERIFY(box->text().startsWith(QStringLiteral("Discard this recording and its edits?")));
        QCOMPARE(box->findChild<QPushButton*>("confirmDiscard")->text(), QString("Discard recording"));
        QCOMPARE(box->defaultButton()->text(), QString("Keep reviewing"));
        QCOMPARE(box->escapeButton(), static_cast<QAbstractButton*>(box->findChild<QPushButton*>("keepReviewing")));
        QVERIFY(box->findChild<QPushButton*>("keepReviewing")->x() < box->findChild<QPushButton*>("confirmDiscard")->x());
        QVERIFY(window.inspect()["discardConfirmation"].toObject()["open"].toBool());
        // While it is open, other controls wait.
        QCOMPARE(window.drive({{"action", "setIn"}}).code, QString("recording_state"));
        QTest::keyClick(box, Qt::Key_Escape);
        PUTGD_TRY_VERIFY(!window.inspect()["discardConfirmation"].toObject()["open"].toBool());
        QCOMPARE(cancelled.size(), 0);
        QVERIFY(window.isVisible());
        // The window's close button asks the same question and stays open.
        window.close();
        QVERIFY(window.isVisible());
        QVERIFY(window.inspect()["discardConfirmation"].toObject()["open"].toBool());
        QVERIFY(window.drive({{"action", "keepReviewing"}}).code.isEmpty());
        QCOMPARE(cancelled.size(), 0);
        QCOMPARE(window.drive({{"action", "confirmDiscard"}}).code, QString("recording_state"));
        // Through drive: ask, then discard; one cancel.
        QVERIFY(window.drive({{"action", "discard"}}).state["discardConfirmation"].toObject()["open"].toBool());
        QVERIFY(window.drive({{"action", "confirmDiscard"}}).code.isEmpty());
        QCOMPARE(cancelled.size(), 1);
        // Once discarded, closing asks nothing more.
        window.close();
        QVERIFY(!window.isVisible());
        QCOMPARE(cancelled.size(), 1);
    }
    // A frame that could not be decoded says so over the frame until a refresh
    // succeeds; an earlier frame left on screen is marked stale.
    void previewFailureShowsOverTheFrame() {
        ReviewWindow window(options(), config());
        window.show();
        QVERIFY(putgd::test::waitForWindowExposed(&window));
        // The fixture's clip does not exist: the first refresh fails.
        PUTGD_TRY_VERIFY(window.inspect()["preview"].toObject()["error"].isString());
        auto preview = window.inspect()["preview"].toObject();
        QCOMPARE(preview["error"].toString(), QString("The frame at 0:00.0 couldn't be decoded (the private recording file is missing)."));
        QVERIFY(!preview["stale"].toBool());
        auto* card = window.findChild<QWidget*>("previewError");
        auto* retry = window.findChild<QPushButton*>("retryPreview");
        QVERIFY(card && card->isVisible() && retry && retry->isVisible());
        auto* frame = window.findChild<FrameView*>();
        QVERIFY(frame->rect().contains(card->geometry()));
        QVERIFY(frame->accessibleDescription().startsWith(QStringLiteral("The frame at")));
        // A decoded frame clears it; the next failure marks that frame as stale.
        QImage image(1280, 720, QImage::Format_RGB32);
        image.fill(Qt::darkCyan);
        frame->setFrame(image, 0);
        QVERIFY(!card->isVisible());
        QVERIFY(window.inspect()["preview"].toObject()["error"].isNull());
        QVERIFY(window.drive({{"action", "seek"}, {"time", 4000000}}).code.isEmpty());
        PUTGD_TRY_VERIFY(window.inspect()["preview"].toObject()["stale"].toBool());
        preview = window.inspect()["preview"].toObject();
        QCOMPARE(preview["error"].toString(), QString("The frame at 0:04.0 couldn't be decoded (the private recording file is missing)."));
        QCOMPARE(preview["frameTimeUs"].toDouble(), 0.0);
        QVERIFY(window.findChild<QLabel*>("previewErrorText")->text().contains(QStringLiteral("earlier frame (0:00.0)")));
        // Retry tries again at once and, failing, keeps the error.
        retry->click();
        QVERIFY(window.inspect()["preview"].toObject()["stale"].toBool());
        QVERIFY(window.drive({{"action", "retryPreview"}}).state["preview"].toObject()["error"].isString());
    }
    // Errors and warnings get their own wrapping row with actions, only when
    // needed; the audio summary beside Output stays short and whole.
    void statusRowAndAudioSummary() {
        auto o = options();
        o.audioRecorded = true;
        auto c = config(); c.audio.system = true;
        ReviewWindow window(o, c);
        QSignalSpy proposals(&window, &ReviewWindow::editProposed);
        QSignalSpy configs(&window, &ReviewWindow::configChanged);
        window.show();
        QVERIFY(putgd::test::waitForWindowExposed(&window));
        auto* row = window.findChild<QWidget*>("statusRow");
        auto* audio = window.findChild<QLabel*>("audioSummary");
        QVERIFY(row && audio);
        QVERIFY(!row->isVisible());
        QCOMPARE(window.inspect()["statusRow"].toArray().size(), 0);
        QCOMPARE(audio->text(), QString("Silent preview"));
        QVERIFY(audio->accessibleName().contains("System audio included"));
        QCOMPARE(window.findChild<QLabel*>("exportAudioSummary")->accessibleName(), QString("System audio included · Silent preview"));
        QVERIFY(window.findChild<QLabel*>("exportAudioSummary")->isVisible());
        // H.265: a warning with its fix; the fix is the format control's own path.
        QVERIFY(window.drive({{"action", "format"}, {"format", "mp4"}, {"codec", "h265"}}).code.isEmpty());
        auto lines = window.inspect()["statusRow"].toArray();
        if (lines.isEmpty()) {
            QVERIFY(window.findChild<QToolButton*>("formatAdvice")->isVisible());
            QVERIFY(window.drive({{"action", "showFormatTips"}}).state["adviceDialog"].toObject()["open"].toBool());
        } else {
            QCOMPARE(lines.size(), 1);
            QCOMPARE(lines[0].toObject()["tone"].toString(), QString("warning"));
            QVERIFY(row->isVisible());
        }
        auto* fix = buttonNamed(window, "Use MP4 (H.264)");
        QVERIFY(fix);
        fix->click();
        QCOMPARE(window.inspect()["format"].toObject()["codec"].toString(), QString("h264"));
        PUTGD_TRY_VERIFY(!row->isVisible());
        // A long export error wraps in full at the minimum width and stays until dismissed.
        window.resize(window.minimumSizeHint());
        const QString failure = QStringLiteral("Export failed: the encoder stopped after 412 frames because the disk holding the private recording is full; free some space and try again");
        window.showError(failure);
        QApplication::processEvents();
        QVERIFY(window.inspect()["errorDetails"].toObject()["visible"].toBool());
        QCOMPARE(window.inspect()["errorDetails"].toObject()["message"].toString(), failure);
        auto details=window.drive({{"action", "showAdvice"}});
        QVERIFY(details.state["adviceDialog"].toObject()["text"].toString().contains(failure));
        window.drive({{"action", "showAdvice"}});
        QVERIFY(window.drive({{"action", "seek"}, {"time", 3000000}}).code.isEmpty());
        QCOMPARE(window.inspect()["errorDetails"].toObject()["message"].toString(), failure);
        QVERIFY(window.drive({{"action", "dismissStatus"}}).state["statusRow"].toArray().isEmpty());
        window.resize(ReviewWindow::defaultSize());
        // GIF: silent, and over its length limit; the trim it needs is one click.
        window.setLimits(5000000, 4096);
        QVERIFY(window.drive({{"action", "format"}, {"format", "gif"}, {"codec", ""}}).code.isEmpty());
        QCOMPARE(audio->text(), QString("Silent preview"));
        QVERIFY(audio->accessibleName().contains("No audio in GIF"));
        QCOMPARE(window.findChild<QLabel*>("exportAudioSummary")->accessibleName(), QString("No audio in GIF · Silent preview"));
        lines = window.inspect()["statusRow"].toArray();
        QCOMPARE(lines.size(), 1);
        QVERIFY(lines[0].toObject()["text"].toString().startsWith(QStringLiteral("GIF is limited to 5 s")));
        auto* keep = buttonNamed(window, "Keep the first 5 s");
        QVERIFY(keep);
        keep->click();
        QCOMPARE(proposed(proposals), QJsonObject({{"op", "setTrim"}, {"start", 0.0}, {"end", 5000000.0}}));
        Q_UNUSED(configs);
    }
    void exportRecoveryUsesTheSameRetryAction() {
        ReviewWindow window(options(), config()); window.show();
        QVERIFY(putgd::test::waitForWindowExposed(&window));
        QSignalSpy retry(&window, &ReviewWindow::retryExportRequested);
        QVERIFY(!window.drive({{"action", "retryExport"}}).code.isEmpty());
        window.showError("Export failed: disk full");
        QApplication::processEvents();
        auto* again = buttonNamed(window, "Try again");
        auto* dismiss = buttonNamed(window, "Dismiss");
        QVERIFY(again && dismiss && again->isVisible());
        QVERIFY(again->mapTo(&window, QPoint()).x() < dismiss->mapTo(&window, QPoint()).x());
        again->click(); QCOMPARE(retry.size(), 1);
        window.drive({{"action", "format"}, {"format", "mp4"}, {"codec", "h265"}});
        window.drive({{"action", "showAdvice"}});
        auto* dialog = window.findChild<QDialog*>("reviewAdvice");
        QVERIFY(dialog); QCOMPARE(dialog->windowTitle(), QString("Export failed"));
        QVERIFY(!buttonNamed(*dialog, "Use MP4 (H.264)"));
        QCOMPARE(dialog->findChild<QLabel*>("reviewAdviceText")->property("plainText").toString(), QString("Export failed: disk full"));
        // Final read: the heading says Export failed, so the text doesn't repeat it, and it has no caret.
        auto* adviceText = dialog->findChild<QLabel*>("reviewAdviceText");
        QCOMPARE(adviceText->text(), QString("Disk full"));
        QVERIFY(adviceText->textInteractionFlags() & Qt::TextSelectableByKeyboard);
        bool caretHidden = false;
        for (auto* control : adviceText->findChildren<QObject*>(QString(), Qt::FindDirectChildrenOnly))
            if (QByteArray(control->metaObject()->className()) == "QWidgetTextControl") caretHidden = control->property("cursorWidth").toInt() == 0;
        QVERIFY(caretHidden);
        auto* primary = qobject_cast<QPushButton*>(buttonNamed(*dialog, "Try again"));
        QVERIFY(primary && primary->isDefault());
        primary->click(); QCOMPARE(retry.size(), 2);
        QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        window.drive({{"action", "showAdvice"}});
        dialog = window.findChild<QDialog*>("reviewAdvice"); QVERIFY(dialog);
        const QJsonValue errorFacts = window.inspect()["statusFacts"];
        auto* close = buttonNamed(*dialog, "Close"); QVERIFY(close); close->click();
        QCOMPARE(window.inspect()["statusFacts"], errorFacts);
        QVERIFY(!window.inspect()["adviceDialog"].toObject()["open"].toBool());
        QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        const auto tips = window.drive({{"action", "showFormatTips"}});
        QVERIFY(tips.code.isEmpty());
        QVERIFY(!tips.state["adviceDialog"].toObject()["text"].toString().contains("disk full"));
        dialog = window.findChild<QDialog*>("reviewAdvice"); QVERIFY(dialog);
        QVERIFY(buttonNamed(*dialog, "Use MP4 (H.264)"));
        buttonNamed(*dialog, "Close")->click();
        QCOMPARE(window.inspect()["statusFacts"], errorFacts);
        window.drive({{"action", "dismissStatus"}});
        QVERIFY(window.inspect()["statusFacts"] != errorFacts);
        window.showError("Couldn't open the folder. Copy the path and open it in your file manager.", false);
        QVERIFY(!buttonNamed(window, "Try again"));
        QVERIFY(!window.drive({{"action", "retryExport"}}).code.isEmpty());
    }

    void replacingVisibleStatusShowsTextAndRecoveryActions() {
        ReviewWindow window(options(), config());
        window.show();
        QVERIFY(putgd::test::waitForWindowExposed(&window));
        window.showError("First export failed");
        auto* row = window.findChild<QWidget*>("statusRow");
        QVERIFY(row && row->isVisible());
        // Replace a row that is already visible, then add a second line.
        const QString failure = QStringLiteral("Export failed: free disk space and try again");
        window.showError(failure);
        QVERIFY(window.drive({{"action", "format"}, {"format", "mp4"}, {"codec", "h265"}}).code.isEmpty());
        auto* tips = window.findChild<QToolButton*>("formatAdvice");
        QVERIFY(tips && tips->isVisible());
        QVERIFY(window.inspect()["formatAdvice"].toObject()["visible"].toBool());
        tips->click();
        QApplication::processEvents();
        int lines = 0;
        for (auto* line : row->findChildren<QWidget*>("statusLine", Qt::FindDirectChildrenOnly)) {
            if (line->isHidden()) continue;
            ++lines;
            QVERIFY(line->isVisible() && !line->visibleRegion().isEmpty());
            auto* text = line->findChild<QLabel*>("status");
            QVERIFY(text && text->isVisible() && !text->visibleRegion().isEmpty());
            QVERIFY(text->height() >= text->fontMetrics().height());
            QVERIFY(row->rect().contains(QRect(text->mapTo(row, QPoint()), text->size())));
            for (auto* button : line->findChildren<QPushButton*>("statusAction")) {
                QVERIFY(button->isVisible() && !button->visibleRegion().isEmpty());
                QVERIFY(button->height() >= 32 && button->width() >= 32);
                QVERIFY(row->rect().contains(QRect(button->mapTo(row, QPoint()), button->size())));
            }
        }
        QCOMPARE(lines, 1);
        QVERIFY(window.inspect()["adviceDialog"].toObject()["open"].toBool());
        auto* advice = window.findChild<QWidget*>("reviewAdviceText");
        QVERIFY(advice && advice->property("plainText").toString().contains("MP4 (H.264)"));
        auto* dismiss = buttonNamed(window, "Dismiss");
        auto* fix = buttonNamed(window, "Use MP4 (H.264)");
        QVERIFY(dismiss && fix);
        fix->click();
        QVERIFY(!window.inspect()["adviceDialog"].toObject()["open"].toBool());
        dismiss = buttonNamed(window, "Dismiss"); QVERIFY(dismiss); dismiss->click();
        QVERIFY(!window.inspect()["status"].toString().contains(failure));
        QApplication::processEvents();
        QVERIFY(window.inspect()["statusRow"].toArray().isEmpty());
        QVERIFY(!row->isVisible());
    }
    // Blur is a visual effect: the editor's guidance shows, readable without
    // hover, once Add blur is invoked, with a switch to Redaction.
    void blurGuidanceAndRedactionNames() {
        ReviewWindow window(options(), config());
        QSignalSpy proposals(&window, &ReviewWindow::editProposed);
        window.setEdit(twoMasks()); window.show();
        QVERIFY(putgd::test::waitForWindowExposed(&window));
        QVERIFY(!window.inspect()["blurGuidance"].toObject()["visible"].toBool());
        QVERIFY(window.drive({{"action", "addMask"}, {"kind", "blur"}}).code.isEmpty());
        QVERIFY(window.inspect()["blurGuidance"].toObject()["visible"].toBool());
        auto* list = window.findChild<QComboBox*>("masks");
        QCOMPARE(list->itemText(1), QString("Redaction 2"));
        QVERIFY(window.drive({{"action", "selectMask"}, {"maskId", "m-blur"}}).code.isEmpty());
        const auto frame = window.findChild<FrameView*>()->geometry();
        QVERIFY(window.drive({{"action", "showMaskOptions"}}).code.isEmpty());
        auto* fix = buttonNamed(window, "Use Redact");
        QVERIFY(fix);
        fix->click();
        QCOMPARE(proposed(proposals)["maskId"].toString(), QString("m-blur"));
        QCOMPARE(proposed(proposals)["mask"].toObject()["kind"].toString(), QString("solid"));
        QVERIFY(!proposed(proposals)["mask"].toObject().contains("strength"));
        QCOMPARE(window.findChild<FrameView*>()->geometry(), frame);
        window.drive({{"action", "dismissBlurTip"}});
        window.drive({{"action", "selectMask"}, {"maskId", QJsonValue(QJsonValue::Null)}});
        auto* add = window.findChild<QToolButton*>("addMask");
        add->menu()->actions()[1]->trigger();
        QVERIFY(!window.inspect()["blurGuidance"].toObject()["visible"].toBool());
        QVERIFY(window.drive({{"action", "useSolidCover"}}).state["armed"].toString() == "solid");
    }
    // Keyboard-only masks: Enter places an armed mask centred at half the
    // frame's size; with the frame focused, arrows move the selected mask,
    // Shift+arrows resize it, Ctrl makes the step 10 px (as in the editor); one edit per burst.
    void masksByKeyboard() {
        ReviewWindow window(options(), config());
        QSignalSpy proposals(&window, &ReviewWindow::editProposed);
        window.show();
        QVERIFY(putgd::test::waitForWindowExposed(&window));
        window.activateWindow();
        QVERIFY(putgd::test::waitForWindowActive(&window));
        auto* frame = window.findChild<FrameView*>();
        QVERIFY(frame && frame->focusPolicy() == Qt::StrongFocus);
        auto* add = window.findChild<QToolButton*>("addMask");
        QVERIFY(add && add->menu());
        add->setFocus(Qt::TabFocusReason);
        // The actual menu is keyboard navigable, then Enter on the frame places it.
        QTimer::singleShot(0, add, [add] {
            add->menu()->setActiveAction(add->menu()->actions().first());
            QTest::keyClick(add->menu(), Qt::Key_Return);
        });
        add->showMenu();
        QCOMPARE(window.inspect()["armed"].toString(), QString("solid"));
        PUTGD_TRY_VERIFY(!add->menu()->isVisible());
        window.activateWindow();
        QVERIFY(putgd::test::waitForWindowActive(&window));
        frame->setFocus(Qt::TabFocusReason);
        QApplication::processEvents();
        QTest::keyClick(frame, Qt::Key_Return);
        QVERIFY(!proposals.isEmpty());
        auto added = proposed(proposals);
        QCOMPARE(added["op"].toString(), QString("addMask"));
        QCOMPARE(added["mask"].toObject()["rect"].toArray(), QJsonArray({320, 180, 640, 360}));
        QVERIFY(window.inspect()["armed"].isNull());
        QVERIFY(frame->hasFocus());
        // The resident's answer selects it; arrows nudge it as one proposal.
        RecordingEdit edit;
        edit.revision = 2;
        RecordingMask m = mask("m-key", "solid", 0, duration);
        m.rect = QRect(320, 180, 640, 360);
        edit.masks = {m};
        window.setEdit(edit);
        QVERIFY(frame->accessibleDescription().contains(QStringLiteral("Redaction 1 selected: 640 × 360 at 320, 180")));
        const auto before = proposals.size();
        for (int i = 0; i < 3; ++i) QTest::keyClick(frame, Qt::Key_Right);
        QTest::keyClick(frame, Qt::Key_Up, Qt::ControlModifier);
        QCOMPARE(proposals.size(), before);
        PUTGD_TRY_VERIFY(proposals.size() == before + 1);
        QCOMPARE(proposed(proposals)["mask"].toObject()["rect"].toArray(), QJsonArray({323, 170, 640, 360}));
        // Shift+arrows resize from the bottom-right, inside the canvas.
        QTest::keyClick(frame, Qt::Key_Down, Qt::ShiftModifier);
        QTest::keyClick(frame, Qt::Key_Left, Qt::ShiftModifier);
        frame->commitNudge();
        QCOMPARE(proposed(proposals)["mask"].toObject()["rect"].toArray(), QJsonArray({323, 170, 639, 361}));
        // Escape clears the selection; arrows then do nothing.
        QTest::keyClick(frame, Qt::Key_Escape);
        QVERIFY(window.inspect()["selectedMaskId"].isNull());
        const auto after = proposals.size();
        QTest::keyClick(frame, Qt::Key_Right);
        frame->commitNudge();
        QCOMPARE(proposals.size(), after);
        // Agents: addMask without a rect is the centred mask; placeMask sets the selected one's.
        QVERIFY(window.drive({{"action", "addMask"}, {"kind", "blur"}}).code.isEmpty());
        QCOMPARE(proposed(proposals)["mask"].toObject()["rect"].toArray(), QJsonArray({320, 180, 640, 360}));
        QCOMPARE(window.drive({{"action", "placeMask"}, {"rect", QJsonArray{0, 0, 10, 10}}}).code, QString("invalid_request"));
        QVERIFY(window.drive({{"action", "selectMask"}, {"maskId", "m-key"}}).code.isEmpty());
        QVERIFY(window.drive({{"action", "placeMask"}, {"rect", QJsonArray{8, 8, 100, 50}}}).code.isEmpty());
        QCOMPARE(proposed(proposals)["mask"].toObject()["rect"].toArray(), QJsonArray({8, 8, 100, 50}));
        QCOMPARE(window.drive({{"action", "placeMask"}, {"rect", QJsonArray{1200, 8, 100, 50}}}).code, QString("invalid_request"));
    }
    // Review timecodes retain one-decimal Mono figures across theme changes.
    void timecodesUseOneMonoFormatAndContextIsNamed() {
        ReviewWindow window(options(), config());
        window.show();
        QVERIFY(putgd::test::waitForWindowExposed(&window));
        for (const auto* name : {"time"}) {
            QLabel* label = nullptr;
            for (auto* l : window.findChildren<QLabel*>(name)) if (l->accessibleName() == "Playhead time" || l->accessibleName() == "Kept interval") label = l;
            QVERIFY2(label, name);
            QCOMPARE(QFontInfo(label->font()).family(), QFontInfo(Ui::monoFont(Ui::Type::technical)).family());
            const auto scheme = Theme::scheme();
            Theme::setScheme(scheme == Theme::Scheme::Dark ? Theme::Scheme::Light : Theme::Scheme::Dark);
            QApplication::processEvents();
            QCOMPARE(QFontInfo(label->font()).family(), QFontInfo(Ui::monoFont(Ui::Type::technical)).family());
            QCOMPARE(label->font().pixelSize(), Ui::monoFont(Ui::Type::technical).pixelSize());
            Theme::setScheme(scheme);
            QVERIFY(label->width() >= label->sizeHint().width());
        }
        auto* trim = window.findChild<QLabel*>("trimGroup");
        QVERIFY(trim && trim->isVisible());
        auto* mask = window.findChild<QLabel*>("maskGroup");
        QVERIFY(mask && !mask->isVisible());
        QCOMPARE(mask->text(), QString("Mask"));

    }
    // #259: Save copy… opens the shared chooser; this test never asks a portal.
    void saveCopyDialogIsDrivenAndSavesTheChosenPath() {
        const auto previous = qgetenv("PUTGD_FILE_DIALOG");
        const auto restore = qScopeGuard([&] { if (previous.isNull()) qunsetenv("PUTGD_FILE_DIALOG"); else qputenv("PUTGD_FILE_DIALOG", previous); });
        qputenv("PUTGD_FILE_DIALOG", "fallback");
        QTemporaryDir folder;
        QVERIFY(folder.isValid());
        ReviewWindow window(options(), config());
        window.show();
        QVERIFY(putgd::test::waitForWindowExposed(&window));
        QSignalSpy saved(&window, &ReviewWindow::saveCopyRequested);
        auto opened = window.drive({{"action", "saveCopyDialog"}});
        QVERIFY2(opened.code.isEmpty(), qPrintable(opened.message));
        auto dialog = opened.state["fileDialog"].toObject();
        QVERIFY(dialog["open"].toBool());
        QCOMPARE(dialog["backend"].toString(), QString("fallback"));
        QCOMPARE(dialog["fallbackReason"].toString(), QString("disabled"));
        QCOMPARE(dialog["mode"].toString(), QString("save"));
        // Other controls wait for the dialog, as they did behind QFileDialog::exec.
        QCOMPARE(window.drive({{"action", "setIn"}}).code, QString("recording_state"));
        QCOMPARE(window.drive({{"action", "saveCopyDialog"}}).code, QString("recording_state"));
        auto closed = window.drive({{"action", "closeFileDialog"}});
        QVERIFY(!closed.state["fileDialog"].toObject()["open"].toBool());
        QCOMPARE(closed.state["fileDialog"].toObject()["last"].toObject()["status"].toString(), QString("cancelled"));
        QCOMPARE(saved.size(), 0);
        // The button opens the same dialog; a name without the extension gets it.
        QPushButton* save = nullptr;
        for (auto* button : window.findChildren<QPushButton*>()) if (button->text() == QString::fromUtf8("Save a copy…")) save = button;
        QVERIFY(save);
        save->click();
        QFileDialog* fallback = nullptr;
        PUTGD_TRY_VERIFY((fallback = window.findChild<QFileDialog*>("putgdFileDialog")) && fallback->isVisible());
        QCOMPARE(fallback->windowTitle(), QString("Save a copy"));
        QCOMPARE(fallback->nameFilters(), QStringList{"MP4 (*.mp4)"});
        fallback->setDirectory(folder.path());
        auto* name = fallback->findChild<QLineEdit*>("fileNameEdit");
        QVERIFY(name);
        name->setText("chosen");
        QMetaObject::invokeMethod(fallback, "accept");
        QCOMPARE(saved.size(), 1);
        QCOMPARE(saved.first().first().toString(), folder.filePath("chosen.mp4"));
        // Starting an export closes an open chooser.
        QVERIFY(window.drive({{"action", "saveCopyDialog"}}).code.isEmpty());
        window.setExporting(true, "Exporting MP4…");
        QVERIFY(!window.inspect()["fileDialog"].toObject()["open"].toBool());
    }
};

QTEST_MAIN(ReviewWindowTest)
#include "test_review_window_actions.moc"
