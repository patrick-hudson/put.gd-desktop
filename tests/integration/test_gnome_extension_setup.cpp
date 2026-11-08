#include "platform/gnome/GnomeExtensionSetup.h"
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QtTest>
using namespace putgd::gnome;

// Every path here works on private directories and an in-memory gsettings: no
// Shell, no dconf, no user files.
class GnomeExtensionSetupTest final:public QObject {
 Q_OBJECT
 struct Fixture {
  QTemporaryDir root;
  QHash<QString,QString> settings;
  QStringList writes;
  ShellFacts shell;
  ProcessResult lockedReply;
  bool locked=false;
  SetupInputs inputs(const QString& bundled={},int packaged=19){
   SetupInputs value;
   value.dataHome=root.filePath("home/.local/share");
   value.dataDirs={root.filePath("system")};
   value.bundledDir=bundled;
   value.packagedVersion=packaged;
   value.shell=[this]{return shell;};
   value.gsettings=[this](const QStringList& args)->ProcessResult{
    if(args.value(0)=="get")return {true,0,settings.value(args.value(2))+"\n",{}};
    if(args.value(0)=="writable")return {true,0,locked?"false\n":"true\n",{}};
    if(args.value(0)=="set"){writes.append(args.value(2)+"="+args.value(3));settings.insert(args.value(2),args.value(3));return {true,0,{},{}};}
    return {};
   };
   return value;
  }
  Fixture(){
   settings.insert("enabled-extensions","@as []");settings.insert("disabled-extensions","@as []");settings.insert("disable-user-extensions","false");settings.insert("allow-extension-installation","true");
   shell.version="50.1";
  }
 };
 static void write(const QString& path,const QByteArray& bytes){QDir().mkpath(QFileInfo(path).path());QFile file(path);QVERIFY(file.open(QIODevice::WriteOnly));file.write(bytes);}
 static QString bundle(Fixture& f,int version=19,const QByteArray& code="// v"){
  const auto dir=f.root.filePath("appimage/usr/share/gnome-shell/extensions/putgd@put.gd");
  write(dir+"/metadata.json",QStringLiteral("{\"uuid\":\"putgd@put.gd\",\"version\":%1}").arg(version).toUtf8());
  write(dir+"/extension.js",code);
  return dir;
 }
 static void system(Fixture& f,int version=19){
  const auto dir=f.root.filePath("system/gnome-shell/extensions/putgd@put.gd");
  write(dir+"/metadata.json",QStringLiteral("{\"uuid\":\"putgd@put.gd\",\"version\":%1}").arg(version).toUtf8());
  write(dir+"/extension.js","// system");
 }
 static QString userDir(Fixture& f){return f.root.filePath("home/.local/share/gnome-shell/extensions/putgd@put.gd");}
private slots:
 void gvariantList(){
  QCOMPARE(*parseStringList("@as []"),QStringList{});
  QCOMPARE(*parseStringList("[]"),QStringList{});
  QCOMPARE(*parseStringList("['a@b', 'c@d']"),(QStringList{"a@b","c@d"}));
  QCOMPARE(*parseStringList("['it\\'s', \"say \\\"hi\\\"\"]"),(QStringList{"it's","say \"hi\""}));
  QVERIFY(!parseStringList("not a list"));QVERIFY(!parseStringList("['unterminated"));QVERIFY(!parseStringList("['a',]"));
  QCOMPARE(*appendToStringList("@as []","x@y"),QString("['x@y']"));
  QCOMPARE(*appendToStringList("['a@b', 'c@d']","x@y"),QString("['a@b', 'c@d', 'x@y']"));
 }
 void enablesOnGnome50AndWaitsForLogin(){
  Fixture f;system(f);
  const auto s=runExtensionSetup(f.inputs());
  QCOMPARE(s.state,QString("login_required"));QCOMPARE(s.message,QString("Log out and back in once to finish setting up put.gd."));
  QCOMPARE(f.writes,QStringList{"enabled-extensions=['putgd@put.gd']"});
 }
 void preservesExistingListExactly(){
  Fixture f;system(f);f.settings["enabled-extensions"]="['ding@rastersoft.com', 'user-theme@gnome.org', 'weird\\'id@x']";
  runExtensionSetup(f.inputs());
  QCOMPARE(f.writes,QStringList{"enabled-extensions=['ding@rastersoft.com', 'user-theme@gnome.org', 'weird\\'id@x', 'putgd@put.gd']"});
 }
 void idempotentAndNoWriteWhenEnabled(){
  Fixture f;system(f);f.settings["enabled-extensions"]="['putgd@put.gd']";
  auto s=runExtensionSetup(f.inputs());QCOMPARE(s.state,QString("login_required"));QVERIFY(f.writes.isEmpty());
  f.shell.extension={{"state",1},{"version",19},{"path",f.root.filePath("system/gnome-shell/extensions/putgd@put.gd")}};
  s=runExtensionSetup(f.inputs());QCOMPARE(s.state,QString("ready"));QVERIFY(f.writes.isEmpty());
 }
 void knownExtensionActivatesWithoutLogin(){
  Fixture f;system(f);f.shell.extension={{"state",6},{"version",19},{"path",f.root.filePath("system/gnome-shell/extensions/putgd@put.gd")}};
  auto activated=f.inputs();int calls=0;activated.shell=[&]{auto s=f.shell;if(++calls>1)s.extension["state"]=1;return s;};
  QCOMPARE(runExtensionSetup(activated).state,QString("ready"));QCOMPARE(f.writes.size(),1);
 }
 void unsupportedShellChangesNothing(){
  Fixture f;f.shell.version="49.3";system(f);const auto s=runExtensionSetup(f.inputs());
  QCOMPARE(s.state,QString("unsupported_shell"));QVERIFY(s.needsUser());QVERIFY(f.writes.isEmpty());
 }
 void noShellYetIsNotAFailure(){Fixture f;f.shell.version.clear();const auto s=runExtensionSetup(f.inputs());QCOMPARE(s.state,QString("checking"));QVERIFY(!s.needsUser());}
 void disabledByUserIsRespected(){
  Fixture f;system(f);f.settings["disabled-extensions"]="['putgd@put.gd']";
  const auto s=runExtensionSetup(f.inputs());QCOMPARE(s.state,QString("disabled_by_user"));QVERIFY(f.writes.isEmpty());
 }
 void disableUserExtensionsReportsTheReason(){
  Fixture f;system(f);f.settings["disable-user-extensions"]="true";
  const auto s=runExtensionSetup(f.inputs());QCOMPARE(s.state,QString("user_extensions_disabled"));QVERIFY(s.message.contains("Extensions app"));
  QCOMPARE(f.writes.size(),1);
 }
 void lockedSettingsAreReported(){
  Fixture f;system(f);f.locked=true;const auto s=runExtensionSetup(f.inputs());QCOMPARE(s.state,QString("settings_locked"));QVERIFY(f.writes.isEmpty());
 }
 void gsettingsFailureIsReported(){
  Fixture f;system(f);auto in=f.inputs();in.gsettings=[](const QStringList&)->ProcessResult{return {false,-1,{},{}};};
  const auto s=runExtensionSetup(in);QCOMPARE(s.state,QString("settings_unavailable"));QVERIFY(s.message.contains("gsettings"));
 }
 void nativeWithoutExtensionIsNotInstalled(){Fixture f;const auto s=runExtensionSetup(f.inputs());QCOMPARE(s.state,QString("not_installed"));QVERIFY(f.writes.isEmpty());}
 void appImageInstallsMarkedCopyAndEnables(){
  Fixture f;const auto dir=bundle(f);const auto s=runExtensionSetup(f.inputs(dir));
  QCOMPARE(s.state,QString("login_required"));QVERIFY(QFile::exists(userDir(f)+"/extension.js"));QVERIFY(QFile::exists(userDir(f)+"/metadata.json"));
  QFile marker(userDir(f)+"/.putgd-managed");QVERIFY(marker.open(QIODevice::ReadOnly));const auto doc=QJsonDocument::fromJson(marker.readAll()).object();
  QCOMPARE(doc["managedBy"].toString(),QString("put.gd"));QCOMPARE(doc["extensionVersion"].toInt(),19);
  QCOMPARE(f.writes.size(),1);
  // Nothing left in the extensions folder but the copy itself.
  QCOMPARE(QDir(f.root.filePath("home/.local/share/gnome-shell/extensions")).entryList(QDir::AllEntries|QDir::NoDotAndDotDot|QDir::Hidden),QStringList{"putgd@put.gd"});
 }
 void systemCopyMeansNoUserCopy(){
  Fixture f;system(f);const auto dir=bundle(f);const auto s=runExtensionSetup(f.inputs(dir));
  QCOMPARE(s.state,QString("login_required"));QVERIFY(!QFile::exists(userDir(f)));
 }
 void appImageReplacesItsOwnCopyOnUpdate(){
  Fixture f;auto dir=bundle(f,18,"// old");runExtensionSetup(f.inputs(dir,18));f.writes.clear();f.shell.extension={{"state",1},{"version",18},{"path",userDir(f)}};
  dir=bundle(f,19,"// new");
  const auto s=runExtensionSetup(f.inputs(dir,19));
  QCOMPARE(s.state,QString("login_required"));QVERIFY(f.writes.isEmpty());
  QFile js(userDir(f)+"/extension.js");QVERIFY(js.open(QIODevice::ReadOnly));QCOMPARE(js.readAll(),QByteArray("// new"));
  QCOMPARE(QDir(f.root.filePath("home/.local/share/gnome-shell")).entryList(QDir::AllEntries|QDir::NoDotAndDotDot|QDir::Hidden),QStringList{"extensions"});
 }
 void olderAppImageDoesNotDowngrade(){
  Fixture f;auto dir=bundle(f,19,"// new");runExtensionSetup(f.inputs(dir));f.shell.extension={{"state",1},{"version",19},{"path",userDir(f)}};
  dir=bundle(f,17,"// old");runExtensionSetup(f.inputs(dir,17));
  QFile js(userDir(f)+"/extension.js");QVERIFY(js.open(QIODevice::ReadOnly));QCOMPARE(js.readAll(),QByteArray("// new"));
 }
 void foreignCopyIsLeftAlone(){
  Fixture f;write(userDir(f)+"/extension.js","// mine");write(userDir(f)+"/metadata.json","{\"uuid\":\"putgd@put.gd\",\"version\":5}");
  const auto dir=bundle(f);const auto s=runExtensionSetup(f.inputs(dir));
  QCOMPARE(s.state,QString("foreign_user_copy"));QVERIFY(s.message.contains(userDir(f)));
  QFile js(userDir(f)+"/extension.js");QVERIFY(js.open(QIODevice::ReadOnly));QCOMPARE(js.readAll(),QByteArray("// mine"));
  QVERIFY(f.writes.isEmpty());
 }
 void editedOwnedCopyCountsAsForeign(){
  Fixture f;const auto dir=bundle(f);runExtensionSetup(f.inputs(dir));f.writes.clear();
  write(userDir(f)+"/extension.js","// edited by hand");
  const auto s=runExtensionSetup(f.inputs(dir));QCOMPARE(s.state,QString("foreign_user_copy"));
  QFile js(userDir(f)+"/extension.js");QVERIFY(js.open(QIODevice::ReadOnly));QCOMPARE(js.readAll(),QByteArray("// edited by hand"));
 }
 void ownedCopyRetiredWhenSystemCopyArrives(){
  Fixture f;const auto dir=bundle(f);runExtensionSetup(f.inputs(dir));system(f);
  const auto s=runExtensionSetup(f.inputs(dir));
  QVERIFY(!QDir(userDir(f)).exists());QCOMPARE(s.state,QString("login_required"));
 }
 void olderSystemCopyWithNewerAppImageIsReported(){
  Fixture f;system(f,18);const auto dir=bundle(f,19);
  const auto s=runExtensionSetup(f.inputs(dir,19));QCOMPARE(s.state,QString("system_copy_outdated"));QVERIFY(!QFile::exists(userDir(f)));
 }
 void blockedPerUserExtensions(){
  Fixture f;f.settings["allow-extension-installation"]="false";const auto dir=bundle(f);
  QCOMPARE(runExtensionSetup(f.inputs(dir)).state,QString("extension_blocked"));
 }
 void extensionErrorIsReported(){
  Fixture f;system(f);f.settings["enabled-extensions"]="['putgd@put.gd']";f.shell.extension={{"state",3},{"version",19},{"error","boom"}};
  const auto s=runExtensionSetup(f.inputs());QCOMPARE(s.state,QString("extension_error"));QVERIFY(s.message.contains("boom"));
 }
 void removeDeletesOwnedCopyAndEntry(){
  Fixture f;const auto dir=bundle(f);f.settings["enabled-extensions"]="['other@x']";runExtensionSetup(f.inputs(dir));f.writes.clear();
  const auto s=removeOwnedUserCopy(f.inputs());
  QVERIFY(!s.needsUser());QVERIFY(s.details["removed"].toBool());QVERIFY(!QDir(userDir(f)).exists());
  QCOMPARE(f.writes,QStringList{"enabled-extensions=['other@x']"});
 }
 void markerWithPathsIsForeign(){
  Fixture f;const auto dir=bundle(f);runExtensionSetup(f.inputs(dir));
  write(userDir(f)+"/.putgd-managed",QJsonDocument(QJsonObject{{"managedBy","put.gd"},{"files",QJsonObject{{"../outside","x"}}}}).toJson());
  QCOMPARE(runExtensionSetup(f.inputs(dir)).state,QString("foreign_user_copy"));
 }
 void extraFileMakesCopyForeign(){
  Fixture f;const auto dir=bundle(f);runExtensionSetup(f.inputs(dir));write(userDir(f)+"/notes.txt","mine");
  QCOMPARE(runExtensionSetup(f.inputs(dir)).state,QString("foreign_user_copy"));
  QVERIFY(QFile::exists(userDir(f)+"/notes.txt"));
 }
 void blockedExtensionsLeaveNothingBehind(){
  Fixture f;f.settings["allow-extension-installation"]="false";const auto dir=bundle(f);runExtensionSetup(f.inputs(dir));
  QVERIFY(!QFile::exists(userDir(f)));QVERIFY(f.writes.isEmpty());
 }
 void removeWhenSettingsFailKeepsCopy(){
  Fixture f;const auto dir=bundle(f);runExtensionSetup(f.inputs(dir));
  auto in=f.inputs();in.gsettings=[](const QStringList&)->ProcessResult{return {true,1,{},"no schema"};};
  QCOMPARE(removeOwnedUserCopy(in).state,QString("settings_unavailable"));QVERIFY(QFile::exists(userDir(f)+"/extension.js"));
 }
 void removeKeepsEntryBelongingToSystemPackage(){
  Fixture f;const auto dir=bundle(f);runExtensionSetup(f.inputs(dir));f.writes.clear();system(f);
  QVERIFY(removeOwnedUserCopy(f.inputs()).details["removed"].toBool());
  QVERIFY(!QDir(userDir(f)).exists());QVERIFY(f.writes.isEmpty());
 }
 void removeLeavesForeignCopy(){
  Fixture f;write(userDir(f)+"/metadata.json","{}");const auto s=removeOwnedUserCopy(f.inputs());
  QCOMPARE(s.state,QString("foreign_user_copy"));QVERIFY(QFile::exists(userDir(f)+"/metadata.json"));
 }
};
QTEST_GUILESS_MAIN(GnomeExtensionSetupTest)
#include "test_gnome_extension_setup.moc"
