#include "SealCheck.h"
#include "OperationService.h"
#include "core/Log.h"
#include "core/SealJob.h"
#include "seal/SealPage.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QProcess>
#include <QUuid>
namespace putgd {
namespace {
QJsonObject failed(const QString& reason,const QString& cleanup={}) {QJsonObject out{{"state","failed"},{"reason",reason}};if(!cleanup.isEmpty())out["cleanup"]=cleanup;return out;}
QString uuid() {return QUuid::createUuid().toString(QUuid::WithoutBraces);}
}
QJsonObject runSealCheck(StorageProvider& provider,const Destination& destination,const QString& staging,const QString& cli,std::shared_ptr<std::atomic_bool> cancelled) {
 if(!cancelled)cancelled=std::make_shared<std::atomic_bool>(false);
 if(destination.profileRevision.isEmpty())return failed(QStringLiteral("The destination's settings revision is missing."));
 const auto caps=provider.capabilities(destination);
 if(!caps.sealUnavailable.isEmpty())return failed(caps.sealUnavailable);
 // The page, as a private file the provider uploads like any artifact.
 const auto page=seal::probePage();
 if(!QDir().mkpath(staging))return failed(QStringLiteral("Couldn't write the test page."));
 Artifact artifact;artifact.id=uuid();artifact.path=staging+"/"+artifact.id;artifact.name=SealJob::randomName("html");artifact.mime="text/html";artifact.category="other";
 artifact.bytes=page.size();artifact.sha256=QString::fromLatin1(QCryptographicHash::hash(page,QCryptographicHash::Sha256).toHex());artifact.namingPrepared=true;
 {QFile file(artifact.path);if(!file.open(QIODevice::WriteOnly|QIODevice::NewOnly)||!file.setPermissions(QFile::ReadOwner|QFile::WriteOwner)||file.write(page)!=page.size())return failed(QStringLiteral("Couldn't write the test page."));}
 const auto removeFile=qScopeGuard([&]{QFile::remove(artifact.path);});
 auto check=awaitProvider<NameCheck>([&](auto cb){provider.checkName(destination,artifact.name,cb,cancelled);});
 if(cancelled->load())return {{"state","cancelled"}};
 if(check.error||check.status!="absent")return failed(check.error?check.error.message:QStringLiteral("Couldn't find a free name for the test page."));
 UploadRequest request{destination,artifact,artifact.name,uuid(),"absent",check.objectReference,check.concurrencyToken,cancelled};
 auto uploaded=awaitProvider<UploadResult>([&](auto cb){provider.upload(request,cb);});
 if(uploaded.status!="uploaded")return failed(uploaded.error?uploaded.error.message:QStringLiteral("The test page didn't upload."),uploaded.status=="unknown"?QStringLiteral("unknown"):QString());
 const auto receipt=uploaded.receipt;const auto url=receipt.directUrl;
 // Removes the test page whatever the fetch found; the destination keeps nothing of it.
 const auto cleanup=[&]()->QString {
  if(!provider.receiptCapabilities(receipt).safeRemoval())return QStringLiteral("left");
  auto removed=awaitProvider<DeleteResult>([&](auto cb){provider.remove({uuid(),receipt,{},std::make_shared<std::atomic_bool>(false)},[](const QJsonObject&,std::function<void(Error)> ack){ack({});},cb);});
  return removed.status=="removed"||removed.status=="missing"?QStringLiteral("removed"):QStringLiteral("failed");
 };
 if(url.isEmpty()){const auto left=cleanup();return failed(QStringLiteral("This destination gives no link for an upload."),left);}
 // On plain http anyone on the network path can swap the page and read the key, so the link
 // recipients open must be https from its first request (or on this computer).
 {const QUrl link(url);if(link.scheme()!="https"&&!QStringList{"localhost","127.0.0.1","::1"}.contains(link.host())){const auto left=cleanup();return failed(QStringLiteral("The link address isn't https. On plain http anyone on the network path could replace the page and read the key."),left);}}
 // Cancelled after the upload: the test page still goes, and nothing more is fetched.
 if(cancelled->load()){const auto left=cleanup();return QJsonObject{{"state","cancelled"},{"cleanup",left}};}
 // The link goes over standard input: a presigned link's signature never reaches a command line.
 QProcess probe;probe.setStandardErrorFile(QProcess::nullDevice());probe.start(cli,{"--seal-probe","-"});
 QJsonObject answer;
 if(probe.waitForStarted(10000)) {
  probe.write(url.toUtf8()+'\n');probe.closeWriteChannel();
  // Up to 90 s, but a cancel stops it at once.
  for(int waited=0;waited<90000&&!probe.waitForFinished(200);waited+=200)if(cancelled->load())break;
  if(probe.state()==QProcess::NotRunning)answer=QJsonDocument::fromJson(probe.readAllStandardOutput().trimmed()).object();
 }
 if(probe.state()!=QProcess::NotRunning){probe.kill();probe.waitForFinished(2000);}
 if(cancelled->load()){const auto left=cleanup();return QJsonObject{{"state","cancelled"},{"cleanup",left}};}
 const auto got=answer["result"].toObject();
 QString reason;
 if(!answer["ok"].toBool())reason=QStringLiteral("Couldn't open the test page's link.");
 else if(!got["network"].toString().isEmpty()||got["httpStatus"].toInt()!=200)reason=got["httpStatus"].toInt()?QStringLiteral("The test page's link answered HTTP %1.").arg(got["httpStatus"].toInt()):QStringLiteral("Couldn't open the test page's link.");
 else if(got["contentType"].toString()!="text/html")reason=QStringLiteral("The destination serves .html files as %1, so browsers won't open a sealed page.").arg(got["contentType"].toString().isEmpty()?QStringLiteral("no type"):got["contentType"].toString());
 else if(got["attachment"].toBool())reason=QStringLiteral("The destination makes browsers download .html files instead of opening them.");
 else if(got["finalScheme"]!="https"&&!QStringList{"localhost","127.0.0.1","::1"}.contains(got["finalHost"].toString()))reason=QStringLiteral("The link address isn't https, and browsers decrypt sealed pages only on https pages.");
 else if(got["sha256"].toString()!=artifact.sha256)reason=QStringLiteral("The destination changed the test page's bytes, so a sealed page can't be checked.");
 const auto left=cleanup();
 logging::info(logSettings(),"Sealed-link check",{{"provider",destination.providerId},{"profileId",destination.profileId},{"passed",reason.isEmpty()},{"httpStatus",got["httpStatus"]},{"contentType",got["contentType"]},{"cleanup",left}});
 if(!reason.isEmpty())return failed(reason,left);
 return {{"state","passed"},{"cleanup",left}};
}
}
