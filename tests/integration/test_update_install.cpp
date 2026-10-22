#include "app/AppImageInstall.h"
#include "cli/UpdateInstall.h"
#include "fixtures/MinisignFixture.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>
#include <QtEndian>
using namespace putgd;
namespace {
const QString nightly="zsync|https://app.put.gd/dl/nightly/appimage.zsync",stable="zsync|https://app.put.gd/dl/stable/appimage.zsync";
const QString tag="v0.1.1-nightly.801",name="putgd-0.1.1-nightly.801-x86_64.AppImage";
QByteArray elfImage(const QByteArray& payload,const QString& updateInformation=nightly) {
 const QByteArray names("\0.shstrtab\0.upd_info\0",21);
 auto info=updateInformation.toUtf8();info.append(QByteArray(1024-info.size(),'\0'));
 QByteArray header(64,'\0');
 header.replace(0,4,"\x7f""ELF");header[4]=2;header[5]=1;header[6]=1;header[8]='A';header[9]='I';header[10]=2;
 qToLittleEndian<quint64>(64+names.size()+info.size(),header.data()+0x28);
 qToLittleEndian<quint16>(64,header.data()+0x3A);qToLittleEndian<quint16>(3,header.data()+0x3C);qToLittleEndian<quint16>(1,header.data()+0x3E);
 QByteArray table(3*64,'\0');
 const auto section=[&](int index,quint32 nameOffset,quint64 offset,quint64 size){
  qToLittleEndian<quint32>(nameOffset,table.data()+index*64);qToLittleEndian<quint64>(offset,table.data()+index*64+24);qToLittleEndian<quint64>(size,table.data()+index*64+32);
 };
 section(1,1,64,names.size());section(2,11,64+names.size(),info.size());
 return header+names+info+table+payload;
}
QString hex(const QByteArray& bytes) {return QString::fromLatin1(QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex());}
void write(const QString& path,const QByteArray& bytes,QFile::Permissions permissions=QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner) {
 QFile file(path);QVERIFY(file.open(QIODevice::WriteOnly|QIODevice::Truncate));file.write(bytes);file.close();QVERIFY(file.setPermissions(permissions));
}
QByteArray read(const QString& path) {QFile file(path);return file.open(QIODevice::ReadOnly)?file.readAll():QByteArray{};}
// A loopback stand-in for app.put.gd's feed, the GitHub release and pkg.put.gd's mirror.
class ReleaseServer final:public QObject {
 Q_OBJECT
public:
 QHash<QByteArray,QByteArray> files;QList<QByteArray> requests;
 ReleaseServer(){QVERIFY(server_.listen(QHostAddress::LocalHost));connect(&server_,&QTcpServer::newConnection,this,&ReleaseServer::accept);}
 QByteArray base() const {return "http://127.0.0.1:"+QByteArray::number(server_.serverPort());}
 bool requested(const QByteArray& line) const {for(const auto& r:requests)if(r.startsWith(line))return true;return false;}
private:
 void accept(){
  while(auto* socket=server_.nextPendingConnection()){
   auto request=std::make_shared<QByteArray>();
   connect(socket,&QTcpSocket::readyRead,socket,[this,socket,request]{
    *request+=socket->readAll();if(!request->contains("\r\n\r\n"))return;
    const auto line=request->left(request->indexOf("\r\n"));requests.append(line);
    const auto parts=line.split(' ');const bool head=parts.value(0)=="HEAD";const auto path=parts.value(1);
    const auto found=files.constFind(path);
    const QByteArray status=found==files.cend()?"404 Not Found":"200 OK";const auto body=found==files.cend()?QByteArray("missing"):*found;
    socket->write("HTTP/1.1 "+status+"\r\nContent-Length: "+QByteArray::number(body.size())+"\r\nConnection: close\r\n\r\n"+(head?QByteArray():body));
    socket->disconnectFromHost();
   });
  }
 }
 QTcpServer server_;
};
}
// The install child (#293 phase 3): zsync through the host's tool, the full
// download as fallback, SHA256SUMS before any swap, and what a refusal leaves.
class UpdateInstallTest:public QObject {
 Q_OBJECT
 QTemporaryDir dir;QString images,tools,state;QByteArray path_;
 // The release signer's key for these tests.
 MinisignKey signer_;
 // The stable channel's own key (#344).
 MinisignKey stableSigner_{QByteArray::fromHex("a4388fb41f96a6d7")};
 const QByteArray old_=elfImage("800"),next_=elfImage("801 with changes");
 static update::Build build(){
  return update::build(QJsonObject{{"version","0.1.1-nightly.800"},{"gitCommit","528fe444b0cb0123456789abcdef0123456789ab"},{"dirty",false},{"sourceProvenance","git"}},"appimage",true);
 }
 QString image() const {return images+"/putgd-0.1.1-nightly.800-x86_64.AppImage";}
 void publish(ReleaseServer& server,const QByteArray& served,const QString& feedSha=QString()) {
  const auto releaseUrl="https://github.com/patrick-hudson/put.gd-desktop/releases/download/"+tag+"/"+name;
  QJsonObject asset{{"name",name},{"url",releaseUrl},{"size",next_.size()}};
  asset["sha256"]=feedSha.isEmpty()?hex(next_):feedSha;
  server.files["/update/v1/nightly.json"]=QJsonDocument(QJsonObject{{"schema",1},{"channel","nightly"},{"release",QJsonObject{{"tag",tag},{"version","0.1.1-nightly.801"},
   {"assets",QJsonObject{{"appimage",asset}}}}}}).toJson();
  const auto sums=(hex(next_)+"  "+name+"\n"+hex("zsync")+"  "+name+".zsync\n").toUtf8();
  server.files["/release/"+tag.toUtf8()+"/SHA256SUMS"]=sums;
  server.files["/mirror/"+tag.toUtf8()+"/SHA256SUMS.minisig"]=signer_.signature(sums,"put.gd "+tag.toUtf8()+" nightly");
  server.files["/release/"+tag.toUtf8()+"/"+name.toUtf8()]=served;
  server.files["/mirror/"+tag.toUtf8()+"/"+name.toUtf8()+".zsync"]="zsync";
  server.files["/mirror/"+tag.toUtf8()+"/"+name.toUtf8()]=served;
  qputenv("PUTGD_UPDATE_FEED_URL",server.base()+"/update/v1");qputenv("PUTGD_UPDATE_RELEASE_URL",server.base()+"/release");qputenv("PUTGD_UPDATE_MIRROR_URL",server.base()+"/mirror");
 }
 // A host appimageupdatetool: records its arguments and SSL_CERT_FILE, writes
 // tool.out under the .zsync's file name in its folder, exits with tool.code.
 void fakeTool(const QByteArray& output,int code) {
  write(tools+"/appimageupdatetool",QString("#!/bin/sh\nprintf '%s\\n' \"$@\" > '%1/tool.args'\nprintf '%s' \"$SSL_CERT_FILE\" > '%1/tool.ca'\n"
   "seed=$3\n/bin/cp '%1/tool.out' \"${seed%/*}/%2\"\nexit %3\n").arg(dir.path(),name).arg(code).toUtf8());
  write(dir.filePath("tool.out"),output);
 }
 InstallOptions options(const QString& method="auto") const {
  InstallOptions o;o.channel="nightly";o.method=method;o.stateRoot=state;o.image=image();o.caBundle="/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem";
  o.publicKeys={{"nightly",signer_.publicFile()},{"stable",stableSigner_.publicFile()}};return o;
 }
 // #344: a release of either channel, its SHA256SUMS signed by signer with trusted comment.
 void release(ReleaseServer& server,const QString& channel,const QString& version,const QByteArray& served,const MinisignKey& signer,const QByteArray& trusted={},const QByteArray& listed={}) {
  const auto releaseTag="v"+version,releaseName="putgd-"+version+"-x86_64.AppImage";const auto bytes=listed.isEmpty()?served:listed;
  server.files["/update/v1/"+channel.toUtf8()+".json"]=QJsonDocument(QJsonObject{{"schema",1},{"channel",channel},{"release",QJsonObject{{"tag",releaseTag},{"version",version},
   {"assets",QJsonObject{{"appimage",QJsonObject{{"name",releaseName},{"url","https://github.com/patrick-hudson/put.gd-desktop/releases/download/"+releaseTag+"/"+releaseName},{"size",bytes.size()},{"sha256",hex(bytes)}}}}}}}}).toJson();
  const auto sums=(hex(bytes)+"  "+releaseName+"\n").toUtf8();
  server.files["/release/"+releaseTag.toUtf8()+"/SHA256SUMS"]=sums;
  server.files["/mirror/"+releaseTag.toUtf8()+"/SHA256SUMS.minisig"]=signer.signature(sums,trusted.isEmpty()?"put.gd "+releaseTag.toUtf8()+" "+channel.toUtf8():trusted);
  server.files["/release/"+releaseTag.toUtf8()+"/"+releaseName.toUtf8()]=served;server.files["/mirror/"+releaseTag.toUtf8()+"/"+releaseName.toUtf8()]=served;
  qputenv("PUTGD_UPDATE_FEED_URL",server.base()+"/update/v1");qputenv("PUTGD_UPDATE_RELEASE_URL",server.base()+"/release");qputenv("PUTGD_UPDATE_MIRROR_URL",server.base()+"/mirror");
 }
 InstallOptions switchTo(const QString& channel,bool allowOlder=false) const {auto o=options();o.channel=channel;o.switchChannel=true;o.allowOlder=allowOlder;return o;}
 static update::Build buildOf(const QString& version){
  return update::build(QJsonObject{{"version",version},{"gitCommit","528fe444b0cb0123456789abcdef0123456789ab"},{"dirty",false},{"sourceProvenance","git"}},"appimage",true);
 }
 QStringList leftovers() const {return QDir(images).entryList({".putgd-update-*"},QDir::AllEntries|QDir::Hidden|QDir::NoDotAndDotDot);}
private slots:
 void initTestCase(){path_=qgetenv("PATH");}
 void init(){
  QVERIFY(dir.isValid());
  for(const auto& name:QDir(dir.path()).entryList(QDir::AllEntries|QDir::Hidden|QDir::NoDotAndDotDot))QFileInfo(dir.filePath(name)).isDir()?QDir(dir.filePath(name)).removeRecursively():QFile::remove(dir.filePath(name));
  images=dir.filePath("Applications");tools=dir.filePath("bin");state=dir.filePath("state");
  QVERIFY(QDir().mkpath(images));QVERIFY(QDir().mkpath(tools));
  write(image(),old_,QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner|QFile::ReadGroup|QFile::ExeGroup);
  // Only the fake tools: no real appimageupdatetool of the build host.
  qputenv("PATH",tools.toUtf8());
 }
 void cleanup(){qputenv("PATH",path_);for(const char* name:{"PUTGD_UPDATE_FEED_URL","PUTGD_UPDATE_RELEASE_URL","PUTGD_UPDATE_MIRROR_URL"})qunsetenv(name);}
 void withoutZsyncToolDownloadsTheReleaseAndKeepsABackup(){
  ReleaseServer server;publish(server,next_);
  QList<QJsonObject> progress;Error error;
  const auto result=installUpdate(build(),options(),[&](const QJsonObject& line){progress.append(line);},error);
  QVERIFY2(!error,qPrintable(error.message+" "+QJsonDocument(error.details).toJson()));
  QCOMPARE(result["installed"].toBool(),true);QCOMPARE(result["method"].toString(),QString("download"));QCOMPARE(result["source"].toString(),QString("release"));
  QCOMPARE(result["zsync"].toObject()["fallbackReason"].toString(),QString("not_installed"));
  QCOMPARE(read(image()),next_);QCOMPARE(QFileInfo(image()).permissions()&QFile::ExeGroup,QFile::ExeGroup);
  QCOMPARE(read(result["backup"].toString()),old_);
  QCOMPARE(leftovers(),QStringList{".putgd-update-backup-0.1.1-nightly.800.AppImage"});
  const auto saved=appimage::readState(state);
  QCOMPARE(saved["installed"].toObject()["version"].toString(),QString("0.1.1-nightly.801"));QCOMPARE(saved["installed"].toObject()["image"].toString(),QFileInfo(image()).canonicalFilePath());
  QCOMPARE(saved["backup"].toObject()["path"].toString(),result["backup"].toString());QVERIFY(saved["staging"].isNull());
  QStringList phases;for(const auto& line:progress)if(phases.isEmpty()||phases.last()!=line["phase"].toString())phases.append(line["phase"].toString());
  QCOMPARE(phases,(QStringList{"checking","verifying","download","verifying","installing","done"}));
  // A second install waits for the restart.
  error={};installUpdate(build(),options(),{},error);QCOMPARE(error.code,QString("update_restart_required"));
 }
 void zsyncRunsTheHostToolWithItsCaBundleOnThePinnedRelease(){
  ReleaseServer server;publish(server,next_);fakeTool(next_,0);
  Error error;const auto result=installUpdate(build(),options(),{},error);
  QVERIFY2(!error,qPrintable(error.message));
  QCOMPARE(result["method"].toString(),QString("zsync"));QCOMPARE(result["zsync"].toObject()["used"].toBool(),true);QCOMPARE(read(image()),next_);
  const auto arguments=QString::fromUtf8(read(dir.filePath("tool.args"))).split('\n',Qt::SkipEmptyParts);
  QCOMPARE(arguments.value(0),QString("-u"));
  QCOMPARE(arguments.value(1),"zsync|"+server.base()+"/mirror/"+tag+"/"+name+".zsync");
  // It works on a link of the image in the private staging folder, never beside the user's files.
  QVERIFY(arguments.value(2).startsWith(QFileInfo(image()).canonicalPath()+"/.putgd-update-"));QVERIFY(arguments.value(2).endsWith("/seed.AppImage"));
  QCOMPARE(read(dir.filePath("tool.ca")),QByteArray("/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem"));
  // No image download: the tool did it.
  QVERIFY(!server.requested("GET /release/"+tag.toUtf8()+"/"+name.toUtf8()));QVERIFY(server.requested("HEAD /mirror/"));
  QCOMPARE(leftovers().size(),1);
 }
 void aFailedZsyncFallsBackToTheDownload(){
  ReleaseServer server;publish(server,next_);fakeTool(next_,1);
  Error error;auto result=installUpdate(build(),options(),{},error);
  QVERIFY2(!error,qPrintable(error.message));
  QCOMPARE(result["method"].toString(),QString("download"));QCOMPARE(result["zsync"].toObject()["fallbackReason"].toString(),QString("exit_1"));QCOMPARE(read(image()),next_);
 }
 void zsyncOutputThatIsNotTheReleaseFallsBack(){
  ReleaseServer server;publish(server,next_);fakeTool(elfImage("something else"),0);
  Error error;const auto result=installUpdate(build(),options(),{},error);
  QVERIFY2(!error,qPrintable(error.message));
  QCOMPARE(result["zsync"].toObject()["fallbackReason"].toString(),QString("verification_failed:sha256"));QCOMPARE(result["method"].toString(),QString("download"));QCOMPARE(read(image()),next_);
 }
 void withoutTheMirrorTheToolIsNotRun(){
  ReleaseServer server;publish(server,next_);server.files.remove("/mirror/"+tag.toUtf8()+"/"+name.toUtf8()+".zsync");fakeTool(next_,0);
  Error error;const auto result=installUpdate(build(),options(),{},error);
  QVERIFY(!error);QCOMPARE(result["zsync"].toObject()["fallbackReason"].toString(),QString("mirror_unavailable"));QVERIFY(!QFileInfo::exists(dir.filePath("tool.args")));
 }
 void zsyncOnlyDoesNotFallBack(){
  ReleaseServer server;publish(server,next_);fakeTool(next_,3);
  Error error;installUpdate(build(),options("zsync"),{},error);
  QCOMPARE(error.code,QString("update_zsync_failed"));QCOMPARE(read(image()),old_);QVERIFY(leftovers().isEmpty());
 }
 void aTamperedDownloadIsRefusedAndNothingChanges(){
  ReleaseServer server;auto tampered=next_;tampered[tampered.size()-1]='X';publish(server,tampered);
  Error error;installUpdate(build(),options("download"),{},error);
  QCOMPARE(error.code,QString("update_verification_failed"));QCOMPARE(error.details["check"].toString(),QString("sha256"));
  QCOMPARE(read(image()),old_);QVERIFY(leftovers().isEmpty());
  QVERIFY(appimage::readState(state)["staging"].isNull());QVERIFY(!appimage::readState(state).contains("installed"));
 }
 void aFeedThatDisagreesWithSha256SumsIsRefusedBeforeDownloading(){
  ReleaseServer server;publish(server,next_,hex("another file"));
  Error error;installUpdate(build(),options(),{},error);
  QCOMPARE(error.code,QString("update_verification_failed"));QVERIFY(!server.requested("GET /release/"+tag.toUtf8()+"/"+name.toUtf8()));QCOMPARE(read(image()),old_);
 }
 // SHA256SUMS counts only with the release signer's signature for this release.
 void anUnsignedOrForeignSignedSha256SumsIsRefused(){
  ReleaseServer server;publish(server,next_);const auto signature="/mirror/"+tag.toUtf8()+"/SHA256SUMS.minisig";const auto sums=server.files["/release/"+tag.toUtf8()+"/SHA256SUMS"];
  const auto refused=[&](const QString& check){
   Error error;installUpdate(build(),options(),{},error);
   QCOMPARE(error.code,QString("update_signature_invalid"));QCOMPARE(error.details["check"].toString(),check);
   QCOMPARE(read(image()),old_);QVERIFY(leftovers().isEmpty());QVERIFY(!server.requested("GET /release/"+tag.toUtf8()+"/"+name.toUtf8()));
  };
  server.files.remove(signature);refused("missing");
  MinisignKey other(QByteArray::fromHex("0102030405060708"));server.files[signature]=other.signature(sums,"put.gd "+tag.toUtf8()+" nightly");refused("signature");
  MinisignKey otherId(QByteArray::fromHex("1111111111111111"));server.files[signature]=otherId.signature(sums,"put.gd "+tag.toUtf8()+" nightly");refused("keyId");
  // Signed, but for another release: an older SHA256SUMS replayed.
  server.files[signature]=signer_.signature(sums,"put.gd v0.1.1-nightly.700 nightly");refused("release");
  // A SHA256SUMS changed after signing.
  server.files[signature]=signer_.signature(sums,"put.gd "+tag.toUtf8()+" nightly");
  server.files["/release/"+tag.toUtf8()+"/SHA256SUMS"]=sums+hex("extra").toUtf8()+"  extra\n";refused("signature");
 }
 void missingSha256SumsLineIsRefused(){
  ReleaseServer server;publish(server,next_);const auto sums=(hex(next_)+"  other.AppImage\n").toUtf8();
  server.files["/release/"+tag.toUtf8()+"/SHA256SUMS"]=sums;server.files["/mirror/"+tag.toUtf8()+"/SHA256SUMS.minisig"]=signer_.signature(sums,"put.gd "+tag.toUtf8()+" nightly");
  Error error;installUpdate(build(),options(),{},error);QCOMPARE(error.code,QString("update_sums_unavailable"));QCOMPARE(read(image()),old_);
 }
 void theNewestVersionHasNothingToInstall(){
  ReleaseServer server;publish(server,next_);
  auto newest=update::build(QJsonObject{{"version","0.1.1-nightly.801"},{"gitCommit","528fe444b0cb0123456789abcdef0123456789ab"},{"dirty",false},{"sourceProvenance","git"}},"appimage",true);
  Error error;installUpdate(newest,options(),{},error);QCOMPARE(error.code,QString("update_not_available"));
 }
 void onlyAppImageBuildsInstall(){
  auto deb=update::build(QJsonObject{{"version","0.1.1-nightly.800"},{"gitCommit","528fe444b0cb0123456789abcdef0123456789ab"},{"dirty",false},{"sourceProvenance","git"}},"debian13",true);
  Error error;installUpdate(deb,options(),{},error);QCOMPARE(error.code,QString("update_install_unavailable"));
 }
 // #344: switching channels installs the other channel's release, checked with
 // that channel's key, trusted comment and permalink.
 void aSwitchToStableChecksTheStableKeyAndPermalink(){
  ReleaseServer server;const auto target=elfImage("0.1.1 stable",stable);release(server,"stable","0.1.1",target,stableSigner_);
  Error error;const auto result=installUpdate(build(),switchTo("stable"),{},error);
  QVERIFY2(!error,qPrintable(error.message+" "+QJsonDocument(error.details).toJson()));
  QCOMPARE(result["switched"].toBool(),true);QCOMPARE(result["channel"].toString(),QString("stable"));QCOMPARE(result["previousChannel"].toString(),QString("nightly"));
  QCOMPARE(result["signature"].toObject()["trustedComment"].toString(),QString("put.gd v0.1.1 stable"));
  QCOMPARE(read(image()),target);QCOMPARE(read(result["backup"].toString()),old_);
  const auto installed=appimage::readState(state)["installed"].toObject();
  QCOMPARE(installed["version"].toString(),QString("0.1.1"));QCOMPARE(installed["channel"].toString(),QString("stable"));QCOMPARE(installed["previousChannel"].toString(),QString("nightly"));
 }
 void anotherChannelWithoutSwitchIsRefused(){
  ReleaseServer server;release(server,"stable","0.1.1",elfImage("0.1.1 stable",stable),stableSigner_);
  auto o=options();o.channel="stable";Error error;installUpdate(build(),o,{},error);
  QCOMPARE(error.code,QString("update_switch_confirm_required"));QVERIFY(server.requests.isEmpty());QCOMPARE(read(image()),old_);
 }
 void aSwitchKeepsEveryTamperRefusal(){
  const auto target=elfImage("0.1.1 stable",stable);
  const auto refused=[&](ReleaseServer& server,const QString& code,const QString& check){
   Error error;installUpdate(build(),switchTo("stable"),{},error);
   QCOMPARE(error.code,code);QCOMPARE(error.details["check"].toString(),check);QCOMPARE(read(image()),old_);QVERIFY(leftovers().isEmpty());
  };
  // Signed with the nightly key (this build's own), or for the nightly channel.
  {ReleaseServer server;release(server,"stable","0.1.1",target,signer_);refused(server,"update_signature_invalid","keyId");}
  {ReleaseServer server;release(server,"stable","0.1.1",target,stableSigner_,"put.gd v0.1.1 nightly");refused(server,"update_signature_invalid","release");}
  // A stable-signed image that still follows nightly's permalink is not the stable release.
  {ReleaseServer server;release(server,"stable","0.1.1",elfImage("0.1.1 stable",nightly),stableSigner_);refused(server,"update_verification_failed","updateInformation");}
  // A flipped byte against the signed SHA256SUMS.
  {ReleaseServer server;auto tampered=target;tampered[tampered.size()-1]='X';release(server,"stable","0.1.1",tampered,stableSigner_,{},target);
   auto o=switchTo("stable");o.method="download";Error error;installUpdate(build(),o,{},error);
   QCOMPARE(error.code,QString("update_verification_failed"));QCOMPARE(error.details["check"].toString(),QString("sha256"));QCOMPARE(read(image()),old_);}
 }
 void anOlderTargetNeedsAllowOlder(){
  // Stable 0.1.1 switching to nightly 0.1.1-nightly.801, which is older.
  write(image(),elfImage("0.1.1 stable",stable));const auto target=elfImage("801 nightly",nightly);
  ReleaseServer server;release(server,"nightly","0.1.1-nightly.801",target,signer_);
  Error error;installUpdate(buildOf("0.1.1"),switchTo("nightly"),{},error);
  QCOMPARE(error.code,QString("update_switch_older"));QCOMPARE(error.details["latest"].toString(),QString("0.1.1-nightly.801"));QCOMPARE(read(image()),elfImage("0.1.1 stable",stable));
  error={};const auto result=installUpdate(buildOf("0.1.1"),switchTo("nightly",true),{},error);
  QVERIFY2(!error,qPrintable(error.message));QCOMPARE(read(image()),target);QCOMPARE(result["previousChannel"].toString(),QString("stable"));
 }
 void noStableReleaseIsNothingToSwitchTo(){
  ReleaseServer server;server.files["/update/v1/stable.json"]=R"({"schema":1,"channel":"stable","release":null})";
  qputenv("PUTGD_UPDATE_FEED_URL",server.base()+"/update/v1");
  Error error;installUpdate(build(),switchTo("stable"),{},error);QCOMPARE(error.code,QString("update_no_release"));QCOMPARE(read(image()),old_);
 }
};
QTEST_GUILESS_MAIN(UpdateInstallTest)
#include "test_update_install.moc"
