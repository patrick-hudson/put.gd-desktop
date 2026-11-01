#include "app/LoginStartup.h"
#include <QTemporaryDir>
#include <QDir>
#include <QFileInfo>
#include <QFile>
#include <QScopeGuard>
#include <QTest>
#include <QtEndian>
#include <cstring>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
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
 void monitorPreparationPreservesTheSameInstallation() {
  QTemporaryDir dir;const auto data=dir.filePath("data"),native=dir.filePath("putgd-launcher"),next=dir.filePath("another-launcher");
  writeImage(native,"#!/bin/sh\nexit 0\n");writeImage(next,"#!/bin/sh\nexit 0\n");
  AppImageEnvironment environment("/other.AppImage","/tmp/.mount_other");
  QVERIFY(!LoginStartup::apply(dir.path(),data,native,true));
  const auto bin=dir.filePath("manager"),receipt=dir.filePath("manager-calls");QVERIFY(QDir().mkpath(bin));
  auto quoted=receipt;quoted.replace("'","'\\''");
  writeImage(QDir(bin).filePath("systemctl"),("#!/bin/sh\nprintf 'called\\n' >> '"+quoted+"'\nprintf 'not-found\\n'\n").toUtf8());
  // A user manager to reach, whatever the machine running this test has (#1112).
  // A short root: a socket path may not pass 107 bytes, however long TMPDIR is.
  QTemporaryDir runtimeRoot("/tmp/pg-rt-XXXXXX");QVERIFY(runtimeRoot.isValid());
  const auto runtime=runtimeRoot.path();QVERIFY(QDir().mkpath(runtime+"/systemd"));
  {
   const int fd=::socket(AF_UNIX,SOCK_STREAM,0);QVERIFY(fd>=0);
   sockaddr_un address{};address.sun_family=AF_UNIX;const auto bytes=QFile::encodeName(runtime+"/systemd/private");
   QVERIFY(size_t(bytes.size())<sizeof(address.sun_path));std::memcpy(address.sun_path,bytes.constData(),size_t(bytes.size()));
   QCOMPARE(::bind(fd,reinterpret_cast<const sockaddr*>(&address),sizeof(address)),0);::close(fd);
  }
  const auto oldRuntime=qgetenv("XDG_RUNTIME_DIR");qputenv("XDG_RUNTIME_DIR",QFile::encodeName(runtime));
  const auto oldPath=qgetenv("PATH");qputenv("PATH",bin.toUtf8()+':'+oldPath);
  const auto unchanged=LoginStartup::prepareMonitor(dir.path(),data,native,true);
  const auto unchangedTouchedManager=QFile::exists(receipt);
  const auto changed=LoginStartup::prepareMonitor(dir.path(),data,next,true);
  const auto changedTouchedManager=QFile::exists(receipt);
  if(oldPath.isNull())qunsetenv("PATH");else qputenv("PATH",oldPath);
  if(oldRuntime.isNull())qunsetenv("XDG_RUNTIME_DIR");else qputenv("XDG_RUNTIME_DIR",oldRuntime);
  QVERIFY(!unchanged);QVERIFY(!unchangedTouchedManager);
  QVERIFY(!changed);QVERIFY(changedTouchedManager);
 }
 void appImageStartupRunsTheLoginScript() {
  QTemporaryDir dir;const auto image=dir.filePath("put.gd 'preview'.AppImage"),data=dir.filePath("data");
  writeImage(image,elfImage("zsync|https://app.put.gd/dl/nightly/appimage.zsync"));
  Error result;{AppImageEnvironment environment(image,"/tmp/.mount_putgd");result=LoginStartup::apply(dir.path(),data,"/tmp/.mount_putgd/usr/bin/putgd-launcher",true);}
  QVERIFY(!result);
  const auto script=LoginStartup::scriptPath(data);
  const auto entry=readAll(dir.filePath("autostart/putgd.desktop"));
  QVERIFY(entry.contains("Exec=/bin/sh \""+script.toUtf8()+"\" --background\n"));
  QVERIFY(entry.contains("\nX-putgd-Managed=true\n"));
  QVERIFY(!entry.contains(".mount_putgd"));
  QVERIFY(!entry.contains("\nTryExec="));
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
  const auto text=readAll(LoginStartup::scriptPath(data));
  QVERIFY(text.contains("\nconfigured='"+next.toUtf8()+"'\n"));QVERIFY(!text.contains(old.toUtf8()));
 }
 void imageWithoutUpdateInformationStartsOnlyItself() {
  QTemporaryDir dir;const auto image=dir.filePath("putgd-0.1.1-dev.1+gabc-x86_64.AppImage"),data=dir.filePath("data");
  writeImage(image,"not an ELF file");
  {AppImageEnvironment environment(image,"/tmp/.mount_putgd");QVERIFY(!LoginStartup::apply(dir.path(),data,"/tmp/.mount_putgd/usr/bin/putgd-launcher",true));}
  QVERIFY(readAll(LoginStartup::scriptPath(data)).contains("\nupdate_information=''\n"));
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
  QTemporaryDir dir;const auto data=dir.filePath("data"),native=dir.filePath("putgd-launcher");writeImage(native,"#!/bin/sh\nexit 0\n");Error result;
  {AppImageEnvironment environment("/other.AppImage","/tmp/.mount_other");result=LoginStartup::apply(dir.path(),data,native,true);}
  QVERIFY(!result);
  QVERIFY(readAll(LoginStartup::scriptPath(data)).contains("\nconfigured='"+native.toUtf8()+"'\n"));
  QVERIFY(readAll(LoginStartup::scriptPath(data)).contains("\nkind='native'\n"));
  QVERIFY(!readAll(dir.filePath("autostart/putgd.desktop")).contains("TryExec="));
 }
 void disablingRemovesOnlyManagedScriptsAndMigratesTheOldPath() {
  QTemporaryDir dir;const auto image=dir.filePath("putgd.AppImage"),data=dir.filePath("data"),script=LoginStartup::scriptPath(data);
  writeImage(image,elfImage("zsync|https://app.put.gd/dl/stable/appimage.zsync"));
  {AppImageEnvironment environment(image,"/tmp/.mount_putgd");QVERIFY(!LoginStartup::apply(dir.path(),data,"/tmp/.mount_putgd/usr/bin/putgd-launcher",true));}
  const auto legacy=LoginStartup::appImageScriptPath(data);QFile old(legacy);QVERIFY(old.open(QIODevice::WriteOnly));old.write("#!/bin/sh\n# X-putgd-Managed=true\n");old.close();
  {AppImageEnvironment environment(image,"/tmp/.mount_putgd");QVERIFY(!LoginStartup::apply(dir.path(),data,"/tmp/.mount_putgd/usr/bin/putgd-launcher",true));}
  QVERIFY(!QFile::exists(legacy));QVERIFY(QFile::exists(script));
  QVERIFY(!LoginStartup::apply(dir.path(),data,"/usr/bin/putgd-launcher",false));
  QVERIFY(!QFile::exists(script));QVERIFY(!QFile::exists(dir.filePath("autostart/putgd.desktop")));
  QFile foreign(script);QVERIFY(foreign.open(QIODevice::WriteOnly));foreign.write("#!/bin/sh\n# the user's own script\n");foreign.close();
  QVERIFY(!LoginStartup::apply(dir.path(),data,"/usr/bin/putgd-launcher",false));QVERIFY(QFile::exists(script));
  {AppImageEnvironment environment(image,"/tmp/.mount_putgd");QCOMPARE(LoginStartup::apply(dir.path(),data,"/tmp/.mount_putgd/usr/bin/putgd-launcher",true).code,QString("autostart_conflict"));}
 }
 void failedAppImageMigrationPreservesLegacyStartupUntilRetry() {
  QTemporaryDir dir;const auto image=dir.filePath("putgd.AppImage"),data=dir.filePath("data"),directory=dir.filePath("autostart");
  const auto path=QDir(directory).filePath("putgd.desktop"),legacy=LoginStartup::appImageScriptPath(data);
  QVERIFY(QDir().mkpath(directory));QVERIFY(QDir().mkpath(QFileInfo(legacy).path()));
  writeImage(image,elfImage("zsync|https://app.put.gd/dl/stable/appimage.zsync"));
  const QByteArray oldScript="#!/bin/sh\n# X-putgd-Managed=true\nexit 0\n";
  writeImage(legacy,oldScript);
  const auto oldEntry=("[Desktop Entry]\nExec=/bin/sh \""+legacy+"\" --background\nX-putgd-Managed=true\n").toUtf8();
  QFile entry(path);QVERIFY(entry.open(QIODevice::WriteOnly));QCOMPARE(entry.write(oldEntry),oldEntry.size());entry.close();
  const auto permissions=QFile::permissions(directory);
  const auto restore=qScopeGuard([&]{QFile::setPermissions(directory,permissions);});
  QVERIFY(QFile::setPermissions(directory,QFile::ReadOwner|QFile::ExeOwner));
  if(QFileInfo(directory).isWritable())QSKIP("Requires an unprivileged user for directory write restrictions");
  AppImageEnvironment environment(image,"/tmp/.mount_putgd");
  const auto result=LoginStartup::apply(dir.path(),data,"/tmp/.mount_putgd/usr/bin/putgd-launcher",true);
  QCOMPARE(result.code,QString("autostart_io"));
  QVERIFY(QFile::exists(LoginStartup::scriptPath(data)));
  QCOMPARE(readAll(path),oldEntry);QCOMPARE(readAll(legacy),oldScript);
  QVERIFY(QFileInfo(legacy).isExecutable());
  QVERIFY(QFile::setPermissions(directory,permissions));
  QVERIFY(!LoginStartup::apply(dir.path(),data,"/tmp/.mount_putgd/usr/bin/putgd-launcher",true));
  QVERIFY(readAll(path).contains("Exec=/bin/sh \""+LoginStartup::scriptPath(data).toUtf8()+"\" --background\n"));
  QVERIFY(!QFile::exists(legacy));
 }
 void missingAppImageCannotPersistTemporaryMount() {
  QTemporaryDir dir;Error result;
  {AppImageEnvironment environment(dir.filePath("missing.AppImage"),"/tmp/.mount_putgd");result=LoginStartup::apply(dir.path(),dir.filePath("data"),"/tmp/.mount_putgd/usr/bin/putgd-launcher",true);}
  QCOMPARE(result.code,QString("autostart_io"));QVERIFY(!QFile::exists(dir.filePath("autostart/putgd.desktop")));
 }
 void nativeStartupUsesAUserScopedCleanupScript() {
  QTemporaryDir dir;const auto executable=dir.filePath("putgd space\\slash\ttab%$`\""),data=dir.filePath("data");
  writeImage(executable,"#!/bin/sh\nexit 0\n");
  QVERIFY(!LoginStartup::apply(dir.path(),data,executable,true));
  const auto entry=readAll(dir.filePath("autostart/putgd.desktop")),script=readAll(LoginStartup::scriptPath(data));
  QVERIFY(entry.contains("Exec=/bin/sh \""+LoginStartup::scriptPath(data).toUtf8()+"\" --background\n"));
  QVERIFY(!entry.contains("TryExec="));QVERIFY(script.contains(executable.toUtf8()));QVERIFY(script.contains("\nkind='native'\n"));
  QVERIFY(!script.contains("__PUTGD_"));
 }
 void templateLookingPathTextRemainsLiteral() {
  QTemporaryDir dir;const auto executable=dir.filePath("__PUTGD_KIND__"),data=dir.filePath("__PUTGD_ENTRY__");
  writeImage(executable,"#!/bin/sh\nexit 0\n");QVERIFY(!LoginStartup::apply(dir.path(),data,executable,true));
  const auto text=readAll(LoginStartup::scriptPath(data));
  QVERIFY(text.contains("\nconfigured='"+executable.toUtf8()+"'\n"));
  QVERIFY(text.contains("\nkind='native'\n"));
 }
 void nativeStartupRepairsTheOldDirectEntry() {
  QTemporaryDir dir;const auto data=dir.filePath("data"),path=dir.filePath("autostart/putgd.desktop"),executable=dir.filePath("putgd-launcher");
  writeImage(executable,"#!/bin/sh\nexit 0\n");QVERIFY(QDir().mkpath(dir.filePath("autostart")));
  QFile entry(path);QVERIFY(entry.open(QIODevice::WriteOnly));entry.write("[Desktop Entry]\nExec=/old/putgd-launcher --background\nTryExec=/old/putgd-launcher\nX-putgd-Managed=true\n");entry.close();
  QVERIFY(!LoginStartup::apply(dir.path(),data,executable,true));
  QVERIFY(!readAll(path).contains("TryExec="));QVERIFY(readAll(path).contains("\nExec=/bin/sh "));
  QVERIFY(!LoginStartup::apply(dir.path(),data,executable,false));QVERIFY(!QFile::exists(path));
 }
 void removedNativeExecutableCannotRecreateTheStartupLink() {
  QTemporaryDir dir;const auto data=dir.filePath("data"),executable=dir.filePath("putgd-launcher");
  QCOMPARE(LoginStartup::apply(dir.path(),data,executable,true).code,QString("autostart_io"));
  QVERIFY(!QFile::exists(dir.filePath("autostart/putgd.desktop")));QVERIFY(!QFile::exists(LoginStartup::scriptPath(data)));
 }
