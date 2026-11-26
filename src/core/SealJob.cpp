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
#include <QScopeGuard>
#include <fcntl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>
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
 const auto pageId=pageIdForTesting_.isEmpty()?QUuid::createUuid().toString(QUuid::WithoutBraces):pageIdForTesting_;
 const auto dataId=dataIdForTesting_.isEmpty()?QUuid::createUuid().toString(QUuid::WithoutBraces):dataIdForTesting_;
 const auto pagePath=directory+"/"+pageId,dataPath=directory+"/"+dataId;
 // The resident makes both outputs itself, each with an exclusive create, and keeps each one's
 // descriptor; the helper only writes into them. A path is removed only while it is still the
 // file made here (same device and inode), so nothing put.gd didn't create is ever deleted.
 struct Owned {QString path;int fd=-1;};
 std::vector<Owned> owned;
 const auto closeAll=qScopeGuard([&owned]{for(const auto& file:owned)::close(file.fd);});
 const auto make=[&owned](const QString& path){const int fd=::open(QFile::encodeName(path).constData(),O_RDONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);if(fd<0)return false;owned.push_back({path,fd});return true;};
 const auto discard=[&owned]{for(const auto& file:owned){struct stat mine{},there{};
  if(::fstat(file.fd,&mine)==0&&::lstat(QFile::encodeName(file.path).constData(),&there)==0&&mine.st_dev==there.st_dev&&mine.st_ino==there.st_ino)QFile::remove(file.path);}};
 if(!make(pagePath)||(split&&!make(dataPath))){discard();result.error=failure("Couldn't create the sealed file's working copy.");return result;}
 QJsonObject request{{"input",file.path},{"bytes",double(file.bytes)},{"name",file.name},{"type",file.mime},{"form",split?"split":"inline"},{"page",pagePath}};
 if(split){request["data"]=dataPath;request["bin"]=binName;}
 if(!passphrase.isEmpty())request["passphrase"]=passphrase;
 auto input=QJsonDocument(request).toJson(QJsonDocument::Compact);
 QProcess process;process.setStandardErrorFile(QProcess::nullDevice());
#ifdef Q_OS_UNIX
 process.setChildProcessModifier([]{const rlimit none{0,0};::setrlimit(RLIMIT_CORE,&none);::setpriority(PRIO_PROCESS,0,5);});
#endif
 QElapsedTimer clock;clock.start();
 process.start(helperPath_,{"--seal-job"});
 if(!process.waitForStarted(10000)){discard();result.error=failure("Couldn't start putgd-cli to seal the file.");return result;}
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
  discard();const auto e=answer["error"].toObject();
  result.error=e["code"].isString()?Error{e["code"].toString(),e["message"].toString(),{}}:failure("Couldn't seal the file because putgd-cli stopped unexpectedly.");
  return result;
 }
 // Cancel that came as the helper finished: nothing is checked or kept.
 if(cancelled&&cancelled->load()){discard();result.error={"cancelled","Sealing was cancelled, so the upload didn't start.",{}};return result;}
 // The files the helper wrote, as the resident checks them itself, read through the descriptors
 // made above, so what is checked is the file put.gd created.
 const auto artifact=[&](const QString& id,const QString& path,const QString& name,const QString& mime,const QJsonObject& reported,Artifact& out)->bool {
  int fd=-1;for(const auto& file:owned)if(file.path==path)fd=file.fd;
  QFile written;if(fd<0||::lseek(fd,0,SEEK_SET)!=0||!written.open(fd,QIODevice::ReadOnly,QFileDevice::DontCloseHandle))return false;
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
