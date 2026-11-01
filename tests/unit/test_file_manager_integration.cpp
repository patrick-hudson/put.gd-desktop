#include "app/FileManagerIntegration.h"
#include "fixtures/Waits.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QProcess>
#include <QTemporaryDir>
#include <QTest>
#include <QtEndian>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <cstring>
using namespace putgd;
namespace {
constexpr auto programMode=QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner|QFile::ReadGroup|QFile::ExeGroup|QFile::ReadOther|QFile::ExeOther;
constexpr auto dataMode=QFile::ReadOwner|QFile::WriteOwner|QFile::ReadGroup|QFile::ReadOther;
constexpr auto modeBits=programMode|QFile::WriteGroup|QFile::WriteOther;
const QString script=QStringLiteral("Upload with put.gd");
QByteArray readAll(const QString& path) {QFile file(path);return file.open(QIODevice::ReadOnly)?file.readAll():QByteArray{};}
void write(const QString& path,const QByteArray& bytes,QFile::Permissions mode=dataMode) {
 QVERIFY(QDir().mkpath(QFileInfo(path).path()));
 QFile file(path);QVERIFY(file.open(QIODevice::WriteOnly|QIODevice::Truncate));file.write(bytes);file.close();
 QVERIFY(file.setPermissions(mode));
}
QFile::Permissions mode(const QString& path) {return QFileInfo(path).permissions()&modeBits;}
// A little-endian ELF64 file with the AppImage magic and a .upd_info section,
// as test_login_startup builds it; no code.
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
const QByteArray updateInformation="zsync|https://app.put.gd/dl/nightly/appimage.zsync";
// Temporary XDG roots. The data root holds a space and a quote, so every
// format's quoting of the launcher path is exercised.
// Fake user-manager tools for code that runs the launcher from this process.
struct SearchPath {
 QByteArray previous=qgetenv("PATH");
 explicit SearchPath(const QString& directory) {qputenv("PATH",directory.toUtf8()+':'+previous);}
 ~SearchPath() {qputenv("PATH",previous);}
};
struct Fixture {
 QTemporaryDir dir;
 FileManagerContext context;
 QString receipt;
 Fixture() {
  context.home=path("home");context.dataHome=path("xdg data");context.configHome=path("xdg config");
  context.dataRoot=path("put.gd's data/putgd");context.dataDirs={path("system share")};context.libraryDirs={path("system lib")};
  context.searchPath=path("bin");context.version="26.1031.1";context.iconSource=path("putgd.svg");
  context.systemLauncher=path("system bin/putgd-launcher");context.appImage=path("Applications/putgd-26.1031.1-x86_64.AppImage");
  receipt=path("receipt");
  for(const auto& directory:{context.home,context.dataHome,context.configHome,context.dataRoot,context.dataDirs[0],context.libraryDirs[0],context.searchPath})QDir().mkpath(directory);
  write(context.iconSource,"<svg xmlns=\"http://www.w3.org/2000/svg\"/>\n");
  write(context.appImage,elfImage(updateInformation),programMode);
  launcher(context.systemLauncher);
  // A user manager that knows no unit: monitor() and cleanup never reach systemd.
  write(path("bin/systemctl"),"#!/bin/sh\nprintf 'not-found\\n'\n",programMode);
 }
 QString path(const QString& relative) const {return dir.filePath(relative);}
 // A launcher that records its arguments, NUL-separated.
 void launcher(const QString& at) const {
  auto quoted=receipt;quoted.replace("'","'\\''");
  write(at,("#!/bin/sh\nprintf '%s\\0' \"$@\" >> '"+quoted+"'\n").toUtf8(),programMode);
 }
 QStringList received() const {
  auto parts=QString::fromUtf8(readAll(receipt)).split(QChar(0));
  if(!parts.isEmpty()&&parts.last().isEmpty())parts.removeLast();
  return parts;
 }
 QString ding() const {return QDir(context.home).filePath(".local/share/nautilus/scripts/"+script);}
 QString nautilus() const {return QDir(context.dataHome).filePath("nautilus/scripts/"+script);}
 QString caja() const {return QDir(context.configHome).filePath("caja/scripts/"+script);}
 QString menu(const QString& relative) const {return QDir(context.dataHome).filePath(relative);}
 QString cosmic() const {return FileManagerIntegration::cosmicPath(context);}
 void installDing() const {write(QDir(context.dataDirs[0]).filePath("gnome-shell/extensions/ding@rastersoft.com/metadata.json"),"{}\n");}
 void installCosmic() const {write(QDir(context.dataDirs[0]).filePath("applications/com.system76.CosmicFiles.desktop"),"[Desktop Entry]\n");}
 void installPlugin(const QString& relative,const QString& name) const {write(QDir(context.libraryDirs[0]).filePath(relative+"/"+name),"\x7f""ELF");}
 int run(const QString& file,const QStringList& arguments,const QString& workingDirectory={},const QHash<QString,QString>& environment={}) const {
  QProcess process;auto env=QProcessEnvironment::systemEnvironment();
  env.insert("PATH",context.searchPath+":/usr/bin:/bin");env.remove("CAJA_SCRIPT_SELECTED_URIS");
  for(auto it=environment.begin();it!=environment.end();++it)env.insert(it.key(),it.value());
  process.setProcessEnvironment(env);if(!workingDirectory.isEmpty())process.setWorkingDirectory(workingDirectory);
  process.start("/bin/sh",QStringList{file}+arguments);
  if(!process.waitForFinished(20000)) {process.kill();process.waitForFinished();return -1;}
  return process.exitCode();
 }
};
const QStringList menuFiles{"kio/servicemenus/putgd-upload.desktop","nemo/actions/putgd-upload.nemo_action","Thunar/sendto/putgd-upload.desktop",
 "file-manager/actions/putgd-upload.desktop","contractor/putgd-upload.contract"};
}
class FileManagerIntegrationTest:public QObject {
 Q_OBJECT
private slots:
 void systemInstallWritesItsPerUserFilesWhereDesktopsNeedThem() {
  Fixture f;auto& c=f.context;c.installation="system";
  auto result=FileManagerIntegration::apply(c);
  QVERIFY2(!result.error,qPrintable(result.error.message));
  QVERIFY(result.firstRun);QVERIFY(result.written.isEmpty());QVERIFY(result.restartable.isEmpty());
  QVERIFY(!QFileInfo::exists(f.ding()));QVERIFY(!QFileInfo::exists(f.cosmic()));
  QCOMPARE(readAll(QDir(c.dataRoot).filePath("file-manager/system.stamp")),QByteArray("26.1031.1\n"));
  // DING installed and COSMIC Files added later: both appear at the next start.
  f.installDing();f.installCosmic();
  result=FileManagerIntegration::apply(c);
  QVERIFY(!result.error);QVERIFY(!result.firstRun);
  QCOMPARE(result.written,QStringList({f.ding(),f.cosmic()}));
  QVERIFY(!QFileInfo::exists(f.nautilus()));
  const auto text=readAll(f.ding());
  QVERIFY(text.startsWith("#!/bin/sh\n"));QVERIFY(text.contains("\n# X-putgd-Managed=true\n"));
  QVERIFY(text.contains("launcher='"+c.systemLauncher.toUtf8()+"'\n"));
  QCOMPARE(mode(f.ding()),programMode);
  QCOMPARE(QString::fromUtf8(readAll(f.cosmic())),"[\n    (name: \"Upload with put.gd\", confirm: false, selection: Any, steps: [\"'"+c.systemLauncher+
   "' --upload -- %F\"]),\n]\n");
  QCOMPARE(mode(f.cosmic()),dataMode);
  QVERIFY(!QFileInfo::exists(f.cosmic()+".putgd-backup"));
  QVERIFY(FileManagerIntegration::apply(c).written.isEmpty());
  // DING removed: the script goes; the COSMIC entry stays while COSMIC Files is installed.
  QVERIFY(QDir(QDir(c.dataDirs[0]).filePath("gnome-shell")).removeRecursively());
  result=FileManagerIntegration::apply(c);
  QCOMPARE(result.removed,QStringList{f.ding()});QVERIFY(!QFileInfo::exists(f.ding()));QVERIFY(QFileInfo::exists(f.cosmic()));
 }
 void systemFirstRunNamesFileManagersToRestart() {
  Fixture f;auto& c=f.context;c.installation="system";c.desktop="ubuntu:GNOME";
  f.installPlugin("nautilus/extensions-4","libputgd-nautilus.so");f.installPlugin("caja/extensions-2.0","libputgd-caja.so");
  f.installPlugin("peony-extensions","libputgd-peony.so");
  write(QDir(c.searchPath).filePath("nautilus"),"#!/bin/sh\n",programMode);write(QDir(c.searchPath).filePath("peony"),"#!/bin/sh\n",programMode);
  auto result=FileManagerIntegration::apply(c);
  QVERIFY(result.firstRun);QCOMPARE(result.restartable,QStringList{"nautilus"});
  result=FileManagerIntegration::apply(c);
  QVERIFY(!result.firstRun);QVERIFY(result.restartable.isEmpty());
  // An upgrade keeps the stamp: no second notice.
  c.version="26.1101.1";result=FileManagerIntegration::apply(c);
  QVERIFY(!result.firstRun);QCOMPARE(readAll(QDir(c.dataRoot).filePath("file-manager/system.stamp")),QByteArray("26.1101.1\n"));
 }
 void firstRunNamesOnlyTheSessionsFileManager_data() {
  QTest::addColumn<QString>("desktop");QTest::addColumn<QString>("installation");QTest::addColumn<QStringList>("restartable");
  const QStringList plugin{"nautilus"};
  QTest::newRow("GNOME")<<"ubuntu:GNOME"<<"system"<<plugin;
  QTest::newRow("Flashback")<<"GNOME-Flashback:GNOME"<<"system"<<plugin;
  QTest::newRow("Unity")<<"Unity:Unity7:ubuntu"<<"system"<<plugin;
  QTest::newRow("MATE")<<"MATE"<<"system"<<QStringList{"caja"};
  QTest::newRow("UKUI")<<"UKUI"<<"system"<<QStringList{"peony"};
  QTest::newRow("LXDE names PCManFM, not Files")<<"LXDE"<<"system"<<QStringList{"pcmanfm"};
  QTest::newRow("LXQt names PCManFM-Qt, not PCManFM")<<"LXQt"<<"system"<<QStringList{"pcmanfm-qt"};
  QTest::newRow("LXDE AppImage")<<"LXDE"<<"appimage"<<QStringList{"pcmanfm"};
  QTest::newRow("LXQt AppImage")<<"LXQt"<<"appimage"<<QStringList{"pcmanfm-qt"};
  QTest::newRow("GNOME AppImage has no plugin")<<"GNOME"<<"appimage"<<QStringList{};
  QTest::newRow("XFCE")<<"XFCE"<<"system"<<QStringList{};
  QTest::newRow("Cinnamon")<<"X-Cinnamon"<<"system"<<QStringList{};
  QTest::newRow("KDE")<<"KDE"<<"system"<<QStringList{};
  QTest::newRow("COSMIC")<<"COSMIC"<<"system"<<QStringList{};
 }
 void firstRunNamesOnlyTheSessionsFileManager() {
  QFETCH(QString,desktop);QFETCH(QString,installation);QFETCH(QStringList,restartable);
  Fixture f;auto& c=f.context;c.installation=installation;c.desktop=desktop;
  f.installPlugin("nautilus/extensions-4","libputgd-nautilus.so");f.installPlugin("caja/extensions-2.0","libputgd-caja.so");
  f.installPlugin("peony-extensions","libputgd-peony.so");
  if(installation=="system")write(QDir(c.dataDirs[0]).filePath("file-manager/actions/putgd-upload.desktop"),"[Desktop Entry]\n");
  for(const auto* name:{"nautilus","caja","peony","pcmanfm","pcmanfm-qt"})write(QDir(c.searchPath).filePath(name),"#!/bin/sh\n",programMode);
  const auto result=FileManagerIntegration::apply(c);
  QVERIFY2(!result.error,qPrintable(result.error.message));
  QVERIFY(result.firstRun);QVERIFY(result.announce);QCOMPARE(result.restartable,restartable);
 }
 void firstRunAnnouncesOnlyAnEntryThatExists_data() {
  QTest::addColumn<QString>("desktop");QTest::addColumn<QString>("kdeVersion");QTest::addColumn<QString>("installation");QTest::addColumn<bool>("announce");
  QTest::newRow("UKUI AppImage")<<"UKUI"<<""<<"appimage"<<false;
  QTest::newRow("UKUI DEB")<<"UKUI"<<""<<"system"<<true;
  QTest::newRow("Plasma 5 AppImage")<<"KDE"<<"5"<<"appimage"<<false;
  QTest::newRow("Plasma 5 DEB")<<"KDE"<<"5"<<"system"<<true;
  QTest::newRow("Plasma 6 AppImage")<<"KDE"<<"6"<<"appimage"<<true;
  QTest::newRow("Deepin")<<"Deepin"<<""<<"system"<<false;
  QTest::newRow("Trinity")<<"TDE"<<""<<"appimage"<<false;
  QTest::newRow("Enlightenment")<<"Enlightenment"<<""<<"system"<<false;
  QTest::newRow("LXDE AppImage")<<"LXDE"<<""<<"appimage"<<true;
  QTest::newRow("COSMIC")<<"COSMIC"<<""<<"system"<<true;
 }
 void firstRunAnnouncesOnlyAnEntryThatExists() {
  QFETCH(QString,desktop);QFETCH(QString,kdeVersion);QFETCH(QString,installation);QFETCH(bool,announce);
  Fixture f;auto& c=f.context;c.installation=installation;c.desktop=desktop;c.kdeVersion=kdeVersion;
  const auto result=FileManagerIntegration::apply(c);
  QVERIFY(result.firstRun);QCOMPARE(result.announce,announce);
  QCOMPARE(result.toJson()["announce"].toBool(),announce);
  // Only the first run: the stamp is written either way, so no notice comes later.
  const auto again=FileManagerIntegration::apply(c);
  QVERIFY(!again.firstRun);QVERIFY(!again.announce);
 }
 void firstRunNeedsTheFileManagerOnPathAndItsEntry() {
  // PCManFM isn't installed: nothing to name.
  Fixture missing;auto& a=missing.context;a.installation="appimage";a.desktop="LXDE";
  QVERIFY(FileManagerIntegration::apply(a).restartable.isEmpty());
  // The AppImage skipped its copy for the system install's entry: this run added nothing.
  Fixture skipped;auto& b=skipped.context;b.installation="appimage";b.desktop="LXDE";
  write(QDir(b.searchPath).filePath("pcmanfm"),"#!/bin/sh\n",programMode);
  write(QDir(b.dataDirs[0]).filePath("file-manager/actions/putgd-upload.desktop"),"[Desktop Entry]\n");
  QVERIFY(FileManagerIntegration::apply(b).restartable.isEmpty());
  // A menu switched off: no first run, no notice.
  Fixture off;auto& d=off.context;d.installation="appimage";d.desktop="LXDE";d.enabled=false;
  write(QDir(d.searchPath).filePath("pcmanfm"),"#!/bin/sh\n",programMode);
  const auto result=FileManagerIntegration::apply(d);
  QVERIFY(!result.firstRun);QVERIFY(result.restartable.isEmpty());
 }
 void pcmanfmRestartBringsBackOnlyTheDesktop() {
  QVERIFY(FileManagerIntegration::restartScript("thunar").isEmpty());
  QCOMPARE(FileManagerIntegration::restartScript("pcmanfm"),FileManagerIntegration::restartScript("pcmanfm-qt"));
  Fixture f;
  // A unique process name, so a real PCManFM on this machine stays out of it.
  const auto name=QStringLiteral("putgdfm%1").arg(QCoreApplication::applicationPid());
  const auto stub=QDir(f.context.searchPath).filePath(name),starts=f.path("starts"),displays=f.path("displays");
  auto quoted=starts,quotedDisplays=displays;quoted.replace("'","'\\''");quotedDisplays.replace("'","'\\''");
  // The start line comes last: once it's there, the stub is ready for SIGTERM.
  write(stub,("#!/bin/sh\ntrap 'exit 0' TERM\nprintf '%s\\n' \"$WAYLAND_DISPLAY\" >> '"+quotedDisplays+"'\nprintf '%s %s\\n' \"$#\" \"$*\" >> '"+quoted+"'\nwhile :; do sleep 0.05; done\n").toUtf8(),programMode);
  const auto lines=[&]{return QString::fromUtf8(readAll(starts)).split('\n',Qt::SkipEmptyParts);};
  const auto restart=[&](QProcess& process){process.start("/bin/sh",{"-c",FileManagerIntegration::restartScript("pcmanfm"),"putgd-restart",name});QVERIFY(process.waitForStarted());};
  {
   QProcess desktop;desktop.start(stub,{"--desktop","--profile","the LXDE"});
   PUTGD_TRY_COMPARE(lines().size(),1);
   QProcess second;restart(second);
   // The first exits cleanly on SIGTERM; its arguments, spaces included, come back.
   QVERIFY(desktop.waitForFinished(5000));QCOMPARE(desktop.exitCode(),0);
   PUTGD_TRY_COMPARE(lines(),QStringList({"3 --desktop --profile the LXDE","3 --desktop --profile the LXDE"}));
   second.terminate();QVERIFY(second.waitForFinished(5000));
  }
  QFile::remove(starts);
  {
   // Just a window: it closes and nothing reopens it.
   QProcess window;window.start(stub,{"/some/folder"});
   PUTGD_TRY_COMPARE(lines().size(),1);
   QProcess second;restart(second);
   QVERIFY(window.waitForFinished(5000));
   QVERIFY(second.waitForFinished(5000));QCOMPARE(second.exitCode(),0);
   QCOMPARE(lines(),QStringList{"1 /some/folder"});
  }
  for(const auto* variable:{"DISPLAY","WAYLAND_DISPLAY"}) {
   // Another session's, on another display: left alone, and nothing starts here.
   QFile::remove(starts);
   QProcess other;auto environment=QProcessEnvironment::systemEnvironment();environment.insert(variable,"putgd-other");
   other.setProcessEnvironment(environment);other.start(stub,{"--desktop"});
   PUTGD_TRY_COMPARE(lines().size(),1);
   QProcess second;restart(second);
   QVERIFY(second.waitForFinished(5000));QCOMPARE(second.exitCode(),0);
   QCOMPARE(other.state(),QProcess::Running);QCOMPARE(lines().size(),1);
   other.terminate();QVERIFY(other.waitForFinished(5000));
  }
  {
   // Wayland's default display, named by one side only, is the same display.
   QFile::remove(starts);QFile::remove(displays);
   QProcess desktop;auto environment=QProcessEnvironment::systemEnvironment();environment.insert("WAYLAND_DISPLAY","wayland-0");
   desktop.setProcessEnvironment(environment);desktop.start(stub,{"--desktop"});
   PUTGD_TRY_COMPARE(lines().size(),1);
   QProcess second;environment.remove("WAYLAND_DISPLAY");second.setProcessEnvironment(environment);restart(second);
   QVERIFY(desktop.waitForFinished(5000));
   PUTGD_TRY_COMPARE(lines().size(),2);
   // It comes back on the display it had, not put.gd's lack of one.
   PUTGD_TRY_COMPARE(QString::fromUtf8(readAll(displays)),QString("wayland-0\nwayland-0\n"));
   second.terminate();QVERIFY(second.waitForFinished(5000));
  }
  // Not running: nothing to do, and nothing starts.
  QFile::remove(starts);
  QProcess none;restart(none);QVERIFY(none.waitForFinished(5000));QCOMPARE(none.exitCode(),0);
  QVERIFY(lines().isEmpty());
 }
 void scriptsKeepArgumentBoundaries() {
  Fixture f;auto& c=f.context;c.installation="system";f.installDing();
  QVERIFY(!FileManagerIntegration::apply(c).error);
  const auto folder=f.path("folder with 'quotes'");QVERIFY(QDir().mkpath(folder));
  const QStringList names{"name with spaces.txt","it's \"quoted\".txt","-leading","${USER}.txt","$(touch pwned).txt"};
  QCOMPARE(f.run(f.ding(),names+QStringList{"/absolute/a b.txt"},folder,{{"NAUTILUS_SCRIPT_SELECTED_FILE_PATHS","/x\n/y\n"}}),0);
  QStringList expected{"--upload","--"};
  const auto real=QFileInfo(folder).canonicalFilePath();
  for(const auto& name:names)expected<<real+"/"+name;
  expected<<"/absolute/a b.txt";
  QCOMPARE(f.received(),expected);
  QVERIFY(!QFileInfo::exists(QDir(folder).filePath("pwned")));
 }
 void nautilusScriptWithoutPathsNotifiesAndDoesNotLaunch() {
  Fixture f;auto& c=f.context;c.installation="system";f.installDing();
  QVERIFY(!FileManagerIntegration::apply(c).error);
  const auto notes=f.path("notes");auto quoted=notes;quoted.replace("'","'\\''");
  write(QDir(c.searchPath).filePath("notify-send"),("#!/bin/sh\nprintf '%s\\n' \"$@\" > '"+quoted+"'\n").toUtf8(),programMode);
  QCOMPARE(f.run(f.ding(),{}),0);
  QVERIFY(f.received().isEmpty());
  const auto lines=QString::fromUtf8(readAll(notes)).split('\n');
  QVERIFY(lines.contains("These items have no local path"));QVERIFY(lines.contains("--app-name=put.gd"));
 }
 void systemScriptRemovesItselfWhenTheLauncherIsGone() {
  Fixture f;auto& c=f.context;c.installation="system";f.installDing();
  QVERIFY(!FileManagerIntegration::apply(c).error);
  QVERIFY(QFile::remove(c.systemLauncher));
  QCOMPARE(f.run(f.ding(),{"/tmp/a.txt"}),0);
  QVERIFY(!QFileInfo::exists(f.ding()));QVERIFY(f.received().isEmpty());
 }
 void cajaScriptPassesSelectedUrisWithoutPaths() {
  Fixture f;auto& c=f.context;c.installation="appimage";
  auto result=FileManagerIntegration::apply(c);
  QVERIFY2(!result.error,qPrintable(result.error.message));
  // The generated script runs the stable launcher; record what it is given.
  f.launcher(result.launcher);
  QCOMPARE(f.run(f.caja(),{},{},{{"CAJA_SCRIPT_SELECTED_URIS","sftp://host/a%20b.txt\nsmb://host/share/%24%7BUSER%7D.txt\n"}}),0);
  QCOMPARE(f.received(),QStringList({"--upload","--","sftp://host/a%20b.txt","smb://host/share/%24%7BUSER%7D.txt"}));
  QFile::remove(f.receipt);
  QCOMPARE(f.run(f.caja(),{"local.txt"},f.path("home")),0);
  QCOMPARE(f.received(),QStringList({"--upload","--",QFileInfo(f.path("home")).canonicalFilePath()+"/local.txt"}));
 }
 void nautilusScriptPassesUrisFromRecentAndStarred() {
  // Nautilus's Recent view passes an item name that is no file, no file paths, and the item's URI (#1105).
  Fixture f;auto& c=f.context;c.installation="appimage";
  auto result=FileManagerIntegration::apply(c);
  QVERIFY2(!result.error,qPrintable(result.error.message));
  f.launcher(result.launcher);
  QCOMPARE(f.run(f.nautilus(),{"0c78a927"},f.path("home"),{{"NAUTILUS_SCRIPT_SELECTED_URIS","recent:///0c78a927\n"},{"NAUTILUS_SCRIPT_SELECTED_FILE_PATHS",""}}),0);
  QCOMPARE(f.received(),QStringList({"--upload","--","recent:///0c78a927"}));
  // With file paths, the arguments stay what they were.
  QFile::remove(f.receipt);
  QCOMPARE(f.run(f.nautilus(),{"local.txt"},f.path("home"),{{"NAUTILUS_SCRIPT_SELECTED_URIS","file:///x/local.txt\n"},{"NAUTILUS_SCRIPT_SELECTED_FILE_PATHS","/x/local.txt\n"}}),0);
  QCOMPARE(f.received(),QStringList({"--upload","--",QFileInfo(f.path("home")).canonicalFilePath()+"/local.txt"}));
 }
 void staleSystemRecordSharesNothing() {
  // A removed DEB leaves its record naming the DING script; the AppImage then owns its own copy.
  Fixture f;auto& c=f.context;f.installDing();
  auto system=c;system.installation="system";
  QVERIFY(!FileManagerIntegration::apply(system).error);
  QVERIFY(QFile::remove(c.systemLauncher));QVERIFY(QFile::remove(f.ding()));
  c.installation="appimage";
  auto result=FileManagerIntegration::apply(c);
  QVERIFY2(!result.error,qPrintable(result.error.message));
  QVERIFY(QFileInfo::exists(f.ding()));QVERIFY(!result.skipped.contains(f.ding()));
  c.enabled=false;result=FileManagerIntegration::apply(c);
  QVERIFY(!result.error);QVERIFY(!QFileInfo::exists(f.ding()));
 }
 void appImageWritesEveryPerUserFile() {
  Fixture f;auto& c=f.context;c.installation="appimage";f.installCosmic();
  auto result=FileManagerIntegration::apply(c);
  QVERIFY2(!result.error,qPrintable(result.error.message));
  QVERIFY(result.firstRun);
  const auto own=QDir(c.dataRoot).filePath("file-manager"),launcher=own+"/putgd-upload",icon=own+"/putgd.svg";
  QCOMPARE(result.launcher,launcher);
  QCOMPARE(mode(launcher),programMode);QCOMPARE(readAll(icon),readAll(c.iconSource));QCOMPARE(mode(icon),dataMode);
  const auto launcherText=QString::fromUtf8(readAll(launcher));
  QVERIFY(launcherText.contains("configured='"+QString(c.appImage).replace("'","'\\''")+"'\n"));
  QVERIFY(launcherText.contains("update_information='"+QString::fromUtf8(updateInformation)+"'\n"));
  QVERIFY(launcherText.contains("name_version()"));QVERIFY(!launcherText.contains("__PUTGD_"));
  QVERIFY(launcherText.contains("--expand-environment=no"));
  // KIO, Thunar and Contractor quote for desktop entries; Nemo and libfm split like a shell.
  auto desktop=launcher;desktop.replace("\\","\\\\").replace("\"","\\\"");
  const auto desktopExec="Exec=\""+desktop+"\" --upload -- ";
  // Key-file escaping doubles the backslash of the shell quoting.
  const auto shellExec="Exec='"+QString(launcher).replace("'","'\\\\''")+"' --upload -- %F\n";
  const QStringList execs{desktopExec+"%F\n",shellExec,desktopExec+"%U\n",shellExec,desktopExec+"%U\n"};
  for(int i=0;i<menuFiles.size();++i) {
   const auto path=f.menu(menuFiles[i]);const auto text=QString::fromUtf8(readAll(path));
   QVERIFY2(result.written.contains(path),qPrintable(path));
   QVERIFY2(text.contains(execs[i]),qPrintable(text));
   QVERIFY(text.contains("\nName=Upload with put.gd\n"));
   QVERIFY(text.contains((i==1?"\nIcon-Name=":"\nIcon=")+icon+'\n'));
   QVERIFY(text.contains("]\nX-putgd-Managed=true\n"));
   QCOMPARE(mode(path),i==0?programMode:dataMode);
  }
  QVERIFY(QString::fromUtf8(readAll(f.menu(menuFiles[0]))).contains("\nMimeType=all/all;\n"));
  QVERIFY(QString::fromUtf8(readAll(f.menu(menuFiles[0]))).contains("\nX-KDE-Priority=TopLevel\n"));
  QVERIFY(!QString::fromUtf8(readAll(f.menu(menuFiles[0]))).contains("X-KDE-Protocols"));
  QVERIFY(QString::fromUtf8(readAll(f.menu(menuFiles[1]))).contains("\nSelection=notnone\nExtensions=any;\n"));
  QVERIFY(!QString::fromUtf8(readAll(f.menu(menuFiles[2]))).contains("\nMimeType="));
  QVERIFY(QString::fromUtf8(readAll(f.menu(menuFiles[3]))).contains("\nMimeTypes=all/all;\n"));
  QVERIFY(QString::fromUtf8(readAll(f.menu(menuFiles[4]))).contains("\nMimeType=application/octet-stream;inode/directory;\n"));
  // Scripts: Nautilus (no system plugin), not DING ($HOME differs, DING absent), Caja.
  for(const auto& path:{f.nautilus(),f.caja()}) {
   QVERIFY(result.written.contains(path));QCOMPARE(mode(path),programMode);
   QVERIFY(readAll(path).contains("launcher='"+QString(launcher).replace("'","'\\''").toUtf8()+"'\n"));
  }
  QVERIFY(readAll(f.caja()).contains("kind='caja'\n"));
  QVERIFY(!QFileInfo::exists(f.ding()));
  // RON escapes the backslash of the shell quoting.
  QVERIFY(readAll(f.cosmic()).contains("steps: [\"'"+QString(launcher).replace("'","'\\\\''").toUtf8()+"' --upload -- %F\"]"));
  const auto record=QString::fromUtf8(readAll(own+"/appimage.owned"));
  for(const auto& path:QStringList{launcher,icon,f.nautilus(),f.caja()}+QStringList{f.menu(menuFiles[0])})QVERIFY2(record.contains("\nfile "+path+'\n'),qPrintable(record));
  QVERIFY(record.contains("\ncosmic "+f.cosmic()+'\n'));QVERIFY(record.contains("\nimage "+c.appImage+'\n'));
  QVERIFY(record.contains("\nupdate "+QString::fromUtf8(updateInformation)+'\n'));
  QCOMPARE(readAll(own+"/appimage.stamp"),QByteArray("26.1031.1\n"));
  result=FileManagerIntegration::apply(c);
  QVERIFY(!result.error);QVERIFY(!result.firstRun);QVERIFY(result.written.isEmpty());QVERIFY(result.removed.isEmpty());
 }
 void appImageOffRemovesOnlyOwnedFiles() {
  Fixture f;auto& c=f.context;c.installation="appimage";f.installCosmic();f.installDing();
  // Someone else's files, and a user file where put.gd would write its own.
  write(f.menu("nemo/actions/other.nemo_action"),"[Nemo Action]\n");
  write(f.menu("Thunar/sendto/putgd-upload.desktop"),"[Desktop Entry]\nName=mine\n");
  const auto unrelated=QByteArray("[\n    (name: \"Mine\", confirm: true, selection: Files, steps: [\"xdg-open %F\"]),\n]\n");
  write(f.cosmic(),unrelated);
  auto result=FileManagerIntegration::apply(c);
  QVERIFY(!result.error);QVERIFY(result.skipped.contains(f.menu("Thunar/sendto/putgd-upload.desktop")));
  QVERIFY(QFileInfo::exists(f.ding()));QVERIFY(QFileInfo::exists(f.nautilus()));
  QCOMPARE(readAll(f.cosmic()+".putgd-backup"),unrelated);
  QVERIFY(readAll(f.cosmic()).startsWith(unrelated.left(unrelated.size()-2)));
  const auto owned=result.written;
  c.enabled=false;result=FileManagerIntegration::apply(c);
  QVERIFY2(!result.error,qPrintable(result.error.message));
  for(const auto& path:owned)if(path!=f.cosmic())QVERIFY2(!QFileInfo::exists(path),qPrintable(path));
  QCOMPARE(readAll(f.cosmic()),unrelated);
  QCOMPARE(readAll(f.menu("nemo/actions/other.nemo_action")),QByteArray("[Nemo Action]\n"));
  QCOMPARE(readAll(f.menu("Thunar/sendto/putgd-upload.desktop")),QByteArray("[Desktop Entry]\nName=mine\n"));
  QVERIFY(!QFileInfo::exists(QDir(c.dataRoot).filePath("file-manager/appimage.owned")));
  // Turned back on: the files return, without a second first-run notice.
  c.enabled=true;result=FileManagerIntegration::apply(c);
  QVERIFY(!result.error);QVERIFY(!result.firstRun);QVERIFY(QFileInfo::exists(f.menu(menuFiles[0])));
 }
 void appImageSkipsWhatTheSystemInstallProvides() {
  Fixture f;auto& c=f.context;c.installation="appimage";f.installCosmic();f.installDing();
  auto result=FileManagerIntegration::apply(c);
  QVERIFY(!result.error);QVERIFY(QFileInfo::exists(f.menu(menuFiles[0])));QVERIFY(QFileInfo::exists(f.caja()));
  // A DEB arrives: its menu files and plugins, then its resident's per-user files.
  for(const auto& relative:menuFiles)write(QDir(c.dataDirs[0]).filePath(relative),"[Desktop Entry]\n");
  f.installPlugin("nautilus/extensions-4","libputgd-nautilus.so");f.installPlugin("caja/extensions-2.0","libputgd-caja.so");
  auto system=c;system.installation="system";
  QVERIFY(!FileManagerIntegration::apply(system).error);
  QVERIFY(readAll(f.ding()).contains("launcher='"+c.systemLauncher.toUtf8()+"'\n"));
  result=FileManagerIntegration::apply(c);
  QVERIFY2(!result.error,qPrintable(result.error.message));
  for(const auto& relative:menuFiles) {
   QVERIFY(result.skipped.contains(f.menu(relative)));QVERIFY(result.removed.contains(f.menu(relative)));QVERIFY(!QFileInfo::exists(f.menu(relative)));
  }
  QVERIFY(result.skipped.contains(f.caja()));QVERIFY(!QFileInfo::exists(f.caja()));
  // Nautilus has the plugin; the DING script is the system install's and stays.
  QVERIFY(!QFileInfo::exists(f.nautilus()));
  QVERIFY(result.skipped.contains(f.ding()));QVERIFY(readAll(f.ding()).contains("launcher='"+c.systemLauncher.toUtf8()+"'\n"));
  // One COSMIC entry: the system install's.
  QVERIFY(result.skipped.contains(f.cosmic()));
  const auto cosmic=QString::fromUtf8(readAll(f.cosmic()));
  QCOMPARE(cosmic.count("(name: "),1);QVERIFY(cosmic.contains(c.systemLauncher));
  QVERIFY(!QString::fromUtf8(readAll(QDir(c.dataRoot).filePath("file-manager/appimage.owned"))).contains("\ncosmic "));
 }
 void cosmicMergeLeavesOtherEntriesUnchanged() {
  Fixture f;auto& c=f.context;c.installation="appimage";f.installCosmic();
  const QByteArray layouts[]={
   "[\n    (name: \"Open\", confirm: false, selection: Any, steps: [\"xdg-open %F\"]),\n]\n",
   "// mine\n[ContextActionPreset(name:r#\"Say \"hi\"\"#,selection:folders,steps:[\"echo \\\"\\u{e9}\\\" %F\",],),/* note */ (confirm:true)]",
   "[]",
   "",
  };
  for(const auto& original:layouts) {
   write(f.cosmic(),original);QFile::remove(f.cosmic()+".putgd-backup");
   auto on=c;on.enabled=true;
   auto result=FileManagerIntegration::apply(on);
   QVERIFY2(!result.error,qPrintable(result.error.message+" "+original));
   const auto text=readAll(f.cosmic());
   QCOMPARE(text.count("Upload with put.gd"),1);
   if(original.contains("Open"))QVERIFY(text.startsWith(original.left(original.size()-2)));
   if(original.contains("Say"))QVERIFY(text.contains("[ContextActionPreset(name:r#\"Say \"hi\"\"#,selection:folders,steps:[\"echo \\\"\\u{e9}\\\" %F\",],),/* note */ (confirm:true),\n"));
   QCOMPARE(readAll(f.cosmic()+".putgd-backup"),original);
   // Applying again changes nothing.
   QVERIFY(!FileManagerIntegration::apply(on).written.contains(f.cosmic()));
   auto off=c;off.enabled=false;
   result=FileManagerIntegration::apply(off);
   QVERIFY2(!result.error,qPrintable(result.error.message));
   const auto after=readAll(f.cosmic());
   QVERIFY(!after.contains("Upload with put.gd"));
   // A pretty list round-trips byte for byte; put.gd's line leaves its own break in a compact one.
   if(original.contains("Open"))QCOMPARE(after,original);
   if(original=="[]")QCOMPARE(after,QByteArray("[\n]"));
   if(original.isEmpty())QCOMPARE(after,QByteArray("[\n]\n"));
   if(original.contains("Say"))QVERIFY(after.startsWith("// mine\n[ContextActionPreset(name:r#\"Say \"hi\"\"#,selection:folders,steps:[\"echo \\\"\\u{e9}\\\" %F\",],),/* note */ (confirm:true),"));
  }
 }
 void unparseableCosmicValueIsLeftAlone() {
  Fixture f;auto& c=f.context;c.installation="appimage";f.installCosmic();
  for(const auto& broken:{QByteArray("[(name: \"x\", bogus: 1)]"),QByteArray("[(name: \"x\""),QByteArray("#![enable(implicit_some)]\n[]"),
       QByteArray("[(selection: Some)]"),QByteArray("[(name: \"\\q\")]"),QByteArray("[\"\xff\"]")}) {
   write(f.cosmic(),broken);QFile::remove(f.cosmic()+".putgd-backup");
   const auto result=FileManagerIntegration::apply(c);
   QCOMPARE(result.error.code,QString("cosmic_context_actions_unparseable"));
   QCOMPARE(readAll(f.cosmic()),broken);QVERIFY(!QFileInfo::exists(f.cosmic()+".putgd-backup"));
   // Everything else is still written.
   QVERIFY(QFileInfo::exists(f.menu(menuFiles[0])));
  }
 }
 void dingScriptFollowsTheDingRule() {
  // AppImage beside a system Nautilus plugin: only DING needs the script.
  Fixture f;auto& c=f.context;c.installation="appimage";f.installPlugin("nautilus/extensions-4","libputgd-nautilus.so");
  auto result=FileManagerIntegration::apply(c);
  QVERIFY(!result.error);QVERIFY(!QFileInfo::exists(f.nautilus()));QVERIFY(!QFileInfo::exists(f.ding()));
  // A user-installed DING counts too; it reads $HOME/.local/share whatever XDG_DATA_HOME says.
  write(QDir(c.home).filePath(".local/share/gnome-shell/extensions/ding@rastersoft.com/metadata.json"),"{}\n");
  result=FileManagerIntegration::apply(c);
  QVERIFY(result.written.contains(f.ding()));QVERIFY(!QFileInfo::exists(f.nautilus()));
 }
 void deletedAppImageCleanupRemovesOwnedFilesOnly() {
  Fixture f;auto& c=f.context;c.installation="appimage";f.installCosmic();
  const SearchPath tools(c.searchPath);
  const auto unrelated=QByteArray("[\n    (name: \"Mine\", steps: [\"xdg-open %F\"]),\n]\n");
  write(f.cosmic(),unrelated);
  QVERIFY(!FileManagerIntegration::apply(c).error);
  QVERIFY(!FileManagerIntegration::cleanupIfMissing(c));
  QVERIFY(QFileInfo::exists(f.menu(menuFiles[0])));
  // An update wrote the next image under a new name: not missing.
  QVERIFY(QFile::remove(c.appImage));
  write(f.path("Applications/putgd-26.1101.1-x86_64.AppImage"),elfImage(updateInformation),programMode);
  QVERIFY(!FileManagerIntegration::cleanupIfMissing(c));
  QVERIFY(QFileInfo::exists(f.menu(menuFiles[0])));
  // Another channel's image is not this installation.
  QVERIFY(QFile::remove(f.path("Applications/putgd-26.1101.1-x86_64.AppImage")));
  write(f.path("Applications/putgd-26.1101.2-x86_64.AppImage"),elfImage("zsync|https://app.put.gd/dl/stable/appimage.zsync"),programMode);
  write(f.menu("kio/servicemenus/other.desktop"),"[Desktop Entry]\n");
  const auto error=FileManagerIntegration::cleanupIfMissing(c);
  QVERIFY2(!error,qPrintable(error.message));
  for(const auto& relative:menuFiles)QVERIFY(!QFileInfo::exists(f.menu(relative)));
  QVERIFY(!QFileInfo::exists(f.nautilus()));QVERIFY(!QFileInfo::exists(f.caja()));
  QVERIFY(!QFileInfo::exists(FileManagerIntegration::launcherPath(c)));
  QVERIFY(!QFileInfo::exists(QDir(c.dataRoot).filePath("file-manager/appimage.owned")));
  QCOMPARE(readAll(f.cosmic()),unrelated);QVERIFY(QFileInfo::exists(f.menu("kio/servicemenus/other.desktop")));
 }
 void launcherShellCleanupMatchesTheRecord() {
  // The cleanup service has no put.gd binary: the launcher's own shell copy runs.
  Fixture f;auto& c=f.context;c.installation="appimage";f.installCosmic();
  const auto unrelated=QByteArray("[\n    (name: \"Mine\", steps: [\"xdg-open %F\"]),\n]\n");
  write(f.cosmic(),unrelated);
  const auto result=FileManagerIntegration::apply(c);
  QVERIFY(!result.error);
  QCOMPARE(f.run(result.launcher,{"--cleanup-if-missing"}),0);
  QVERIFY(QFileInfo::exists(f.menu(menuFiles[0])));
  QVERIFY(QFile::remove(c.appImage));
  QCOMPARE(f.run(result.launcher,{"--cleanup-if-missing"}),0);
  for(const auto& path:result.written)if(path!=f.cosmic())QVERIFY2(!QFileInfo::exists(path),qPrintable(path));
  QCOMPARE(readAll(f.cosmic()),unrelated);
  QVERIFY(!QFileInfo::exists(QDir(c.dataRoot).filePath("file-manager/appimage.owned")));
 }
 void appImageLauncherStartsTheCurrentImage() {
  Fixture f;auto& c=f.context;c.installation="appimage";
  // A stand-in image that records its arguments; the cgroup leaf is a scope.
  f.launcher(c.appImage);write(f.path("cgroup"),"0::/user.slice/user-1000.slice/user@1000.service/app.slice/app-nautilus-1.scope\n");
  c.cgroupFile=f.path("cgroup");
  const auto result=FileManagerIntegration::apply(c);
  QVERIFY(!result.error);
  QCOMPARE(f.run(result.launcher,{"--upload","--","/a b/${USER}.txt"}),0);
  QCOMPARE(f.received(),QStringList({"--upload","--","/a b/${USER}.txt"}));
 }
 // A user manager at this runtime directory; nothing answers the socket (#1112).
 static void managerSocket(const QString& runtime) {
  QVERIFY(QDir().mkpath(runtime+"/systemd"));
  const int fd=::socket(AF_UNIX,SOCK_STREAM,0);QVERIFY(fd>=0);
  sockaddr_un address{};address.sun_family=AF_UNIX;const auto bytes=QFile::encodeName(runtime+"/systemd/private");
  QVERIFY(size_t(bytes.size())<sizeof(address.sun_path));std::memcpy(address.sun_path,bytes.constData(),size_t(bytes.size()));
  QCOMPARE(::bind(fd,reinterpret_cast<const sockaddr*>(&address),sizeof(address)),0);::close(fd);
 }
 void monitorFailureNamesItsCause() {
  // A short root: a socket path may not pass 107 bytes, however long TMPDIR is.
  QTemporaryDir runtimeRoot("/tmp/pg-rt-XXXXXX");QVERIFY(runtimeRoot.isValid());
  Fixture f;auto& c=f.context;c.installation="appimage";c.userRuntime=runtimeRoot.path();managerSocket(c.userRuntime);
  QVERIFY(!FileManagerIntegration::apply(c).error);
  write(f.path("bin/systemd-run"),"#!/bin/sh\nprintf 'synthetic refusal\\n' >&2\nexit 1\n",programMode);
  const auto runtime=qgetenv("XDG_RUNTIME_DIR");qunsetenv("XDG_RUNTIME_DIR");
  SearchPath search(f.path("bin"));
  const auto error=FileManagerIntegration::monitor(c,true);
  if(!runtime.isNull())qputenv("XDG_RUNTIME_DIR",runtime);
  QCOMPARE(error.code,QString("file_manager_monitor"));
  QCOMPARE(error.details["stage"].toString(),QString("exit"));QCOMPARE(error.details["exitCode"].toInt(),1);
  QVERIFY2(error.details["reason"].toString().contains("synthetic refusal"),qPrintable(error.details["reason"].toString()));
  QVERIFY(error.message.contains("deleted"));
 }
 void monitorWithoutAUserManagerIsNoError() {
  Fixture f;auto& c=f.context;c.installation="appimage";c.userRuntime=f.path("no-manager");
  QVERIFY(!FileManagerIntegration::apply(c).error);
  const auto runtime=qgetenv("XDG_RUNTIME_DIR");qunsetenv("XDG_RUNTIME_DIR");
  const auto armed=FileManagerIntegration::monitor(c,true),stopped=FileManagerIntegration::monitor(c,false);
  if(!runtime.isNull())qputenv("XDG_RUNTIME_DIR",runtime);
  QVERIFY2(!armed,qPrintable(armed.message));QVERIFY2(!stopped,qPrintable(stopped.message));
 }
 void aProblemIsAnnouncedOnceUntilItChangesOrClears() {
  Fixture f;auto& c=f.context;c.installation="appimage";
  const Error first{"file_manager_monitor","first",{}},second{"file_manager_monitor","second",{}};
  QVERIFY(FileManagerIntegration::announceOnce(c,first));
  QVERIFY(!FileManagerIntegration::announceOnce(c,first));
  QVERIFY(FileManagerIntegration::announceOnce(c,second));
  QVERIFY(!FileManagerIntegration::announceOnce(c,Error{}));
  QVERIFY(FileManagerIntegration::announceOnce(c,second));
 }
 void toJsonCarriesTheResult() {
  FileManagerResult result;result.firstRun=true;result.written={"/a"};result.restartable={"caja"};result.error={"x","y",{}};
  const auto json=result.toJson();
  QCOMPARE(json["firstRun"].toBool(),true);QCOMPARE(json["written"].toArray().first().toString(),QString("/a"));
  QCOMPARE(json["restartable"].toArray().first().toString(),QString("caja"));QCOMPARE(json["error"].toObject()["code"].toString(),QString("x"));
 }
};
QTEST_GUILESS_MAIN(FileManagerIntegrationTest)
#include "test_file_manager_integration.moc"
