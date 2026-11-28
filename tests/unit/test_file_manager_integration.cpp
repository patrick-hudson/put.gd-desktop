#include "app/FileManagerIntegration.h"
#include "fixtures/ElfImage.h"
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
const QString encryptedScript=QStringLiteral("Upload encrypted with put.gd"); // #2119
QByteArray readAll(const QString& path) {QFile file(path);return file.open(QIODevice::ReadOnly)?file.readAll():QByteArray{};}
void write(const QString& path,const QByteArray& bytes,QFile::Permissions mode=dataMode) {
 QVERIFY(QDir().mkpath(QFileInfo(path).path()));
 QFile file(path);QVERIFY(file.open(QIODevice::WriteOnly|QIODevice::Truncate));file.write(bytes);file.close();
 QVERIFY(file.setPermissions(mode));
}
QFile::Permissions mode(const QString& path) {return QFileInfo(path).permissions()&modeBits;}
using putgd::test::elfImage;
const QByteArray updateInformation="zsync|https://app.put.gd/dl/nightly/appimage.zsync";
// A package's menu file as the DEB/RPM installs it, byte for byte (#2132).
QByteArray packaged(const QString& relative) {return FileManagerIntegration::packagedMenu(relative);}
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
  context.systemLauncher=path("system bin/putgd-launcher");context.stateRoot=path("xdg state/putgd");context.systemOwner=::getuid();context.appImage=path("Applications/putgd-26.1031.1-x86_64.AppImage");
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
 QString ding(const QString& name=script) const {return QDir(context.home).filePath(".local/share/nautilus/scripts/"+name);}
 QString nautilus(const QString& name=script) const {return QDir(context.dataHome).filePath("nautilus/scripts/"+name);}
 QString caja(const QString& name=script) const {return QDir(context.configHome).filePath("caja/scripts/"+name);}
 QString menu(const QString& relative) const {return QDir(context.dataHome).filePath(relative);}
 QString cosmic() const {return FileManagerIntegration::cosmicPath(context);}
 void installDing() const {write(QDir(context.dataDirs[0]).filePath("gnome-shell/extensions/ding@rastersoft.com/metadata.json"),"{}\n");}
 // The package's menu file and, where it has one, its plain or encrypted sibling, as the DEB/RPM installs them.
 void package(const QString& relative,const QString& root={}) const {
  const QDir data(root.isEmpty()?context.dataDirs[0]:root);
  write(data.filePath(relative),packaged(relative));
  if(relative.startsWith("kio/"))return;
  auto sibling=relative;
  if(sibling.contains("-encrypted"))sibling.remove("-encrypted");else sibling.insert(sibling.indexOf("putgd-upload")+12,"-encrypted");
  write(data.filePath(sibling),packaged(sibling));
 }
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
// Upload encrypted with put.gd (#2119): KIO's is a second action in its one service menu.
const QStringList encryptedMenuFiles{"nemo/actions/putgd-upload-encrypted.nemo_action","Thunar/sendto/putgd-upload-encrypted.desktop",
 "file-manager/actions/putgd-upload-encrypted.desktop","contractor/putgd-upload-encrypted.contract"};
