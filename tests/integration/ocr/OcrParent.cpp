#include "OcrParent.h"
#include "core/BuildInfo.h"
#include "ipc/UiControl.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPainter>
#include <QProcessEnvironment>
#include <QUuid>
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#ifndef CLOSE_RANGE_CLOEXEC
#define CLOSE_RANGE_CLOEXEC (1U << 2)
#endif

namespace putgd::test {
namespace {
QJsonObject envelope(const QString& nonce){return {{"role","text-job"},{"nonce",nonce},{"generation","1"}};}
bool writeAll(int fd,const char* data,qsizetype size){
 while(size>0){const auto n=::write(fd,data,size_t(size));if(n<0&&errno==EINTR)continue;if(n<=0)return false;data+=n;size-=n;}
 return true;
}
void little(char* p,uint32_t v){p[0]=char(v);p[1]=char(v>>8);p[2]=char(v>>16);p[3]=char(v>>24);}
// The child's peak resident set, read while it waits for checkpointAccepted:
// all its work is done by then.
long peakResident(qint64 pid){
 QFile status(QStringLiteral("/proc/%1/status").arg(pid));
 if(!status.open(QIODevice::ReadOnly))return 0;
 for(const auto& line:status.readAll().split('\n'))if(line.startsWith("VmHWM:"))return line.mid(6).trimmed().split(' ').value(0).toLong();
 return 0;
}
bool processExists(qint64 pid){return pid>0&&QFileInfo::exists(QStringLiteral("/proc/%1").arg(pid));}
}
QString defaultDataDirectory(){
 for(const auto& d:{QStringLiteral("/usr/share/tesseract-ocr/5/tessdata"),QStringLiteral("/usr/share/tesseract/tessdata"),QStringLiteral("/usr/share/tessdata")})
  if(QFileInfo::exists(d+QStringLiteral("/eng.traineddata")))return d;
 return {};
}
OcrRun runOcr(const QImage& image,const OcrRequest& request){
 QElapsedTimer clock;clock.start();
 QImage flat(image.size(),QImage::Format_RGB32);flat.fill(Qt::white);
 {QPainter painter(&flat);painter.drawImage(0,0,image);}
 const auto grey=flat.convertToFormat(QImage::Format_Grayscale8);
 QByteArray bytes;bytes.reserve(qsizetype(grey.width())*grey.height());
 for(int y=0;y<grey.height();++y)bytes.append(reinterpret_cast<const char*>(grey.constScanLine(y)),grey.width());
 const auto converted=clock.elapsed();
 auto run=runOcrGrey(bytes,grey.width(),grey.height(),grey.width(),request);
 run.inputMs+=converted;run.totalMs+=converted;
 return run;
}
OcrRun runOcrGrey(const QByteArray& grey,int width,int height,int stride,const OcrRequest& request){
 OcrRun run;QElapsedTimer clock;clock.start();
 const int input=int(::syscall(SYS_memfd_create,"putgd-ocr-input",MFD_CLOEXEC|MFD_ALLOW_SEALING));
 const int output=int(::syscall(SYS_memfd_create,"putgd-ocr-output",MFD_CLOEXEC|MFD_ALLOW_SEALING));
 struct Close{int fd;~Close(){if(fd>=0)::close(fd);}};
 Close inputOwner{input},outputOwner{output};
 if(input<0||output<0){run.error="text_job_io";return run;}
 char header[16]={'P','G','R','Y'};little(header+4,uint32_t(width));little(header+8,uint32_t(height));little(header+12,uint32_t(stride));
 if(!writeAll(input,header,16)||!writeAll(input,grey.constData(),grey.size())){run.error="text_job_io";return run;}
 if(!request.unsealedInput&&::fcntl(input,F_ADD_SEALS,F_SEAL_SEAL|F_SEAL_WRITE|F_SEAL_SHRINK|F_SEAL_GROW)!=0){run.error="text_job_io";return run;}
 int childOutput=output;Close readOnly{-1};
 if(request.readOnlyOutput){readOnly.fd=::open(QStringLiteral("/proc/self/fd/%1").arg(output).toLatin1().constData(),O_RDONLY|O_CLOEXEC);childOutput=readOnly.fd;}
 // Above any number the child modifier writes, so dup2 never overwrites one of them.
 const int highInput=::fcntl(input,F_DUPFD_CLOEXEC,64),highOutput=::fcntl(childOutput,F_DUPFD_CLOEXEC,64);
 Close highInputOwner{highInput},highOutputOwner{highOutput};
 run.inputMs=clock.elapsed();
 QProcess process;process.setProcessChannelMode(QProcess::SeparateChannels);process.setReadChannel(QProcess::StandardOutput);
 process.setStandardErrorFile(QProcess::nullDevice());
 QProcessEnvironment environment;
 environment.insert("PUTGD_OCR_DATA",request.dataDirectories.join(':'));
 environment.insert("OMP_THREAD_LIMIT","1");environment.insert("LC_ALL","C.UTF-8");
 process.setProcessEnvironment(environment);
 const auto parent=::getpid();
 process.setChildProcessModifier([highInput,highOutput,parent]{
  if(::dup2(highInput,3)<0||::dup2(highOutput,4)<0)::_exit(126);
  // Everything else closes at exec.
  ::syscall(SYS_close_range,5u,~0u,CLOSE_RANGE_CLOEXEC);
  if(::prctl(PR_SET_PDEATHSIG,SIGKILL,0L,0L,0L)!=0||::getppid()!=parent)::_exit(127);
 });
 UiControl control(process);bool ready=false,result=false,protocolFailed=false;QJsonObject reply;
 const auto nonce=QUuid::createUuid().toString(QUuid::WithoutBraces);
 QJsonObject build=QJsonDocument::fromJson(buildManifestJson()).object();
 if(request.corruptBuild)build["version"]="0.0.0-wrong";
 QJsonArray languages;for(const auto& l:request.languages)languages.append(l);
 auto bootstrap=envelope(nonce);bootstrap["build"]=build;bootstrap["task"]="recognize";bootstrap["words"]=request.words;bootstrap["candidates"]=request.candidates;
 bootstrap["languages"]=languages;bootstrap["strictLanguages"]=request.strictLanguages;
 bootstrap["limits"]=QJsonObject{{"inputPixels","24000000"},{"outputBytes","8388608"}};
 QElapsedTimer stage;
 QObject::connect(&control,&UiControl::failed,&process,[&](Error){protocolFailed=true;});
 QObject::connect(&control,&UiControl::frame,&process,[&](const QString& type,const QString&,const QJsonObject& payload){
  if(payload["role"]!="text-job"||payload["nonce"]!=nonce||payload["generation"]!="1"){protocolFailed=true;return;}
  if(type=="incompatible"&&!ready){protocolFailed=true;return;}
  if(type=="ready"&&!ready){
   ready=true;run.spawnMs=stage.elapsed();stage.restart();
   if(request.stopAfterReady==1){process.kill();return;}
   if(request.stopAfterReady==2)return;
   control.send("activate",envelope(nonce));return;
  }
  if(type=="checkpoint"&&ready&&!result){
   result=true;reply=payload;run.recognitionMs=stage.elapsed();stage.restart();
   run.peakRssKiB=peakResident(process.processId());
   control.send("checkpointAccepted",envelope(nonce));return;
  }
  protocolFailed=true;
 });
 stage.start();
 process.start(request.helper,{});
 const auto pid=[&]{return process.processId();};
 qint64 childPid=0;
 if(process.waitForStarted(5000)){childPid=pid();control.send("bootstrap",bootstrap);}
 else run.error="text_helper_missing";
 while(run.error.isEmpty()&&process.state()!=QProcess::NotRunning&&!protocolFailed&&clock.elapsed()<request.timeoutMs)
  process.waitForReadyRead(int(std::clamp<qint64>(request.timeoutMs-clock.elapsed(),1,50)));
 if(run.error.isEmpty()&&clock.elapsed()>=request.timeoutMs)run.error="text_job_timeout";
 if(run.error.isEmpty()&&protocolFailed)run.error="text_incompatible";
 if(process.state()!=QProcess::NotRunning){control.stop();process.terminate();if(!process.waitForFinished(500)){process.kill();process.waitForFinished(1500);}}
 control.stop();
 run.exitCode=process.exitCode();run.exitStatus=process.exitStatus();
 if(run.exitStatus==QProcess::CrashExit)run.signal=run.exitCode;  // Qt reports the signal there
 struct rusage usage{};if(!run.peakRssKiB&&::getrusage(RUSAGE_CHILDREN,&usage)==0)run.peakRssKiB=usage.ru_maxrss;
 run.childGone=!processExists(childPid);
 if(run.error.isEmpty()&&run.exitStatus!=QProcess::NormalExit)run.error="text_job_failed";
 if(run.error.isEmpty()&&(!ready||!result))run.error="text_job_failed";
 if(run.error.isEmpty()&&!reply["ok"].toBool()){run.error=reply["code"].toString();run.language=reply["language"].toString();}
 if(run.error.isEmpty()&&run.exitCode!=0)run.error="text_job_failed";
 if(run.error.isEmpty()){
  stage.restart();
  struct stat s{};const auto bytes=reply["bytes"].toString().toLongLong();
  if(::fstat(output,&s)||s.st_size!=bytes||bytes<1||bytes>8*1024*1024){run.error="text_job_io";}
  else{
   run.output.resize(qsizetype(bytes));
   if(::pread(output,run.output.data(),size_t(bytes),0)!=bytes||QCryptographicHash::hash(run.output,QCryptographicHash::Sha256).toHex()!=reply["sha256"].toString().toLatin1())run.error="text_job_io";
   else{QJsonParseError parse;const auto document=QJsonDocument::fromJson(run.output,&parse);if(parse.error!=QJsonParseError::NoError||!document.isObject())run.error="text_incompatible";else run.result=document.object();}
  }
  run.copyMs=stage.elapsed();
 }
 run.totalMs=clock.elapsed();
 return run;
}
}
