#include "app/FileUploads.h"
#include "core/ArtifactStore.h"
#include "app/UploadClient.h"
#include "app/FileManagerIntegration.h"
#include "core/Log.h"
#include "fixtures/U2Fixtures.h"
#include "fixtures/Waits.h"
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <fcntl.h>
#include <unistd.h>
using namespace putgd;
// "Upload with put.gd" from a file manager (#1105): the checks upload.files
// runs before an operation exists, its stored route, and the --upload client's
// pure parts (cgroup leaf, library path, URI resolution and gio parsing).
namespace {
QJsonObject wait(std::function<void(JsonReply)> start,Error* error=nullptr) {
 bool done=false;QJsonObject value;
 start([&](Error e,QJsonObject result){if(error)*error=e;value=result;done=true;});
 if(!putgd::test::waitFor([&]{return done;},20000))qWarning("timed out");
 return value;
}
QJsonObject profile(const QString& id){return {{"id",id},{"label","Synthetic "+id},{"provider","opaque"},{"settings",QJsonObject{{"workspace","fixture-workspace"}}},{"credentials",QJsonObject{{"mode","session"},{"reference",QJsonValue::Null}}}};}
Settings settingsWith(const QJsonArray& profiles,const QJsonObject& captureDefaults={},const QJsonObject& preferences={}){
 auto document=test::configuration(QDir::tempPath());document["profiles"]=profiles;
 auto actions=document["actions"].toObject();for(const auto& name:Settings::actionNames()){auto action=actions[name].toObject();action["profileId"]=profiles.isEmpty()?QJsonValue(QJsonValue::Null):profiles.first().toObject()["id"];actions[name]=action;}document["actions"]=actions;
 if(!captureDefaults.isEmpty())document["captureDefaults"]=captureDefaults;
 auto prefs=document["preferences"].toObject();for(auto it=preferences.begin();it!=preferences.end();++it)prefs[it.key()]=it.value();document["preferences"]=prefs;
 return Settings::fromValidated(document);
}
QString write(const QString& path,const QByteArray& bytes){QDir().mkpath(QFileInfo(path).absolutePath());QFile file(path);if(!file.open(QIODevice::WriteOnly))return {};file.write(bytes);return path;}
}
class FileUploadsContract final:public QObject {
 Q_OBJECT
 QTemporaryDir dir_;
 std::shared_ptr<test::OpaqueProvider> provider_=std::make_shared<test::OpaqueProvider>();
 std::unique_ptr<ProviderRegistry> registry_;std::unique_ptr<SettingsService> settings_;std::unique_ptr<test::Source> source_;
 std::unique_ptr<OperationService> operations_;
 QString files(const QString& name) const {return dir_.filePath("files/"+name);}
 void patch(const QJsonObject& value){
  auto document=settings_->document();for(auto it=value.begin();it!=value.end();++it)document[it.key()]=it.value();
  QVERIFY(settings_->save(document,settings_->revision()));
 }
 QJsonObject finished(const QString& id){
  QJsonObject operation;
  putgd::test::waitFor([&]{operation=wait([&](JsonReply reply){operations_->inspect(id,std::move(reply));});return !Operation::activeState(operation["state"].toString());},30000);
  return operation;
 }
private slots:
 void initTestCase() {
  QVERIFY(dir_.isValid());logging::open(dir_.filePath("state/logs"));
  registry_=std::make_unique<ProviderRegistry>();registry_->add("opaque",provider_);
  auto* registry=registry_.get();
  settings_=std::make_unique<SettingsService>(dir_.filePath("config.json"),[registry](const auto& id,const auto& value){return registry->validate(id,value);});
  QVERIFY(settings_->save(test::configuration(dir_.path()),{}));
  source_=std::make_unique<test::Source>();source_->payload=test::textPayload();
  operations_=std::make_unique<OperationService>(dir_.filePath("data"),*settings_,*registry_,*source_);PUTGD_TRY_VERIFY(operations_->ready());
  operations_->setInstallation("system");
 }
 void cleanupTestCase() {operations_->shutdown();logging::close();}

