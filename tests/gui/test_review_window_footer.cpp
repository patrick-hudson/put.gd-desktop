#include "ReviewWindowFixture.h"

class ReviewWindowTest : public ReviewWindowFixture {
    Q_OBJECT
private slots:
    void footerChildrenNeverOverlapAcrossWidths_data() {
        QTest::addColumn<int>("pixels");
        QTest::newRow("standard") << 14;
        QTest::newRow("150%") << 21;
    }
    void footerChildrenNeverOverlapAcrossWidths() {
        QFETCH(int, pixels);
        auto font = qApp->font(); font.setPixelSize(pixels); qApp->setFont(font);
        for (const int frame : {1, -1}) {
            Ui::WindowHeader::setCustomFrameOverride(frame);
            auto o = options(); o.audioRecorded = true; o.canvas = QSize(960, 600); o.formats = QStringList{"mp4", "webm", "gif", "webp"};
            auto c = config(); c.audio.system = true;
            ReviewWindow window(o, c); window.show();
            QVERIFY(putgd::test::waitForWindowExposed(&window));
            RecordingEdit edit; edit.revision = 1; edit.masks = {mask("solid", "solid", 8000000, 21000000), mask("blur", "blur", 3000000, 12000000)};
            window.setEdit(edit);
            window.drive({{"action", "selectMask"}, {"maskId", "blur"}});
            // Both directions: the one-row/two-row switch has hysteresis.
            QList<int> widths;
            for (int width = 640; width <= 1440; width += 6) widths.append(width);
            for (int width = 900; width <= 1440; width += 10) widths.append(width);
            std::sort(widths.begin(), widths.end());
            widths.erase(std::unique(widths.begin(), widths.end()), widths.end());
            auto descending = widths; std::reverse(descending.begin(), descending.end());
            widths.append(descending);
            widths.append(1060);
            for (const int width : widths) {
                window.resize(width, 720);
                for (int i = 0; i < 3; ++i) QApplication::processEvents();
                auto* actions = window.findChild<QWidget*>("reviewActions");
                QList<QWidget*> leaves;
                for (auto* widget : actions->findChildren<QWidget*>()) {
                    if (widget->isWindow() || !widget->isVisibleTo(&window)) continue;
                    const auto children = widget->findChildren<QWidget*>(QString(), Qt::FindDirectChildrenOnly);
                    if (std::none_of(children.cbegin(), children.cend(), [&](QWidget* child) { return !child->isWindow() && child->isVisibleTo(&window); })) leaves.append(widget);
                }
                for (int i = 0; i < leaves.size(); ++i) for (int j = i + 1; j < leaves.size(); ++j) {
                    const QRect a(leaves[i]->mapTo(&window, QPoint()), leaves[i]->size()), b(leaves[j]->mapTo(&window, QPoint()), leaves[j]->size());
                    QVERIFY2(!a.intersects(b), qPrintable(QString("%1/%2 overlap at %3").arg(leaves[i]->objectName(), leaves[j]->objectName()).arg(width)));
                }
                auto* destination = window.findChild<QWidget*>("reviewDestinationSummary");
                auto* metadata = window.findChild<QWidget*>("reviewMetadata");
                QComboBox* format = nullptr;
                for (auto* combo : window.findChildren<QComboBox*>()) if (combo->accessibleName() == "Output format") format = combo;
                QVERIFY(format);
                const auto bounds = [&](QWidget* widget) { return QRect(widget->mapTo(&window, QPoint()), widget->size()); };
                QList<QWidget*> groups{destination, metadata, format, buttonNamed(window, "Save a copy…"), window.findChild<QPushButton*>("discard"), buttonNamed(window, "Upload")};
                for (int i = 0; i < groups.size(); ++i) for (int j = i + 1; j < groups.size(); ++j) {
                    if (!actions->isAncestorOf(groups[i]) || !actions->isAncestorOf(groups[j]) || !groups[i]->isVisibleTo(&window) || !groups[j]->isVisibleTo(&window)) continue;
                    const auto a = bounds(groups[i]), b = bounds(groups[j]);
                    if (a.top() > b.bottom() || b.top() > a.bottom()) continue;
                    QVERIFY2(!a.intersects(b), qPrintable(QString("%1/%2 overlap at %3").arg(groups[i]->objectName(), groups[j]->objectName()).arg(width)));
                }

            }
        }
    }

};

QTEST_MAIN(ReviewWindowTest)
#include "test_review_window_footer.moc"