#endif
 void preferenceOwnsOnlyManagedAutostartEntry() {
  QTemporaryDir dir;const auto executable=dir.filePath("app space/putgd-launcher"),data=dir.filePath("data");
  QVERIFY(QDir().mkpath(QFileInfo(executable).path()));writeImage(executable,"#!/bin/sh\nexit 0\n");
  QVERIFY(!LoginStartup::apply(dir.path(),data,executable,true));QFile entry(dir.filePath("autostart/putgd.desktop"));QVERIFY(entry.open(QIODevice::ReadOnly));const auto bytes=entry.readAll();
#ifdef Q_OS_LINUX
  QVERIFY(bytes.contains("Exec=/bin/sh "));
#else
  QVERIFY(bytes.contains("Exec=\""+executable.toUtf8()+"\" --background"));
#endif
  QVERIFY(bytes.contains("X-putgd-Managed=true"));entry.close();
  QFile unrelated(dir.filePath("autostart/other.desktop"));QVERIFY(unrelated.open(QIODevice::WriteOnly));unrelated.write("preserve");unrelated.close();QVERIFY(!LoginStartup::apply(dir.path(),data,executable,false));QVERIFY(!entry.exists());QVERIFY(unrelated.exists());
  QVERIFY(entry.open(QIODevice::WriteOnly));entry.write("user-created entry");entry.close();QCOMPARE(LoginStartup::apply(dir.path(),data,executable,true).code,QString("autostart_conflict"));QCOMPARE(LoginStartup::apply(dir.path(),data,executable,false).code,QString("autostart_conflict"));
 }
};QTEST_GUILESS_MAIN(LoginStartupTest)
#include "test_login_startup.moc"
