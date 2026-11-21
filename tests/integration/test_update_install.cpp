#include "app/AppImageInstall.h"
#include "cli/UpdateInstall.h"
#include "fixtures/ElfImage.h"
#include "fixtures/MinisignFixture.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QScopeGuard>
#include <QSet>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>
#include <QtEndian>
#include <unistd.h>
using namespace putgd;
namespace {
const QString nightly="zsync|https://app.put.gd/dl/nightly/appimage.zsync",stable="zsync|https://app.put.gd/dl/stable/appimage.zsync";
const QString tag="v0.1.1-nightly.801",name="putgd-0.1.1-nightly.801-x86_64.AppImage";
QByteArray elfImage(const QByteArray& payload,const QString& updateInformation=nightly) {return putgd::test::elfImage(updateInformation.toUtf8(),payload);}
QString hex(const QByteArray& bytes) {return QString::fromLatin1(QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex());}
void write(const QString& path,const QByteArray& bytes,QFile::Permissions permissions=QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner) {
 QFile file(path);QVERIFY(file.open(QIODevice::WriteOnly|QIODevice::Truncate));file.write(bytes);file.close();QVERIFY(file.setPermissions(permissions));
}
QByteArray read(const QString& path) {QFile file(path);return file.open(QIODevice::ReadOnly)?file.readAll():QByteArray{};}
// A loopback stand-in for app.put.gd's feed, the GitHub release and pkg.put.gd's
// mirror. Range requests get a 206: one part, or multipart/byteranges as
// CloudFront sends for several; ignoreRanges answers them with the whole file,
// corruptRanges flips the first byte of every part, trailingBytes streams that
// much more past the closing boundary. A path in redirects answers 302 to its
// location; one in stalls is never answered.
class ReleaseServer final:public QObject {
 Q_OBJECT
public:
 QHash<QByteArray,QByteArray> files;QList<QByteArray> requests,ranges;
 QHash<QByteArray,QByteArray> redirects;QSet<QByteArray> stalls;
 bool ignoreRanges=false,corruptRanges=false;qint64 trailingBytes=0;
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
    QByteArray range;
    for(const auto& header:request->left(request->indexOf("\r\n\r\n")).split('\n'))if(header.toLower().startsWith("range:"))range=header.mid(6).trimmed();
    const auto parts=line.split(' ');const bool head=parts.value(0)=="HEAD";const auto path=parts.value(1);
    if(stalls.contains(path))return;
    if(redirects.contains(path)){socket->write("HTTP/1.1 302 Found\r\nLocation: "+redirects[path]+"\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");socket->disconnectFromHost();return;}
    const auto found=files.constFind(path);
    QByteArray status=found==files.cend()?"404 Not Found":"200 OK",headers,body=found==files.cend()?QByteArray("missing"):*found;
    if(found!=files.cend()&&range.startsWith("bytes=")&&!ignoreRanges) {
     ranges.append(range);
     QList<std::pair<qint64,qint64>> spans;
     for(const auto& spec:range.mid(6).split(',')){const auto dash=spec.indexOf('-');spans.append({spec.left(dash).toLongLong(),spec.mid(dash+1).toLongLong()});}
     const auto piece=[&](const std::pair<qint64,qint64>& span){auto bytes=found->mid(span.first,span.second-span.first+1);if(corruptRanges)bytes[0]=char(bytes[0]^0x55);return bytes;};
     const auto contentRange=[&](const std::pair<qint64,qint64>& span){return "bytes "+QByteArray::number(span.first)+"-"+QByteArray::number(span.second)+"/"+QByteArray::number(found->size());};
     status="206 Partial Content";
     if(spans.size()==1){headers="Content-Range: "+contentRange(spans[0])+"\r\n";body=piece(spans[0]);}
     else {
      headers="Content-Type: multipart/byteranges; boundary=CloudFront:TEST\r\n";body.clear();
      for(const auto& span:spans)body+="\r\n--CloudFront:TEST\r\nContent-Type: application/vnd.appimage\r\nContent-Range: "+contentRange(span)+"\r\n\r\n"+piece(span);
      body+="\r\n--CloudFront:TEST--\r\n"+QByteArray(trailingBytes,'x');
     }
    }
    socket->write("HTTP/1.1 "+status+"\r\n"+headers+"Content-Length: "+QByteArray::number(body.size())+"\r\nConnection: close\r\n\r\n"+(head?QByteArray():body));
    socket->disconnectFromHost();
   });
  }
 }
 QTcpServer server_;
};
// The release's .zsync as zsync 0.6.2's zsyncmake writes it: the header, then
// per block (the last zero-padded) the trailing rsum bytes of its big-endian
// a and b and the leading bytes of its MD4.
QByteArray zsyncmake(const QByteArray& file,const QString& filename,int blocksize=1024,const QByteArray& hashLengths="2,2,5",const QByteArray& sha1={}) {
 const auto lengths=hashLengths.split(',');const int rsumBytes=lengths.value(1).toInt(),checksumBytes=lengths.value(2).toInt();
 QByteArray out="zsync: 0.6.2\nFilename: "+filename.toUtf8()+"\nMTime: Fri, 20 Nov 2026 08:45:04 +0000\nBlocksize: "+QByteArray::number(blocksize)+
  "\nLength: "+QByteArray::number(file.size())+"\nHash-Lengths: "+hashLengths+"\nURL: https://pkg.put.gd/releases/x/"+filename.toUtf8()+
  "\nSHA-1: "+(sha1.isEmpty()?QCryptographicHash::hash(file,QCryptographicHash::Sha1).toHex():sha1)+"\n\n";
 for(qsizetype at=0;at<file.size();at+=blocksize) {
  auto block=file.mid(at,blocksize);block.append(QByteArray(blocksize-block.size(),'\0'));
  quint16 a=0,b=0;
  for(int i=0;i<blocksize;++i){a=quint16(a+uchar(block[i]));b=quint16(b+(blocksize-i)*uchar(block[i]));}
  uchar rsum[4]{uchar(a>>8),uchar(a),uchar(b>>8),uchar(b)};
  out.append(reinterpret_cast<const char*>(rsum)+4-rsumBytes,rsumBytes);
  out.append(QCryptographicHash::hash(block,QCryptographicHash::Md4).left(checksumBytes));
 }
 return out;
}
// Bytes that don't repeat, so blocks only match where they were copied.
QByteArray noise(int bytes,quint32 seed) {QByteArray out(bytes,'\0');for(auto& c:out){seed=seed*1664525u+1013904223u;c=char(seed>>24);}return out;}
}
// The install child (#293 phase 3): zsync through the host's tool, the full
// download as fallback, SHA256SUMS before any swap, and what a refusal leaves.
class UpdateInstallTest:public QObject {
 Q_OBJECT
 QTemporaryDir dir;QString images,state;
 // The release signer's key for these tests.
 MinisignKey signer_;
 // The stable channel's own key (#344).
 MinisignKey stableSigner_{QByteArray::fromHex("a4388fb41f96a6d7")};
 // The next version moves the old one's bytes by an odd offset, changes some
 // and grows a short last block: most of it is in the old image, not aligned.
 const QByteArray payload_=noise(200*1024,7);
 const QByteArray old_=elfImage("800"+payload_),
  next_=elfImage("801"+payload_.left(1000)+"abc"+payload_.mid(1000,99000)+noise(6000,9)+payload_.mid(106000)+noise(2000,11));
 static update::Build build(){
  return update::build(QJsonObject{{"version","0.1.1-nightly.800"},{"gitCommit","528fe444b0cb0123456789abcdef0123456789ab"},{"dirty",false},{"sourceProvenance","git"}},"appimage",true);
 }
 QString image() const {return images+"/putgd-0.1.1-nightly.800-x86_64.AppImage";}
 void publish(ReleaseServer& server,const QByteArray& served,const QString& feedSha=QString(),const QByteArray& listed={}) {
  // listed is the release SHA256SUMS and the .zsync describe; served is what the image URLs return.
  const auto release=listed.isEmpty()?next_:listed;
  const auto releaseUrl="https://github.com/patrick-hudson/put.gd-desktop/releases/download/"+tag+"/"+name;
  QJsonObject asset{{"name",name},{"url",releaseUrl},{"size",release.size()}};
  asset["sha256"]=feedSha.isEmpty()?hex(release):feedSha;
  server.files["/update/v1/nightly.json"]=QJsonDocument(QJsonObject{{"schema",1},{"channel","nightly"},{"release",QJsonObject{{"tag",tag},{"version","0.1.1-nightly.801"},
   {"assets",QJsonObject{{"appimage",asset}}}}}}).toJson();
  server.files["/release/"+tag.toUtf8()+"/"+name.toUtf8()]=served;
  server.files["/mirror/"+tag.toUtf8()+"/"+name.toUtf8()]=served;
  serveControl(server,release,zsyncmake(release,name));
  qputenv("PUTGD_UPDATE_FEED_URL",server.base()+"/update/v1");qputenv("PUTGD_UPDATE_RELEASE_URL",server.base()+"/release");qputenv("PUTGD_UPDATE_MIRROR_URL",server.base()+"/mirror");
 }
 // The mirror's .zsync, and the signed SHA256SUMS listing release and (unless
 // unlisted) that .zsync.
 void serveControl(ReleaseServer& server,const QByteArray& release,const QByteArray& control,bool listed=true) {
  const auto sums=(hex(release)+"  "+name+"\n"+(listed?hex(control)+"  "+name+".zsync\n":QString())).toUtf8();
  server.files["/release/"+tag.toUtf8()+"/SHA256SUMS"]=sums;
  server.files["/mirror/"+tag.toUtf8()+"/SHA256SUMS.minisig"]=signer_.signature(sums,"put.gd "+tag.toUtf8()+" nightly");
  server.files["/mirror/"+tag.toUtf8()+"/"+name.toUtf8()+".zsync"]=control;
 }
 InstallOptions options(const QString& method="auto") const {
  InstallOptions o;o.channel="nightly";o.method=method;o.stateRoot=state;o.image=image();
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
 void init(){
  QVERIFY(dir.isValid());
  for(const auto& name:QDir(dir.path()).entryList(QDir::AllEntries|QDir::Hidden|QDir::NoDotAndDotDot))QFileInfo(dir.filePath(name)).isDir()?QDir(dir.filePath(name)).removeRecursively():QFile::remove(dir.filePath(name));
  images=dir.filePath("Applications");state=dir.filePath("state");
  QVERIFY(QDir().mkpath(images));
  write(image(),old_,QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner|QFile::ReadGroup|QFile::ExeGroup);
 }
 void cleanup(){for(const char* name:{"PUTGD_UPDATE_FEED_URL","PUTGD_UPDATE_RELEASE_URL","PUTGD_UPDATE_MIRROR_URL"})qunsetenv(name);}
 void aDownloadInstallsFromTheMirrorAndKeepsABackup(){
  ReleaseServer server;publish(server,next_);
  QList<QJsonObject> progress;Error error;
  const auto result=installUpdate(build(),options("download"),[&](const QJsonObject& line){progress.append(line);},error);
  QVERIFY2(!error,qPrintable(error.message+" "+QJsonDocument(error.details).toJson()));
  QCOMPARE(result["installed"].toBool(),true);QCOMPARE(result["method"].toString(),QString("download"));QCOMPARE(result["source"].toString(),QString("mirror"));
  QVERIFY(result["zsync"].isNull());QCOMPARE(result["downloadedBytes"].toInteger(),qint64(next_.size()));
  QVERIFY(!server.requested("GET /release/"+tag.toUtf8()+"/"+name.toUtf8()));QVERIFY(!server.requested("GET /mirror/"+tag.toUtf8()+"/"+name.toUtf8()+".zsync"));
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
 // #1765: a mirror without the image, with a short one or with wrong bytes falls back to GitHub.
 // An earlier install's staging folder that can't be removed keeps its record:
 // the next install refuses instead of forgetting a partly written image.
 void anEarlierStagingFolderIsRemovedBeforeANewOne(){
  ReleaseServer server;publish(server,next_);
  const auto earlier=QDir(images).filePath(".putgd-update-AbC123");
  QVERIFY(QDir().mkpath(earlier+"/not-ours"));write(earlier+"/partial.AppImage","partial");
  QVERIFY(appimage::writeState(state,QJsonObject{{"staging",earlier}}));
  Error error;installUpdate(build(),options(),{},error);
  QCOMPARE(error.code,QString("update_install_failed"));QCOMPARE(appimage::readState(state)["staging"].toString(),earlier);QCOMPARE(read(image()),old_);
  // Once it can go, it goes, and the install runs.
  QVERIFY(QDir(earlier+"/not-ours").removeRecursively());
  error={};const auto result=installUpdate(build(),options(),{},error);
  QVERIFY2(!error,qPrintable(error.message));QVERIFY(!QFileInfo::exists(earlier));QCOMPARE(read(image()),next_);QCOMPARE(result["installed"].toBool(),true);
 }
 void aMirrorWithoutTheImageFallsBackToTheRelease(){
  for(const QString mirror:{"missing","short","wrong"}){
   init();
   ReleaseServer server;publish(server,next_);const auto mirrored="/mirror/"+tag.toUtf8()+"/"+name.toUtf8();
   if(mirror=="missing")server.files.remove(mirrored);
   else if(mirror=="short")server.files[mirrored]=next_.left(next_.size()/2);
   else {auto wrong=next_;wrong[wrong.size()-1]=char(wrong.at(wrong.size()-1)^1);server.files[mirrored]=wrong;}
   Error error;const auto result=installUpdate(build(),options("download"),{},error);
   QVERIFY2(!error,qPrintable(mirror+": "+error.message+" "+QJsonDocument(error.details).toJson()));
   QCOMPARE(result["method"].toString(),QString("download"));QCOMPARE(result["source"].toString(),QString("release"));
   QCOMPARE(result["downloads"].toArray().first().toObject()["sha256"].toString(),mirror=="wrong"?QString("mismatch"):QString());
   QVERIFY(server.requested("GET "+mirrored));QCOMPARE(read(image()),next_);
  }
 }
 // The running image supplies most blocks; only the rest is fetched, several
 // runs per request (multipart) or one (a single 206 part).
 void zsyncReusesTheRunningImageAndFetchesOnlyTheRest(){
  for(const bool oneRun:{false,true}) {
   init();
   const auto next=oneRun?QByteArray(old_).replace(5000,4,"5678"):next_;
   ReleaseServer server;publish(server,next,{},next);
   QList<QJsonObject> progress;Error error;
   const auto result=installUpdate(build(),options("zsync"),[&](const QJsonObject& line){progress.append(line);},error);
   QVERIFY2(!error,qPrintable(error.message+" "+QJsonDocument(error.details).toJson()));
   QCOMPARE(result["method"].toString(),QString("zsync"));QCOMPARE(read(image()),next);QCOMPARE(read(result["backup"].toString()),old_);
   const auto zsync=result["zsync"].toObject();
   QCOMPARE(zsync["used"].toBool(),true);QVERIFY(!zsync.contains("fallbackReason"));
   QVERIFY2(zsync["reusedBytes"].toInteger()>next.size()*85/100,qPrintable(QJsonDocument(zsync).toJson()));
   QCOMPARE(zsync["reusedBytes"].toInteger()+zsync["downloadedBytes"].toInteger(),qint64(next.size()));
   QCOMPARE(result["reusedBytes"],zsync["reusedBytes"]);
   QCOMPARE(appimage::readState(state)["installed"].toObject()["method"].toString(),QString("zsync"));
   // Only ranges of the mirror's image: never the whole file, from either source.
   QVERIFY(!server.requested("GET /release/"+tag.toUtf8()+"/"+name.toUtf8()));QVERIFY(!server.ranges.isEmpty());
   QCOMPARE(server.ranges.size(),zsync["requests"].toInt());QCOMPARE(server.requests.count("GET /mirror/"+tag.toUtf8()+"/"+name.toUtf8()+" HTTP/1.1"),server.ranges.size());
   QCOMPARE(server.ranges.first().contains(','),!oneRun);
   QStringList steps;for(const auto& line:progress)if(line["phase"]=="zsync"&&!steps.contains(line["step"].toString()))steps.append(line["step"].toString());
   QCOMPARE(steps,(QStringList{"reading","fetching"}));
   cleanup();
  }
 }
 // A server that answers ranges with the whole file, sends wrong bytes or
 // keeps sending, a .zsync the signed SHA256SUMS doesn't list or doesn't match,
 // one put.gd can't use, one whose SHA-1 is another file's, one that builds a
 // file SHA256SUMS doesn't name, none at all, and one that never arrives: each
 // falls back to the whole download, with the reason.
 void aZsyncThatCantFinishFallsBackToTheDownload(){
  const auto control="/mirror/"+tag.toUtf8()+"/"+name.toUtf8()+".zsync";
  const auto recompressed=QByteArray(zsyncmake(next_,name)).replace("\nSHA-1:","\nSafe: Recompress\nRecompress: 1f8b0800 0\nSHA-1:");
  // Another file of the same length, with its own .zsync, listed and served.
  auto other=next_;other[150000]=char(other[150000]^1);
  const QList<std::pair<QString,std::function<void(ReleaseServer&)>>> cases{
   {"range_refused",[](ReleaseServer& server){server.ignoreRanges=true;}},
   {"range_checksum",[](ReleaseServer& server){server.corruptRanges=true;}},
   {"range_too_large",[](ReleaseServer& server){server.trailingBytes=1<<20;}},
   {"control_unlisted",[&](ReleaseServer& server){serveControl(server,next_,zsyncmake(next_,name),false);}},
   {"control_sha256_mismatch",[&](ReleaseServer& server){server.files[control]=zsyncmake(next_,name,2048);}},
   {"bad_header",[&](ReleaseServer& server){serveControl(server,next_,"zsync");}},
   {"unsupported_hash_lengths",[&](ReleaseServer& server){serveControl(server,next_,zsyncmake(next_,name,1024,"3,2,5"));}},
   // 2^32+2 would be 2 once narrowed to int.
   {"unsupported_hash_lengths",[&](ReleaseServer& server){serveControl(server,next_,zsyncmake(next_,name,1024,"4294967298,2,5"));}},
   {"unsupported_header",[&](ReleaseServer& server){serveControl(server,next_,recompressed);}},
   {"sha1_mismatch",[&](ReleaseServer& server){serveControl(server,next_,zsyncmake(next_,name,1024,"2,2,5",QByteArray(40,'0')));}},
   {"verification_failed:sha256",[&](ReleaseServer& server){serveControl(server,next_,zsyncmake(other,name));server.files["/mirror/"+tag.toUtf8()+"/"+name.toUtf8()]=other;}},
   {"mirror_unavailable",[&](ReleaseServer& server){server.files.remove(control);}},
   {"timeout",[&](ReleaseServer& server){server.stalls.insert(control);}}};
  for(const auto& [reason,breakIt]:cases) {
   init();
   ReleaseServer server;publish(server,next_);breakIt(server);
   auto o=options();if(reason=="timeout")o.zsyncTimeoutMs=300;
   Error error;const auto result=installUpdate(build(),o,{},error);
   QVERIFY2(!error,qPrintable(reason+": "+error.message));
   QCOMPARE(result["zsync"].toObject()["fallbackReason"].toString(),reason);QCOMPARE(result["zsync"].toObject()["used"].toBool(),false);
   QCOMPARE(result["method"].toString(),QString("download"));QCOMPARE(read(image()),next_);QCOMPARE(leftovers().size(),1);
   cleanup();
  }
 }
 void zsyncOnlyDoesNotFallBack(){
  ReleaseServer server;publish(server,next_);server.ignoreRanges=true;
  Error error;installUpdate(build(),options("zsync"),{},error);
  QCOMPARE(error.code,QString("update_zsync_failed"));QCOMPARE(error.details["zsync"].toObject()["fallbackReason"].toString(),QString("range_refused"));
  QCOMPARE(read(image()),old_);QVERIFY(leftovers().isEmpty());QVERIFY(!server.requested("GET /release/"+tag.toUtf8()+"/"+name.toUtf8()));
 }
 // Qt's error text names the URL it fetched; a redirect's signed URL carries
 // a token in its query, which must not reach update.status or the log.
 void aRedirectsTokenNeverReachesTheErrorDetails(){
  ReleaseServer server;publish(server,next_);
  const auto signedUrl=server.base()+"/signed/asset?token=SECRET123&X-Amz-Signature=SECRET456";
  for(const auto& path:{"/mirror/"+tag.toUtf8()+"/"+name.toUtf8()+".zsync","/mirror/"+tag.toUtf8()+"/"+name.toUtf8(),"/release/"+tag.toUtf8()+"/"+name.toUtf8()})server.redirects[path]=signedUrl;
  for(const QString method:{"zsync","download"}) {
   Error error;installUpdate(build(),options(method),{},error);
   QCOMPARE(error.code,method=="zsync"?QString("update_zsync_failed"):QString("update_download_failed"));
   const auto details=QJsonDocument(error.details).toJson()+error.message.toUtf8();
   QVERIFY2(details.contains("ContentNotFoundError"),details.constData());QVERIFY2(!details.contains("SECRET")&&!details.contains("/signed/"),details.constData());
   QCOMPARE(read(image()),old_);
  }
 }
 void aTamperedDownloadIsRefusedAndNothingChanges(){
  ReleaseServer server;auto tampered=next_;tampered[tampered.size()-1]='X';publish(server,tampered);
  Error error;installUpdate(build(),options("download"),{},error);
  QCOMPARE(error.code,QString("update_verification_failed"));QCOMPARE(error.details["check"].toString(),QString("sha256"));
  QCOMPARE(read(image()),old_);QVERIFY(leftovers().isEmpty());
  QVERIFY(appimage::readState(state)["staging"].isNull());QVERIFY(!appimage::readState(state).contains("installed"));
 }
 // #412: an image put.gd must not replace is refused before anything is
 // fetched or written, with the reason Settings shows.
 void anImageItMustNotReplaceIsRefusedBeforeDownloading(){
  ReleaseServer server;publish(server,next_);
  const auto folder=QFileInfo(image()).canonicalPath();
  auto otherOwner=options();otherOwner.access.ownedByUser=[](const QString&){return false;};
  auto readOnly=options();readOnly.access.readOnly=[](const QString&){return true;};
  auto unwritable=options();unwritable.access.writable=[](const QString&){return false;};
  const QList<std::pair<InstallOptions,QString>> refusals{
   {otherOwner,"The AppImage belongs to another user, so put.gd won't replace it"},
   {readOnly,"The AppImage's folder is on a read-only file system"},
   {unwritable,"put.gd can't write to the AppImage's folder"}};
  for(const auto& [refused,message]:refusals) {
   QList<QJsonObject> progress;Error error;
   installUpdate(build(),refused,[&](const QJsonObject& line){progress.append(line);},error);
   QCOMPARE(error.code,QString("update_image_not_writable"));QCOMPARE(error.message,message);QCOMPARE(error.details["directory"].toString(),folder);
   // Only the feed was read; the install never got to verifying.
   for(const auto& line:progress)QCOMPARE(line["phase"].toString(),QString("checking"));
   QCOMPARE(read(image()),old_);QVERIFY(leftovers().isEmpty());QVERIFY(!QFileInfo::exists(appimage::statePath(state)));
  }
  QVERIFY(!server.requested("GET /release/"+tag.toUtf8()+"/SHA256SUMS"));QVERIFY(!server.requested("GET /release/"+tag.toUtf8()+"/"+name.toUtf8()));
  QVERIFY(!server.requested("GET /mirror/"+tag.toUtf8()+"/"+name.toUtf8()));
  // A folder without write access, for real.
  if(::getuid()==0)QSKIP("root writes anywhere");
  QVERIFY(QFile::setPermissions(images,QFile::ReadOwner|QFile::ExeOwner));
  const auto restore=qScopeGuard([&]{QFile::setPermissions(images,QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner);});
  Error error;installUpdate(build(),options(),{},error);
  QCOMPARE(error.code,QString("update_image_not_writable"));QCOMPARE(error.message,QString("put.gd can't write to the AppImage's folder"));
  QCOMPARE(read(image()),old_);QVERIFY(leftovers().isEmpty());
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
