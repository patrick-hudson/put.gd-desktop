#include "../fixtures/Waits.h"
#include "ui/Ui.h"
#include "ui/WindowButtons.h"
#include <QApplication>
#include <QFile>
#include <QLabel>
#include <QMenu>
#include <QSaveFile>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QToolButton>
#include <QVBoxLayout>
#include <QtTest>

using namespace putgd;
class WindowButtonsTest : public QObject {
    Q_OBJECT
    static QByteArray fixture(const char* name) {
        const auto path = QFINDTESTDATA(qPrintable(QStringLiteral("../fixtures/window-buttons/") + name));
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) return {};
        return file.readAll();
    }
private slots:
    void initTestCase() { Ui::apply(*qApp); }
    void fixtureLayouts() {
        const auto left = WindowButtons::fromGSettings(QString::fromUtf8(fixture("gnome-left.txt")));
        QCOMPARE(left.left, (QStringList{"close", "minimize", "maximize"}));
        QVERIFY(left.right.isEmpty());
        const auto right = WindowButtons::fromGSettings(QString::fromUtf8(fixture("cinnamon-right.txt")));
        QVERIFY(right.left.isEmpty());
        QCOMPARE(right.right, (QStringList{"minimize", "maximize", "close"}));
        QCOMPARE(WindowButtons::fromGSettings("invalid").right, right.right);
        QCOMPARE(WindowButtons::fromGSettings("'menu:minimize,maximize,close,close'").right, right.right);
        QCOMPARE(WindowButtons::fromGSettings("':close'").right, QStringList{"close"});
        QVERIFY(WindowButtons::fromGSettings("':'").right.isEmpty());
        const auto splitPath = QFINDTESTDATA("../fixtures/window-buttons/kwin-split.conf");
        const auto split = WindowButtons::fromKWin(splitPath);
        QCOMPARE(split.left, QStringList{"close"});
        QCOMPARE(split.right, (QStringList{"maximize", "minimize"}));
        const auto leftPath = QFINDTESTDATA("../fixtures/window-buttons/kwin-left.conf");
        QCOMPARE(WindowButtons::fromKWin(leftPath).left, left.left);
        QVERIFY(WindowButtons::fromKWin(leftPath).right.isEmpty());
        QTemporaryDir directory;
        QCOMPARE(WindowButtons::fromKWin(directory.filePath("missing")).right, right.right);
        WindowButtons unknown("unknown", directory.path());
        QCOMPARE(unknown.layout().right, right.right);
    }
    void headersFollowSidesOrderAndOmittedButtons() {
        Ui::WindowHeader::setCustomFrameOverride(1);
        auto restore = qScopeGuard([] { Ui::WindowHeader::setCustomFrameOverride(0); });
        for (const auto density : {Ui::WindowHeader::Density::Standard, Ui::WindowHeader::Density::Compact}) {
            QWidget window;
            QVBoxLayout row(&window);
            QMenu menu;
            auto* header = Ui::WindowHeader::install(&window, &row, &menu, density);
            header->setDocument("Review recording");
            auto* status = new QLabel("Not uploaded");
            header->addTrailing(status);
            window.resize(1200, 200);
            window.show();
            auto x = [header](QWidget* widget) { return widget->mapTo(header, QPoint()).x(); };
            auto* close = header->findChild<QToolButton*>("windowClose");
            auto* minimize = header->findChild<QToolButton*>("windowMinimize");
            auto* maximize = header->findChild<QToolButton*>("windowMaximize");
            auto* brand = header->findChild<QLabel*>("brandName");
            header->setButtonLayout(WindowButtons::fromGSettings("close,minimize,maximize:"));
            QCoreApplication::processEvents();
            QVERIFY(x(close) < x(minimize)); QVERIFY(x(minimize) < x(maximize)); QVERIFY(x(maximize) < x(brand));
            QCOMPARE(x(header->findChild<QToolButton*>("applicationMenu")) - x(maximize) - maximize->width(), Ui::space(Ui::Space::l));
            QCOMPARE(close->nextInFocusChain(), minimize);
            QCOMPARE(minimize->nextInFocusChain(), maximize);
            header->setButtonLayout(WindowButtons::fromGSettings(":maximize,minimize,close"));
            QCoreApplication::processEvents();
            QVERIFY(x(brand) < x(maximize)); QVERIFY(x(status) < x(maximize));
            QVERIFY(x(maximize) < x(minimize)); QVERIFY(x(minimize) < x(close));
            header->setButtonLayout(WindowButtons::fromGSettings(":close"));
            QCoreApplication::processEvents();
            QVERIFY(minimize->isHidden()); QVERIFY(maximize->isHidden()); QVERIFY(close->isVisible());
            QTest::mouseClick(close, Qt::LeftButton);
            QVERIFY(!window.isVisible());
        }
    }
    void gsettingsReadsAsynchronouslyAndMonitorsTheDesktopSchema_data() {
        QTest::addColumn<QString>("desktop"); QTest::addColumn<QString>("schema"); QTest::addColumn<bool>("monitor");
        QTest::newRow("gnome-query") << "GNOME" << "org.gnome.desktop.wm.preferences" << false;
        QTest::newRow("cinnamon-monitor") << "X-Cinnamon" << "org.cinnamon.desktop.wm.preferences" << true;
        QTest::newRow("mate-monitor") << "MATE" << "org.mate.Marco.general" << true;
    }
    void gsettingsReadsAsynchronouslyAndMonitorsTheDesktopSchema() {
#ifndef Q_OS_LINUX
        QSKIP("Linux desktop settings");
#else
        QFETCH(QString, desktop); QFETCH(QString, schema); QFETCH(bool, monitor);
        QTemporaryDir directory;
        QFile script(directory.filePath("gsettings"));
        QVERIFY(script.open(QIODevice::WriteOnly));
        script.write("#!/bin/sh\n[ \"$2\" = \"" + schema.toUtf8() + "\" ] || exit 1\n"
                     "if [ \"$1\" = get ]; then printf \"'close,minimize,maximize:'\\n\"; "
                     "else " + QByteArray(monitor ? "printf \"button-layout: ':close'\\n\"" : ":") + "; fi\n");
        script.close();
        QVERIFY(script.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        const auto path = qgetenv("PATH");
        auto restore = qScopeGuard([&] { qputenv("PATH", path); });
        qputenv("PATH", directory.path().toUtf8() + ':' + path);
        WindowButtons buttons(desktop, directory.path());
        // Construction returns with the cached fallback before any child runs.
        QVERIFY(buttons.layout().left.isEmpty());
        QSignalSpy changed(&buttons, &WindowButtons::changed);
        if (monitor) {
            PUTGD_TRY_COMPARE(buttons.layout().right, QStringList{"close"});
            QVERIFY(buttons.layout().left.isEmpty());
        } else {
            PUTGD_TRY_COMPARE(buttons.layout().left, (QStringList{"close", "minimize", "maximize"}));
            QVERIFY(buttons.layout().right.isEmpty());
        }
        QVERIFY(!changed.isEmpty());
#endif
    }
    void kwinAtomicChangesRefreshTheCache() {
#ifndef Q_OS_LINUX
        QSKIP("Linux desktop settings");
#else
        QTemporaryDir directory;
        auto write = [&](const QByteArray& contents) {
            QSaveFile file(directory.filePath("kwinrc"));
            if (!file.open(QIODevice::WriteOnly)) return false;
            return file.write(contents) == contents.size() && file.commit();
        };
        QVERIFY(write(fixture("kwin-left.conf")));
        WindowButtons buttons("KDE", directory.path());
        QCOMPARE(buttons.layout().left, (QStringList{"close", "minimize", "maximize"}));
        QSignalSpy changed(&buttons, &WindowButtons::changed);
        QVERIFY(write(fixture("kwin-split.conf")));
        PUTGD_TRY_COMPARE(buttons.layout().right, (QStringList{"maximize", "minimize"}));
        QVERIFY(!changed.isEmpty());
        changed.clear();
        QVERIFY(write(fixture("kwin-split.conf")));
        PUTGD_VERIFY_STEADY(changed.isEmpty(),50);
        QVERIFY(write(fixture("kwin-left.conf")));
        PUTGD_TRY_VERIFY(buttons.layout().right.isEmpty());
#endif
    }
};
QTEST_MAIN(WindowButtonsTest)
#include "test_window_buttons.moc"
