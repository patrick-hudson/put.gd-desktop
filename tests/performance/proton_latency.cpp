#include "storage/proton/ProtonProvider.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QUuid>
#include <cstdio>
#ifdef Q_OS_UNIX
#include <unistd.h>
#endif
using namespace putgd;
static Error journal(const QString& path,const QJsonObject& value){
 QSaveFile file(path);if(!file.open(QIODevice::WriteOnly))return {"probe_journal_io","Private probe journal unavailable",{}};
 file.setPermissions(QFile::ReadOwner|QFile::WriteOwner);const auto bytes=QJsonDocument(value).toJson(QJsonDocument::Compact);
 if(file.write(bytes)!=bytes.size()||!file.commit())return {"probe_journal_io","Private probe journal unavailable",{}};
 return {};
}
static void report(const QString& stage,qint64 milliseconds,const QString& status,const Error& error){
 const auto bytes=QJsonDocument(QJsonObject{{"stage",stage},{"milliseconds",milliseconds},{"status",status},{"errorCode",error.code}}).toJson(QJsonDocument::Compact);
 std::fwrite(bytes.constData(),1,bytes.size(),stdout);std::fputc('\n',stdout);std::fflush(stdout);
}
int main(int argc,char** argv){
 QCoreApplication app(argc,argv);const auto arguments=app.arguments();if(arguments.size()!=3&&arguments.size()!=4)return 2;
 bool sizeOk=true;const qint64 payloadBytes=arguments.size()==4?arguments[3].toLongLong(&sizeOk):65536;
 if(!sizeOk||payloadBytes<1||payloadBytes>64*1024*1024)return 2;
 QFile config(arguments[1]);if(!config.open(QIODevice::ReadOnly))return 2;
 const auto doc=QJsonDocument::fromJson(config.readAll()).object();Destination destination;int selected=0;
 for(const auto& value:doc["profiles"].toArray()){const auto profile=value.toObject();if(profile["provider"]!="proton-drive")continue;++selected;destination={"proton-drive",profile["id"].toString(),"latency-probe",profile["settings"].toObject(),profile["credentials"].toObject()};}
 ProtonProvider provider;
 if(selected!=1||!destination.settings["folderName"].toString().startsWith("put.gd-")||provider.validateSettings(destination.settings)||provider.validateCredentials(destination.credentialReference))return 2;
 const QFileInfo output(arguments[2]);if(!output.isDir()||output.isSymLink()||output.canonicalFilePath()!=output.absoluteFilePath()||output.permissions()&(QFile::ReadGroup|QFile::WriteGroup|QFile::ExeGroup|QFile::ReadOther|QFile::WriteOther|QFile::ExeOther))return 2;
 #ifdef Q_OS_UNIX
 if(output.ownerId()!=getuid())return 2;
#endif
 if(QFileInfo::exists(QDir(output.absoluteFilePath()).filePath("publication.json"))||QFileInfo::exists(QDir(output.absoluteFilePath()).filePath("sharing-progress.json")))return 2;
 const auto id=QUuid::createUuid().toString(QUuid::WithoutBraces);const auto candidate="putgd-latency-"+id+".bin";
 QFile payload(QDir(output.absoluteFilePath()).filePath(candidate));if(!payload.open(QIODevice::WriteOnly|QIODevice::NewOnly))return 2;
 payload.setPermissions(QFile::ReadOwner|QFile::WriteOwner);const QByteArray bytes(payloadBytes,'p');if(payload.write(bytes)!=bytes.size())return 2;payload.close();
 const auto journalPath=QDir(output.absoluteFilePath()).filePath("publication.json");
 if(journal(journalPath,{{"candidate",candidate},{"publicationId",id},{"destination",destination.toJson()},{"phase","prepared"}}))return 2;
 QElapsedTimer elapsed;elapsed.start();NameCheck check;provider.checkName(destination,candidate,[&](auto result){check=result;});report("checkName",elapsed.elapsed(),check.status,check.error);if(check.status!="absent"||check.error)return 1;
 Artifact artifact;artifact.id=id;artifact.path=payload.fileName();artifact.name=candidate;artifact.mime="application/octet-stream";artifact.category="other";artifact.bytes=bytes.size();artifact.sha256=QString::fromLatin1(QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex());
 UploadRequest upload;upload.destination=destination;upload.artifact=artifact;upload.candidate=candidate;upload.publicationId=id;upload.condition="absent";upload.objectReference=check.objectReference;
 QFile progressFile(QDir(output.absoluteFilePath()).filePath("upload-progress.jsonl"));
 if(!progressFile.open(QIODevice::WriteOnly|QIODevice::NewOnly))return 2;
 if(!progressFile.setPermissions(QFile::ReadOwner|QFile::WriteOwner))return 2;
 qint64 transferFrames=0,preparationFrames=0,maxTransferred=0,lastTransferred=0;bool uploadComplete=false,transferBeforeCompletion=false;Error progressError;
 upload.progress=[&](const UploadProgress& progress){
  if(progress.phase=="uploading"&&progress.bytesTransferred){++transferFrames;lastTransferred=*progress.bytesTransferred;maxTransferred=qMax(maxTransferred,lastTransferred);transferBeforeCompletion|=!uploadComplete;}
  if(progress.phase=="preparing")++preparationFrames;
  const auto event=QJsonDocument(QJsonObject{{"milliseconds",elapsed.elapsed()},{"progress",progress.toJson()}}).toJson(QJsonDocument::Compact)+'\n';
  if(progressFile.write(event)!=event.size()||!progressFile.flush())progressError={"probe_progress_io","Private progress evidence unavailable",{}};
 };
 elapsed.restart();UploadResult uploaded;provider.upload(upload,[&](auto result){uploadComplete=true;uploaded=result;});report("upload",elapsed.elapsed(),uploaded.status,uploaded.error);
 progressFile.close();
 if(journal(QDir(output.absoluteFilePath()).filePath("progress-summary.json"),{{"payloadBytes",payloadBytes},{"transferFrames",transferFrames},{"preparationFrames",preparationFrames},{"maxTransferred",maxTransferred},{"lastTransferred",lastTransferred},{"transferBeforeCompletion",transferBeforeCompletion},{"progressErrorCode",progressError.code}}))return 2;
 if(uploaded.status!="uploaded"||uploaded.error||progressError)return 1;
 auto receipt=uploaded.receipt;receipt.directUrl.clear();if(journal(journalPath,{{"receipt",receipt.toJson()},{"phase","share_intent"}}))return 2;
 ShareRequest request;request.id=id;request.action="create";request.receipt=receipt;
 const auto checkpoint=[&](const QJsonObject& progress,auto ack){ack(journal(QDir(output.absoluteFilePath()).filePath("sharing-progress.json"),progress));};
 elapsed.restart();ShareResult shared;provider.share(request,checkpoint,[&](auto result){shared=result;});report("share",elapsed.elapsed(),shared.status,shared.error);
 if(shared.status!="shared"||shared.error)return 1;
 // Keep capability URLs out of benchmark evidence; revoke only the new probe link.
 receipt=shared.receipt;receipt.directUrl.clear();if(journal(journalPath,{{"receipt",receipt.toJson()},{"phase","revoke_intent"}}))return 2;
 request.action="revoke";request.receipt=receipt;
 elapsed.restart();ShareResult revoked;provider.share(request,checkpoint,[&](auto result){revoked=result;});report("revoke",elapsed.elapsed(),revoked.status,revoked.error);
 return revoked.status=="revoked"&&!revoked.error?0:1;
}
