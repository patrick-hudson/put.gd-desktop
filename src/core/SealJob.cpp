#include "SealJob.h"
#include "HelperFiles.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QProcess>
#include <QRandomGenerator>
#include <QUuid>
#ifdef Q_OS_UNIX
#include <sys/resource.h>
#endif
namespace putgd {
namespace {
Error failure(const QString& message) {return {"seal_failed",message,{}};}
}
SealJob::SealJob(QString helperPath,int timeoutMs):helperPath_(helperPath.isEmpty()?defaultHelper():std::move(helperPath)),timeoutMs_(timeoutMs) {}
QString SealJob::defaultHelper() {return QCoreApplication::applicationDirPath()+"/putgd-cli";}
QString SealJob::randomName(const QString& extension) {
 static const char alphabet[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
 QString name;auto* random=QRandomGenerator::system();
 for(int i=0;i<12;++i)name.append(QLatin1Char(alphabet[random->bounded(62)]));
 return name+"."+extension;
}
SealJob::Result SealJob::run(const Artifact& file,const QString& directory,const QString& form,const QString& pageName,const QString& binName,const QString& passphrase,Cancel cancelled) const {
 Result result;
 const QFileInfo helper(helperPath_);
 if(!helper.isFile()||!helper.isExecutable()){result.error=failure("Couldn't seal the file because putgd-cli is missing from this installation.");return result;}
 const bool split=form=="split";
 const auto pageId=QUuid::createUuid().toString(QUuid::WithoutBraces),dataId=QUuid::createUuid().toString(QUuid::WithoutBraces);
 const auto pagePath=directory+"/"+pageId,dataPath=directory+"/"+dataId;
 // Only files this job made go again. A path already there (the helper's exclusive create
 // refuses it) is never touched, and a helper that answers removes what it made itself.
 const auto present=[](const QString& path){const QFileInfo info(path);return info.exists()||info.isSymLink();};
 if(present(pagePath)||(split&&present(dataPath))){result.error=failure("Couldn't seal the file because its working file name is already taken.");return result;}
 QJsonObject request{{"input",file.path},{"bytes",double(file.bytes)},{"name",file.name},{"type",file.mime},{"form",split?"split":"inline"},{"page",pagePath}};
 if(split){request["data"]=dataPath;request["bin"]=binName;}
 if(!passphrase.isEmpty())request["passphrase"]=passphrase;
 auto input=QJsonDocument(request).toJson(QJsonDocument::Compact);
 const auto discard=[&]{QFile::remove(pagePath);if(split)QFile::remove(dataPath);};
 QProcess process;process.setStandardErrorFile(QProcess::nullDevice());
#ifdef Q_OS_UNIX
 process.setChildProcessModifier([]{const rlimit none{0,0};::setrlimit(RLIMIT_CORE,&none);::setpriority(PRIO_PROCESS,0,5);});
#endif
 QElapsedTimer clock;clock.start();
 process.start(helperPath_,{"--seal-job"});
 if(!process.waitForStarted(10000)){result.error=failure("Couldn't start putgd-cli to seal the file.");return result;}
 process.write(input);input.fill('\0');process.closeWriteChannel();
 QByteArray output;
 while(process.state()!=QProcess::NotRunning) {
  if(cancelled&&cancelled->load()){process.kill();process.waitForFinished(2000);discard();result.error={"cancelled","Sealing was cancelled, so the upload didn't start.",{}};return result;}
  if(clock.elapsed()>timeoutMs_){process.kill();process.waitForFinished(2000);discard();result.error=failure("Sealing took too long, so the upload didn't start.");return result;}
  process.waitForReadyRead(100);output+=process.readAllStandardOutput();if(output.size()>65536)break;
 }
 if(process.state()!=QProcess::NotRunning){process.kill();process.waitForFinished(2000);}
 output+=process.readAllStandardOutput();
 const auto answer=QJsonDocument::fromJson(output.trimmed()).object();
 if(process.exitStatus()!=QProcess::NormalExit||!answer["ok"].toBool()) {
  if(process.exitStatus()!=QProcess::NormalExit||!answer["error"].isObject())discard();
  const auto e=answer["error"].toObject();
  result.error=e["code"].isString()?Error{e["code"].toString(),e["message"].toString(),{}}:failure("Couldn't seal the file because putgd-cli stopped unexpectedly.");
  return result;
 }
 // The files the helper wrote, as the resident checks them itself.
 const auto artifact=[&](const QString& id,const QString& path,const QString& name,const QString& mime,const QJsonObject& reported,Artifact& out)->bool {
  QFile written(path);if(!written.open(QIODevice::ReadOnly))return false;
  const auto sha=helperfiles::digest(written,written.size(),[&]{return cancelled&&cancelled->load();});
  if(sha.isEmpty()||written.size()!=qint64(reported["bytes"].toDouble(-1))||sha!=reported["sha256"].toString())return false;
  out.id=id;out.path=path;out.name=name;out.mime=mime;out.category="other";out.sha256=sha;out.bytes=written.size();out.namingPrepared=true;
  return true;
 };
 if(!artifact(pageId,pagePath,pageName,"text/html",answer["page"].toObject(),result.page)||(split&&!artifact(dataId,dataPath,binName,"application/octet-stream",answer["data"].toObject(),result.data))) {
  discard();result.page={};result.data={};
  // A check cut short by Cancel is a cancelled upload, not a mismatch.
  result.error=cancelled&&cancelled->load()?Error{"cancelled","Sealing was cancelled, so the upload didn't start.",{}}:failure("The sealed file didn't match what putgd-cli reported, so the upload didn't start.");return result;
 }
 result.key=answer["key"].toString();
 if(result.key.size()!=46||!result.key.startsWith("v1.")){discard();result.error=failure("putgd-cli returned no usable key.");result.page={};result.data={};}
 return result;
}
}
