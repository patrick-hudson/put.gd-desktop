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
                const auto problem = putgd::test::reviewFooterProblem(window);
                QVERIFY2(problem.isEmpty(), qPrintable(QString("frame %1: %2").arg(frame).arg(problem)));
                auto* actions = window.findChild<QWidget*>("reviewActions");
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
                    const int gap = std::max(a.left(), b.left()) - std::min(a.right(), b.right()) - 1;
                    QVERIFY2(gap >= int(Ui::Space::s), qPrintable(QString("%1/%2 gap %3 at %4").arg(groups[i]->objectName(), groups[j]->objectName()).arg(gap).arg(width)));
                }
                if (actions->layout()->property("singleRow").toBool() && actions->isAncestorOf(destination)) {
                    QVERIFY(bounds(destination).right() < bounds(metadata).left());
                    QVERIFY(bounds(format).left() - bounds(metadata).right() - 1 >= int(Ui::Space::xl));
                }

            }
        }
    }

};

QTEST_MAIN(ReviewWindowTest)
#include "test_review_window_footer.moc"
