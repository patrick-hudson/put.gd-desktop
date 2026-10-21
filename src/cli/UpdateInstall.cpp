#include "UpdateInstall.h"
#include "UpdateCheck.h"
#include "Minisign.h"
#include "app/AppImageInstall.h"
#include "core/BuildInfo.h"
#include "core/CaBundle.h"
#include "core/HostEnvironment.h"
#include "core/Log.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QProcess>
#include <QTemporaryDir>
#include <QTimer>
#include <cstdio>
#include <cstring>
namespace putgd {
namespace {
constexpr qint64 maximumSumsBytes=1024*1024,maximumSignatureBytes=16*1024;
// No bytes for this long ends a transfer; Qt measures it between reads.
constexpr int stallTimeoutMs=60000;
// A zsync tool whose output doesn't grow for this long, or that runs longer, is stopped.
constexpr qint64 zsyncStallMs=2*60*1000,zsyncTotalMs=15*60*1000;
constexpr int progressIntervalMs=500;
void printLine(const QJsonObject& object) {
 const auto bytes=QJsonDocument(object).toJson(QJsonDocument::Compact)+'\n';std::fwrite(bytes.constData(),1,bytes.size(),stdout);std::fflush(stdout);
}
struct Transfer {int status=0;QString network;bool tooLarge=false,ok=false;qint64 bytes=0;};
// One GET (or HEAD). With a sink the body streams into it and into hash;
// without one it is returned. limit bounds the body.
QByteArray fetch(QNetworkAccessManager& network,const QUrl& url,qint64 limit,QIODevice* sink,QCryptographicHash* hash,
                 const std::function<void(qint64,qint64)>& progress,Transfer& transfer,bool head=false) {
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
 auto* reply=head?network.head(request):network.get(request);
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
 if(transfer.network.isEmpty()&&reply->error()!=QNetworkReply::NoError)transfer.network=reply->errorString().left(200);
 transfer.ok=!transfer.tooLarge&&transfer.network.isEmpty()&&transfer.status==200;
 reply->deleteLater();
 return body;
}
QJsonObject transferDetails(const QUrl& url,const Transfer& transfer) {
 return {{"url",url.toString()},{"httpStatus",transfer.status?QJsonValue(transfer.status):QJsonValue()},{"network",transfer.network.isEmpty()?QJsonValue():QJsonValue(transfer.network)},{"tooLarge",transfer.tooLarge}};
}
// Bytes the tool has written in the staging folder so far, besides the seed.
qint64 written(const QString& staging,const QString& seed) {
 qint64 total=0;
 for(const auto& entry:QDir(staging).entryInfoList(QDir::Files|QDir::Hidden|QDir::NoDotAndDotDot))if(entry.absoluteFilePath()!=seed)total+=entry.size();
 return total;
}
// Runs the zsync tool; true when it exited 0. reason says why not.
bool runZsync(const appimage::ZsyncTool& tool,const QStringList& arguments,const QString& staging,const QString& seed,const QProcessEnvironment& environment,
              qint64 total,const std::function<void(qint64)>& progress,QString& reason,QByteArray& output) {
 QProcess process;process.setProgram(tool.path);process.setArguments(arguments);process.setWorkingDirectory(staging);
 process.setProcessEnvironment(environment);process.setProcessChannelMode(QProcess::MergedChannels);process.setStandardInputFile(QProcess::nullDevice());
 QEventLoop loop;QTimer tick;tick.setInterval(progressIntervalMs);
 QElapsedTimer started,grew;started.start();grew.start();qint64 last=-1;bool stopped=false;
 QObject::connect(&process,&QProcess::readyRead,&process,[&]{output+=process.readAll();if(output.size()>8192)output=output.right(4096);});
 QObject::connect(&process,&QProcess::finished,&loop,&QEventLoop::quit);
 QObject::connect(&process,&QProcess::errorOccurred,&loop,[&](QProcess::ProcessError error){if(error==QProcess::FailedToStart)loop.quit();});
 QObject::connect(&tick,&QTimer::timeout,&loop,[&]{
  const auto bytes=written(staging,seed);
  if(bytes!=last){last=bytes;grew.restart();if(progress)progress(qMin(bytes,total));}
  if(!stopped&&(grew.elapsed()>zsyncStallMs||started.elapsed()>zsyncTotalMs)){
   stopped=true;reason=grew.elapsed()>zsyncStallMs?"stalled":"timeout";
   process.terminate();QTimer::singleShot(5000,&process,[&process]{process.kill();});
  }
 });
 process.start();
 if(!process.waitForStarted(10000)){reason="failed_to_start";return false;}
 tick.start();
 if(process.state()!=QProcess::NotRunning)loop.exec();
 tick.stop();
 output+=process.readAll();
 if(stopped)return false;
 if(process.exitStatus()!=QProcess::NormalExit){reason="crashed";return false;}
 if(process.exitCode()!=0){reason=QString("exit_%1").arg(process.exitCode());return false;}
 return true;
}
QString firstLine(const QByteArray& output) {
 // The tool's last lines name what went wrong; only for the result's details.
 const auto lines=QString::fromUtf8(output).split('\n',Qt::SkipEmptyParts);
 return lines.isEmpty()?QString():lines.mid(qMax(0,int(lines.size())-4)).join(" | ").left(400);
}
}
QJsonObject installUpdate(const update::Build& build,const InstallOptions& options,const std::function<void(const QJsonObject&)>& emitLine,Error& error) {
 // A new phase is reported at once; byte counts at most every 250 ms.
 QString lastPhase;QElapsedTimer sinceLast;
 const auto progress=[&](const QString& phase,const QString& method,qint64 bytes,qint64 total){
  if(!emitLine||(phase==lastPhase&&sinceLast.isValid()&&sinceLast.elapsed()<250&&bytes<total))return;
  lastPhase=phase;sinceLast.restart();
  emitLine({{"event","progress"},{"phase",phase},{"method",method.isEmpty()?QJsonValue():QJsonValue(method)},{"bytes",bytes},{"total",total}});
 };
 if(build.target!="appimage"||!build.supported){error={"update_install_unavailable","Only AppImage builds install updates themselves",{{"packageTarget",build.target},{"reason",build.reason}}};return {};}
 if(!QStringList{"auto","zsync","download"}.contains(options.method)){error={"invalid_request","method is auto, zsync or download",{}};return {};}
 progress("checking",{},0,0);
 const auto check=fetchUpdate(build,options.channel,error);if(error)return {};
 if(!check["updateAvailable"].toBool()){error={"update_not_available","This is the newest version on the channel",{{"current",build.version},{"latest",check["latest"]}}};return {};}
 const auto asset=check["asset"].toObject();const auto tag=check["tag"].toString(),version=check["latest"].toString(),name=asset["name"].toString();
 if(asset.isEmpty()||!name.endsWith(".AppImage")){error={"update_install_unavailable","The release names no AppImage to install",{{"tag",tag}}};return {};}
 const auto size=asset["size"].toInteger();
 if(size<=0||size>appimage::maximumImageBytes){error={"update_verification_failed","The release's AppImage has an implausible size",{{"size",size}}};return {};}
 auto image=appimage::inspect(options.image,size,error);if(error)return {};
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
 // The release signer's signature over exactly these bytes, by this build's
 // channel key: a SHA256SUMS that is unsigned, signed by another key or for
 // another release is refused.
 const auto signatureUrl=update::mirrorFileUrl(tag,"SHA256SUMS.minisig",error);if(error)return {};
 Transfer signatureTransfer;const auto signature=fetch(network,signatureUrl,maximumSignatureBytes,nullptr,nullptr,{},signatureTransfer);
 if(!signatureTransfer.ok){
  auto details=transferDetails(signatureUrl,signatureTransfer);details["check"]="missing";
  error={"update_signature_invalid","The release's SHA256SUMS has no signature; nothing was changed",details};return {};
 }
 const auto key=options.publicKey.isEmpty()?minisign::releaseKey(build.channel):options.publicKey;
 QString trusted;
 if(auto failed=minisign::verify(sums,signature,key,&trusted)){error=failed;error.message="The release's SHA256SUMS signature isn't valid ("+failed.message.toLower()+"); nothing was changed";return {};}
 if(trusted!="put.gd "+tag+" "+build.channel){error={"update_signature_invalid","The SHA256SUMS signature is for another release; nothing was changed",{{"check","release"},{"trustedComment",trusted}}};return {};}
 const auto expected=appimage::sumsLine(sums,name);
 if(expected.isEmpty()){error={"update_sums_unavailable","The release's SHA256SUMS doesn't list the AppImage",{{"name",name}}};return {};}
 if(asset.contains("sha256")&&asset["sha256"].toString()!=expected){
  error={"update_verification_failed","The update feed and the release's SHA256SUMS disagree; nothing was changed",{{"feed",asset["sha256"]},{"sums",expected}}};return {};
 }
 QJsonObject result{{"installed",false},{"version",version},{"tag",tag},{"previousVersion",build.version},{"image",image.path},{"sha256",expected},{"method",QJsonValue()},
  {"zsync",QJsonValue()},{"backup",QJsonValue()},{"signature",QJsonObject{{"trustedComment",trusted}}}};
 // Already on disk (an earlier install, or another updater, replaced the file):
 // nothing to fetch, only the restart is left.
 if(appimage::sha256(image.path)==expected) {
  state["installed"]=QJsonObject{{"image",image.path},{"version",version},{"previousVersion",build.version},{"sha256",expected},{"method","none"},{"installedAt",QDateTime::currentDateTimeUtc().toString(Qt::ISODate)}};
  state["backup"]=QJsonValue();state["staging"]=QJsonValue();appimage::writeState(options.stateRoot,state);
  result["installed"]=true;result["method"]="none";progress("done","none",size,size);
  return result;
 }

 QTemporaryDir staging(QDir(image.directory).filePath(QString(appimage::stagingPrefix)+"XXXXXX"));
 if(!staging.isValid()){error={"update_image_not_writable","Cannot create a staging folder beside the AppImage",{{"directory",image.directory}}};return {};}
 state["staging"]=staging.path();state["backup"]=QJsonValue();
 if(!appimage::writeState(options.stateRoot,state)){error={"update_install_failed","Cannot save the update's state",{}};return {};}
 const auto clearStaging=[&]{auto current=appimage::readState(options.stateRoot);current["staging"]=QJsonValue();appimage::writeState(options.stateRoot,current);};
 const auto candidatePath=QDir(staging.path()).filePath(name);
 QString candidate;QJsonObject zsync;

 if(options.method!="download") {
  const auto host=host::environment();
  const auto tool=appimage::findZsyncTool(host);
  zsync={{"tool",tool?QJsonValue(tool.kind):QJsonValue()}};
  QString reason;
  if(!tool)reason="not_installed";
  Error urlError;const auto zsyncUrl=update::mirrorFileUrl(tag,name+".zsync",urlError);
  if(reason.isEmpty()&&urlError)reason="mirror_invalid";
  // The mirror must hold this release before the tool is asked to use it.
  if(reason.isEmpty()){Transfer probe;fetch(network,zsyncUrl,4*1024*1024,nullptr,nullptr,{},probe,true);if(!probe.ok)reason="mirror_unavailable";}
  if(reason.isEmpty()) {
   const auto seed=QDir(staging.path()).filePath("seed.AppImage");
   if(!appimage::linkOrCopy(image.path,seed))reason="seed_failed";
   else {
    QByteArray output;progress("zsync",tool.kind,0,size);
    const bool ran=runZsync(tool,appimage::zsyncArguments(tool,seed,candidatePath,zsyncUrl.toString()),staging.path(),seed,
                            appimage::zsyncEnvironment(host,options.caBundle),size,[&](qint64 bytes){progress("zsync",tool.kind,bytes,size);},reason,output);
    zsync["output"]=firstLine(output);
    if(ran&&!QFileInfo(candidatePath).isFile())reason="no_output";
    else if(ran) {
     progress("verifying",tool.kind,size,size);
     if(const auto failed=appimage::verify(candidatePath,expected,image.updateInformation))reason="verification_failed:"+failed.details["check"].toString();
     else candidate=candidatePath;
    }
    QFile::remove(seed);
   }
  }
  zsync["used"]=!candidate.isEmpty();
  if(candidate.isEmpty())zsync["fallbackReason"]=reason;
  result["zsync"]=zsync;
  if(candidate.isEmpty()&&options.method=="zsync"){clearStaging();error={"update_zsync_failed","The zsync update didn't work: "+reason,{{"zsync",zsync}}};return {};}
 }

 if(candidate.isEmpty()) {
  QFile::remove(candidatePath);
  const auto part=QDir(staging.path()).filePath("download.part");
  QJsonArray attempts;QString lastFailure;
  for(const auto& source:{QString("release"),QString("mirror")}) {
   Error urlError;const auto url=source=="release"?update::releaseFileUrl(tag,name,urlError):update::mirrorFileUrl(tag,name,urlError);
   if(urlError)continue;
   QFile file(part);
   if(!file.open(QIODevice::WriteOnly|QIODevice::Truncate)){clearStaging();error={"update_image_not_writable","Cannot write the download beside the AppImage",{{"directory",image.directory}}};return {};}
   QCryptographicHash hash(QCryptographicHash::Sha256);Transfer transfer;
   progress("download",source,0,size);
   fetch(network,url,size,&file,&hash,[&](qint64 received,qint64){progress("download",source,received,size);},transfer);
   file.close();
   attempts.append(QJsonObject{{"source",source},{"transfer",transferDetails(url,transfer)}});
   if(!transfer.ok||transfer.bytes!=size){lastFailure=transfer.tooLarge?"larger than the release says":transfer.network.isEmpty()?QString("HTTP %1").arg(transfer.status):transfer.network;continue;}
   // A complete file with the wrong bytes is not retried from elsewhere.
   if(QString::fromLatin1(hash.result().toHex())!=expected){
    clearStaging();
    error={"update_verification_failed","The download doesn't match the release's SHA256SUMS; nothing was changed",{{"check","sha256"},{"expected",expected},{"actual",QString::fromLatin1(hash.result().toHex())},{"source",source}}};return {};
   }
   progress("verifying",source,size,size);
   if(!QFile::rename(part,candidatePath)){clearStaging();error={"update_install_failed","Cannot prepare the download beside the AppImage",{}};return {};}
   if(auto failed=appimage::verify(candidatePath,expected,image.updateInformation)){clearStaging();error=failed;error.details["source"]=source;return {};}
   candidate=candidatePath;result["method"]="download";result["source"]=source;
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
 state["installed"]=QJsonObject{{"image",image.path},{"version",version},{"previousVersion",build.version},{"sha256",expected},{"method",result["method"]},
  {"installedAt",QDateTime::currentDateTimeUtc().toString(Qt::ISODate)}};
 appimage::writeState(options.stateRoot,state);
 result["installed"]=true;result["backup"]=backup["path"];
 progress("done",result["method"].toString(),size,size);
 return result;
}
bool isInstallMode(int argc,char** argv) {return argc>=3&&std::strcmp(argv[1],"--install-update")==0;}
int runInstallMode(int argc,char** argv) {
 QCoreApplication app(argc,argv);QCoreApplication::setOrganizationName("putgd");QCoreApplication::setApplicationName("putgd-cli");
 InstallOptions options;options.channel=QString::fromLocal8Bit(argv[2]);options.stateRoot=logging::stateRoot();
 const auto arguments=app.arguments();
 for(int index=3;index<arguments.size();index+=2) {
  if(index+1>=arguments.size()){printLine({{"schemaVersion",1},{"ok",false},{"result",QJsonObject{}},{"error",Error{"invalid_request","--method and --state take a value",{}}.toJson()}});return 2;}
  if(arguments[index]=="--method")options.method=arguments[index+1];
  else if(arguments[index]=="--state")options.stateRoot=arguments[index+1];
  else {printLine({{"schemaVersion",1},{"ok",false},{"result",QJsonObject{}},{"error",Error{"invalid_request","Unknown option "+arguments[index].left(40),{}}.toJson()}});return 2;}
 }
 // The image this process runs from, and the host CA bundle AppRun found (#329):
 // for put.gd's own downloads and, as SSL_CERT_FILE, for zsync2.
 options.image=qEnvironmentVariable("APPIMAGE");options.caBundle=hostCaBundle();
 useHostCaBundle();
 const auto build=update::build(QJsonDocument::fromJson(buildManifestJson()).object());
 Error error;const auto result=installUpdate(build,options,printLine,error);
 printLine({{"schemaVersion",1},{"ok",!error},{"result",error?QJsonObject{}:result},{"error",error.toJson()}});
 return error?1:0;
}
}
