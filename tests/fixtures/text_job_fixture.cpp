// The fake putgd-ocr helper (#1428): the real child half of the text-job
// contract (src/ocr/TextJobChild) around a canned engine. TextJob starts it
// like the product helper, with a cleared environment, so it takes its plan
// from fixture.json in the first data directory (tests/fixtures/FakeTextHelper.h
// writes it). Nothing here exists in a product binary.
#include "ocr/TextJobChild.h"
#include "ipc/StdioDevice.h"
#include "ipc/UiControl.h"
#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QThread>
#include <dirent.h>
#include <sys/resource.h>
#include <unistd.h>

extern char** environ;
using namespace putgd;
using namespace putgd::textjob;

namespace {
QList<TextWord> words(const QJsonValue& value){
 QList<TextWord> list;
 for(const auto& entry:value.toArray()){const auto word=entry.toObject();const auto rect=word["rect"].toArray();list.append({word["text"].toString(),QRect(rect[0].toInt(),rect[1].toInt(),rect[2].toInt(),rect[3].toInt()),word["confidence"].toInt()});}
 return list;
}
// A plan's result is taken as written, valid or not: the parent has to judge it.
TextResult canned(const QJsonObject& object){
 TextResult result;result.text=object["text"].toString();
 for(const auto& top:object["lineTops"].toArray())result.lineTops.append(top.toInt());
 result.words=words(object["words"]);result.candidates=words(object["candidates"]);
 for(const auto& entry:object["codes"].toArray()){
  const auto code=entry.toObject();TextCode decoded;decoded.format=code["format"].toString();decoded.binary=code["kind"]=="binary";decoded.text=code["text"].toString();decoded.length=code["length"].toInteger();
  for(const auto& corner:code["quad"].toArray())decoded.quad.append({corner.toArray()[0].toInt(),corner.toArray()[1].toInt()});
  result.codes.append(decoded);
 }
 result.truncated=object["truncated"].toBool();result.banded=object["banded"].toBool();
 for(const auto& language:object["languagesUsed"].toArray())result.languagesUsed.append(language.toString());
 for(const auto& language:object["languagesSkipped"].toArray())result.languagesSkipped.append(language.toString());
 result.engineStartMs=1;result.recognizeMs=2;
 return result;
}
// The descriptors this process was started with, in order.
QString descriptors(){
 QStringList open;DIR* directory=::opendir("/proc/self/fd");if(!directory)return {};
 while(const auto* entry=::readdir(directory)){bool number=false;const int fd=QByteArray(entry->d_name).toInt(&number);if(number&&fd!=::dirfd(directory))open.append(QString::number(fd));}
 ::closedir(directory);std::sort(open.begin(),open.end(),[](const QString& a,const QString& b){return a.toInt()<b.toInt();});
 return open.join(',');
}
QString environment(){
 QStringList names;for(char** entry=environ;entry&&*entry;++entry)names.append(QString::fromLocal8Bit(*entry).section('=',0,0));
 names.sort();return names.join(',');
}
}
// The two answers a parent must refuse, which the contract's own child never
// gives: a ready from another build, and a result above the output cap.
int raw(int& argc,char** argv,const QString& mode) {
 QCoreApplication app(argc,argv);StdioDevice pipes;if(!pipes.isOpen())return 2;
 UiControl control(pipes);QString nonce;
 QObject::connect(&pipes,&QIODevice::readChannelFinished,&app,[&]{app.exit(5);});
 QObject::connect(&control,&UiControl::failed,&app,[&]{app.exit(3);});
 QObject::connect(&control,&UiControl::frame,&app,[&](const QString& type,const QString&,const QJsonObject& payload){
  if(type=="bootstrap"){nonce=payload["nonce"].toString();control.send("ready",ready(nonce,mode=="wrong-build"?QJsonObject{{"version","another build"}}:payload["build"].toObject()));}
  else if(type=="activate"){
   const QByteArray output(maximumOutputBytes+1,' ');qint64 written=0;
   while(written<output.size()){const auto count=::pwrite(outputDescriptor,output.constData()+written,size_t(output.size()-written),off_t(written));if(count<=0)break;written+=count;}
   control.send("checkpoint",checkpoint(nonce,output));
  }else app.exit(type=="checkpointAccepted"?0:3);
 });
 return app.exec();
}
int main(int argc,char** argv) {
 const rlimit noCore{0,0};::setrlimit(RLIMIT_CORE,&noCore);
 const auto inherited=descriptors();
 const auto directory=qEnvironmentVariable(dataPathVariable).section(':',0,0);
 QFile planFile(directory+"/fixture.json");QJsonObject plan;
 if(planFile.open(QIODevice::ReadOnly))plan=QJsonDocument::fromJson(planFile.readAll()).object();
 const auto mode=plan["mode"].toString("describe");
 {QFile pid(directory+"/fixture.pid");if(pid.open(QIODevice::WriteOnly|QIODevice::Truncate))pid.write(QByteArray::number(qint64(::getpid())));}
 if(mode=="wrong-build"||mode=="oversize")return raw(argc,argv,mode);
 const Engine engine=[&](const GreyImage& image,const Options& options,const QStringList& directories,TextResult& result)->ChildFailure {
  if(mode=="stall")for(;;)::pause();
  if(mode=="child-error")return {plan["code"].toString(),plan["language"].toString()};
  if(plan.contains("delayMs"))QThread::msleep(static_cast<unsigned long>(plan["delayMs"].toInt()));
  if(mode=="result"){result=canned(plan["result"].toObject());return {};}
  // describe: what arrived, so a test sees the transfer end to end.
  quint64 sum=0;for(int y=0;y<image.height;++y)for(int x=0;x<image.width;++x)sum+=image.data[qsizetype(y)*image.stride+x];
  // The input is sealed: a write through the inherited descriptor has to fail.
  const char byte=0;const bool writable=::pwrite(inputDescriptor,&byte,1,0)==1;
  result.text=QStringLiteral("image %1x%2 stride %3 sum %4\noptions words=%5 candidates=%6 required=%7 languages=%8\ndescriptors %9\nenvironment %10\ndirectories %11\ninput %12")
   .arg(image.width).arg(image.height).arg(image.stride).arg(sum).arg(options.words).arg(options.candidates).arg(options.languagesRequired).arg(options.languages.join('+'),inherited,environment())
   .arg(directories.size()).arg(writable?"writable":"sealed");
  result.lineTops={0,1,2,3,4,5};result.languagesUsed=options.languages;result.engineStartMs=1;result.recognizeMs=2;
  return {};
 };
 return runChild(argc,argv,engine);
}
