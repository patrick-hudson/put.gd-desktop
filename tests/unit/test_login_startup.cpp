#include "app/LoginStartup.h"
#include <QTemporaryDir>
#include <QFile>
#include <QTest>
#include <QtEndian>
using namespace putgd;
namespace {
// A little-endian ELF64 file with the AppImage magic and a .upd_info section,
// laid out as the type 2 runtime is; no code.
QByteArray elfImage(const QByteArray& updateInformation) {
 const QByteArray names("\0.shstrtab\0.upd_info\0",21);
 auto info=updateInformation;info.append(QByteArray(1024-info.size(),'\0'));
 QByteArray header(64,'\0');
 header.replace(0,4,"\x7f""ELF");header[4]=2;header[5]=1;header[6]=1;header[8]='A';header[9]='I';header[10]=2;
 const quint64 tableOffset=64+names.size()+info.size();
 qToLittleEndian<quint64>(tableOffset,header.data()+0x28);
 qToLittleEndian<quint16>(64,header.data()+0x3A);qToLittleEndian<quint16>(3,header.data()+0x3C);qToLittleEndian<quint16>(1,header.data()+0x3E);
 QByteArray table(3*64,'\0');
 const auto section=[&](int index,quint32 name,quint64 offset,quint64 size){
  qToLittleEndian<quint32>(name,table.data()+index*64);qToLittleEndian<quint64>(offset,table.data()+index*64+24);qToLittleEndian<quint64>(size,table.data()+index*64+32);
 };
 section(1,1,64,names.size());section(2,11,64+names.size(),info.size());
 return header+names+info+table;
}
struct AppImageEnvironment {
 QByteArray image,directory;
 AppImageEnvironment(const QString& appImage,const QString& appDirectory):image(qgetenv("APPIMAGE")),directory(qgetenv("APPDIR")) {
  qputenv("APPIMAGE",appImage.toUtf8());qputenv("APPDIR",appDirectory.toUtf8());
 }
 ~AppImageEnvironment() {
  if(image.isNull())qunsetenv("APPIMAGE");else qputenv("APPIMAGE",image);
  if(directory.isNull())qunsetenv("APPDIR");else qputenv("APPDIR",directory);
 }
};
QByteArray readAll(const QString& path) {QFile file(path);return file.open(QIODevice::ReadOnly)?file.readAll():QByteArray{};}
void writeImage(const QString& path,const QByteArray& bytes) {
 QFile file(path);QVERIFY(file.open(QIODevice::WriteOnly));file.write(bytes);file.close();
 QVERIFY(file.setPermissions(QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner));
}
}
class LoginStartupTest:public QObject {
 Q_OBJECT
private slots:
#ifdef Q_OS_LINUX
 void appImageStartupRunsTheLoginScript() {
  QTemporaryDir dir;const auto image=dir.filePath("put.gd 'preview'.AppImage"),data=dir.filePath("data");
  writeImage(image,elfImage("zsync|https://app.put.gd/dl/nightly/appimage.zsync"));
  Error result;{AppImageEnvironment environment(image,"/tmp/.mount_putgd");result=LoginStartup::apply(dir.path(),data,"/tmp/.mount_putgd/usr/bin/putgd-launcher",true);}
  QVERIFY(!result);
  const auto script=LoginStartup::appImageScriptPath(data);
  const auto entry=readAll(dir.filePath("autostart/putgd.desktop"));
  QVERIFY(entry.contains("Exec=/bin/sh \""+script.toUtf8()+"\" --background\n"));
  QVERIFY(entry.contains("\nX-putgd-Managed=true\n"));
  QVERIFY(!entry.contains(".mount_putgd"));
  const auto text=readAll(script);
  QVERIFY(text.startsWith("#!/bin/sh\n"));
  QVERIFY(text.contains("\n# X-putgd-Managed=true\n"));
  auto quoted=image;quoted.replace("'","'\\''");
  QVERIFY(text.contains("\nconfigured='"+quoted.toUtf8()+"'\n"));
  QVERIFY(text.contains("\nupdate_information='zsync|https://app.put.gd/dl/nightly/appimage.zsync'\n"));
  QVERIFY(!text.contains("__PUTGD_"));
  QVERIFY((QFile::permissions(script)&~(QFile::ReadUser|QFile::WriteUser|QFile::ExeUser))==(QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner));
 }
 void startFromAnUpdatedImageRepairsTheScript() {
  QTemporaryDir dir;const auto old=dir.filePath("putgd-0.1.1-nightly.800-x86_64.AppImage"),next=dir.filePath("putgd-0.1.1-nightly.801-x86_64.AppImage"),data=dir.filePath("data");
  writeImage(old,elfImage("zsync|https://app.put.gd/dl/nightly/appimage.zsync"));writeImage(next,elfImage("zsync|https://app.put.gd/dl/nightly/appimage.zsync"));
  {AppImageEnvironment environment(old,"/tmp/.mount_a");QVERIFY(!LoginStartup::apply(dir.path(),data,"/tmp/.mount_a/usr/bin/putgd-launcher",true));}
  QVERIFY(QFile::remove(old));
  {AppImageEnvironment environment(next,"/tmp/.mount_b");QVERIFY(!LoginStartup::apply(dir.path(),data,"/tmp/.mount_b/usr/bin/putgd-launcher",true));}
  const auto text=readAll(LoginStartup::appImageScriptPath(data));
  QVERIFY(text.contains("\nconfigured='"+next.toUtf8()+"'\n"));QVERIFY(!text.contains(old.toUtf8()));
 }
 void imageWithoutUpdateInformationStartsOnlyItself() {
  QTemporaryDir dir;const auto image=dir.filePath("putgd-0.1.1-dev.1+gabc-x86_64.AppImage"),data=dir.filePath("data");
  writeImage(image,"not an ELF file");
  {AppImageEnvironment environment(image,"/tmp/.mount_putgd");QVERIFY(!LoginStartup::apply(dir.path(),data,"/tmp/.mount_putgd/usr/bin/putgd-launcher",true));}
  QVERIFY(readAll(LoginStartup::appImageScriptPath(data)).contains("\nupdate_information=''\n"));
 }
 void updateInformationIsReadFromTheRuntimeSection() {
  QTemporaryDir dir;const auto image=dir.filePath("image");
  writeImage(image,elfImage("zsync|http://127.0.0.1:8765/appimage.zsync"));
  QCOMPARE(LoginStartup::appImageUpdateInformation(image),QString("zsync|http://127.0.0.1:8765/appimage.zsync"));
  writeImage(image,elfImage("zsync|bad\nline"));QVERIFY(LoginStartup::appImageUpdateInformation(image).isEmpty());
  auto truncated=elfImage("zsync|x");truncated.chop(10);writeImage(image,truncated);QVERIFY(LoginStartup::appImageUpdateInformation(image).isEmpty());
  QVERIFY(LoginStartup::appImageUpdateInformation(dir.filePath("missing")).isEmpty());
 }
 void inheritedAppImageDoesNotOverrideNativeStartup() {
  QTemporaryDir dir;const auto data=dir.filePath("data");Error result;
  {AppImageEnvironment environment("/other.AppImage","/tmp/.mount_other");result=LoginStartup::apply(dir.path(),data,"/usr/bin/putgd-launcher",true);}
  QVERIFY(!result);
  QVERIFY(readAll(dir.filePath("autostart/putgd.desktop")).contains("Exec=\"/usr/bin/putgd-launcher\" --background"));
  QVERIFY(!QFile::exists(LoginStartup::appImageScriptPath(data)));
 }
 void nativeStartupAndDisablingRemoveOnlyPutgdsScript() {
  QTemporaryDir dir;const auto image=dir.filePath("putgd.AppImage"),data=dir.filePath("data"),script=LoginStartup::appImageScriptPath(data);
  writeImage(image,elfImage("zsync|https://app.put.gd/dl/stable/appimage.zsync"));
  {AppImageEnvironment environment(image,"/tmp/.mount_putgd");QVERIFY(!LoginStartup::apply(dir.path(),data,"/tmp/.mount_putgd/usr/bin/putgd-launcher",true));}
  QVERIFY(QFile::exists(script));
  QVERIFY(!LoginStartup::apply(dir.path(),data,"/usr/bin/putgd-launcher",true));QVERIFY(!QFile::exists(script));
  {AppImageEnvironment environment(image,"/tmp/.mount_putgd");QVERIFY(!LoginStartup::apply(dir.path(),data,"/tmp/.mount_putgd/usr/bin/putgd-launcher",true));}
  QVERIFY(!LoginStartup::apply(dir.path(),data,"/usr/bin/putgd-launcher",false));
  QVERIFY(!QFile::exists(script));QVERIFY(!QFile::exists(dir.filePath("autostart/putgd.desktop")));
  QFile foreign(script);QVERIFY(foreign.open(QIODevice::WriteOnly));foreign.write("#!/bin/sh\n# the user's own script\n");foreign.close();
  QVERIFY(!LoginStartup::apply(dir.path(),data,"/usr/bin/putgd-launcher",false));QVERIFY(QFile::exists(script));
 }
 void missingAppImageCannotPersistTemporaryMount() {
  QTemporaryDir dir;Error result;
  {AppImageEnvironment environment(dir.filePath("missing.AppImage"),"/tmp/.mount_putgd");result=LoginStartup::apply(dir.path(),dir.filePath("data"),"/tmp/.mount_putgd/usr/bin/putgd-launcher",true);}
  QCOMPARE(result.code,QString("autostart_io"));QVERIFY(!QFile::exists(dir.filePath("autostart/putgd.desktop")));
 }
#endif
 void preferenceOwnsOnlyManagedAutostartEntry() {
  QTemporaryDir dir;const auto executable=dir.filePath("app space/putgd-launcher"),data=dir.filePath("data");
  QVERIFY(!LoginStartup::apply(dir.path(),data,executable,true));QFile entry(dir.filePath("autostart/putgd.desktop"));QVERIFY(entry.open(QIODevice::ReadOnly));const auto bytes=entry.readAll();QVERIFY(bytes.contains("Exec=\""+executable.toUtf8()+"\" --background"));QVERIFY(bytes.contains("X-putgd-Managed=true"));entry.close();
  QFile unrelated(dir.filePath("autostart/other.desktop"));QVERIFY(unrelated.open(QIODevice::WriteOnly));unrelated.write("preserve");unrelated.close();QVERIFY(!LoginStartup::apply(dir.path(),data,executable,false));QVERIFY(!entry.exists());QVERIFY(unrelated.exists());
  QVERIFY(entry.open(QIODevice::WriteOnly));entry.write("user-created entry");entry.close();QCOMPARE(LoginStartup::apply(dir.path(),data,executable,true).code,QString("autostart_conflict"));QCOMPARE(LoginStartup::apply(dir.path(),data,executable,false).code,QString("autostart_conflict"));
 }
};QTEST_GUILESS_MAIN(LoginStartupTest)
#include "test_login_startup.moc"