const QStringList allMenuFiles=menuFiles+encryptedMenuFiles;
}
class FileManagerIntegrationTest:public QObject {
 Q_OBJECT
private slots:
 void systemInstallWritesItsPerUserFilesWhereDesktopsNeedThem() {
  Fixture f;auto& c=f.context;c.installation="system";
  auto result=FileManagerIntegration::apply(c);
  QVERIFY2(!result.error,qPrintable(result.error.message));
  QVERIFY(result.firstRun);QVERIFY(!result.announce);QVERIFY(result.written.isEmpty());QVERIFY(result.restartable.isEmpty());
  QVERIFY(!QFileInfo::exists(f.ding()));QVERIFY(!QFileInfo::exists(f.cosmic()));
  QCOMPARE(readAll(QDir(c.dataRoot).filePath("file-manager/system.stamp")),QByteArray("26.1031.1\n"));
  // DING installed and COSMIC Files added later: both appear at the next start.
  f.installDing();f.installCosmic();
  result=FileManagerIntegration::apply(c);
  QVERIFY(!result.error);QVERIFY(!result.firstRun);
  QCOMPARE(result.written,QStringList({f.ding(),f.ding(encryptedScript),f.cosmic()}));
  QVERIFY(!QFileInfo::exists(f.nautilus()));
  const auto text=readAll(f.ding());
  QVERIFY(text.startsWith("#!/bin/sh\n"));QVERIFY(text.contains("\n# X-putgd-Managed=true\n"));
  QVERIFY(text.contains("launcher='"+c.systemLauncher.toUtf8()+"'\n"));
  QCOMPARE(mode(f.ding()),programMode);
  QVERIFY(text.contains("\nencrypt=false\n"));QVERIFY(readAll(f.ding(encryptedScript)).contains("\nencrypt=true\n"));
  QCOMPARE(mode(f.ding(encryptedScript)),programMode);
  QCOMPARE(QString::fromUtf8(readAll(f.cosmic())),"[\n    (name: \"Upload with put.gd\", confirm: false, selection: Any, steps: [\"'"+c.systemLauncher+
   "' --upload -- %F\"]),\n    (name: \"Upload encrypted with put.gd\", confirm: false, selection: Any, steps: [\"'"+c.systemLauncher+
   "' --upload --encrypt -- %F\"]),\n]\n");
  QCOMPARE(mode(f.cosmic()),dataMode);
  QVERIFY(!QFileInfo::exists(f.cosmic()+".putgd-backup"));
  QVERIFY(FileManagerIntegration::apply(c).written.isEmpty());
  // DING removed: the script goes; the COSMIC entry stays while COSMIC Files is installed.
  QVERIFY(QDir(QDir(c.dataDirs[0]).filePath("gnome-shell")).removeRecursively());
  result=FileManagerIntegration::apply(c);
  QCOMPARE(result.removed,QStringList({f.ding(),f.ding(encryptedScript)}));QVERIFY(!QFileInfo::exists(f.ding()));QVERIFY(QFileInfo::exists(f.cosmic()));
 }
 void systemFirstRunHasNoNotice_data() {
  QTest::addColumn<QString>("desktop");QTest::addColumn<QString>("plugin");QTest::addColumn<QString>("manager");
  // Fedora's RPM on GNOME: the package installed the Files plugin, so no notice and no Restart Files.
  QTest::newRow("GNOME")<<"GNOME"<<"nautilus/extensions-4"<<"nautilus";
  QTest::newRow("MATE")<<"MATE"<<"caja/extensions-2.0"<<"caja";
  QTest::newRow("UKUI")<<"UKUI"<<"peony-extensions"<<"peony";
  QTest::newRow("LXDE")<<"LXDE"<<""<<"pcmanfm";
  QTest::newRow("LXQt")<<"LXQt"<<""<<"pcmanfm-qt";
  QTest::newRow("KDE")<<"KDE"<<""<<"dolphin";
  QTest::newRow("Cinnamon")<<"X-Cinnamon"<<""<<"nemo";
 }
 void systemFirstRunHasNoNotice() {
  QFETCH(QString,desktop);QFETCH(QString,plugin);QFETCH(QString,manager);
  Fixture f;auto& c=f.context;c.installation="system";c.desktop=desktop;
  if(!plugin.isEmpty())f.installPlugin(plugin,"libputgd-"+manager+".so");
  f.package("file-manager/actions/putgd-upload.desktop");
  write(QDir(c.searchPath).filePath(manager),"#!/bin/sh\n",programMode);
  auto result=FileManagerIntegration::apply(c);
  QVERIFY2(!result.error,qPrintable(result.error.message));
  QVERIFY(result.firstRun);QVERIFY(!result.announce);QVERIFY(result.restartable.isEmpty());
  QVERIFY(!result.toJson()["announce"].toBool());
  result=FileManagerIntegration::apply(c);
  QVERIFY(!result.firstRun);QVERIFY(!result.announce);
  // An upgrade keeps the stamp: no second notice.
  c.version="26.1101.1";result=FileManagerIntegration::apply(c);
  QVERIFY(!result.firstRun);QCOMPARE(readAll(QDir(c.dataRoot).filePath("file-manager/system.stamp")),QByteArray("26.1101.1\n"));
 }
 void firstRunNamesOnlyTheSessionsFileManager_data() {
  QTest::addColumn<QString>("desktop");QTest::addColumn<QStringList>("restartable");
  QTest::newRow("LXDE names PCManFM")<<"LXDE"<<QStringList{"pcmanfm"};
  QTest::newRow("LXQt names PCManFM-Qt, not PCManFM")<<"LXQt"<<QStringList{"pcmanfm-qt"};
  QTest::newRow("GNOME has no plugin")<<"GNOME"<<QStringList{};
  QTest::newRow("MATE")<<"MATE"<<QStringList{};
  QTest::newRow("XFCE")<<"XFCE"<<QStringList{};
  QTest::newRow("Cinnamon")<<"X-Cinnamon"<<QStringList{};
  QTest::newRow("KDE")<<"KDE"<<QStringList{};
  QTest::newRow("Budgie names no file manager")<<"Budgie:LXDE"<<QStringList{};
  QTest::newRow("Two desktops: the first decides")<<"LXQt:LXDE"<<QStringList{"pcmanfm-qt"};
  QTest::newRow("Two desktops, the other way round")<<"LXDE:LXQt"<<QStringList{"pcmanfm"};
  QTest::newRow("COSMIC")<<"COSMIC"<<QStringList{};
 }
 void firstRunNamesOnlyTheSessionsFileManager() {
  QFETCH(QString,desktop);QFETCH(QStringList,restartable);
  Fixture f;auto& c=f.context;c.installation="appimage";c.desktop=desktop;
  for(const auto* name:{"nautilus","caja","peony","pcmanfm","pcmanfm-qt"})write(QDir(c.searchPath).filePath(name),"#!/bin/sh\n",programMode);
  if(desktop=="COSMIC")f.installCosmic();
  const auto result=FileManagerIntegration::apply(c);
  QVERIFY2(!result.error,qPrintable(result.error.message));
  QVERIFY(result.firstRun);QVERIFY(result.announce);QCOMPARE(result.restartable,restartable);
 }
 void firstRunAnnouncesOnlyAnEntryThatExists_data() {
  QTest::addColumn<QString>("desktop");QTest::addColumn<QString>("kdeVersion");QTest::addColumn<QString>("installation");QTest::addColumn<bool>("announce");
  QTest::newRow("UKUI AppImage")<<"UKUI"<<""<<"appimage"<<false;
  QTest::newRow("UKUI DEB")<<"UKUI"<<""<<"system"<<false;
  QTest::newRow("Plasma 5 AppImage")<<"KDE"<<"5"<<"appimage"<<false;
  QTest::newRow("Plasma 5 DEB")<<"KDE"<<"5"<<"system"<<false;
  QTest::newRow("Plasma 6 DEB")<<"KDE"<<"6"<<"system"<<false;
  QTest::newRow("GNOME RPM")<<"GNOME"<<""<<"system"<<false;
  QTest::newRow("Cinnamon DEB")<<"X-Cinnamon"<<""<<"system"<<false;
  QTest::newRow("Plasma 6 AppImage")<<"KDE"<<"6"<<"appimage"<<true;
  QTest::newRow("Deepin")<<"Deepin"<<""<<"appimage"<<false;
  QTest::newRow("Trinity")<<"TDE"<<""<<"appimage"<<false;
  QTest::newRow("Enlightenment")<<"Enlightenment"<<""<<"appimage"<<false;
  QTest::newRow("LXDE AppImage")<<"LXDE"<<""<<"appimage"<<true;
  QTest::newRow("COSMIC without COSMIC Files")<<"COSMIC"<<""<<"appimage"<<false;
  QTest::newRow("Cinnamon")<<"X-Cinnamon"<<""<<"appimage"<<true;
 }
 void firstRunAnnouncesCosmicWithCosmicFiles() {
  Fixture f;auto& c=f.context;c.installation="appimage";c.desktop="COSMIC";f.installCosmic();
  QVERIFY(FileManagerIntegration::apply(c).announce);
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
  skipped.package("file-manager/actions/putgd-upload.desktop");
  QVERIFY(FileManagerIntegration::apply(b).restartable.isEmpty());
  // A menu switched off: no first run, no notice.
  Fixture off;auto& d=off.context;d.installation="appimage";d.desktop="LXDE";d.enabled=false;
  write(QDir(d.searchPath).filePath("pcmanfm"),"#!/bin/sh\n",programMode);
  const auto result=FileManagerIntegration::apply(d);
  QVERIFY(!result.firstRun);QVERIFY(result.restartable.isEmpty());
 }
 void pcmanfmRestartBringsBackOnlyTheDesktop() {
  QVERIFY(FileManagerIntegration::restartScript("thunar").isEmpty());
  Fixture f;
  // A unique process name, so a real PCManFM on this machine stays out of it.
  const auto name=QStringLiteral("putgdfm%1").arg(QCoreApplication::applicationPid());
  const auto stub=QDir(f.context.searchPath).filePath(name),starts=f.path("starts");
  auto quoted=starts;quoted.replace("'","'\\''");
  // The start line comes last: once it's there, the stub is ready for SIGTERM.
  write(stub,("#!/bin/sh\ntrap 'exit 0' TERM\nprintf '%s %s\\n' \"$#\" \"$*\" >> '"+quoted+"'\nwhile :; do sleep 0.05; done\n").toUtf8(),programMode);
  const auto lines=[&]{return QString::fromUtf8(readAll(starts)).split('\n',Qt::SkipEmptyParts);};
  const auto restart=[&](QProcess& process){process.start("/bin/sh",{"-c",FileManagerIntegration::restartScript("pcmanfm"),"putgd-restart",name});QVERIFY(process.waitForStarted(putgd::test::scaled(5000)));};
  for(const auto* variable:{"DISPLAY","WAYLAND_DISPLAY"}) {
   // Another session's, on another display: left alone, and nothing starts here.
   QFile::remove(starts);
   QProcess other;auto environment=QProcessEnvironment::systemEnvironment();environment.insert(variable,"putgd-other");
   other.setProcessEnvironment(environment);other.start(stub,{"--desktop"});
   // The stub can write its marker before Qt consumes the startup notification.
   // Establish Running before checking that the restart leaves it untouched.
   QVERIFY(other.waitForStarted(putgd::test::scaled(5000)));
   PUTGD_TRY_COMPARE(lines().size(),1);
   QProcess second;restart(second);
   PUTGD_TRY_COMPARE(second.state(),QProcess::NotRunning);QCOMPARE(second.exitCode(),0);
   QCOMPARE(other.state(),QProcess::Running);QCOMPARE(lines().size(),1);
   other.terminate();PUTGD_TRY_COMPARE(other.state(),QProcess::NotRunning);
  }
  // Not running: nothing to do, and nothing starts.
  QFile::remove(starts);
  QProcess none;restart(none);PUTGD_TRY_COMPARE(none.state(),QProcess::NotRunning);QCOMPARE(none.exitCode(),0);
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
  // Upload encrypted with put.gd (#2119): the same paths, with --encrypt before them.
  QFile::remove(f.receipt);
  QCOMPARE(f.run(f.ding(encryptedScript),names+QStringList{"/absolute/a b.txt"},folder,{{"NAUTILUS_SCRIPT_SELECTED_FILE_PATHS","/x\n/y\n"}}),0);
  expected.insert(1,"--encrypt");
  QCOMPARE(f.received(),expected);
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
  // The encrypted entry passes the same URIs, encrypted (#2119).
  QFile::remove(f.receipt);
  QCOMPARE(f.run(f.caja(encryptedScript),{},{},{{"CAJA_SCRIPT_SELECTED_URIS","sftp://host/a%20b.txt\n"}}),0);
  QCOMPARE(f.received(),QStringList({"--upload","--encrypt","--","sftp://host/a%20b.txt"}));
 }
 void nautilusScriptPassesUrisFromRecentAndStarred() {
  // Nautilus's Recent view passes an item name that is no file, no file paths, and the item's URI (#1105).
  Fixture f;auto& c=f.context;c.installation="appimage";
  auto result=FileManagerIntegration::apply(c);
  QVERIFY2(!result.error,qPrintable(result.error.message));
  f.launcher(result.launcher);
  QCOMPARE(f.run(f.nautilus(),{"0c78a927"},f.path("home"),{{"NAUTILUS_SCRIPT_SELECTED_URIS","recent:///0c78a927\n"},{"NAUTILUS_SCRIPT_SELECTED_FILE_PATHS",""}}),0);
  QCOMPARE(f.received(),QStringList({"--upload","--","recent:///0c78a927"}));
  QFile::remove(f.receipt);
  QCOMPARE(f.run(f.nautilus(encryptedScript),{"0c78a927"},f.path("home"),{{"NAUTILUS_SCRIPT_SELECTED_URIS","recent:///0c78a927\n"},{"NAUTILUS_SCRIPT_SELECTED_FILE_PATHS",""}}),0);
  QCOMPARE(f.received(),QStringList({"--upload","--encrypt","--","recent:///0c78a927"}));
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
  // Upload encrypted with put.gd (#2119): KIO's second action, then a file of its own for the others.
  const auto kio=QString::fromUtf8(readAll(f.menu(menuFiles[0])));
  QVERIFY(kio.contains("\nActions=putgdUpload;putgdUploadEncrypted;\n"));
  QVERIFY2(kio.contains("[Desktop Action putgdUploadEncrypted]\nName=Upload encrypted with put.gd\nIcon="+icon+"\n"+desktopExec.chopped(3)+"--encrypt -- %F\n"),qPrintable(kio));
  const QString encryptedDesktopExec="Exec=\""+desktop+"\" --upload --encrypt -- ",encryptedShellExec="Exec='"+QString(launcher).replace("'","'\\\\''")+"' --upload --encrypt -- %F\n";
  const QStringList encryptedExecs{encryptedShellExec,encryptedDesktopExec+"%U\n",encryptedShellExec,encryptedDesktopExec+"%U\n"};
  for(int i=0;i<encryptedMenuFiles.size();++i) {
   const auto path=f.menu(encryptedMenuFiles[i]);const auto text=QString::fromUtf8(readAll(path));
   QVERIFY2(result.written.contains(path),qPrintable(path));
   QVERIFY2(text.contains(encryptedExecs[i]),qPrintable(text));QVERIFY(!text.contains("--upload -- "));
   QVERIFY(text.contains("\nName=Upload encrypted with put.gd\n"));
   QVERIFY(text.contains((i==0?"\nIcon-Name=":"\nIcon=")+icon+'\n'));
   QVERIFY(text.contains("]\nX-putgd-Managed=true\n"));
   QCOMPARE(mode(path),dataMode);
  }
  // Scripts: Nautilus (no system plugin), not DING ($HOME differs, DING absent), Caja.
  for(const auto& path:{f.nautilus(),f.caja(),f.nautilus(encryptedScript),f.caja(encryptedScript)}) {
   QVERIFY(result.written.contains(path));QCOMPARE(mode(path),programMode);
   QVERIFY(readAll(path).contains("launcher='"+QString(launcher).replace("'","'\\''").toUtf8()+"'\n"));
   QVERIFY(readAll(path).contains(path.endsWith(encryptedScript)?"\nencrypt=true\n":"\nencrypt=false\n"));
  }
  QVERIFY(readAll(f.caja()).contains("kind='caja'\n"));
  QVERIFY(!QFileInfo::exists(f.ding()));
  // RON escapes the backslash of the shell quoting.
  QVERIFY(readAll(f.cosmic()).contains("steps: [\"'"+QString(launcher).replace("'","'\\\\''").toUtf8()+"' --upload -- %F\"]"));
  QVERIFY(readAll(f.cosmic()).contains("(name: \"Upload encrypted with put.gd\", confirm: false, selection: Any, steps: [\"'"+QString(launcher).replace("'","'\\\\''").toUtf8()+"' --upload --encrypt -- %F\"]"));
  const auto record=QString::fromUtf8(readAll(own+"/appimage.owned"));
  for(const auto& path:QStringList{launcher,icon,f.nautilus(),f.caja(),f.nautilus(encryptedScript),f.caja(encryptedScript)}+QStringList{f.menu(menuFiles[0]),f.menu(encryptedMenuFiles[0])})QVERIFY2(record.contains("\nfile "+path+'\n'),qPrintable(record));
  // One cosmic-line per entry, after the cosmic file they belong to.
  QCOMPARE(record.count("\ncosmic-line "),2);QVERIFY(record.indexOf("\ncosmic ")<record.indexOf("\ncosmic-line "));
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
  for(const auto& relative:allMenuFiles)f.package(relative);
  f.installPlugin("nautilus/extensions-4","libputgd-nautilus.so");f.installPlugin("caja/extensions-2.0","libputgd-caja.so");
  auto system=c;system.installation="system";
  // The package's start already removes the AppImage's copies its own files replace (#2119).
  const auto systemResult=FileManagerIntegration::apply(system);
  QVERIFY(!systemResult.error);
  for(const auto& relative:allMenuFiles)QVERIFY2(systemResult.removed.contains(f.menu(relative)),qPrintable(relative));
  QVERIFY(readAll(f.ding()).contains("launcher='"+c.systemLauncher.toUtf8()+"'\n"));
  result=FileManagerIntegration::apply(c);
  QVERIFY2(!result.error,qPrintable(result.error.message));
  for(const auto& relative:allMenuFiles) {
   QVERIFY(result.skipped.contains(f.menu(relative)));QVERIFY(!QFileInfo::exists(f.menu(relative)));
  }
  QVERIFY(result.skipped.contains(f.caja()));QVERIFY(!QFileInfo::exists(f.caja()));
  QVERIFY(result.skipped.contains(f.caja(encryptedScript)));QVERIFY(!QFileInfo::exists(f.caja(encryptedScript)));QVERIFY(!QFileInfo::exists(f.nautilus(encryptedScript)));
  QVERIFY(result.skipped.contains(f.ding(encryptedScript)));QVERIFY(readAll(f.ding(encryptedScript)).contains("launcher='"+c.systemLauncher.toUtf8()+"'\n"));
  // Nautilus has the plugin; the DING script is the system install's and stays.
  QVERIFY(!QFileInfo::exists(f.nautilus()));
  QVERIFY(result.skipped.contains(f.ding()));QVERIFY(readAll(f.ding()).contains("launcher='"+c.systemLauncher.toUtf8()+"'\n"));
  // One pair of COSMIC entries: the system install's.
  QVERIFY(result.skipped.contains(f.cosmic()));
  const auto cosmic=QString::fromUtf8(readAll(f.cosmic()));
  QCOMPARE(cosmic.count("(name: "),2);QVERIFY(cosmic.contains(c.systemLauncher));
  QVERIFY(!QString::fromUtf8(readAll(QDir(c.dataRoot).filePath("file-manager/appimage.owned"))).contains("\ncosmic "));
 }
 // #2119: an AppImage's per-user menu file shadows the package's file of the same name, so an
 // older one hides the package's encrypted entry. The package's start moves the AppImage's exact
 // copies aside wherever it has its own, keeps them restorable, and leaves everything else.
 static QString replaced(const Fixture& f) {
  const QDir root(QDir(f.context.stateRoot).filePath("file-manager-replaced"));
  const auto stamps=root.entryList(QDir::Dirs|QDir::NoDotAndDotDot);
  return stamps.size()==1?root.filePath(stamps.first()):QString();
 }
 void systemInstallMovesAsideAppImageCopiesThatShadowItsOwn() {
  Fixture f;auto& c=f.context;c.installation="appimage";
  QVERIFY(!FileManagerIntegration::apply(c).error);
  const auto recordFile=QDir(c.dataRoot).filePath("file-manager/appimage.owned");const auto recordBefore=QString::fromUtf8(readAll(recordFile));
  // A user's own file of that name, which isn't put.gd's, stays whatever happens.
  QVERIFY(QFile::remove(f.menu("contractor/putgd-upload.contract")));
  write(f.menu("contractor/putgd-upload.contract"),"[Contractor Entry]\nName=mine\n");
  const QStringList relatives{"nemo/actions/putgd-upload.nemo_action","nemo/actions/putgd-upload-encrypted.nemo_action","Thunar/sendto/putgd-upload.desktop","Thunar/sendto/putgd-upload-encrypted.desktop","contractor/putgd-upload-encrypted.contract"};
  QHash<QString,QByteArray> before;for(const auto& relative:relatives)before[relative]=readAll(f.menu(relative));
  const auto cajaBefore=readAll(f.caja());const auto cajaMode=mode(f.caja());
  // The package arrives with only some formats: Nemo and Thunar (both entries), Contractor, and the Caja plugin.
  for(const auto& relative:relatives+QStringList{"contractor/putgd-upload.contract"})f.package(relative);
  f.installPlugin("caja/extensions-2.0","libputgd-caja.so");
  auto system=c;system.installation="system";
  auto result=FileManagerIntegration::apply(system);
  QVERIFY2(!result.error,qPrintable(result.error.message));
  const auto backup=replaced(f);QVERIFY(!backup.isEmpty());
  for(const auto& relative:relatives) {
   QVERIFY2(result.removed.contains(f.menu(relative))&&!QFileInfo::exists(f.menu(relative)),qPrintable(relative));
   // Restorable: the same bytes and mode under data/<relative>.
   QCOMPARE(readAll(backup+"/data/"+relative),before[relative]);QCOMPARE(mode(backup+"/data/"+relative),dataMode);
  }
  QVERIFY(!QFileInfo::exists(f.caja()));QCOMPARE(readAll(backup+"/config/caja/scripts/"+script),cajaBefore);QCOMPARE(mode(backup+"/config/caja/scripts/"+script),cajaMode);
  QVERIFY(!QFileInfo::exists(f.caja(encryptedScript)));QVERIFY(QFileInfo::exists(backup+"/config/caja/scripts/"+encryptedScript));
  // Only where the package has its own copy: the AppImage keeps its Dolphin, PCManFM and Files entries.
  for(const auto& path:{f.menu("kio/servicemenus/putgd-upload.desktop"),f.menu("file-manager/actions/putgd-upload.desktop"),f.nautilus(),f.nautilus(encryptedScript)})
   QVERIFY2(QFileInfo::exists(path)&&!result.removed.contains(path),qPrintable(path));
  QCOMPARE(readAll(f.menu("contractor/putgd-upload.contract")),QByteArray("[Contractor Entry]\nName=mine\n"));
  // The record drops only the moved files; its other lines (identity, image, update, other files) stay as they were.
  const auto record=QString::fromUtf8(readAll(recordFile));
  for(const auto& relative:relatives)QVERIFY(!record.contains("\nfile "+f.menu(relative)+'\n'));
  QVERIFY(!record.contains("\nfile "+f.caja()+'\n'));
  const auto movedLine=[&](const QString& line){
   const auto path=line.startsWith("file ")?line.mid(5):line.startsWith("hash ")?line.mid(70):QString();
   return !path.isEmpty()&&(relatives.contains(QDir(c.dataHome).relativeFilePath(path))||path.startsWith(QDir(c.configHome).filePath("caja/scripts/")));
  };
  QStringList expected;for(const auto& line:recordBefore.split('\n'))if(!movedLine(line))expected<<line;
  QCOMPARE(record,expected.join('\n'));
  // The AppImage's next start skips what the package provides and recreates none of it.
  result=FileManagerIntegration::apply(c);
  QVERIFY2(!result.error,qPrintable(result.error.message));
  for(const auto& relative:relatives)QVERIFY2(!QFileInfo::exists(f.menu(relative)),qPrintable(relative));
  QVERIFY(QFileInfo::exists(f.menu("kio/servicemenus/putgd-upload.desktop")));
 }
 // A removed AppImage record leaves its files behind; their exact bytes still identify them.
 void systemInstallMovesAsideOrphanedAppImageCopies() {
  Fixture f;auto& c=f.context;c.installation="appimage";
  QVERIFY(!FileManagerIntegration::apply(c).error);
  QVERIFY(QFile::remove(QDir(c.dataRoot).filePath("file-manager/appimage.owned")));
  QVERIFY(QFile::remove(FileManagerIntegration::launcherPath(c)));
  for(const auto& relative:allMenuFiles)f.package(relative);
  auto system=c;system.installation="system";
  const auto result=FileManagerIntegration::apply(system);
  QVERIFY2(!result.error,qPrintable(result.error.message));
  for(const auto& relative:allMenuFiles)QVERIFY2(!QFileInfo::exists(f.menu(relative))&&QFileInfo::exists(replaced(f)+"/data/"+relative),qPrintable(relative));
  // No record to update: none is created.
  QVERIFY(!QFileInfo::exists(QDir(c.dataRoot).filePath("file-manager/appimage.owned")));
 }
 // An older AppImage's KIO service menu (only Upload with put.gd) is recognized byte for byte too.
 void systemInstallRecognizesTheEarlierKioTemplate() {
  Fixture f;auto& c=f.context;
  const auto launcher=FileManagerIntegration::launcherPath(c),icon=QDir(c.dataRoot).filePath("file-manager/putgd.svg");
  const auto quote=[](QString v){return v.contains(' ')||v.contains('\'')?"\""+v.replace("\\","\\\\").replace("\"","\\\"")+"\"":v;};
  const QByteArray legacy="# \"Upload with put.gd\" in Dolphin and Konqueror (#1105). A KIO service menu for\n# files and folders (all/all) on every protocol, so remote folders show it too\n"
   "# (KIOFuse or kioexec supplies the path), at the top level of the menu.\n[Desktop Entry]\nX-putgd-Managed=true\nType=Service\nMimeType=all/all;\nActions=putgdUpload;\nX-KDE-Priority=TopLevel\n\n"
   "[Desktop Action putgdUpload]\nName=Upload with put.gd\nIcon="+icon.toUtf8()+"\nExec="+quote(launcher).toUtf8()+" --upload -- %F\n";
  write(f.menu("kio/servicemenus/putgd-upload.desktop"),legacy,programMode);
  f.package("kio/servicemenus/putgd-upload.desktop");
  c.installation="system";
  const auto result=FileManagerIntegration::apply(c);
  QVERIFY2(!result.error,qPrintable(result.error.message));
  QVERIFY(!QFileInfo::exists(f.menu("kio/servicemenus/putgd-upload.desktop")));
  QCOMPARE(readAll(replaced(f)+"/data/kio/servicemenus/putgd-upload.desktop"),legacy);QCOMPARE(mode(replaced(f)+"/data/kio/servicemenus/putgd-upload.desktop"),programMode);
 }
 // Only an exact AppImage copy moves: an edited one, a lookalike naming the launcher in a comment,
 // a menu folder that is a link, and a package file that isn't put.gd's all leave things alone.
 void systemInstallLeavesAnythingElse() {
  const auto nemo=QStringLiteral("nemo/actions/putgd-upload.nemo_action");
  {
   Fixture f;auto& c=f.context;c.installation="appimage";QVERIFY(!FileManagerIntegration::apply(c).error);
   // Customized: still marked, still runs the launcher, one line added.
   const auto edited=readAll(f.menu(nemo))+"Quote=single\n";write(f.menu(nemo),edited);
   // A lookalike that only mentions the launcher in a comment.
   const auto lookalike="[Desktop Entry]\nX-putgd-Managed=true\n# "+FileManagerIntegration::launcherPath(c).toUtf8()+"\nName=mine\n";
   write(f.menu("Thunar/sendto/putgd-upload.desktop"),lookalike);
   for(const auto* relative:{"nemo/actions/putgd-upload.nemo_action","Thunar/sendto/putgd-upload.desktop"})f.package(relative);
   auto system=c;system.installation="system";const auto result=FileManagerIntegration::apply(system);
   QVERIFY2(!result.error,qPrintable(result.error.message));
   QCOMPARE(readAll(f.menu(nemo)),edited);QCOMPARE(readAll(f.menu("Thunar/sendto/putgd-upload.desktop")),lookalike);
   // Only the exact encrypted copies beside them moved.
   QVERIFY(!QFileInfo::exists(replaced(f)+"/data/"+nemo));QVERIFY(!QFileInfo::exists(replaced(f)+"/data/Thunar/sendto/putgd-upload.desktop"));
   QVERIFY(QFileInfo::exists(replaced(f)+"/data/nemo/actions/putgd-upload-encrypted.nemo_action"));
  }
  {
   // The user's Nemo actions folder is a link to somewhere else.
   Fixture f;auto& c=f.context;c.installation="appimage";QVERIFY(!FileManagerIntegration::apply(c).error);
   const auto elsewhere=f.path("elsewhere");QVERIFY(QDir().rename(f.menu("nemo/actions"),elsewhere));QVERIFY(QFile::link(elsewhere,f.menu("nemo/actions")));
   f.package(nemo);
   auto system=c;system.installation="system";const auto result=FileManagerIntegration::apply(system);
   QVERIFY2(!result.error,qPrintable(result.error.message));
   QVERIFY(QFileInfo::exists(elsewhere+"/putgd-upload.nemo_action"));QVERIFY(replaced(f).isEmpty());
  }
  {
   // A system file of that name that isn't put.gd's: no counterpart. The AppImage keeps writing its copy.
   Fixture f;auto& c=f.context;c.installation="appimage";
   write(QDir(c.dataDirs[0]).filePath(nemo),"[Nemo Action]\nName=Other\nExec=/usr/bin/other %F\n");
   QVERIFY(!FileManagerIntegration::apply(c).error);QVERIFY(QFileInfo::exists(f.menu(nemo)));
   auto system=c;system.installation="system";const auto result=FileManagerIntegration::apply(system);
   QVERIFY2(!result.error,qPrintable(result.error.message));QVERIFY(QFileInfo::exists(f.menu(nemo)));QVERIFY(replaced(f).isEmpty());
  }
  {
   // An XDG_DATA_DIRS entry that is the user's own folder under another name: its "system" copy is the AppImage's own file.
   Fixture f;auto& c=f.context;c.installation="appimage";QVERIFY(!FileManagerIntegration::apply(c).error);
   const auto alias=f.path("alias of data");QVERIFY(QFile::link(c.dataHome,alias));c.dataDirs={alias};
   // The AppImage doesn't skip its own file either.
   QVERIFY(!FileManagerIntegration::apply(c).skipped.contains(f.menu(nemo)));
   auto system=c;system.installation="system";const auto result=FileManagerIntegration::apply(system);
   QVERIFY2(!result.error,qPrintable(result.error.message));QVERIFY(QFileInfo::exists(f.menu(nemo)));QVERIFY(replaced(f).isEmpty());
  }
 }
 // #2132 N1: when a package arrives, the AppImage's own pass never deletes a copy changed since it wrote it: it moves it aside.
 void appImageMovesAsideItsChangedCopyInsteadOfDeleting() {
  Fixture f;auto& c=f.context;c.installation="appimage";QVERIFY(!FileManagerIntegration::apply(c).error);
  const auto nemo=QStringLiteral("nemo/actions/putgd-upload.nemo_action");
  const auto edited=readAll(f.menu(nemo))+"Quote=single\n";write(f.menu(nemo),edited);
  f.package(nemo);
  const auto result=FileManagerIntegration::apply(c);
  QVERIFY2(!result.error,qPrintable(result.error.message));QVERIFY(result.skipped.contains(f.menu(nemo)));
  QVERIFY(!QFileInfo::exists(f.menu(nemo)));QCOMPARE(readAll(replaced(f)+"/data/"+nemo),edited);
  // Turned off, an exact copy goes; a changed one is moved aside too.
  const auto thunar=QStringLiteral("Thunar/sendto/putgd-upload.desktop");const auto changed=readAll(f.menu(thunar))+"# mine\n";write(f.menu(thunar),changed);
  auto off=c;off.enabled=false;off.replacedStamp="off";QVERIFY(!FileManagerIntegration::apply(off).error);
  QVERIFY(!QFileInfo::exists(f.menu("kio/servicemenus/putgd-upload.desktop")));QVERIFY(!QFileInfo::exists(f.menu(thunar)));
  QCOMPARE(readAll(QDir(c.stateRoot).filePath("file-manager-replaced/off/data/"+thunar)),changed);
  QVERIFY(!QFileInfo::exists(QDir(c.stateRoot).filePath("file-manager-replaced/off/data/kio/servicemenus/putgd-upload.desktop")));
 }
 // #2132 N2: an older package's menu file (only Upload with put.gd) or a lookalike isn't the package's copy.
 // #2132 N2, L5, S5: the package's counterpart is a packaged put.gd file offering both entries, in
 // the highest-priority system folder that has the file.
 void onlyThePackagesFileWithBothEntriesIsACounterpart() {
  const auto kio=QStringLiteral("kio/servicemenus/putgd-upload.desktop"),nemo=QStringLiteral("nemo/actions/putgd-upload.nemo_action");
  const auto counted=[](Fixture& f,const QString& relative){f.context.installation="appimage";const auto result=FileManagerIntegration::apply(f.context);return !result.error&&result.skipped.contains(f.menu(relative));};
  {Fixture f;f.package(kio);QVERIFY(counted(f,kio));}
  // Not byte for byte this build's, but packaged with both actions: still the package's.
  {Fixture f;write(QDir(f.context.dataDirs[0]).filePath(kio),packaged(kio)+"\n# a later comment\n");QVERIFY(counted(f,kio));}
  // The old plain-only KIO menu offers only Upload with put.gd.
  {Fixture f;write(QDir(f.context.dataDirs[0]).filePath(kio),"[Desktop Entry]\nActions=putgdUpload;\n\n[Desktop Action putgdUpload]\nName=Upload with put.gd\nExec=/usr/bin/putgd-launcher --upload -- %F\n");QVERIFY(!counted(f,kio));}
  // Nemo's plain action without its encrypted sibling, and a lookalike that runs something else.
  {Fixture f;write(QDir(f.context.dataDirs[0]).filePath(nemo),packaged(nemo));QVERIFY(!counted(f,nemo));}
  {Fixture f;f.package(nemo);write(QDir(f.context.dataDirs[0]).filePath(nemo),"[Nemo Action]\nName=Upload with put.gd\nExec=/opt/other --upload -- %F\n");QVERIFY(!counted(f,nemo));}
  // Not owned like the package's files: here, by "another" owner than the one expected.
  {Fixture f;f.package(nemo);f.context.systemOwner=::getuid()+1;QVERIFY(!counted(f,nemo));}
  // An earlier XDG data folder with a different file of that name decides, even with a packaged one later.
  {Fixture f;const auto first=f.path("first share");write(QDir(first).filePath(nemo),"[Nemo Action]\nName=mine\n");f.package(nemo);f.context.dataDirs.prepend(first);QVERIFY(!counted(f,nemo));}
  // The user's own folder under another name, first in line, decides likewise.
  {Fixture f;const auto alias=f.path("alias of data");QVERIFY(QFile::link(f.context.dataHome,alias));
   QVERIFY(!FileManagerIntegration::apply([&]{auto c=f.context;c.installation="appimage";return c;}()).error);
   f.package(nemo);f.context.dataDirs.prepend(alias);QVERIFY(!counted(f,nemo));}
 }
 // #2132 N3: the package's DING script, recorded under another name for the same folder, is never deleted by the AppImage.
 void appImageNeverDeletesThePackagesDingScriptUnderAnotherName() {
  Fixture f;auto& c=f.context;f.installDing();
  QVERIFY(QDir().mkpath(QDir(c.home).filePath(".local/share")));
  const auto alias=f.path("home alias");QVERIFY(QFile::link(c.home,alias));
  auto image=c;image.installation="appimage";image.home=alias;
  QVERIFY(!FileManagerIntegration::apply(image).error);
  auto system=c;system.installation="system";QVERIFY(!FileManagerIntegration::apply(system).error);
  QVERIFY(readAll(f.ding()).contains("launcher='"+c.systemLauncher.toUtf8()+"'\n"));
  const auto result=FileManagerIntegration::apply(image);
  QVERIFY2(!result.error,qPrintable(result.error.message));
  QVERIFY(QFileInfo::exists(f.ding()));QVERIFY(readAll(f.ding()).contains("launcher='"+c.systemLauncher.toUtf8()+"'\n"));
 }
 // #2132 N4: a move never replaces or removes what is already at its target.
 void moveAsideNeverReplacesAnExistingBackup() {
  Fixture f;auto& c=f.context;c.installation="appimage";QVERIFY(!FileManagerIntegration::apply(c).error);
  const auto nemo=QStringLiteral("nemo/actions/putgd-upload.nemo_action");const auto mine=readAll(f.menu(nemo));
  const auto target=QDir(c.stateRoot).filePath("file-manager-replaced/fixed/data/"+nemo);write(target,"already here\n");
  f.package(nemo);
  auto system=c;system.installation="system";system.replacedStamp="fixed";
  const auto result=FileManagerIntegration::apply(system);
  QCOMPARE(result.error.code,QString("file_manager_io"));
  QCOMPARE(readAll(target),QByteArray("already here\n"));QCOMPARE(readAll(f.menu(nemo)),mine);
 }
 // #2132 S9: a moved entry comes back from its backup with its original bytes and mode.
 void aMovedEntryRestoresFromItsBackup() {
  Fixture f;auto& c=f.context;c.installation="appimage";QVERIFY(!FileManagerIntegration::apply(c).error);
  const auto kio=QStringLiteral("kio/servicemenus/putgd-upload.desktop");const auto original=readAll(f.menu(kio));
  f.package(kio);auto system=c;system.installation="system";system.replacedStamp="restore";
  QVERIFY(!FileManagerIntegration::apply(system).error);QVERIFY(!QFileInfo::exists(f.menu(kio)));
  // What a user does: move the backup back where the file manager reads it.
  QVERIFY(QFile::rename(QDir(c.stateRoot).filePath("file-manager-replaced/restore/data/"+kio),f.menu(kio)));
  QCOMPARE(readAll(f.menu(kio)),original);QCOMPARE(mode(f.menu(kio)),programMode);
 }
 // #2132 S1: an edited DING script is moved aside before the package writes its own.
 void anEditedDingScriptIsMovedAsideBeforeThePackageReplacesIt() {
  Fixture f;auto& c=f.context;c.installation="appimage";f.installDing();QVERIFY(!FileManagerIntegration::apply(c).error);
  const auto edited=readAll(f.ding())+"# my change\n";write(f.ding(),edited,programMode);
  auto system=c;system.installation="system";system.replacedStamp="ding";
  const auto result=FileManagerIntegration::apply(system);
  QVERIFY2(!result.error,qPrintable(result.error.message));
  QVERIFY(readAll(f.ding()).contains("launcher='"+c.systemLauncher.toUtf8()+"'\n"));
  QCOMPARE(readAll(QDir(c.stateRoot).filePath("file-manager-replaced/ding/home/.local/share/nautilus/scripts/"+script)),edited);
  // An exact AppImage script is simply replaced, with nothing to keep.
  QVERIFY(!QFileInfo::exists(QDir(c.stateRoot).filePath("file-manager-replaced/ding/home/.local/share/nautilus/scripts/"+encryptedScript)));
 }
 // #2132 S2, L2: the AppImage is deleted right after the package started, with no AppImage pass in
 // between. Its removal (in process and the launcher's shell) leaves the package's DING scripts and
 // any edited entry, and removes only its own exact files.
 void appImageRemovalLeavesThePackagesAndEditedFiles() {
  for(const bool shell:{false,true}) {
   Fixture f;auto& c=f.context;c.installation="appimage";f.installDing();
   const SearchPath tools(c.searchPath);
   const auto result=FileManagerIntegration::apply(c);QVERIFY(!result.error);
   const auto nemo=QStringLiteral("nemo/actions/putgd-upload.nemo_action");const auto edited=readAll(f.menu(nemo))+"Quote=single\n";write(f.menu(nemo),edited);
   auto system=c;system.installation="system";QVERIFY(!FileManagerIntegration::apply(system).error);
   QVERIFY(QFile::remove(c.appImage));
   if(shell)QCOMPARE(f.run(result.launcher,{"--cleanup-if-missing"}),0);
   else QVERIFY(!FileManagerIntegration::cleanupIfMissing(c));
   QVERIFY2(readAll(f.ding()).contains("launcher='"+c.systemLauncher.toUtf8()+"'\n"),shell?"shell":"in process");
   QVERIFY(readAll(f.ding(encryptedScript)).contains("launcher='"+c.systemLauncher.toUtf8()+"'\n"));
   QCOMPARE(readAll(f.menu(nemo)),edited);
   QVERIFY(!QFileInfo::exists(f.menu("kio/servicemenus/putgd-upload.desktop")));
   QVERIFY(!QFileInfo::exists(QDir(c.dataRoot).filePath("file-manager/appimage.owned")));
  }
 }
 // #2132 S6: a retirement that fails stays recorded and succeeds on a later pass.
 void aFailedRetirementIsRetried() {
  Fixture f;auto& c=f.context;c.installation="appimage";QVERIFY(!FileManagerIntegration::apply(c).error);
  const auto nemo=QStringLiteral("nemo/actions/putgd-upload.nemo_action");const auto edited=readAll(f.menu(nemo))+"Quote=single\n";write(f.menu(nemo),edited);
  f.package(nemo);
  // The backup can't be made: the state folder is a file.
  QVERIFY(QDir().mkpath(QFileInfo(c.stateRoot).path()));write(c.stateRoot,"not a folder\n");
  auto result=FileManagerIntegration::apply(c);
  QCOMPARE(result.error.code,QString("file_manager_io"));QCOMPARE(readAll(f.menu(nemo)),edited);
  QVERIFY(QString::fromUtf8(readAll(QDir(c.dataRoot).filePath("file-manager/appimage.owned"))).contains("\nfile "+f.menu(nemo)+'\n'));
  QVERIFY(QFile::remove(c.stateRoot));c.replacedStamp="retry";
  result=FileManagerIntegration::apply(c);
  QVERIFY2(!result.error,qPrintable(result.error.message));QVERIFY(!QFileInfo::exists(f.menu(nemo)));
  QCOMPARE(readAll(QDir(c.stateRoot).filePath("file-manager-replaced/retry/data/"+nemo)),edited);
  QVERIFY(!QString::fromUtf8(readAll(QDir(c.dataRoot).filePath("file-manager/appimage.owned"))).contains("\nfile "+f.menu(nemo)+'\n'));
 }
 // #2132 L4: an entry written under one spelling of the home folder (/home) is recognized under
 // another (/var/home): here a link to the same home.
 void entriesWrittenUnderAnotherHomeSpellingAreRecognized() {
  Fixture f;
  const auto real=f.path("real home"),alias=f.path("aliased home");QVERIFY(QDir().mkpath(real));QVERIFY(QFile::link(real,alias));
  auto image=f.context;image.installation="appimage";image.home=real;image.dataHome=real+"/.local/share";image.configHome=real+"/.config";image.dataRoot=real+"/.local/share/putgd";
  QVERIFY(!FileManagerIntegration::apply(image).error);
  const auto nemo=QStringLiteral("nemo/actions/putgd-upload.nemo_action");
  f.package(nemo);
  auto system=image;system.installation="system";system.home=alias;system.dataHome=alias+"/.local/share";system.configHome=alias+"/.config";system.dataRoot=alias+"/.local/share/putgd";system.replacedStamp="alias";
  const auto result=FileManagerIntegration::apply(system);
  QVERIFY2(!result.error,qPrintable(result.error.message));
  QVERIFY(!QFileInfo::exists(real+"/.local/share/"+nemo));QVERIFY(QFileInfo::exists(QDir(f.context.stateRoot).filePath("file-manager-replaced/alias/data/"+nemo)));
 }
 // DING's scripts are the package's own: one reached through another name for the same folder isn't moved (#2119).
 void dingScriptComparedCanonically() {
  Fixture f;auto& c=f.context;c.installation="appimage";f.installDing();
  QVERIFY(QDir().mkpath(QDir(c.home).filePath(".local/share")));
  const auto alias=f.path("data alias");QVERIFY(QFile::link(QDir(c.home).filePath(".local/share"),alias));c.dataHome=alias;
  QVERIFY(!FileManagerIntegration::apply(c).error);QVERIFY(QFileInfo::exists(f.ding()));
  f.installPlugin("nautilus/extensions-4","libputgd-nautilus.so");
  auto system=c;system.installation="system";const auto result=FileManagerIntegration::apply(system);
  QVERIFY2(!result.error,qPrintable(result.error.message));
  QVERIFY(!result.removed.contains(QDir(alias).filePath("nautilus/scripts/"+script)));QVERIFY(replaced(f).isEmpty());
  QVERIFY(readAll(f.ding()).contains("launcher='"+c.systemLauncher.toUtf8()+"'\n"));
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
   QCOMPARE(text.count("Upload with put.gd"),1);QCOMPARE(text.count("Upload encrypted with put.gd"),1);
   if(original.contains("Open"))QVERIFY(text.startsWith(original.left(original.size()-2)));
   if(original.contains("Say"))QVERIFY(text.contains("[ContextActionPreset(name:r#\"Say \"hi\"\"#,selection:folders,steps:[\"echo \\\"\\u{e9}\\\" %F\",],),/* note */ (confirm:true),\n"));
   QCOMPARE(readAll(f.cosmic()+".putgd-backup"),original);
   // Applying again changes nothing.
   QVERIFY(!FileManagerIntegration::apply(on).written.contains(f.cosmic()));
   auto off=c;off.enabled=false;
   result=FileManagerIntegration::apply(off);
   QVERIFY2(!result.error,qPrintable(result.error.message));
   const auto after=readAll(f.cosmic());
   QVERIFY(!after.contains("Upload with put.gd"));QVERIFY(!after.contains("Upload encrypted with put.gd"));
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
  for(const auto& relative:allMenuFiles)QVERIFY(!QFileInfo::exists(f.menu(relative)));
  for(const auto& name:{script,encryptedScript}){QVERIFY(!QFileInfo::exists(f.nautilus(name)));QVERIFY(!QFileInfo::exists(f.caja(name)));}
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
  QFile::remove(f.receipt);
  QCOMPARE(f.run(result.launcher,{"--upload","--encrypt","--","/a b"}),0);
  QCOMPARE(f.received(),QStringList({"--upload","--encrypt","--","/a b"}));
 }
 // An older AppImage wrote one COSMIC entry: the next start replaces it with both, in order,
 // and the launcher's shell cleanup later removes both lines (#2119).
 void cosmicSingleEntryBecomesBoth() {
  Fixture f;auto& c=f.context;c.installation="appimage";f.installCosmic();
  const SearchPath tools(c.searchPath);
  const auto launcher=FileManagerIntegration::launcherPath(c);
  const auto quoted="'"+QString(launcher).replace("'","'\\\\''")+"'";
  const auto mine=QByteArray("[\n    (name: \"Mine\", steps: [\"xdg-open %F\"]),\n");
  write(f.cosmic(),mine+"    (name: \"Upload with put.gd\", confirm: false, selection: Any, steps: [\""+quoted.toUtf8()+" --upload -- %F\"]),\n]\n");
  auto result=FileManagerIntegration::apply(c);
  QVERIFY2(!result.error,qPrintable(result.error.message));QVERIFY(result.written.contains(f.cosmic()));
  const auto text=QString::fromUtf8(readAll(f.cosmic()));
  QVERIFY2(text.startsWith(QString::fromUtf8(mine)),qPrintable(text));
  QCOMPARE(text.count("--upload -- %F"),1);QCOMPARE(text.count("--upload --encrypt -- %F"),1);
  QVERIFY(text.indexOf("Upload with put.gd")<text.indexOf("Upload encrypted with put.gd"));
  QVERIFY(!FileManagerIntegration::apply(c).written.contains(f.cosmic()));
  QVERIFY(QFile::remove(c.appImage));
  QCOMPARE(f.run(launcher,{"--cleanup-if-missing"}),0);
  QCOMPARE(readAll(f.cosmic()),mine+"]\n");
  // The backup is the list as it was before the cleanup, with both entries.
  QCOMPARE(QString::fromUtf8(readAll(f.cosmic()+".putgd-backup")),text);
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
