#include "UpdateInstall.h"
#include "UpdateCheck.h"
#include "Minisign.h"
#include "Zsync.h"
#include "app/AppImageInstall.h"
#include "core/BuildInfo.h"
#include "core/CaBundle.h"
#include "core/Log.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDeadlineTimer>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QTemporaryDir>
#include <QTimer>
#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstring>
namespace putgd {
namespace {
constexpr qint64 maximumSumsBytes=1024*1024,maximumSignatureBytes=16*1024;
// No bytes for this long ends a transfer; Qt measures it between reads.
constexpr int stallTimeoutMs=60000;
// A .zsync names 7 bytes per 4 KiB block of an image no larger than this.
constexpr qint64 maximumControlBytes=8*1024*1024;
void printLine(const QJsonObject& object) {
 const auto bytes=QJsonDocument(object).toJson(QJsonDocument::Compact)+'\n';std::fwrite(bytes.constData(),1,bytes.size(),stdout);std::fflush(stdout);
}
struct Transfer {int status=0;QString network;bool tooLarge=false,timedOut=false,ok=false;qint64 bytes=0;};
// One GET. With a sink the body streams into it and into hash; without one it
// is returned. limit bounds the body; past deadline the request is aborted.
QByteArray fetch(QNetworkAccessManager& network,const QUrl& url,qint64 limit,QIODevice* sink,QCryptographicHash* hash,
                 const std::function<void(qint64,qint64)>& progress,Transfer& transfer,const QDeadlineTimer& deadline=QDeadlineTimer(QDeadlineTimer::Forever)) {
 QNetworkRequest request(url);
 request.setHeader(QNetworkRequest::UserAgentHeader,"put.gd-update/1");
 request.setAttribute(QNetworkRequest::CookieLoadControlAttribute,QNetworkRequest::Manual);
 request.setAttribute(QNetworkRequest::CookieSaveControlAttribute,QNetworkRequest::Manual);
 request.setAttribute(QNetworkRequest::CacheLoadControlAttribute,QNetworkRequest::AlwaysNetwork);
 request.setAttribute(QNetworkRequest::CacheSaveControlAttribute,false);
 // GitHub's release downloads redirect to its asset host; never to plain http.
 request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,QNetworkRequest::NoLessSafeRedirectPolicy);
 request.setMaximumRedirectsAllowed(5);request.setTransferTimeout(stallTimeoutMs);
 QEventLoop loop;QByteArray body;
 auto* reply=network.get(request);
 QTimer expiry;expiry.setSingleShot(true);
 QObject::connect(&expiry,&QTimer::timeout,reply,[&]{transfer.timedOut=true;reply->abort();});
 if(!deadline.isForever())expiry.start(int(qBound(qint64(0),deadline.remainingTime(),qint64(INT_MAX))));
 const auto drain=[&]{
  const auto chunk=reply->readAll();if(chunk.isEmpty())return;
  transfer.bytes+=chunk.size();
  if(transfer.bytes>limit){transfer.tooLarge=true;reply->abort();return;}
  if(hash)hash->addData(chunk);
  if(sink){if(sink->write(chunk)!=chunk.size()){transfer.network="Cannot write the download";reply->abort();}}
  else body+=chunk;
 };
 QObject::connect(reply,&QNetworkReply::readyRead,reply,drain);
 if(progress)QObject::connect(reply,&QNetworkReply::downloadProgress,reply,[&](qint64 received,qint64 total){progress(received,total);});
 QObject::connect(reply,&QNetworkReply::finished,&loop,&QEventLoop::quit);
 if(!reply->isFinished())loop.exec();
 if(reply->isOpen()&&!transfer.tooLarge)drain();
 transfer.status=reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
 if(transfer.network.isEmpty()&&reply->error()!=QNetworkReply::NoError)transfer.network=networkErrorText(*reply);
 transfer.ok=!transfer.tooLarge&&!transfer.timedOut&&transfer.network.isEmpty()&&transfer.status==200;
 reply->deleteLater();
 return body;
}
QJsonObject transferDetails(const QUrl& url,const Transfer& transfer) {
 return {{"url",url.toString(QUrl::RemoveUserInfo|QUrl::RemoveQuery|QUrl::RemoveFragment)},{"httpStatus",transfer.status?QJsonValue(transfer.status):QJsonValue()},{"network",transfer.network.isEmpty()?QJsonValue():QJsonValue(transfer.network)},{"tooLarge",transfer.tooLarge}};
}
}
QJsonObject installUpdate(const update::Build& build,const InstallOptions& options,const std::function<void(const QJsonObject&)>& emitLine,Error& error) {
 // zsync must give up while the whole-file download still has its time: its
 // deadline counts from the child's start, not from the .zsync request.
 const QDeadlineTimer zsyncDeadline(options.zsyncTimeoutMs);
 // A new phase (or zsync step) is reported at once; byte counts at most every 250 ms.
 QString lastPhase;QElapsedTimer sinceLast;
 const auto progress=[&](const QString& phase,const QString& method,qint64 bytes,qint64 total,const QJsonObject& extra={}){
  const auto key=phase+'/'+extra["step"].toString();
  if(!emitLine||(key==lastPhase&&sinceLast.isValid()&&sinceLast.elapsed()<250&&bytes<total))return;
  lastPhase=key;sinceLast.restart();
  QJsonObject line{{"event","progress"},{"phase",phase},{"method",method.isEmpty()?QJsonValue():QJsonValue(method)},{"bytes",bytes},{"total",total}};
  for(auto it=extra.begin();it!=extra.end();++it)line[it.key()]=it.value();
  emitLine(line);
 };
 if(build.target!="appimage"||!build.supported){error={"update_install_unavailable","Only AppImage builds install updates themselves",{{"packageTarget",build.target},{"reason",build.reason}}};return {};}
 if(!QStringList{"auto","zsync","download"}.contains(options.method)){error={"invalid_request","method is auto, zsync or download",{}};return {};}
 // Another channel's image is a switch, never an update (#344).
 const bool switching=options.channel!=build.channel;
 if(switching&&!options.switchChannel){error={"update_switch_confirm_required","Installing another channel's version is a channel switch, which must be asked for",{{"channel",options.channel},{"buildChannel",build.channel}}};return {};}
 progress("checking",{},0,0);
 const auto check=fetchUpdate(build,options.channel,error);if(error)return {};
 if(switching) {
  if(!check["latest"].isString()){error={"update_no_release","There's no "+options.channel+" release yet",{{"channel",options.channel}}};return {};}
  const auto order=update::compareVersions(check["latest"].toString(),build.version);
  if(order&&*order<0&&!options.allowOlder){
   error={"update_switch_older",options.channel+" "+check["latest"].toString()+" is older than this build ("+build.version+"); switching to it must be confirmed",{{"latest",check["latest"]},{"current",build.version}}};return {};
  }
 } else if(!check["updateAvailable"].toBool()){error={"update_not_available","This is the newest version on the channel",{{"current",build.version},{"latest",check["latest"]}}};return {};}
 const auto asset=check["asset"].toObject();const auto tag=check["tag"].toString(),version=check["latest"].toString(),name=asset["name"].toString();
 if(asset.isEmpty()||!name.endsWith(".AppImage")){error={"update_install_unavailable","The release names no AppImage to install",{{"tag",tag}}};return {};}
 const auto size=asset["size"].toInteger();
 if(size<=0||size>appimage::maximumImageBytes){error={"update_verification_failed","The release's AppImage has an implausible size",{{"size",size}}};return {};}
 auto image=appimage::inspect(options.image,size,error,options.access);if(error)return {};
 // What the new image must embed: the same channel's permalink, or the target's.
 const auto information=switching?update::switchedUpdateInformation(image.updateInformation,build.channel,options.channel):image.updateInformation;
 if(information.isEmpty()){error={"update_install_unavailable","This AppImage doesn't follow put.gd's "+build.channel+" update permalink, so it can't switch channels",{{"updateInformation",image.updateInformation}}};return {};}
 auto state=appimage::readState(options.stateRoot);
 // An install that is waiting for its restart is finished first.
 const auto pending=state.value("installed").toObject();
 if(!state.value("backup").toObject().isEmpty()&&pending["image"]==image.path&&pending["version"]!=build.version){
  error={"update_restart_required","Restart put.gd to finish the update that is already installed",{{"installed",pending["version"]}}};return {};
 }

 // The release's own list, over TLS from the release, is the authority.
 progress("verifying",{},0,0);
 QNetworkAccessManager network;
 const auto sumsUrl=update::releaseFileUrl(tag,"SHA256SUMS",error);if(error)return {};
 Transfer sumsTransfer;const auto sums=fetch(network,sumsUrl,maximumSumsBytes,nullptr,nullptr,{},sumsTransfer);
 if(!sumsTransfer.ok){error={"update_sums_unavailable","Cannot read the release's SHA256SUMS",transferDetails(sumsUrl,sumsTransfer)};return {};}
 // The release signer's signature over exactly these bytes, by the release's
 // channel key: a SHA256SUMS that is unsigned, signed by another key (the
 // other channel's too) or for another release is refused.
 const auto signatureUrl=update::mirrorFileUrl(tag,"SHA256SUMS.minisig",error);if(error)return {};
 Transfer signatureTransfer;const auto signature=fetch(network,signatureUrl,maximumSignatureBytes,nullptr,nullptr,{},signatureTransfer);
 if(!signatureTransfer.ok){
  auto details=transferDetails(signatureUrl,signatureTransfer);details["check"]="missing";
  error={"update_signature_invalid","The release's SHA256SUMS has no signature; nothing was changed",details};return {};
 }
 const auto key=options.publicKeys.value(options.channel,minisign::releaseKey(options.channel));
 QString trusted;
 if(auto failed=minisign::verify(sums,signature,key,&trusted)){error=failed;error.message="The release's SHA256SUMS signature isn't valid ("+failed.message.toLower()+"); nothing was changed";return {};}
 if(trusted!="put.gd "+tag+" "+options.channel){error={"update_signature_invalid","The SHA256SUMS signature is for another release; nothing was changed",{{"check","release"},{"trustedComment",trusted}}};return {};}
 const auto expected=appimage::sumsLine(sums,name);
 if(expected.isEmpty()){error={"update_sums_unavailable","The release's SHA256SUMS doesn't list the AppImage",{{"name",name}}};return {};}
 if(asset.contains("sha256")&&asset["sha256"].toString()!=expected){
  error={"update_verification_failed","The update feed and the release's SHA256SUMS disagree; nothing was changed",{{"feed",asset["sha256"]},{"sums",expected}}};return {};
 }
 QJsonObject result{{"installed",false},{"version",version},{"tag",tag},{"previousVersion",build.version},{"image",image.path},{"sha256",expected},{"method",QJsonValue()},
  {"zsync",QJsonValue()},{"backup",QJsonValue()},{"signature",QJsonObject{{"trustedComment",trusted}}},
  {"channel",options.channel},{"previousChannel",build.channel},{"switched",switching}};
 // Already on disk (an earlier install, or another updater, replaced the file):
 // nothing to fetch, only the restart is left.
 if(appimage::sha256(image.path)==expected) {
  state["installed"]=QJsonObject{{"image",image.path},{"version",version},{"previousVersion",build.version},{"channel",options.channel},{"previousChannel",build.channel},
   {"sha256",expected},{"method","none"},{"installedAt",QDateTime::currentDateTimeUtc().toString(Qt::ISODate)}};
  state["backup"]=QJsonValue();
  if(appimage::removeStaging(state["staging"].toString()))state["staging"]=QJsonValue();
  appimage::writeState(options.stateRoot,state);
  result["installed"]=true;result["method"]="none";progress("done","none",size,size);
  return result;
 }

 // An earlier install's staging folder beside this image goes first; its record
 // stays until it has, so a retry never forgets a partly written image. One left
 // in another folder (the image moved) doesn't block this image's update.
 if(const auto earlier=state["staging"].toString();!earlier.isEmpty()&&!appimage::removeStaging(earlier)
    &&QDir::cleanPath(QFileInfo(earlier).absolutePath())==QDir::cleanPath(QFileInfo(image.directory).absoluteFilePath())){
  error={"update_install_failed","An earlier update's staging folder can't be removed",{{"staging",earlier}}};return {};
 }
 QTemporaryDir staging(QDir(image.directory).filePath(QString(appimage::stagingPrefix)+"XXXXXX"));
 if(!staging.isValid()){error={"update_image_not_writable","Cannot create a staging folder beside the AppImage",{{"directory",image.directory}}};return {};}
 state["staging"]=staging.path();state["backup"]=QJsonValue();
 if(!appimage::writeState(options.stateRoot,state)){error={"update_install_failed","Cannot save the update's state",{}};return {};}
 const auto clearStaging=[&]{auto current=appimage::readState(options.stateRoot);current["staging"]=QJsonValue();appimage::writeState(options.stateRoot,current);};
 const auto candidatePath=QDir(staging.path()).filePath(name);
 QString candidate;QJsonObject zsync;

 // zsync (#1766): the blocks the running image already has, at any offset,
 // and only the rest from the mirror's image with Range requests. Any
 // failure falls back to the whole file.
 qint64 downloaded=0,reused=0;
 if(options.method!="download") {
  QElapsedTimer took;took.start();
  // One deadline from the child's start to the last range.
  const auto& deadline=zsyncDeadline;
  QString reason;QJsonObject details;zsync::Control control;
  zsync={{"used",false},{"reusedBytes",0},{"downloadedBytes",0},{"controlBytes",0},{"requests",0},{"ranges",0}};
  Error urlError;const auto controlUrl=update::mirrorFileUrl(tag,name+".zsync",urlError);const auto imageUrl=update::mirrorFileUrl(tag,name,urlError);
  // The signed SHA256SUMS lists the .zsync too: nothing in it is trusted before it matches.
  const auto controlSha256=appimage::sumsLine(sums,name+".zsync");
  if(urlError)reason="mirror_invalid";
  else if(controlSha256.isEmpty())reason="control_unlisted";
  if(reason.isEmpty()) {
   progress("zsync","zsync",0,image.size,{{"step","reading"}});
   Transfer transfer;const auto data=fetch(network,controlUrl,maximumControlBytes,nullptr,nullptr,{},transfer,deadline);
   downloaded+=transfer.bytes;zsync["controlBytes"]=transfer.bytes;
   if(!transfer.ok){reason=transfer.timedOut?"timeout":transfer.tooLarge?"control_too_large":"mirror_unavailable";details=transferDetails(controlUrl,transfer);}
   else if(const auto actual=QString::fromLatin1(QCryptographicHash::hash(data,QCryptographicHash::Sha256).toHex());actual!=controlSha256){reason="control_sha256_mismatch";details={{"expected",controlSha256},{"actual",actual}};}
   // The .zsync must describe the file SHA256SUMS names.
   else if(zsync::parse(data,control,reason)&&(control.filename!=name||control.length!=size)){reason="control_mismatch";details={{"filename",control.filename},{"length",control.length}};}
  }
  if(reason.isEmpty()) {
   QFile seed(image.path),output(candidatePath);
   zsync::Assembly assembly;zsync::Fetch fetched;
   if(!seed.open(QIODevice::ReadOnly|QIODevice::Unbuffered))reason="seed_unreadable";
   else if(!output.open(QIODevice::ReadWrite|QIODevice::Truncate)||!output.resize(size))reason="write_failed";
   else if(zsync::reuse(control,seed,output,assembly,deadline,[&](qint64 bytes,qint64 total){progress("zsync","zsync",bytes,total,{{"step","reading"}});},reason)) {
    reused=assembly.reusedBytes;zsync["reusedBytes"]=reused;
    const auto ranges=zsync::missing(control,assembly);zsync["ranges"]=qint64(ranges.size());
    const auto wanted=size-reused;
    progress("zsync","zsync",0,wanted,{{"step","fetching"},{"reused",reused}});
    const bool ok=zsync::fetch(network,imageUrl,control,ranges,output,fetched,deadline,[&](qint64 bytes){progress("zsync","zsync",bytes,wanted,{{"step","fetching"},{"reused",reused}});},reason,details);
    downloaded+=fetched.downloadedBytes;zsync["downloadedBytes"]=fetched.downloadedBytes;zsync["requests"]=fetched.requests;
    if(ok&&!output.flush())reason="write_failed";
    output.close();
    if(ok&&reason.isEmpty()) {
     progress("verifying","zsync",size,size);
     if(deadline.hasExpired())reason="timeout";
     else if(zsync::sha1(candidatePath)!=control.sha1)reason="sha1_mismatch";
     else if(const auto failed=appimage::verify(candidatePath,expected,information))reason="verification_failed:"+failed.details["check"].toString();
     else candidate=candidatePath;
    }
   }
  }
  zsync["used"]=!candidate.isEmpty();zsync["seconds"]=double(took.elapsed())/1000;
  if(candidate.isEmpty()){zsync["fallbackReason"]=reason;if(!details.isEmpty())zsync["details"]=details;}
  result["zsync"]=zsync;
  if(candidate.isEmpty()&&options.method=="zsync"){QFile::remove(candidatePath);clearStaging();error={"update_zsync_failed","The zsync update didn't work: "+reason,{{"zsync",zsync}}};return {};}
 }

 if(candidate.isEmpty()) {
  QFile::remove(candidatePath);
  const auto part=QDir(staging.path()).filePath("download.part");
  QJsonArray attempts;QString lastFailure;
  // The mirror (CloudFront, no redirect) first; GitHub when it lacks the image or fails (#1765).
  for(const auto& source:{QString("mirror"),QString("release")}) {
   Error urlError;const auto url=source=="release"?update::releaseFileUrl(tag,name,urlError):update::mirrorFileUrl(tag,name,urlError);
   if(urlError)continue;
   QFile file(part);
   if(!file.open(QIODevice::WriteOnly|QIODevice::Truncate)){clearStaging();error={"update_image_not_writable","Cannot write the download beside the AppImage",{{"directory",image.directory}}};return {};}
   QCryptographicHash hash(QCryptographicHash::Sha256);Transfer transfer;
   progress("download",source,0,size);
   fetch(network,url,size,&file,&hash,[&](qint64 received,qint64){progress("download",source,received,size);},transfer);
   file.close();downloaded+=transfer.bytes;
   attempts.append(QJsonObject{{"source",source},{"transfer",transferDetails(url,transfer)}});
   if(!transfer.ok||transfer.bytes!=size){lastFailure=transfer.tooLarge?"larger than the release says":transfer.network.isEmpty()?QString("HTTP %1").arg(transfer.status):transfer.network;continue;}
   // A mirror copy with the wrong bytes falls back to GitHub; wrong bytes from GitHub stop the install.
   if(QString::fromLatin1(hash.result().toHex())!=expected){
    if(source=="mirror"){auto attempt=attempts.last().toObject();attempt["sha256"]="mismatch";attempts.replace(attempts.size()-1,attempt);lastFailure="the pkg.put.gd copy doesn't match SHA256SUMS";continue;}
    clearStaging();
    error={"update_verification_failed","The download doesn't match the release's SHA256SUMS; nothing was changed",{{"check","sha256"},{"expected",expected},{"actual",QString::fromLatin1(hash.result().toHex())},{"source",source}}};return {};
   }
   progress("verifying",source,size,size);
   if(!QFile::rename(part,candidatePath)){clearStaging();error={"update_install_failed","Cannot prepare the download beside the AppImage",{}};return {};}
   if(auto failed=appimage::verify(candidatePath,expected,information)){clearStaging();error=failed;error.details["source"]=source;return {};}
   candidate=candidatePath;result["method"]="download";result["source"]=source;reused=0;
   break;
  }
  result["downloads"]=attempts;
  if(candidate.isEmpty()){clearStaging();error={"update_download_failed","Cannot download the new version: "+lastFailure,{{"attempts",attempts}}};return {};}
 } else result["method"]="zsync";

 progress("installing",result["method"].toString(),size,size);
 const auto backup=appimage::swap(candidate,image,build.version,error);
 if(error){clearStaging();return {};}
 state=appimage::readState(options.stateRoot);
 state["staging"]=QJsonValue();state["backup"]=backup;
 state["installed"]=QJsonObject{{"image",image.path},{"version",version},{"previousVersion",build.version},{"channel",options.channel},{"previousChannel",build.channel},
  {"sha256",expected},{"method",result["method"]},{"downloadedBytes",downloaded},{"reusedBytes",reused},
  {"installedAt",QDateTime::currentDateTimeUtc().toString(Qt::ISODate)}};
 appimage::writeState(options.stateRoot,state);
 result["installed"]=true;result["backup"]=backup["path"];result["downloadedBytes"]=downloaded;result["reusedBytes"]=reused;
 progress("done",result["method"].toString(),size,size);
 return result;
}
bool isInstallMode(int argc,char** argv) {return argc>=3&&std::strcmp(argv[1],"--install-update")==0;}
int runInstallMode(int argc,char** argv) {
 QCoreApplication app(argc,argv);QCoreApplication::setOrganizationName("putgd");QCoreApplication::setApplicationName("putgd-cli");
 InstallOptions options;options.channel=QString::fromLocal8Bit(argv[2]);options.stateRoot=logging::stateRoot();
 const auto arguments=app.arguments();
 for(int index=3;index<arguments.size();++index) {
  if(arguments[index]=="--switch"){options.switchChannel=true;continue;}
  if(arguments[index]=="--allow-older"){options.allowOlder=true;continue;}
  if(index+1>=arguments.size()){printLine({{"schemaVersion",1},{"ok",false},{"result",QJsonObject{}},{"error",Error{"invalid_request","--method and --state take a value",{}}.toJson()}});return 2;}
  if(arguments[index]=="--method")options.method=arguments[++index];
  else if(arguments[index]=="--state")options.stateRoot=arguments[++index];
  else {printLine({{"schemaVersion",1},{"ok",false},{"result",QJsonObject{}},{"error",Error{"invalid_request","Unknown option "+arguments[index].left(40),{}}.toJson()}});return 2;}
 }
 // The image this process runs from, and the host CA bundle AppRun found
 // (#329) for put.gd's own downloads.
 options.image=qEnvironmentVariable("APPIMAGE");
 useHostCaBundle();
 const auto build=update::build(QJsonDocument::fromJson(buildManifestJson()).object());
 Error error;const auto result=installUpdate(build,options,printLine,error);
 printLine({{"schemaVersion",1},{"ok",!error},{"result",error?QJsonObject{}:result},{"error",error.toJson()}});
 return error?1:0;
}
}
