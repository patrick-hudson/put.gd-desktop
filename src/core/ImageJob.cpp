#include "ImageJob.h"
#include "ImageFormat.h"
#include "BuildInfo.h"
#include "app/UiControl.h"
#include <QBuffer>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QFile>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QUuid>
#include <algorithm>
#include <climits>
#ifdef Q_OS_UNIX
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace putgd {
namespace {
constexpr qint64 maximumInput=1099511627776LL;
constexpr qint64 discoveryBudget=32*1024*1024;
Error failure(const QString& code){
 if(code=="cancelled")return {code,"Image processing cancelled",{}};
 if(code=="input_excessive")return {code,"Image exceeds resource limits",{}};
 if(code=="source_unsupported")return {code,"Image could not be decoded",{}};
 if(code=="image_job_timeout")return {code,"Image processing timed out",{}};
 return {code,"The image worker could not complete this request",{}};
}
QJsonObject build(){return QJsonDocument::fromJson(buildManifestJson()).object();}
QJsonObject envelope(const QString& nonce){return {{"role","image-job"},{"nonce",nonce},{"generation","1"}};}
bool validEnvelope(const QJsonObject& payload,const QString& nonce){return payload["role"]=="image-job"&&payload["nonce"]==nonce&&payload["generation"]=="1";}
bool number(const QJsonValue& value,qint64& out){bool ok=false;if(!value.isString())return false;out=value.toString().toLongLong(&ok);return ok&&out>=0&&QString::number(out)==value.toString();}
#ifdef Q_OS_UNIX
bool privateFile(const struct stat& s,mode_t mode){return S_ISREG(s.st_mode)&&s.st_uid==getuid()&&(s.st_mode&0777)==mode&&s.st_nlink==1;}
bool sameFile(const struct stat& a,const struct stat& b){return a.st_dev==b.st_dev&&a.st_ino==b.st_ino;}
bool sameModificationTime(const struct stat& a,const struct stat& b){
#ifdef Q_OS_DARWIN
 return a.st_mtimespec.tv_sec==b.st_mtimespec.tv_sec&&a.st_mtimespec.tv_nsec==b.st_mtimespec.tv_nsec;
#else
 return a.st_mtim.tv_sec==b.st_mtim.tv_sec&&a.st_mtim.tv_nsec==b.st_mtim.tv_nsec;
#endif
}
QJsonObject descriptor(const QString& path,const struct stat& s,const QString& hash={}){
 return {{"path",path},{"device",QString::number(qulonglong(s.st_dev))},{"inode",QString::number(qulonglong(s.st_ino))},{"bytes",QString::number(s.st_size)},{"sha256",hash}};
}
bool snapshot(QFile& file,struct stat& s){return file.handle()>=0&&!::fstat(file.handle(),&s);}
struct DirectoryGuard {
 QTemporaryDir& directory;struct stat original{};
 ~DirectoryGuard(){struct stat current{};if(::lstat(QFile::encodeName(directory.path()).constData(),&current)||!sameFile(original,current)||!S_ISDIR(current.st_mode)||current.st_uid!=getuid()||(current.st_mode&0777)!=0700)directory.setAutoRemove(false);}
};
#endif
}
ImageJob::ImageJob(QString path,int timeout):helperPath_(path.isEmpty()?QCoreApplication::applicationDirPath()+"/putgd-ui":std::move(path)),timeoutMs_(std::clamp(timeout,100,10000)){}

QByteArray ImageJob::run(const QString& task,const QByteArray* bytes,const QString& path,
                         qint64 maxPixels,qint64 imageBytes,qint64 outputBytes,Error& error,const Cancel& cancelled) const {
 error={};
#ifndef Q_OS_UNIX
 Q_UNUSED(task);Q_UNUSED(bytes);Q_UNUSED(path);Q_UNUSED(maxPixels);Q_UNUSED(imageBytes);Q_UNUSED(outputBytes);Q_UNUSED(cancelled);
 error=failure("image_job_start");return {};
#else
 QElapsedTimer clock;clock.start();
 const auto stopped=[&]{if(cancelled&&cancelled->load()){error=failure("cancelled");return true;}if(clock.elapsed()>=timeoutMs_){error=failure("image_job_timeout");return true;}return false;};
 if(stopped())return {};
 if(maxPixels<1||maxPixels>1000000000LL||imageBytes<1||imageBytes>maximumInput||outputBytes<1||outputBytes>maximumInput){error=failure("input_excessive");return {};}
 QTemporaryDir directory("/tmp/pgi-XXXXXX");if(!directory.isValid()||!QFile::setPermissions(directory.path(),QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner)){error=failure("image_job_io");return {};}
 DirectoryGuard directoryGuard{directory};
 if(::lstat(QFile::encodeName(directory.path()).constData(),&directoryGuard.original)||!S_ISDIR(directoryGuard.original.st_mode)||directoryGuard.original.st_uid!=getuid()||(directoryGuard.original.st_mode&0777)!=0700){directory.setAutoRemove(false);error=failure("image_job_io");return {};}
 const auto inputPath=directory.path()+"/input.bin",outputPath=directory.path()+"/output.bin";
 QFile input(inputPath),output(outputPath);struct stat inputIdentity{},outputIdentity{};QString inputHash;
 if(task!="discover"){
  if(!input.open(QIODevice::ReadWrite|QIODevice::NewOnly)||!input.setPermissions(QFile::ReadOwner|QFile::WriteOwner)){error=failure("image_job_io");return {};}
  QCryptographicHash hash(QCryptographicHash::Sha256);qint64 total=0;
  QFile source;struct stat original{};
  if(!bytes){
   const int fd=::open(QFile::encodeName(path).constData(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
   if(fd<0||::fstat(fd,&original)||!S_ISREG(original.st_mode)||original.st_size<1||original.st_size>maximumInput){if(fd>=0)::close(fd);error=failure("image_job_io");return {};}
   if(!source.open(fd,QIODevice::ReadOnly,QFileDevice::AutoCloseHandle)){::close(fd);error=failure("image_job_io");return {};}
  }
  if(bytes&&(bytes->isEmpty()||bytes->size()>imageBytes)){error=failure("input_excessive");return {};}
  for(;;){
   if(stopped())return {};
   QByteArray chunk;
   if(bytes){if(total>=bytes->size())break;chunk=bytes->mid(total,1024*1024);}
   else{chunk=source.read(1024*1024);if(chunk.isEmpty()){if(source.error()!=QFile::NoError){error=failure("image_job_io");return {};}break;}}
   if(chunk.size()>maximumInput-total||input.write(chunk)!=chunk.size()){error=failure("image_job_io");return {};}
   total+=chunk.size();hash.addData(chunk);
  }
  if(!bytes){struct stat after{};if(!snapshot(source,after)||!sameFile(original,after)||original.st_size!=after.st_size||!sameModificationTime(original,after)||total!=original.st_size){error=failure("image_job_io");return {};}}
  if(total<1||!input.flush()||!input.setPermissions(QFile::ReadOwner)||!snapshot(input,inputIdentity)||!privateFile(inputIdentity,0400)){error=failure("image_job_io");return {};}
  inputHash=QString::fromLatin1(hash.result().toHex());
 }
 if(!output.open(QIODevice::ReadWrite|QIODevice::NewOnly)||!output.setPermissions(QFile::ReadOwner|QFile::WriteOwner)||!snapshot(output,outputIdentity)||!privateFile(outputIdentity,0600)||outputIdentity.st_size!=0){error=failure("image_job_io");return {};}
 if(stopped())return {};
 const auto nonce=QUuid::createUuid().toString(QUuid::WithoutBraces);
 auto request=envelope(nonce);request["build"]=build();request["task"]=task;
 request["input"]=task=="discover"?QJsonValue(QJsonValue::Null):QJsonValue(descriptor(inputPath,inputIdentity,inputHash));request["output"]=descriptor(outputPath,outputIdentity);
 request["limits"]=QJsonObject{{"imagePixels",QString::number(maxPixels)},{"imageBytes",QString::number(imageBytes)},{"outputBytes",QString::number(outputBytes)}};
 QProcess process;process.setProcessChannelMode(QProcess::SeparateChannels);process.setReadChannel(QProcess::StandardOutput);
 UiControl control(process);bool ready=false,result=false,protocolFailed=false;QJsonObject reply;qint64 stderrBytes=0;
 QObject::connect(&control,&UiControl::failed,&process,[&](Error){protocolFailed=true;});
 QObject::connect(&process,&QProcess::readyReadStandardError,&process,[&]{
  process.setReadChannel(QProcess::StandardError);
  while(process.bytesAvailable()>0){const auto discarded=process.read(16384);stderrBytes+=discarded.size();if(stderrBytes>1024*1024){protocolFailed=true;break;}}
  process.setReadChannel(QProcess::StandardOutput);
 });
 QObject::connect(&control,&UiControl::frame,&process,[&](const QString& type,const QString&,const QJsonObject& payload){
  if(protocolFailed||!validEnvelope(payload,nonce)){protocolFailed=true;return;}
  if(type=="ready"&&!ready&&!result&&payload.size()==4&&payload["build"].toObject()==build()){
   ready=true;if(control.send("activate",envelope(nonce)).isEmpty())protocolFailed=true;return;
  }
  if(type=="checkpoint"&&ready&&!result&&payload["ok"].isBool()){
   if(!payload["ok"].toBool()){
    const auto code=payload["code"].toString();
    if(payload.size()!=5||(code!="input_excessive"&&code!="source_unsupported"&&code!="image_job_io")){protocolFailed=true;return;}
   }else{
    qint64 count=0;const auto hash=payload["sha256"].toString();
    if(payload.size()!=8||!number(payload["bytes"],count)||count<1||count>outputBytes||hash.size()!=64||QByteArray::fromHex(hash.toLatin1()).toHex()!=hash.toLatin1()||!payload["width"].isDouble()||!payload["height"].isDouble()){protocolFailed=true;return;}
    const auto width=payload["width"].toInteger(),height=payload["height"].toInteger();
    if(payload["width"].toDouble()!=width||payload["height"].toDouble()!=height||width<0||height<0||width>INT_MAX||height>INT_MAX||(task=="discover"?(width!=0||height!=0):(width<1||height<1||(task=="preview"?(width>256||height>256):width>maxPixels/height)))){protocolFailed=true;return;}
   }
   result=true;reply=payload;if(control.send("checkpointAccepted",envelope(nonce)).isEmpty())protocolFailed=true;return;
  }
  protocolFailed=true;
 });
 // Codec work never shows a window, so the child never connects to the
 // compositor or X server. The offscreen platform ships with Qt GUI itself
 // (libqt6gui6, qt6-qtbase-gui), unlike the other optional platform plugins.
 auto childEnvironment=QProcessEnvironment::systemEnvironment();childEnvironment.insert("QT_QPA_PLATFORM","offscreen");process.setProcessEnvironment(childEnvironment);
 process.start(helperPath_,{"--image-job"});
 bool started=false;
 while(!started&&!protocolFailed&&!stopped()){
  if(process.waitForStarted(int(std::clamp<qint64>(timeoutMs_-clock.elapsed(),1,50)))){started=true;break;}
  if(process.error()==QProcess::FailedToStart){error=failure("image_job_start");break;}
 }
 if(started&&!error){if(control.send("bootstrap",request).isEmpty())protocolFailed=true;}
 while(started&&process.state()!=QProcess::NotRunning&&!protocolFailed&&!stopped())process.waitForReadyRead(int(std::clamp<qint64>(timeoutMs_-clock.elapsed(),1,50)));
 if(protocolFailed)error=failure("image_job_protocol");
 if(process.state()!=QProcess::NotRunning){control.stop();process.terminate();if(!process.waitForFinished(500)){process.kill();process.waitForFinished(1500);}}
 if(!error&&(!started||!ready||!result||process.state()!=QProcess::NotRunning||process.exitStatus()!=QProcess::NormalExit||process.exitCode()!=0))error=failure("image_job_exit");
 control.stop();if(error)return {};
 if(stopped())return {};
 if(!reply["ok"].toBool()){error=failure(reply["code"].toString());return {};}
 struct stat finalOutput{},namedOutput{},finalInput{},namedInput{},namedDirectory{};
 if(::lstat(QFile::encodeName(directory.path()).constData(),&namedDirectory)||!sameFile(directoryGuard.original,namedDirectory)||!S_ISDIR(namedDirectory.st_mode)||namedDirectory.st_uid!=getuid()||(namedDirectory.st_mode&0777)!=0700){error=failure("image_job_io");return {};}
 if(!snapshot(output,finalOutput)||!privateFile(finalOutput,0600)||!sameFile(outputIdentity,finalOutput)||::lstat(QFile::encodeName(outputPath).constData(),&namedOutput)||!sameFile(finalOutput,namedOutput)||!privateFile(namedOutput,0600)||finalOutput.st_size!=reply["bytes"].toString().toLongLong()||finalOutput.st_size>outputBytes){error=failure("image_job_io");return {};}
 if(task!="discover"){
  if(!snapshot(input,finalInput)||!privateFile(finalInput,0400)||!sameFile(inputIdentity,finalInput)||finalInput.st_size!=inputIdentity.st_size||!sameModificationTime(finalInput,inputIdentity)||::lstat(QFile::encodeName(inputPath).constData(),&namedInput)||!sameFile(inputIdentity,namedInput)||!privateFile(namedInput,0400)||!input.seek(0)){error=failure("image_job_io");return {};}
  QCryptographicHash check(QCryptographicHash::Sha256);qint64 checked=0;
  while(checked<inputIdentity.st_size){if(stopped())return {};const auto chunk=input.read(std::min<qint64>(1024*1024,inputIdentity.st_size-checked));if(chunk.isEmpty()){error=failure("image_job_io");return {};}check.addData(chunk);checked+=chunk.size();}
  if(QString::fromLatin1(check.result().toHex())!=inputHash){error=failure("image_job_io");return {};}
 }
 if(!output.seek(0)){error=failure("image_job_io");return {};}auto encoded=output.read(finalOutput.st_size+1);
 if(encoded.size()!=finalOutput.st_size||QString::fromLatin1(QCryptographicHash::hash(encoded,QCryptographicHash::Sha256).toHex())!=reply["sha256"].toString()){error=failure("image_job_io");return {};}
 if(task!="discover"){
  if(imageFormatHint(encoded)!="png"){error=failure("image_job_protocol");return {};}
  QBuffer buffer(&encoded);buffer.open(QIODevice::ReadOnly);QImageReader reader(&buffer,"png");
  const auto size=reader.size();if(size.width()!=reply["width"].toInt()||size.height()!=reply["height"].toInt()){error=failure("image_job_protocol");return {};}
 }
 return encoded;
#endif
}
QList<QByteArray> ImageJob::discover(Error& error,Cancel cancelled) const {
 const auto bytes=run("discover",nullptr,{},1,1,discoveryBudget,error,cancelled);if(error)return {};
 QJsonParseError parse;const auto document=QJsonDocument::fromJson(bytes,&parse);
 if(parse.error!=QJsonParseError::NoError||!document.isArray()||document.array().size()>1024){error=failure("image_job_protocol");return {};}
 QList<QByteArray> types;for(const auto& value:document.array()){
  if(!value.isString()){error=failure("image_job_protocol");return {};}
  const auto mime=value.toString().toLatin1();if(mime.isEmpty()||mime.size()>4096||QString::fromLatin1(mime)!=value.toString()){error=failure("image_job_protocol");return {};}types.append(mime);
 }
 return types;
}
QByteArray ImageJob::decodePng(const QByteArray& input,const ResourceLimits& limits,Error& error,Cancel cancelled) const {
 return run("decode",&input,{},limits.imagePixels,limits.imageBytes,limits.maxFileBytes,error,cancelled);
}
QImage ImageJob::decode(const QByteArray& input,const ResourceLimits& limits,Error& error,Cancel cancelled) const {
 auto encoded=decodePng(input,limits,error,cancelled);if(error)return {};
 QBuffer buffer(&encoded);buffer.open(QIODevice::ReadOnly);QImageReader reader(&buffer,"png");
 const auto size=reader.size();if(!size.isValid()||qint64(size.width())*size.height()>limits.imagePixels||qint64(size.width())*size.height()>limits.imageBytes/4){error=failure("input_excessive");return {};}
 auto image=reader.read();if(image.isNull()){error=failure("source_unsupported");return {};}
 if(image.sizeInBytes()>limits.imageBytes){error=failure("input_excessive");return {};}
 if(cancelled&&cancelled->load()){error=failure("cancelled");return {};}return image;
}
QByteArray ImageJob::preview(const QString& file,qint64 maxPixels,qint64 budget,Error& error,Cancel cancelled) const {
 return run("preview",nullptr,file,maxPixels,maximumInput,budget,error,cancelled);
}
}
