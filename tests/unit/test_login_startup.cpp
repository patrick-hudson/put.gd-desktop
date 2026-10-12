#include "app/LoginStartup.h"
#include <QTemporaryDir>
#include <QFile>
#include <QTest>
using namespace putgd;
class LoginStartupTest:public QObject {
 Q_OBJECT
private slots:
#ifdef Q_OS_LINUX
 void appImageStartupUsesPersistentImage() {
  QTemporaryDir dir; const auto image=dir.filePath("put.gd preview.AppImage");
  QFile executable(image); QVERIFY(executable.open(QIODevice::WriteOnly)); executable.write("fixture"); executable.close();
  QVERIFY(executable.setPermissions(QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner));
  const auto original=qgetenv("APPIMAGE"),originalDirectory=qgetenv("APPDIR"); qputenv("APPIMAGE",image.toUtf8());qputenv("APPDIR","/tmp/.mount_putgd");
  const auto result=LoginStartup::apply(dir.path(),"/tmp/.mount_putgd/usr/bin/putgd-launcher",true);
  if(original.isNull())qunsetenv("APPIMAGE");else qputenv("APPIMAGE",original);
  if(originalDirectory.isNull())qunsetenv("APPDIR");else qputenv("APPDIR",originalDirectory);
  QVERIFY(!result); QFile entry(dir.filePath("autostart/putgd.desktop")); QVERIFY(entry.open(QIODevice::ReadOnly));
  const auto bytes=entry.readAll(); QVERIFY(bytes.contains("Exec=\""+image.toUtf8()+"\" --background"));
  QVERIFY(!bytes.contains(".mount_putgd"));
 }
 void inheritedAppImageDoesNotOverrideNativeStartup() {
  QTemporaryDir dir;const auto original=qgetenv("APPIMAGE"),originalDirectory=qgetenv("APPDIR");
  qputenv("APPIMAGE","/other.AppImage");qputenv("APPDIR","/tmp/.mount_other");
  const auto result=LoginStartup::apply(dir.path(),"/usr/bin/putgd-launcher",true);
  if(original.isNull())qunsetenv("APPIMAGE");else qputenv("APPIMAGE",original);
  if(originalDirectory.isNull())qunsetenv("APPDIR");else qputenv("APPDIR",originalDirectory);
  QVERIFY(!result);QFile entry(dir.filePath("autostart/putgd.desktop"));QVERIFY(entry.open(QIODevice::ReadOnly));
  QVERIFY(entry.readAll().contains("Exec=\"/usr/bin/putgd-launcher\" --background"));
 }
 void missingAppImageCannotPersistTemporaryMount() {
  QTemporaryDir dir;const auto original=qgetenv("APPIMAGE"),originalDirectory=qgetenv("APPDIR");qputenv("APPIMAGE",dir.filePath("missing.AppImage").toUtf8());qputenv("APPDIR","/tmp/.mount_putgd");
  const auto result=LoginStartup::apply(dir.path(),"/tmp/.mount_putgd/usr/bin/putgd-launcher",true);
  if(original.isNull())qunsetenv("APPIMAGE");else qputenv("APPIMAGE",original);
  if(originalDirectory.isNull())qunsetenv("APPDIR");else qputenv("APPDIR",originalDirectory);
  QCOMPARE(result.code,QString("autostart_io"));QVERIFY(!QFile::exists(dir.filePath("autostart/putgd.desktop")));
 }
#endif
 void preferenceOwnsOnlyManagedAutostartEntry() {
  QTemporaryDir dir;const auto executable=dir.filePath("app space/putgd-launcher");
  QVERIFY(!LoginStartup::apply(dir.path(),executable,true));QFile entry(dir.filePath("autostart/putgd.desktop"));QVERIFY(entry.open(QIODevice::ReadOnly));const auto bytes=entry.readAll();QVERIFY(bytes.contains("Exec=\""+executable.toUtf8()+"\" --background"));QVERIFY(bytes.contains("X-putgd-Managed=true"));entry.close();
  QFile unrelated(dir.filePath("autostart/other.desktop"));QVERIFY(unrelated.open(QIODevice::WriteOnly));unrelated.write("preserve");unrelated.close();QVERIFY(!LoginStartup::apply(dir.path(),executable,false));QVERIFY(!entry.exists());QVERIFY(unrelated.exists());
  QVERIFY(entry.open(QIODevice::WriteOnly));entry.write("user-created entry");entry.close();QCOMPARE(LoginStartup::apply(dir.path(),executable,true).code,QString("autostart_conflict"));QCOMPARE(LoginStartup::apply(dir.path(),executable,false).code,QString("autostart_conflict"));
 }
};QTEST_GUILESS_MAIN(LoginStartupTest)
#include "test_login_startup.moc"
