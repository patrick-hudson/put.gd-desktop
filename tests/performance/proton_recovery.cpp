#include "storage/proton/ProtonProvider.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSaveFile>
#include <cstdio>
#include <sys/stat.h>
#include <unistd.h>
using namespace putgd;
namespace {
void output(const QJsonObject& value) {
 const auto bytes=QJsonDocument(value).toJson(QJsonDocument::Compact)+'\n';
 std::fwrite(bytes.constData(),1,bytes.size(),stdout);std::fflush(stdout);
}
Error save(const QString& path,QJsonObject value) {
 auto receipt=value["receipt"].toObject();receipt.remove("directUrl");
 if(value.contains("receipt"))value["receipt"]=receipt;
 QSaveFile file(path);if(!file.open(QIODevice::WriteOnly))return {"probe_journal_io","Private probe journal unavailable",{}};
 file.setPermissions(QFile::ReadOwner|QFile::WriteOwner);
 const auto bytes=QJsonDocument(value).toJson(QJsonDocument::Compact);
 if(file.write(bytes)!=bytes.size()||!file.commit())return {"probe_journal_io","Private probe journal unavailable",{}};
 return {};
}
}
int main(int argc,char** argv) {
 QCoreApplication app(argc,argv);struct stat pipe{};
 if(argc!=1||::fstat(STDOUT_FILENO,&pipe)||(!S_ISFIFO(pipe.st_mode)&&!S_ISSOCK(pipe.st_mode)))return 2;
 QFile input;if(!input.open(stdin,QIODevice::ReadOnly))return 2;
 const auto bytes=input.read(128*1024+1);QJsonParseError parse;
 const auto document=QJsonDocument::fromJson(bytes,&parse);
 if(bytes.size()>128*1024||parse.error!=QJsonParseError::NoError||!document.isObject())return 2;
 const auto request=document.object();const auto method=request["method"].toString();
 const auto destination=Destination::fromJson(request["destination"].toObject());
 const QFileInfo root(request["output"].toString());
 if(!root.isAbsolute()||!root.isDir()||root.isSymLink()||root.ownerId()!=getuid()||
    root.canonicalFilePath()!=root.absoluteFilePath()||root.permissions()&(QFile::ReadGroup|QFile::WriteGroup|QFile::ExeGroup|QFile::ReadOther|QFile::WriteOther|QFile::ExeOther))return 2;
 if(QFileInfo::exists(QDir(root.filePath()).filePath("intent.json")))return 2;
 ProtonProvider provider;Error error;QJsonObject result;
 if(destination.providerId!="proton-drive"||provider.validateSettings(destination.settings)||provider.validateCredentials(destination.credentialReference))return 2;
 if(method=="share"||method=="stat") {
  const auto receipt=StorageReceipt::fromJson(request["receipt"].toObject());
  if(receipt.destination.toJson()!=destination.toJson()||!receipt.objectReference["name"].toString().startsWith("putgd-p1-"))return 2;
 }
 const bool setup=QStringList{"inspectAccount","connect","createFolder","storeSharePassword"}.contains(method);
 if(!setup&&!destination.settings["folderName"].toString().startsWith("put.gd-p1-"))return 2;
 if(method=="createFolder"&&!request["options"].toObject()["name"].toString().startsWith("put.gd-p1-"))return 2;
 QJsonObject intent{{"method",method},{"id",request["id"]},{"destination",destination.toJson()}};
 if(request.contains("artifact"))intent["artifact"]=request["artifact"];
 if(request.contains("receipt"))intent["receipt"]=request["receipt"];
 if(method=="share") {
  const auto options=request["options"].toObject();QJsonObject protection;
  for(const auto& key:QStringList{"action","expirationDays","passwordReference"})if(options.contains(key))protection[key]=options[key];
  intent["options"]=protection;
 }
 for(const auto& key:QStringList{"candidate","publicationId"})if(request.contains(key))intent[key]=request[key];
 if(save(QDir(root.filePath()).filePath("intent.json"),intent))return 2;
 const auto checkpoint=[&](const QJsonObject& progress,auto ack){ack(save(QDir(root.filePath()).filePath("sharing-progress.json"),progress));};
 if(setup)result=provider.setup(method,destination,request["options"].toObject(),error,[](const QJsonObject& event){output(event);});
 else if(method=="checkName")provider.checkName(destination,request["candidate"].toString(),[&](auto r){error=r.error;result={{"status",r.status}};});
 else if(method=="upload"||method=="reconcile") {
  UploadRequest upload;upload.destination=destination;upload.artifact=Artifact::fromJson(request["artifact"].toObject());
  upload.candidate=request["candidate"].toString();upload.publicationId=request["publicationId"].toString();upload.condition="absent";
  upload.cancellation=std::make_shared<std::atomic_bool>(false);
  upload.progress=[&](const UploadProgress& p){if(request["cancelOnProgress"].toBool()&&p.bytesTransferred&&*p.bytesTransferred>0)upload.cancellation->store(true);};
  if(!upload.candidate.startsWith("putgd-p1-"))return 2;
  if(method=="upload")provider.upload(upload,[&](auto r){error=r.error;result={{"status",r.status},{"receipt",r.receipt.toJson()}};});
  else provider.reconcile(upload,[&](auto r){error=r.error;result={{"status",r.status},{"receipt",r.receipt.toJson()}};});
 } else if(method=="stat")provider.stat(StorageReceipt::fromJson(request["receipt"].toObject()),[&](auto r){error=r.error;result={{"status",r.status},{"receipt",r.receipt.toJson()}};});
 else if(method=="share") {
  ShareRequest share;share.id=request["id"].toString();share.action=request["options"].toObject()["action"].toString();
  share.options=request["options"].toObject();share.receipt=StorageReceipt::fromJson(request["receipt"].toObject());
  if(!QStringList{"create","inspect","revoke"}.contains(share.action)||!share.receipt.objectReference["name"].toString().startsWith("putgd-p1-"))return 2;
  provider.share(share,checkpoint,[&](auto r){error=r.error;result={{"status",r.status},{"receipt",r.receipt.toJson()}};});
 } else return 2;
 auto saved=result;saved["error"]=error.toJson();
 if(auto failure=save(QDir(root.filePath()).filePath("completion.json"),saved)) {
  error=failure;auto receipt=result["receipt"].toObject();receipt.remove("directUrl");
  if(result.contains("receipt"))result["receipt"]=receipt;
 }
 output({{"ok",!error},{"error",error.toJson()},{"result",result}});return 0;
}
