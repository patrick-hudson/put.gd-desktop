#include "app/LoginStartup.h"
#include <QTemporaryDir>
#include <QFile>
#include <QTest>
using namespace putgd;
class LoginStartupTest:public QObject {
 Q_OBJECT
private slots:
 void preferenceOwnsOnlyManagedAutostartEntry() {
  QTemporaryDir dir;const auto executable=dir.filePath("app space/putgd-launcher");
  QVERIFY(!LoginStartup::apply(dir.path(),executable,true));QFile entry(dir.filePath("autostart/putgd.desktop"));QVERIFY(entry.open(QIODevice::ReadOnly));const auto bytes=entry.readAll();QVERIFY(bytes.contains("Exec=\""+executable.toUtf8()+"\" --background"));QVERIFY(bytes.contains("X-putgd-Managed=true"));entry.close();
  QFile unrelated(dir.filePath("autostart/other.desktop"));QVERIFY(unrelated.open(QIODevice::WriteOnly));unrelated.write("preserve");unrelated.close();QVERIFY(!LoginStartup::apply(dir.path(),executable,false));QVERIFY(!entry.exists());QVERIFY(unrelated.exists());
  QVERIFY(entry.open(QIODevice::WriteOnly));entry.write("user-created entry");entry.close();QCOMPARE(LoginStartup::apply(dir.path(),executable,true).code,QString("autostart_conflict"));QCOMPARE(LoginStartup::apply(dir.path(),executable,false).code,QString("autostart_conflict"));
 }
};QTEST_GUILESS_MAIN(LoginStartupTest)
#include "test_login_startup.moc"
