#include "ImageJob.h"
#include "app/UiControl.h"
#include "core/BuildInfo.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QImageWriter>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QTimer>
#include <QUuid>
#include <algorithm>
#ifdef Q_OS_UNIX
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace putgd {
#ifdef Q_OS_UNIX
namespace {
constexpr qint64 maximumBytes=1099511627776LL;
constexpr qint64 discoveryBudget=32*1024*1024;
bool number(const QJsonValue& value,qint64& result){
 bool ok=false;if(!value.isString())return false;
 result=value.toString().toLongLong(&ok);
 return ok&&result>=0&&QString::number(result)==value.toString();
}
bool hashText(const QString& value){return value.size()==64&&QByteArray::fromHex(value.toLatin1()).toHex()==value.toLatin1();}
bool privateFile(const struct stat& s,mode_t mode){return S_ISREG(s.st_mode)&&s.st_uid==getuid()&&(s.st_mode&0777)==mode&&s.st_nlink==1;}
bool sameFile(const struct stat& a,const struct stat& b){return a.st_dev==b.st_dev&&a.st_ino==b.st_ino;}
bool sameModificationTime(const struct stat& a,const struct stat& b){
#ifdef Q_OS_DARWIN
 return a.st_mtimespec.tv_sec==b.st_mtimespec.tv_sec&&a.st_mtimespec.tv_nsec==b.st_mtimespec.tv_nsec;
#else
 return a.st_mtim.tv_sec==b.st_mtim.tv_sec&&a.st_mtim.tv_nsec==b.st_mtim.tv_nsec;
#endif
}
bool sameInput(const struct stat& a,const struct stat& b){return sameFile(a,b)&&a.st_size==b.st_size&&sameModificationTime(a,b);}
QJsonObject envelope(const QString& nonce){return {{"role","image-job"},{"nonce",nonce},{"generation","1"}};}
bool validEnvelope(const QJsonObject& object,const QString& nonce){return object["role"]=="image-job"&&object["nonce"]==nonce&&object["generation"]=="1";}
class LimitedFile final:public QFile {
public:
 qint64 maximum=0;bool excessive=false;
protected:
 qint64 writeData(const char* data,qint64 count)override{
  if(count<0||pos()<0||count>maximum-pos()){excessive=true;return -1;}
  return QFile::writeData(data,count);
 }
};
class PrivateFiles final {
public:
 ~PrivateFiles(){if(directoryFd>=0)::close(directoryFd);}
 bool open(const QJsonValue& inputValue,const QJsonObject& outputValue,bool discovery,qint64 inputMaximum,qint64 outputMaximum){
  if(outputValue.size()!=5||!outputValue["path"].isString())return false;
  outputPath=outputValue["path"].toString();directory=QFileInfo(outputPath).absolutePath();
  static const QRegularExpression pattern(QStringLiteral("^/tmp/pgi-[A-Za-z0-9]{6}$"));
  if(!pattern.match(directory).hasMatch()||outputPath!=directory+"/output.bin")return false;
  directoryFd=::open(QFile::encodeName(directory).constData(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_DIRECTORY);
  if(directoryFd<0||::fstat(directoryFd,&directoryIdentity)||!validDirectory(directoryIdentity))return false;
  if(!openFile(output,outputValue,"output.bin",0600,outputIdentity)||outputIdentity.st_size!=0||outputValue["sha256"]!="")return false;
  output.maximum=outputMaximum;
  if(discovery)return inputValue.isNull()&&namesIntact(false);
  if(!inputValue.isObject())return false;
  const auto object=inputValue.toObject();inputPath=object["path"].toString();inputHash=object["sha256"].toString();
  if(inputPath!=directory+"/input.bin"||!hashText(inputHash)||!openFile(input,object,"input.bin",0400,inputIdentity)||inputIdentity.st_size<1||inputIdentity.st_size>inputMaximum)return false;
  return namesIntact(true)&&checkInput();
 }
 bool namesIntact(bool hasInput){
  struct stat directoryNow{},outputNow{},outputNamed{},inputNow{},inputNamed{};
  if(::lstat(QFile::encodeName(directory).constData(),&directoryNow)||!sameFile(directoryIdentity,directoryNow)||!validDirectory(directoryNow))return false;
  if(::fstat(output.handle(),&outputNow)||!sameFile(outputIdentity,outputNow)||!privateFile(outputNow,0600)||::fstatat(directoryFd,"output.bin",&outputNamed,AT_SYMLINK_NOFOLLOW)||!sameFile(outputIdentity,outputNamed)||!privateFile(outputNamed,0600))return false;
  return !hasInput||(!::fstat(input.handle(),&inputNow)&&sameInput(inputIdentity,inputNow)&&privateFile(inputNow,0400)&&!::fstatat(directoryFd,"input.bin",&inputNamed,AT_SYMLINK_NOFOLLOW)&&sameInput(inputIdentity,inputNamed)&&privateFile(inputNamed,0400));
 }
 bool checkInput(){
  if(!input.seek(0))return false;
  QCryptographicHash hash(QCryptographicHash::Sha256);qint64 total=0;
  while(total<inputIdentity.st_size){const auto chunk=input.read(std::min<qint64>(1024*1024,inputIdentity.st_size-total));if(chunk.isEmpty())return false;hash.addData(chunk);total+=chunk.size();}
  return QString::fromLatin1(hash.result().toHex())==inputHash&&input.seek(0);
 }
 QByteArray outputHash(){
  if(!output.seek(0))return {};
  QCryptographicHash hash(QCryptographicHash::Sha256);qint64 total=0;const auto size=output.size();
  while(total<size){const auto chunk=output.read(std::min<qint64>(1024*1024,size-total));if(chunk.isEmpty())return {};hash.addData(chunk);total+=chunk.size();}
  return hash.result().toHex();
 }
 QFile input;LimitedFile output;
private:
 bool validDirectory(const struct stat& s)const{return S_ISDIR(s.st_mode)&&s.st_uid==getuid()&&(s.st_mode&0777)==0700;}
 bool openFile(QFile& file,const QJsonObject& object,const char* name,mode_t mode,struct stat& identity){
  qint64 device=0,inode=0,size=0;
  if(object.size()!=5||!object["path"].isString()||!object["sha256"].isString()||!number(object["device"],device)||!number(object["inode"],inode)||!number(object["bytes"],size))return false;
  const int fd=::openat(directoryFd,name,(mode==0400?O_RDONLY:O_RDWR)|O_CLOEXEC|O_NOFOLLOW);
  if(fd<0)return false;
  if(::fstat(fd,&identity)||!privateFile(identity,mode)||qulonglong(identity.st_dev)!=qulonglong(device)||qulonglong(identity.st_ino)!=qulonglong(inode)||identity.st_size!=size){::close(fd);return false;}
  if(!file.open(fd,(mode==0400?QIODevice::ReadOnly:QIODevice::ReadWrite)|QIODevice::Unbuffered,QFileDevice::AutoCloseHandle)){::close(fd);return false;}
  return true;
 }
 int directoryFd=-1;struct stat directoryIdentity{},inputIdentity{},outputIdentity{};
 QString directory,inputPath,outputPath,inputHash;
};
QString execute(const QString& task,PrivateFiles& files,qint64 maxPixels,qint64 imageBytes,int& width,int& height){
 if(task=="discover"){
  const auto formats=QImageReader::supportedMimeTypes();if(formats.size()>1024)return "input_excessive";
  QJsonArray array;
  for(const auto& format:formats){if(format.isEmpty()||format.size()>4096)return "input_excessive";array.append(QString::fromLatin1(format));}
  const auto encoded=QJsonDocument(array).toJson(QJsonDocument::Compact);
  if(encoded.size()>discoveryBudget||files.output.write(encoded)!=encoded.size())return files.output.excessive?"input_excessive":"image_job_io";
  width=0;height=0;return {};
 }
 QImageReader reader(&files.input);const auto original=reader.size();
 if(!original.isValid())return "source_unsupported";
 const auto pixels=qint64(original.width())*original.height();
 if(pixels>maxPixels||pixels>imageBytes/4)return "input_excessive";
 if(task=="preview")reader.setScaledSize(original.scaled(256,256,Qt::KeepAspectRatio));
 auto image=reader.read();if(image.isNull())return "source_unsupported";
 if(task=="decode"&&(qint64(image.width())*image.height()>maxPixels||image.sizeInBytes()>imageBytes))return "input_excessive";
 if(task=="preview")image=image.scaled(256,256,Qt::KeepAspectRatio,Qt::SmoothTransformation);
 if(image.isNull())return "source_unsupported";
 width=image.width();height=image.height();
 QImageWriter writer(&files.output,"png");
 if(!writer.write(image))return files.output.excessive?"input_excessive":"source_unsupported";
 return {};
}
}
#endif
int runImageJob(QCoreApplication& app,QIODevice& pipes){
#ifndef Q_OS_UNIX
 Q_UNUSED(app);Q_UNUSED(pipes);return 2;
#else
 UiControl control(pipes);PrivateFiles files;
 QString nonce,task;bool bootstrapped=false,active=false,terminal=false;
 qint64 maxPixels=0,imageBytes=0,outputBytes=0;
 const auto build=QJsonDocument::fromJson(buildManifestJson()).object();
 QTimer deadline;deadline.setSingleShot(true);deadline.start(10000);
 const auto fail=[&]{control.stop();app.exit(3);};
 QObject::connect(&deadline,&QTimer::timeout,&app,fail);
 QObject::connect(&pipes,&QIODevice::readChannelFinished,&app,fail);
 QObject::connect(&control,&UiControl::failed,&app,[&](Error){fail();});
 QObject::connect(&control,&UiControl::frame,&app,[&](const QString& type,const QString&,const QJsonObject& object){
  if(type=="bootstrap"&&!bootstrapped){
   nonce=object["nonce"].toString();task=object["task"].toString();const auto limits=object["limits"].toObject();
   const QUuid id(nonce);
   if(object.size()!=8||nonce.isEmpty()||id.isNull()||id.toString(QUuid::WithoutBraces)!=nonce||!validEnvelope(object,nonce)||object["build"].toObject()!=build||(task!="discover"&&task!="decode"&&task!="preview")||limits.size()!=3||!number(limits["imagePixels"],maxPixels)||maxPixels<1||maxPixels>1000000000LL||!number(limits["imageBytes"],imageBytes)||imageBytes<1||imageBytes>maximumBytes||!number(limits["outputBytes"],outputBytes)||outputBytes<1||outputBytes>maximumBytes||(task=="discover"&&outputBytes>discoveryBudget)||!object["output"].isObject()||!files.open(object["input"],object["output"].toObject(),task=="discover",task=="decode"?imageBytes:maximumBytes,outputBytes)){fail();return;}
   bootstrapped=true;auto ready=envelope(nonce);ready["build"]=build;
   if(control.send("ready",ready).isEmpty())fail();
   return;
  }
  if(!bootstrapped||object.size()!=3||!validEnvelope(object,nonce)){fail();return;}
  if(type=="activate"&&!active&&!terminal){
   active=true;auto result=envelope(nonce);QString error;int width=0,height=0;
   if(!files.namesIntact(task!="discover")||(task!="discover"&&!files.checkInput()))error="image_job_io";
   else error=execute(task,files,maxPixels,imageBytes,width,height);
   if(!files.output.flush()||!files.namesIntact(task!="discover")||(task!="discover"&&!files.checkInput()))error="image_job_io";
   if(error.isEmpty()){
    const auto digest=files.outputHash();const auto size=files.output.size();
    if(size<1||size>outputBytes||digest.size()!=64)error="image_job_io";
    else{result["ok"]=true;result["bytes"]=QString::number(size);result["sha256"]=QString::fromLatin1(digest);result["width"]=width;result["height"]=height;}
   }
   if(!error.isEmpty()){result["ok"]=false;result["code"]=error;}
   terminal=true;
   if(control.send("checkpoint",result).isEmpty())fail();
   return;
  }
  if(type=="checkpointAccepted"&&active&&terminal){deadline.stop();QTimer::singleShot(0,&app,&QCoreApplication::quit);return;}
  fail();
 });
 const auto exitCode=app.exec();control.stop();return exitCode;
#endif
}
}