 // Routing: requested profile, else a capture default with a profile, else the only destination.
 void routeResolution() {
  const auto two=QJsonArray{profile("a"),profile("b")};
  auto route=FileUploads::resolve(settingsWith(two,{{"destination","upload"},{"profileId","b"},{"publicationMode","private"}}));
  QVERIFY(!route.error);QCOMPARE(route.profileId,QString("b"));QCOMPARE(route.publicationMode,QString("private"));
  route=FileUploads::resolve(settingsWith(two,{{"destination","upload"},{"profileId","b"},{"publicationMode","share"}}),"a");
  QCOMPARE(route.profileId,QString("a"));QCOMPARE(route.publicationMode,QString("share"));
  QCOMPARE(FileUploads::resolve(settingsWith(two),"missing").error.code,QString("profile_not_found"));
  // A capture default with a null profile, or a local one, does not choose.
  QCOMPARE(FileUploads::resolve(settingsWith(two,{{"destination","upload"},{"profileId",QJsonValue::Null},{"publicationMode","default"}})).error.code,QString("destination_required"));
  QCOMPARE(FileUploads::resolve(settingsWith(two,{{"destination","local"},{"profileId","a"},{"publicationMode","default"}})).error.code,QString("destination_required"));
  route=FileUploads::resolve(settingsWith(QJsonArray{profile("only")},{{"destination","local"},{"profileId",QJsonValue::Null},{"publicationMode","default"}}));
  QVERIFY(!route.error);QCOMPARE(route.profileId,QString("only"));
  QCOMPARE(FileUploads::resolve(settingsWith({})).error.code,QString("destination_required"));
  // The operation's own settings carry the route and still validate.
  const auto document=FileUploads::routedDocument(settingsWith(two),{{},"b","private"});
  QVERIFY(!Settings::validate(document,[](const QString&,const QJsonObject&){return Error{};}));
  QCOMPARE(document["captureDefaults"]["profileId"].toString(),QString("b"));QCOMPARE(document["captureDefaults"]["publicationMode"].toString(),QString("private"));
 }
 void installationIdentity() {
  QVERIFY(!FileUploads::matchInstallation("system","system"));
  const auto error=FileUploads::matchInstallation("appimage","system");
  QCOMPARE(error.code,QString("installation_mismatch"));QVERIFY(error.message.contains("installed package"));
 }
 // Every bad item is named; nothing is accepted when any is bad.
 void selectionNamesEveryBadFile() {
  const auto settings=settingsWith(QJsonArray{profile("a")});
  const auto good=write(files("good.txt"),"content"),empty=write(files("empty.txt"),{}),newline=write(files("bad\nname.txt"),"x");
  auto selection=FileUploads::select({good},settings,"files",0,-1);
  QVERIFY(!selection.error);QCOMPARE(selection.files,QStringList{good});QCOMPARE(selection.origins,QStringList{good});
  selection=FileUploads::select({good,empty,newline,files("missing.txt"),"relative.txt"},settings,"files",0,-1);
  QCOMPARE(selection.error.code,QString("files_rejected"));QVERIFY(selection.files.isEmpty());
  const auto bad=selection.error.details["files"].toArray();QCOMPARE(bad.size(),4);
  QVERIFY(selection.error.message.contains("empty.txt"));QVERIFY(selection.error.message.contains("missing.txt"));
  // Sizes: the destination's largest object, the file limit, the batch limit and the disk.
  selection=FileUploads::select({good},settings,"files",3,-1);QCOMPARE(selection.error.code,QString("destination_object_too_large"));
  selection=FileUploads::select({good},settings,"files",0,2);QCOMPARE(selection.error.code,QString("disk_space_insufficient"));
  // Out of room only because of the reserve: the message says so, in true sizes.
  ArtifactStore::DiskBudget disk;disk.freeBytes=700*1024*1024;disk.reserveBytes=1024*1024*1024;
  selection=FileUploads::select({good},settings,"files",0,disk);QCOMPARE(selection.error.code,QString("disk_space_insufficient"));
  const auto needs=QFileInfo(good).size();QVERIFY(needs<1024);
  QCOMPARE(selection.error.message,QStringLiteral("Not enough free disk space: this needs %1 B. The disk with put.gd's data folder has 700 MB free, and put.gd keeps 1 GB free for the system. Free up at least 325 MB and try again.").arg(needs));
  // Bytes other uploads hold aren't the system's, and the advice rounds up, so it is always enough.
  disk.reservedBytes=20*1024*1024;
  selection=FileUploads::select({good},settings,"files",0,disk);
  QCOMPARE(selection.error.message,QStringLiteral("Not enough free disk space: this needs %1 B. The disk with put.gd's data folder has 700 MB free, and put.gd keeps 1 GB free for the system and 20 MB for uploads in progress. Free up at least 345 MB and try again.").arg(needs));
  QCOMPARE(selection.error.details["requiredBytes"].toInteger(),needs);QCOMPARE(selection.error.details["availableBytes"].toInteger(),0LL);
  // Spaces, quotes, Unicode and a leading '-' upload.
  for(const auto& name:{QStringLiteral("a b.txt"),QStringLiteral("it's \"q\".txt"),QStringLiteral("é ü.txt"),QStringLiteral("-rf.txt"),QStringLiteral("${USER}.txt")}){
   const auto path=write(files(name),"x");QVERIFY(!FileUploads::select({path},settings,"files",0,-1).error);
  }
 }
 void foldersFollowTheSetting() {
  const auto root=dir_.filePath("tree/Photos");
  write(root+"/a.jpg","a");write(root+"/nested/b.jpg","bb");write(root+"/.hidden","h");QDir().mkpath(root+"/empty");
  QFile::link(root+"/a.jpg",root+"/link.jpg");
  auto selection=FileUploads::select({root},settingsWith(QJsonArray{profile("a")},{},{{"fileManagerFolders","files"}}),"files",0,-1);
  QVERIFY2(!selection.error,qPrintable(selection.error.message));
  QCOMPARE(selection.files.size(),3); // the symlink is skipped
  for(const auto& origin:selection.origins)QCOMPARE(origin,root);
  QVERIFY(!selection.zipFolders);
  selection=FileUploads::select({root},settingsWith(QJsonArray{profile("a")}),"files",0,-1);
  QVERIFY2(!selection.error,qPrintable(selection.error.message));QVERIFY(selection.zipFolders);QCOMPARE(selection.files,QStringList{root});
  QVERIFY(selection.diskBytes>=selection.sourceBytes);
  // A backslash can't go into a zip portably: refused by name, like files mode's naming policy.
  const auto odd=dir_.filePath("tree/Odd");write(odd+"/a\\b.txt","x");
  selection=FileUploads::select({odd},settingsWith(QJsonArray{profile("a")}),"files",0,-1);
  QCOMPARE(selection.error.code,QString("files_rejected"));QVERIFY(selection.error.message.contains("backslash"));
  // Names that are not UTF-8 can't keep their name in a zip, and could collide.
  const auto latin=dir_.filePath("tree/Latin");QDir().mkpath(latin);
  {const auto name=QFile::encodeName(latin)+"/a\xff.txt";const int fd=::open(name.constData(),O_WRONLY|O_CREAT|O_TRUNC,0600);QVERIFY(fd>=0);QCOMPARE(::write(fd,"x",1),ssize_t(1));::close(fd);}
  selection=FileUploads::select({latin},settingsWith(QJsonArray{profile("a")}),"files",0,-1);
  QCOMPARE(selection.error.code,QString("files_rejected"));QVERIFY(selection.error.message.contains("UTF-8"));
  // A valid name may start with U+FEFF, which a QString round trip would drop.
  const auto bom=dir_.filePath(QStringLiteral("tree/\uFEFFphotos"));write(bom+"/a.jpg","a");
  QVERIFY2(!FileUploads::select({bom},settingsWith(QJsonArray{profile("a")}),"files",0,-1).error,"a U+FEFF folder name is valid UTF-8");
  // Over the file limit in files mode: the folder is named.
  auto document=settingsWith(QJsonArray{profile("a")},{},{{"fileManagerFolders","files"}}).document;auto limits=document["limits"].toObject();limits["maxFiles"]=2;document["limits"]=limits;
  selection=FileUploads::select({root},Settings::fromValidated(document),"files",0,-1);
  QCOMPARE(selection.error.code,QString("input_excessive"));QVERIFY(selection.error.message.contains("Photos"));
 }

