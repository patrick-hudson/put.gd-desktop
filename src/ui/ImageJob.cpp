#include "ImageJob.h"
#include "StagingDirectory.h"
#include "ipc/UiControl.h"
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
using staging::number;
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
 bool open(const QJsonValue& inputValue,const QJsonObject& outputValue,bool discovery,qint64 inputMaximum,qint64 outputMaximum){
  if(outputValue.size()!=5||!outputValue["path"].isString())return false;
  const auto outputPath=outputValue["path"].toString();const auto directory=QFileInfo(outputPath).absolutePath();
  static const QRegularExpression pattern(QStringLiteral("^/tmp/pgi-[A-Za-z0-9]{6}$"));
  if(!pattern.match(directory).hasMatch()||outputPath!=directory+"/output.bin")return false;
  if(!staging_.open(directory))return false;
  if(!staging_.openFile(output,outputValue,"output.bin",0600,outputIdentity)||outputIdentity.st_size!=0||outputValue["sha256"]!="")return false;
  output.maximum=outputMaximum;
  if(discovery)return inputValue.isNull()&&namesIntact(false);
  if(!inputValue.isObject())return false;
  const auto object=inputValue.toObject();inputHash=object["sha256"].toString();
  if(object["path"].toString()!=directory+"/input.bin"||!staging::hashText(inputHash)||!staging_.openFile(input,object,"input.bin",0400,inputIdentity)||inputIdentity.st_size<1||inputIdentity.st_size>inputMaximum)return false;
  return namesIntact(true)&&checkInput();
 }
 bool namesIntact(bool hasInput){
  return staging_.intact()&&staging_.outputIntact(output,"output.bin",outputIdentity)&&(!hasInput||staging_.inputIntact(input,"input.bin",inputIdentity));
 }
 bool checkInput(){return staging::hashMatches(input,inputIdentity.st_size,inputHash);}
 QByteArray outputHash(){
  if(!output.seek(0))return {};
  QCryptographicHash hash(QCryptographicHash::Sha256);qint64 total=0;const auto size=output.size();
  while(total<size){const auto chunk=output.read(std::min<qint64>(1024*1024,size-total));if(chunk.isEmpty())return {};hash.addData(chunk);total+=chunk.size();}
  return hash.result().toHex();
 }
 QFile input;LimitedFile output;
private:
 staging::Directory staging_;struct stat inputIdentity{},outputIdentity{};
 QString inputHash;
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
