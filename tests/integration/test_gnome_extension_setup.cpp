#include "platform/gnome/GnomeExtensionSetup.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QtTest>
#include <unistd.h>
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
 // A name installCopy gives a staging ("new") or retired ("old") directory.
 static QString staged(const char* kind,char fill){return QStringLiteral(".putgd-extension-%1-%2").arg(QLatin1String(kind),QString(32,QLatin1Char(fill)));}
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
 void disabledByUserIsRespected(){
  Fixture f;system(f);f.settings["disabled-extensions"]="['putgd@put.gd']";
  const auto s=runExtensionSetup(f.inputs());QCOMPARE(s.state,QString("disabled_by_user"));QVERIFY(f.writes.isEmpty());
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
 void stagingCleanupLeavesAnythingUncertain(){
  Fixture f;const auto dir=bundle(f);const QString parent=f.root.filePath("home/.local/share/gnome-shell/");
  // Matches the old wide glob, but isn't a name put.gd makes.
  const QStringList otherNames{".putgd-extension-backup",staged("new",'b')+"x",staged("new",'B'),".putgd-extension-old-short"};
  for(const auto& name:otherNames)write(parent+name+"/extension.js","mine");
  // Put.gd's exact name, with contents put.gd wouldn't have left.
  const QString extra=parent+staged("new",'c');write(extra+"/extension.js","mine");write(extra+"/notes.txt","mine");
  const QString nested=parent+staged("old",'d');write(nested+"/extension.js","mine");write(nested+"/sub/file","mine");
  const QString linkedFile=parent+staged("new",'e');write(f.root.filePath("elsewhere.txt"),"mine");QDir().mkpath(linkedFile);QVERIFY(QFile::link(f.root.filePath("elsewhere.txt"),linkedFile+"/extension.js"));
  runExtensionSetup(f.inputs(dir));
  for(const auto& name:otherNames)QVERIFY2(QFile::exists(parent+name+"/extension.js"),qPrintable(name));
  QVERIFY(QFile::exists(extra+"/extension.js"));QVERIFY(QFile::exists(extra+"/notes.txt"));
  QVERIFY(QFile::exists(nested+"/extension.js"));QVERIFY(QFile::exists(nested+"/sub/file"));
  QVERIFY(QFileInfo(linkedFile+"/extension.js").isSymLink());QVERIFY(QFile::exists(f.root.filePath("elsewhere.txt")));
 }
 void stagingCleanupDoesNotFollowASymlink(){
  Fixture f;const auto dir=bundle(f);const QString parent=f.root.filePath("home/.local/share/gnome-shell/");
  const QString target=f.root.filePath("target");write(target+"/extension.js","mine");write(target+"/.hidden","mine");
  const QString link=parent+staged("old",'f');QDir().mkpath(parent);QVERIFY(QFile::link(target,link));
  runExtensionSetup(f.inputs(dir));
  QVERIFY(QFileInfo(link).isSymLink());QVERIFY(QFile::exists(target+"/extension.js"));QVERIFY(QFile::exists(target+"/.hidden"));
 }
 void removeLeavesForeignCopy(){
  Fixture f;write(userDir(f)+"/metadata.json","{}");const auto s=removeOwnedUserCopy(f.inputs());
  QCOMPARE(s.state,QString("foreign_user_copy"));QVERIFY(QFile::exists(userDir(f)+"/metadata.json"));
 }
};
QTEST_GUILESS_MAIN(GnomeExtensionSetupTest)
#include "test_gnome_extension_setup.moc"