 // The session method end to end: an operation that uploads, links and copies like a capture.
 void uploadsThroughTheStoredRoute() {
  const auto a=write(files("route-a.txt"),"alpha"),b=write(files("route-b.txt"),"beta");
  Error error;const auto accepted=wait([&](JsonReply reply){operations_->uploadFiles({a,b},{},"system",std::move(reply));},&error);
  QVERIFY2(!error,qPrintable(error.message));
  const auto operation=finished(accepted["operationId"].toString());
  QCOMPARE(operation["state"].toString(),QString("completed"));QCOMPARE(operation["action"].toString(),QString(OperationService::fileManagerAction));
  QCOMPARE(operation["items"].toArray().size(),2);
  for(const auto& item:operation["items"].toArray())QCOMPARE(item.toObject()["uploadStatus"].toString(),QString("uploaded"));
  QCOMPARE(operation["destination"]["profileId"].toString(),QString("test"));
 }
 void refusalsComeBeforeAnyOperation() {
  const auto a=write(files("refused.txt"),"x");
  Error error;wait([&](JsonReply reply){operations_->uploadFiles({a},{},"appimage",std::move(reply));},&error);
  QCOMPARE(error.code,QString("installation_mismatch"));
  wait([&](JsonReply reply){operations_->uploadFiles({a,files("gone.txt")},{},"system",std::move(reply));},&error);
  QCOMPARE(error.code,QString("files_rejected"));
  wait([&](JsonReply reply){operations_->uploadFiles({a},"nope","system",std::move(reply));},&error);
  QCOMPARE(error.code,QString("profile_not_found"));
  // No destination: refused, Settings opens, nothing saved locally.
  auto document=settings_->document();const auto saved=document;
  auto actions=document["actions"].toObject();for(const auto& name:Settings::actionNames()){auto action=actions[name].toObject();action["profileId"]=QJsonValue::Null;action["destination"]="local";actions[name]=action;}
  document["actions"]=actions;document["profiles"]=QJsonArray{};QVERIFY(settings_->save(document,settings_->revision()));
  QSignalSpy opened(operations_.get(),&OperationService::destinationRequired);
  wait([&](JsonReply reply){operations_->uploadFiles({a},{},"system",std::move(reply));},&error);
  QCOMPARE(error.code,QString("destination_required"));QCOMPARE(opened.size(),1);
  QVERIFY(settings_->save(saved,settings_->revision()));
 }

