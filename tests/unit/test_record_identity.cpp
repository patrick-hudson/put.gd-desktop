#include "record/AppIdentity.h"
#include "ui/Theme.h"
#include <QFile>
#include <QFileInfo>
#include <QWidget>
#include <QtTest>

// Issue #264: putgd-record's windows (the review window included) showed a
// generic icon because the helper set no window icon and no desktop file name.
class RecordIdentityTest : public QObject {
    Q_OBJECT
private slots:
    void helperWindowsCarryTheBrandIconAndDesktopEntry() {
        auto& app = *qobject_cast<QApplication*>(QCoreApplication::instance());
        putgd::record::applyIdentity(app);
        QCOMPARE(QGuiApplication::desktopFileName(), QString("putgd-record"));
        const auto icon = QApplication::windowIcon();
        QVERIFY(!icon.isNull());
        const auto expected = putgd::Ui::icon("brand").pixmap(48).toImage();
        QCOMPARE(icon.pixmap(48).toImage(), expected);
        // Top-level windows inherit it unless they set their own.
        QWidget review;
        review.setWindowTitle("Review recording");
        QCOMPARE(review.windowIcon().pixmap(48).toImage(), expected);
    }
    void installedEntryMatchesTheDesktopFileName() {
        const QFileInfo entry(QStringLiteral(PUTGD_RECORD_DESKTOP_ENTRY));
        QCOMPARE(entry.fileName(), QString("%1.desktop").arg(QLatin1String(putgd::record::desktopFileName)));
        QFile file(entry.filePath());
        QVERIFY2(file.open(QIODevice::ReadOnly), qPrintable(entry.filePath()));
        const auto lines = QString::fromUtf8(file.readAll()).split('\n');
        QVERIFY(lines.contains("Icon=putgd"));
        QVERIFY(lines.contains("NoDisplay=true"));
        QVERIFY(lines.contains(QString("StartupWMClass=%1").arg(QLatin1String(putgd::record::desktopFileName))));
    }
    // KWin offers a restricted Wayland interface only to the executable of an
    // entry that lists it: the screencast for recordings, and through Plasma
    // 6.7 the fake input a scrolling capture turns the wheel with (#1376).
    void installedEntryListsTheKWinInterfacesTheHelperBinds() {
        QFile file(QStringLiteral(PUTGD_RECORD_DESKTOP_ENTRY));
        QVERIFY2(file.open(QIODevice::ReadOnly), qPrintable(file.fileName()));
        QStringList interfaces;
        QString exec;
        for (const auto& line : QString::fromUtf8(file.readAll()).split('\n')) {
            if (line.startsWith("X-KDE-Wayland-Interfaces=")) interfaces = line.section('=', 1).split(',', Qt::SkipEmptyParts);
            if (line.startsWith("Exec=")) exec = line.section('=', 1).section(' ', 0, 0);
        }
        QCOMPARE(interfaces, QStringList({"zkde_screencast_unstable_v1", "org_kde_kwin_fake_input"}));
        // KWin compares the first Exec token with the client's executable.
        QVERIFY2(exec.endsWith("/putgd-record"), qPrintable(exec));
    }
};

QTEST_MAIN(RecordIdentityTest)
#include "test_record_identity.moc"
