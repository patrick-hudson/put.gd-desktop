// putgd-ocr: the temporary text-recognition helper (#1428). One job, then exit.
// It is the only binary that links Tesseract, Leptonica and ZXing-cpp, and it
// needs QtCore alone: no QtGui, QtSql, QtNetwork or QtDBus.
#include "Confinement.h"
#include "Descriptors.h"
#include "Recognize.h"
#include "core/BuildInfo.h"
#include "ipc/StdioDevice.h"
#include "ipc/UiControl.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>
#include <QUuid>
#include <cstdlib>
#include <sys/syscall.h>
#include <unistd.h>

#if defined(__SANITIZE_ADDRESS__)
#define PUTGD_OCR_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define PUTGD_OCR_ASAN 1
#endif
#endif
#ifdef PUTGD_OCR_ASAN
// LeakSanitizer stops the world with a clone that is not a thread, which the
// seccomp filter kills; sanitizer builds check the helper's memory otherwise.
extern "C" const char* __asan_default_options(){return "detect_leaks=0";}
#endif

namespace {
using namespace putgd::ocr;
// The memory budget (decision 3): peak RSS under 128 MiB with the shipped
// English data, growing by other languages' data, 192 MiB at most.
// RLIMIT_DATA is about one and a half times the budget.
constexpr qint64 mib=1024*1024;
constexpr qint64 baseBudget=128*mib,largestBudget=192*mib;
constexpr int cpuSeconds=21;  // just above the parent's 20 s deadline
QJsonObject envelope(const QString& nonce){return {{"role","text-job"},{"nonce",nonce},{"generation","1"}};}
bool validEnvelope(const QJsonObject& o,const QString& nonce){return o["role"]=="text-job"&&o["nonce"]==nonce&&o["generation"]=="1";}
bool number(const QJsonValue& value,qint64& out){bool ok=false;if(!value.isString())return false;out=value.toString().toLongLong(&ok);return ok&&out>=0&&QString::number(out)==value.toString();}
QStringList dataDirectories(){
 // The parent sets the language directories in resolution order: the user's
 // own first, then the packaged or system one. Nothing else comes from the
 // environment.
 QStringList out;
 for(const auto& d:QString::fromLocal8Bit(qgetenv("PUTGD_OCR_DATA")).split(QLatin1Char(':'),Qt::SkipEmptyParts))
  if(d.startsWith(QLatin1Char('/'))&&!d.contains(QLatin1String("/../"))&&!d.endsWith(QLatin1String("/.."))&&!out.contains(d))out.append(d);
 return out;
}
}

int main(int argc,char** argv){
 // Only the control pipes, stderr and the two memfds stay open.
 ::syscall(SYS_close_range,5u,~0u,0u);
 putgd::UiControlPipes pipes;
 const auto directories=dataDirectories();
 Confinement confinement;confinement.readable=readableDirectories(directories);
 confinement.dataBytes=largestBudget*3/2;confinement.cpuSeconds=cpuSeconds;confinement.fileBytes=maximumOutputBytes;confinement.descriptors=32;
 QString confinementFailure;int abi=0;
 const bool confined=confine(confinement,confinementFailure,abi);
 QCoreApplication app(argc,argv);
 if(!pipes.valid())return 2;
 putgd::StdioDevice device(pipes.input(),pipes.output());
 putgd::UiControl control(device);
 const auto build=QJsonDocument::fromJson(putgd::buildManifestJson()).object();
 QString nonce;bool bootstrapped=false,active=false,terminal=false;JobOptions options;
 QTimer deadline;deadline.setSingleShot(true);deadline.start(25000);
 const auto fail=[&]{control.stop();app.exit(3);};
 QObject::connect(&deadline,&QTimer::timeout,&app,fail);
 QObject::connect(&device,&QIODevice::readChannelFinished,&app,fail);
 QObject::connect(&control,&putgd::UiControl::failed,&app,[&](putgd::Error){fail();});
 QObject::connect(&control,&putgd::UiControl::frame,&app,[&](const QString& type,const QString&,const QJsonObject& o){
  if(type=="bootstrap"&&!bootstrapped){
   nonce=o["nonce"].toString();const QUuid id(nonce);const auto limits=o["limits"].toObject();qint64 pixels=0,output=0;
   const auto languages=o["languages"].toArray();
   if(id.isNull()||id.toString(QUuid::WithoutBraces)!=nonce||!validEnvelope(o,nonce)){fail();return;}
   if(o["build"].toObject()!=build){
    // A different build of put.gd started this helper: say so, then stop.
    auto incompatible=envelope(nonce);incompatible["build"]=build;control.send("incompatible",incompatible);
    QTimer::singleShot(0,&app,[&]{control.stop();app.exit(3);});return;
   }
   if(o.size()!=10||o["task"]!="recognize"
      ||!o["words"].isBool()||!o["candidates"].isBool()||!o["strictLanguages"].isBool()||languages.isEmpty()||languages.size()>maximumLanguages
      ||limits.size()!=2||!number(limits["inputPixels"],pixels)||pixels<1||pixels>maximumInputPixels||!number(limits["outputBytes"],output)||output<1||output>maximumOutputBytes){fail();return;}
   options.languages.clear();
   for(const auto& l:languages){if(!l.isString()||!validLanguage(l.toString())||options.languages.contains(l.toString())){fail();return;}options.languages.append(l.toString());}
   if(options.languages==QStringList{QStringLiteral("eng")}&&!lowerDataLimit(baseBudget*3/2)){fail();return;}
   options.words=o["words"].toBool();options.candidates=o["candidates"].toBool();options.strictLanguages=o["strictLanguages"].toBool();options.directories=directories;
   bootstrapped=true;auto ready=envelope(nonce);ready["build"]=build;
   if(control.send("ready",ready).isEmpty())fail();
   return;
  }
  if(!bootstrapped||o.size()!=3||!validEnvelope(o,nonce)){fail();return;}
  if(type=="activate"&&!active&&!terminal){
   active=true;auto reply=envelope(nonce);QString code,language;QByteArray encoded;
   InputImage input;
   if(!confined)code=QStringLiteral("helper_sandbox_unavailable");
   else if(!outputReady(outputDescriptor))code=QStringLiteral("text_job_io");
   else code=input.map(inputDescriptor,maximumInputPixels);
   if(code.isEmpty()){
    const auto result=recognizeImage(input.view(),options);
    code=result.error;language=result.failedLanguage;
    if(code.isEmpty()){
     encoded=encodeResult(result,options);
     if(encoded.size()>maximumOutputBytes||!writeOutput(outputDescriptor,encoded))code=QStringLiteral("text_job_io");
    }
   }
   if(code.isEmpty()){
    reply["ok"]=true;reply["bytes"]=QString::number(encoded.size());
    reply["sha256"]=QString::fromLatin1(QCryptographicHash::hash(encoded,QCryptographicHash::Sha256).toHex());
   }else{
    reply["ok"]=false;reply["code"]=code;
    if(code=="language_unavailable"&&validLanguage(language))reply["language"]=language;
   }
   terminal=true;
   if(control.send("checkpoint",reply).isEmpty())fail();
   return;
  }
  if(type=="checkpointAccepted"&&active&&terminal){deadline.stop();QTimer::singleShot(0,&app,&QCoreApplication::quit);return;}
  fail();
 });
 Q_UNUSED(abi);
 const int status=app.exec();control.stop();return status;
}