 // A resident started for an upload holds it until its saved credentials are read (#1105).
 void uploadGateRefusesRetryably() {
  const auto a=write(files("gated.txt"),"x");
  operations_->setUploadGate([]{return Error{"initializing","put.gd is reading its saved credentials",{}};});
  Error error;wait([&](JsonReply reply){operations_->uploadFiles({a},{},"system",std::move(reply));},&error);
  QCOMPARE(error.code,QString("initializing"));
  operations_->setUploadGate({});
  wait([&](JsonReply reply){operations_->uploadFiles({a},{},"system",std::move(reply));},&error);
  QVERIFY2(!error,qPrintable(error.message));
 }
 // The AppImage watches these for a DEB or RPM installed beside it.
 void systemDirectoriesThatExist() {
  QTemporaryDir root;QVERIFY(root.isValid());
  QVERIFY(QDir().mkpath(root.filePath("share/nemo/actions")));QVERIFY(QDir().mkpath(root.filePath("lib/nautilus/extensions-4")));
  FileManagerContext context;context.dataDirs={root.filePath("share")};context.libraryDirs={root.filePath("lib")};
  auto directories=FileManagerIntegration::systemDirectories(context);directories.sort();
  QCOMPARE(directories,(QStringList{root.filePath("lib/nautilus/extensions-4"),root.filePath("share/nemo/actions")}));
 }
 void cgroupLeaf() {
  QVERIFY(UploadClient::leafUnitIsService("0::/user.slice/user-1000.slice/user@1000.service/app.slice/dbus-:1.2-org.gnome.Nautilus@0.service\n"));
  QVERIFY(!UploadClient::leafUnitIsService("0::/user.slice/user-1000.slice/user@1000.service/app.slice/app-gnome-org.gnome.Terminal-1234.scope\n"));
  QVERIFY(!UploadClient::leafUnitIsService("0::/user.slice/user-1000.slice/session-2.scope\n"));
  QVERIFY(!UploadClient::leafUnitIsService("1:name=systemd:/user.slice/x.service\n"));
 }
 void privateLibrariesOnly() {
  QCOMPARE(UploadClient::withoutPrivateLibraries("/usr/bin/../lib/x86_64-linux-gnu/putgd:/home/u/lib","/usr/bin"),QString("/home/u/lib"));
  QCOMPARE(UploadClient::withoutPrivateLibraries("/usr/bin/../lib64/putgd","/usr/bin"),QString());
  QCOMPARE(UploadClient::withoutPrivateLibraries("/opt/putgd:/usr/lib","/usr/bin"),QString("/opt/putgd:/usr/lib"));
 }
 void uriResolution() {
  QVERIFY(UploadClient::decodedNewline("sftp://host/a%0Ab.txt"));QVERIFY(!UploadClient::decodedNewline("sftp://host/a%20b.txt"));
  QCOMPARE(UploadClient::normalizedUri("SFTP://Host:22/a%20b"),UploadClient::normalizedUri("sftp://host/a%20b"));
  const auto parsed=UploadClient::parseGioInfo("type: regular\nuri: sftp://host/%C3%A9.txt\nlocal path: /run/user/1000/gvfs/sftp:host=host/é.txt\nattributes:\n  standard::type: 1\n"
                                               "type: regular\nuri: smb://srv/share/x\nattributes:\n");
  QCOMPARE(parsed.value(UploadClient::normalizedUri("sftp://host/%C3%A9.txt")),QString("/run/user/1000/gvfs/sftp:host=host/é.txt"));
  QVERIFY(!parsed.contains(UploadClient::normalizedUri("smb://srv/share/x")));
  // Recent and Starred items: no local path, a file:// target (#1105).
  const auto recent=UploadClient::parseGioInfo("type: shortcut\nuri: recent:///0c78a927\nattributes:\n  standard::type: 6\n  standard::target-uri: file:///home/lab/a%20b.png\n");
  QCOMPARE(recent.value(UploadClient::normalizedUri("recent:///0c78a927")),QString("/home/lab/a b.png"));
  // AppImage: file:// is decoded in-process; other schemes go to the host gio, newlines never do.
  QStringList asked;
  const auto result=UploadClient::resolve({"/abs/a.txt","file:///tmp/b%20c.txt","sftp://host/%C3%A9.txt","sftp://host/a%0Ab.txt","relative","-platform"},true,[&](const QStringList& uris){
   asked=uris;UploadClient::GioAnswer answer;answer.found.insert(UploadClient::normalizedUri("sftp://host/%C3%A9.txt"),"/run/user/1000/gvfs/sftp:host=host/é.txt");return answer;});
  QCOMPARE(asked,QStringList{"sftp://host/%C3%A9.txt"});
  QCOMPARE(result.paths,(QStringList{"/abs/a.txt","/tmp/b c.txt","/run/user/1000/gvfs/sftp:host=host/é.txt"}));
  QCOMPARE(result.unresolved,(QStringList{"sftp://host/a%0Ab.txt","relative","-platform"}));
  QVERIFY(result.needingGio.isEmpty());
  // A gio that couldn't run is told apart from one that found no local path.
  // Only what gio had to resolve is put down to it.
  const auto gioCantRun=[](const QStringList&){UploadClient::GioAnswer answer;answer.unavailable=true;return answer;};
  const auto noGio=UploadClient::resolve({"recent:///43a92608","relative.txt","/abs/a.txt"},true,gioCantRun);
  QCOMPARE(noGio.unresolved,(QStringList{"recent:///43a92608","relative.txt"}));QCOMPARE(noGio.needingGio,QStringList{"recent:///43a92608"});
  QVERIFY(UploadClient::resolve({"file:///tmp/x"},true,gioCantRun).needingGio.isEmpty());
  QVERIFY(UploadClient::resolve({"recent:///43a92608"},true,[](const QStringList&){return UploadClient::GioAnswer{};}).needingGio.isEmpty());
  // DEB/RPM: GIO in-process.
  QCOMPARE(UploadClient::resolve({"file:///tmp/x%20y"},false,{}).paths,QStringList{"/tmp/x y"});
 }
 void refusalNames() {
  QCOMPARE(UploadClient::noticeName("smb://srv/share/a%20b.png"),QString("a b.png"));
  // Recent's last segment is a hash of the item, not its name.
  QCOMPARE(UploadClient::noticeName("recent:///43a926080238a02829512b7d6ae7ba8b"),QString("recent:///43a926080238a02829512b7d6ae7ba8b"));
  QCOMPARE(UploadClient::noticeName("relative.txt"),QString("relative.txt"));
 }
 // A build before upload.files answers invalid_request to any unknown method
 // with parameters; without them, unknown_method. A current resident's
 // invalid_request stays that resident's own refusal.
 void residentBeforeTheFeature() {
  int asked=0;
  const auto answering=[&](const QString& code){return [&asked,code]{++asked;return code;};};
  QVERIFY(UploadClient::residentLacksUpload("unknown_method",answering("")));QCOMPARE(asked,0);
  QVERIFY(UploadClient::residentLacksUpload("invalid_request",answering("unknown_method")));QCOMPARE(asked,1);
  QVERIFY(!UploadClient::residentLacksUpload("invalid_request",answering("invalid_request")));QCOMPARE(asked,2);
  QVERIFY(!UploadClient::residentLacksUpload("invalid_request",answering("client_unavailable")));QCOMPARE(asked,3);
  QVERIFY(!UploadClient::residentLacksUpload("files_rejected",answering("unknown_method")));QCOMPARE(asked,3);
 }
 void fuseMountThatStopped() {
  QTemporaryDir runtime;QVERIFY(QDir(runtime.path()).mkpath("gvfs/smb-share:server=live"));
  const auto gvfs=runtime.filePath("gvfs/");
  const auto missing=[](const QString& path,const QString& code="source_missing"){return QJsonObject{{"path",path},{"code",code},{"message","no longer exists"}};};
  // The mount answers: that file really is gone.
  QVERIFY(UploadClient::unreachableOverFuse({missing(gvfs+"smb-share:server=live/a.png")},runtime.path()).isEmpty());
  QCOMPARE(UploadClient::unreachableOverFuse({missing(gvfs+"smb-share:server=gone/d/a.png"),missing(gvfs+"smb-share:server=live/b.png"),missing(gvfs+"smb-share:server=gone/c.png")},runtime.path()),(QStringList{"a.png","c.png"}));
  // Not GVfs's folder, or not this problem.
  QVERIFY(UploadClient::unreachableOverFuse({missing("/home/lab/a.png")},runtime.path()).isEmpty());
  QVERIFY(UploadClient::unreachableOverFuse({missing(gvfs+"smb-share:server=gone/a.png","source_empty")},runtime.path()).isEmpty());
  QVERIFY(UploadClient::unreachableOverFuse({missing("gvfs/smb-share:server=gone/a.png")},QString()).isEmpty());
 }
 // No notification server: the first dialog tool that shows it, in the host's environment.
 void dialogWithoutNotificationServer() {
  QTemporaryDir tools;const auto receipt=tools.filePath("receipt");
  const auto tool=[&](const QString& name,int exitCode){
   QFile file(tools.filePath(name));QVERIFY(file.open(QIODevice::WriteOnly));
   file.write(QStringLiteral("#!/bin/sh\nprintf '%s\\0' %1 \"$@\" >> \"$RECEIPT\"\nexit %2\n").arg(name).arg(exitCode).toUtf8());file.close();
   QVERIFY(file.setPermissions(file.permissions()|QFile::ExeOwner));
  };
  QProcessEnvironment environment;environment.insert("PATH",tools.path());environment.insert("RECEIPT",receipt);
  const auto received=[&]{QFile file(receipt);if(!file.open(QIODevice::ReadOnly))return QStringList{};const auto text=QString::fromUtf8(file.readAll());file.remove();return text.split(QChar(0),Qt::SkipEmptyParts);};
  tool("kdialog",0);
  // No display: nothing can show it, so nothing runs.
  QVERIFY(!UploadClient::showDialog("Title","<b>Body & more",environment));QVERIFY(received().isEmpty());
  environment.insert("DISPLAY",":9");
  QVERIFY(UploadClient::showDialog("Title","<b>Body & more",environment));
  QCOMPARE(received(),(QStringList{"kdialog","--title","Title","--error","<b>Body & more"}));
  // A tool that can't show it passes to the next; zenity takes the text literally.
  tool("zenity",1);
  QVERIFY(UploadClient::showDialog("Title","<b>Body & more",environment));
  QCOMPARE(received(),(QStringList{"zenity","--error","--no-markup","--title=Title","--text=<b>Body & more","kdialog","--title","Title","--error","<b>Body & more"}));
  QVERIFY(QFile::remove(tools.filePath("kdialog")));QVERIFY(QFile::remove(tools.filePath("zenity")));
  // xmessage doesn't wrap by itself.
  tool("xmessage",0);
  QVERIFY(UploadClient::showDialog("Title",QString(50,'a')+" "+QString(50,'b'),environment));
  QCOMPARE(received(),(QStringList{"xmessage","-center","Title\n\n"+QString(50,'a')+"\n"+QString(50,'b')+"\n"}));
  QVERIFY(QFile::remove(tools.filePath("xmessage")));
  QVERIFY(!UploadClient::showDialog("Title","Body",environment));
 }
 // The real client refuses what it can't resolve before anything else, with no resident needed.
 void clientRefusesWithoutLocalPaths() {
  const auto putgd=qEnvironmentVariable("PUTGD_BINARY");
  if(putgd.isEmpty())QSKIP("PUTGD_BINARY is not set");
  QProcess client;auto environment=QProcessEnvironment::systemEnvironment();environment.remove("DBUS_SESSION_BUS_ADDRESS");environment.insert("XDG_RUNTIME_DIR",dir_.filePath("runtime"));
  client.setProcessEnvironment(environment);client.start(putgd,{"--upload","--","relative.txt"});QVERIFY(client.waitForFinished(20000));
  QCOMPARE(client.exitCode(),1);QVERIFY(client.readAllStandardError().contains("no local path"));
 }
};
QTEST_GUILESS_MAIN(FileUploadsContract)
#include "test_file_uploads.moc"
