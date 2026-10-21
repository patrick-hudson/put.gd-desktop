#include "core/SystemUpdates.h"
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>
using namespace putgd;
// Which DEB/RPM installs update from put.gd's own repository (#293 phase 2),
// read from the repository file the package adds, and which updater opens.
class SystemUpdatesTest:public QObject {
 Q_OBJECT
 QTemporaryDir root;
 const QString repository="https://pkg.put.gd";
 void write(const QString& path,const QByteArray& text){
  QDir().mkpath(QFileInfo(root.filePath(path.mid(1))).path());QFile file(root.filePath(path.mid(1)));QVERIFY(file.open(QIODevice::WriteOnly));file.write(text);
 }
 QJsonObject action(const QString& target,update::UpdaterChoice choice={}) {return update::systemAction(target,root.path(),repository,choice);}
 static QByteArray sources(const QByteArray& extra={},const QByteArray& uri="https://pkg.put.gd/apt/ubuntu24.04"){
  return "# put.gd nightly packages\nTypes: deb\nURIs: "+uri+"\nSuites: nightly\nComponents: main\nArchitectures: amd64\n"+extra+
   "Signed-By:\n -----BEGIN PGP PUBLIC KEY BLOCK-----\n .\n mQINBGc=\n -----END PGP PUBLIC KEY BLOCK-----\n";
 }
private slots:
 void init(){QVERIFY(root.isValid());QDir(root.path()+"/etc").removeRecursively();}
 void aptSourceInUseMakesTheSystemUpdaterOwnUpdates(){
  write("/etc/apt/sources.list.d/putgd.sources",sources());
  const auto result=action("ubuntu24.04",{true,{"mintupdate","Update Manager","/usr/bin/mintupdate",{}}});
  QCOMPARE(result["kind"].toString(),QString("software-updates"));QCOMPARE(result["manager"].toString(),QString("apt"));
  QCOMPARE(result["channel"].toString(),QString("nightly"));QCOMPARE(result["command"].toString(),QString("sudo apt update && sudo apt upgrade"));
  QCOMPARE(result["updater"].toObject()["id"].toString(),QString("mintupdate"));
  // Without any updater the command is still offered.
  QVERIFY(action("ubuntu24.04")["updater"].isNull());
 }
 void disabledDeletedOrForeignSourcesFallBackToDownload(){
  QCOMPARE(action("debian13")["repository"].toString(),QString("absent"));
  write("/etc/apt/sources.list.d/putgd.sources",sources("Enabled: no\n","https://pkg.put.gd/apt/debian13"));
  QCOMPARE(action("debian13")["kind"].toString(),QString("download"));QCOMPARE(action("debian13")["repository"].toString(),QString("disabled"));
  // Another build label's archive, or another host, isn't this build's repository.
  write("/etc/apt/sources.list.d/putgd.sources",sources({},"https://pkg.put.gd/apt/ubuntu26.04"));
  QCOMPARE(action("debian13")["repository"].toString(),QString("other"));
  write("/etc/apt/sources.list.d/putgd.sources",sources({},"https://mirror.example/apt/debian13"));
  QCOMPARE(action("debian13")["repository"].toString(),QString("other"));
  // AppImage, Flatpak and development builds never have one.
  QCOMPARE(action("appimage")["repository"].toString(),QString("none"));
  QCOMPARE(action("")["repository"].toString(),QString("none"));
 }
 void dnfRepositoryFollowsItsEnabledFlag(){
  const QByteArray repo="[putgd-nightly]\nname=put.gd (nightly)\nbaseurl=https://pkg.put.gd/rpm/fedora/44/x86_64/nightly\nenabled=1\ngpgcheck=0\nrepo_gpgcheck=1\n";
  write("/etc/yum.repos.d/putgd.repo",repo);
  auto result=action("fedora44");QCOMPARE(result["kind"].toString(),QString("software-updates"));QCOMPARE(result["manager"].toString(),QString("dnf"));
  QCOMPARE(result["channel"].toString(),QString("nightly"));QCOMPARE(result["command"].toString(),QString("sudo dnf upgrade --refresh"));
  write("/etc/yum.repos.d/putgd.repo",QByteArray(repo).replace("enabled=1","enabled=0"));
  QCOMPARE(action("fedora44")["repository"].toString(),QString("disabled"));
  // The apt file means nothing to an RPM install.
  QFile::remove(root.filePath("etc/yum.repos.d/putgd.repo"));write("/etc/apt/sources.list.d/putgd.sources",sources());
  QCOMPARE(action("fedora44")["repository"].toString(),QString("absent"));
 }
 void theDesktopsOwnUpdaterComesFirst(){
  const auto all=[](const QString& program){return "/usr/bin/"+program;};
  QCOMPARE(update::softwareUpdater("X-Cinnamon",all).updater.id,QString("mintupdate"));
  QCOMPARE(update::softwareUpdater("KDE",all).updater.id,QString("plasma-discover"));
  QCOMPARE(update::softwareUpdater("KDE",all).updater.arguments,(QStringList{"--mode","update"}));
  QCOMPARE(update::softwareUpdater("ubuntu:GNOME",all).updater.id,QString("gnome-software"));
  // Plasma without Discover (d69) uses GNOME Software.
  const auto noDiscover=[](const QString& program){return program=="plasma-discover"?QString():"/usr/bin/"+program;};
  QCOMPARE(update::softwareUpdater("KDE",[](const QString& p){return p=="gnome-software"?QString("/usr/bin/gnome-software"):QString();}).updater.id,QString("gnome-software"));
  QCOMPARE(update::softwareUpdater("KDE",noDiscover).updater.program,QString("/usr/bin/mintupdate"));
  QVERIFY(!update::softwareUpdater("GNOME",[](const QString&){return QString();}).found);
  // Only the three known updaters are ever started.
  QVERIFY(!update::openSoftwareUpdates({{"kind","software-updates"},{"updater",QJsonObject{{"id","sh"},{"program","/bin/sh"}}}}));
  QVERIFY(!update::openSoftwareUpdates({{"kind","download"}}));
 }
};
QTEST_GUILESS_MAIN(SystemUpdatesTest)
#include "test_system_updates.moc"
