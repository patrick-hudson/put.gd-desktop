#include "TextJob.h"
#include "BuildInfo.h"
#include "ParentDeath.h"
#include "ipc/UiControl.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonDocument>
#include <QPainter>
#include <QProcess>
#include <QProcessEnvironment>
#include <QUuid>
#include <algorithm>
#include <cerrno>
#include <cstring>
#ifdef Q_OS_LINUX
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#ifndef CLOSE_RANGE_CLOEXEC
#define CLOSE_RANGE_CLOEXEC (1U << 2)
#endif
#endif

namespace putgd {
namespace {
std::atomic_bool running{false};
#ifdef Q_OS_LINUX
QJsonObject build(){return QJsonDocument::fromJson(buildManifestJson()).object();}
struct Descriptor {
 int fd=-1;
 ~Descriptor(){reset();}
 void reset(){if(fd>=0)::close(fd);fd=-1;}
};
// A memfd above the two fixed child descriptors, so dup2 in the child can never
// replace one of them with the other.
int memfd(const char* name,bool sealable){
 int fd=int(::syscall(SYS_memfd_create,name,MFD_CLOEXEC|(sealable?MFD_ALLOW_SEALING:0)));
 if(fd>=0&&fd<=textjob::outputDescriptor){const int moved=::fcntl(fd,F_DUPFD_CLOEXEC,textjob::outputDescriptor+1);::close(fd);fd=moved;}
 return fd;
}
bool writeAll(int fd,const char* data,qint64 size){
 while(size>0){const auto written=::write(fd,data,size_t(size));if(written<0&&errno==EINTR)continue;if(written<=0)return false;data+=written;size-=written;}
 return true;
}
#endif
}
TextJob::TextJob(QString path,int timeout):helperPath_(path.isEmpty()?defaultHelper():std::move(path)),timeoutMs_(std::clamp(timeout,100,textjob::maximumDeadlineMs)){}
QString TextJob::defaultHelper(){return QCoreApplication::applicationDirPath()+"/putgd-ocr";}
bool TextJob::busy(){return running.load();}
QStringList TextJob::dataDirectories() const {
 if(!dataDirectories_.isEmpty())return dataDirectories_;
 QStringList paths;for(const auto& directory:textjob::dataDirectories(QDir::homePath(),QCoreApplication::applicationDirPath()))paths.append(directory.path);
 return paths;
}
QImage TextJob::grey(const QImage& source){
 if(source.isNull()||source.format()==QImage::Format_Grayscale8)return source;
 QImage out(source.size(),QImage::Format_Grayscale8);if(out.isNull())return {};
 // In strips, so a large capture never has a second full-colour copy.
 constexpr int strip=256;
 for(int y=0;y<source.height();y+=strip){
  const int rows=std::min(strip,source.height()-y);
  QImage flat(source.width(),rows,QImage::Format_RGB32);if(flat.isNull())return {};
  flat.fill(Qt::white);{QPainter painter(&flat);painter.drawImage(QPoint(0,0),source,QRect(0,y,source.width(),rows));}
  const auto grey=flat.convertToFormat(QImage::Format_Grayscale8);
  for(int row=0;row<rows;++row)std::memcpy(out.scanLine(y+row),grey.constScanLine(row),size_t(source.width()));
 }
 return out;
}
TextResult TextJob::recognize(const QImage& image,const Options& options,Error& error,Cancel cancelled) const {
 error={};
#ifndef Q_OS_LINUX
 Q_UNUSED(image);Q_UNUSED(options);Q_UNUSED(cancelled);
 error=textjob::failure("text_helper_missing");return {};
#else
 // The deadline starts before anything is prepared and covers the whole job.
 QElapsedTimer clock;clock.start();
 bool idle=false;if(!running.compare_exchange_strong(idle,true)){error=textjob::failure("text_busy");return {};}
 struct Release {~Release(){running.store(false);}} release;
 const auto stopped=[&]{if(cancelled&&cancelled->load()){error=textjob::failure("cancelled");return true;}if(clock.elapsed()>=timeoutMs_){error=textjob::failure("text_job_timeout");return true;}return false;};
 if(stopped())return {};
 if(image.isNull()||image.format()!=QImage::Format_Grayscale8){error=textjob::failure("text_job_failed");return {};}
 const textjob::InputHeader header{image.width(),image.height(),int(image.bytesPerLine())};
 if(!textjob::admitted(header.width,header.height,header.stride)){error=textjob::failure("input_excessive");return {};}
 if(!textjob::validLanguages(options.languages)){error=textjob::failure("language_unavailable",options.languages.value(0));return {};}
 const QFileInfo helper(helperPath_);if(!helper.isFile()||!helper.isExecutable()){error=textjob::failure("text_helper_missing");return {};}
 Descriptor input{memfd("putgd-text-input",true)},output{memfd("putgd-text-output",false)};
 if(input.fd<0||output.fd<0||!writeAll(input.fd,textjob::encodeInputHeader(header).constData(),textjob::inputHeaderBytes)){error=textjob::failure("text_job_failed");return {};}
 const auto* pixels=reinterpret_cast<const char*>(image.constBits());const qint64 total=qint64(header.stride)*header.height;
 for(qint64 offset=0;offset<total;offset+=1024*1024){
  if(stopped())return {};
  if(!writeAll(input.fd,pixels+offset,std::min<qint64>(1024*1024,total-offset))){error=textjob::failure("text_job_failed");return {};}
 }
 // Sealed, so the child reads exactly what was written here and nothing can change under it.
 if(::fcntl(input.fd,F_ADD_SEALS,textjob::requiredSeals|F_SEAL_SEAL)!=0){error=textjob::failure("text_job_failed");return {};}
 const auto nonce=QUuid::createUuid().toString(QUuid::WithoutBraces);
 QProcess process;process.setProcessChannelMode(QProcess::SeparateChannels);process.setReadChannel(QProcess::StandardOutput);
 UiControl control(process);bool ready=false,result=false,protocolFailed=false,flooded=false;Error childError;qint64 bytes=0,stderrBytes=0,spawnMs=0;QString sha256;
 QObject::connect(&control,&UiControl::failed,&process,[&](Error){protocolFailed=true;});
 // Descriptor 2 stays a capped, discarded pipe, as for the image worker.
 QObject::connect(&process,&QProcess::readyReadStandardError,&process,[&]{
  process.setReadChannel(QProcess::StandardError);
  while(process.bytesAvailable()>0){stderrBytes+=process.read(16384).size();if(stderrBytes>1024*1024){flooded=true;break;}}
  process.setReadChannel(QProcess::StandardOutput);
 });
 QObject::connect(&control,&UiControl::frame,&process,[&](const QString& type,const QString&,const QJsonObject& payload){
  if(protocolFailed)return;
  if(type=="ready"&&!ready&&!result&&textjob::validReady(payload,nonce,build())){
   ready=true;spawnMs=clock.elapsed();if(control.send("activate",textjob::envelope(nonce)).isEmpty())protocolFailed=true;return;
  }
  if(type=="checkpoint"&&ready&&!result&&textjob::decodeCheckpoint(payload,nonce,childError,bytes,sha256)){
   result=true;if(control.send("checkpointAccepted",textjob::envelope(nonce)).isEmpty())protocolFailed=true;return;
  }
  protocolFailed=true;
 });
 // Cleared, then only the data path, one OpenMP thread and a C UTF-8 locale.
 QProcessEnvironment environment;environment.insert(textjob::dataPathVariable,dataDirectories().join(':'));environment.insert("OMP_THREAD_LIMIT","1");environment.insert("LC_ALL","C.UTF-8");
 process.setProcessEnvironment(environment);
 const auto parent=::getpid();const int in=input.fd,out=output.fd;
 process.setChildProcessModifier([parent,in,out]{
  // Only async-signal-safe calls between fork and exec. dup2 clears close-on-exec.
  if(!dieWithParent(parent,SIGKILL))::_exit(127);
  if(::dup2(in,textjob::inputDescriptor)<0||::dup2(out,textjob::outputDescriptor)<0)::_exit(126);
  // Nothing else crosses exec: every other descriptor is closed by it.
#ifdef SYS_close_range
  if(::syscall(SYS_close_range,unsigned(textjob::outputDescriptor+1),~0U,CLOSE_RANGE_CLOEXEC)==0)return;
#endif
  struct rlimit limit{};const int last=::getrlimit(RLIMIT_NOFILE,&limit)==0&&limit.rlim_cur<65536?int(limit.rlim_cur):65536;
  for(int fd=textjob::outputDescriptor+1;fd<last;++fd)::fcntl(fd,F_SETFD,FD_CLOEXEC);
 });
 process.start(helperPath_,QStringList{});
 bool started=false;
 while(!started&&!protocolFailed&&!stopped()){
  if(process.waitForStarted(int(std::clamp<qint64>(timeoutMs_-clock.elapsed(),1,50)))){started=true;break;}
  if(process.error()==QProcess::FailedToStart){error=textjob::failure("text_helper_missing");break;}
 }
 // The child holds its own copy now; the pixels never count in this process again.
 input.reset();
 if(started&&!error&&control.send("bootstrap",textjob::bootstrap(nonce,build(),header,options)).isEmpty())protocolFailed=true;
 while(started&&process.state()!=QProcess::NotRunning&&!protocolFailed&&!flooded&&!stopped())process.waitForReadyRead(int(std::clamp<qint64>(timeoutMs_-clock.elapsed(),1,50)));
 if(!error&&protocolFailed)error=textjob::failure("text_incompatible");
 if(!error&&flooded)error=textjob::failure("text_job_failed");
 // The exact child: terminate, then kill.
 if(process.state()!=QProcess::NotRunning){control.stop();process.terminate();if(!process.waitForFinished(500)){process.kill();process.waitForFinished(1500);}}
 control.stop();
 if(!error&&(!started||process.state()!=QProcess::NotRunning||process.exitStatus()!=QProcess::NormalExit||process.exitCode()!=0||!ready||!result))error=textjob::failure("text_job_failed");
 if(error)return {};
 if(childError){error=childError;return {};}
 // The output is read through the descriptor opened here, never by a path.
 struct stat status{};
 if(::fstat(output.fd,&status)!=0||status.st_size!=bytes){error=textjob::failure("text_incompatible");return {};}
 QByteArray json(qsizetype(bytes),Qt::Uninitialized);qint64 read=0;
 while(read<bytes){const auto count=::pread(output.fd,json.data()+read,size_t(bytes-read),off_t(read));if(count<0&&errno==EINTR)continue;if(count<=0)break;read+=count;}
 TextResult decoded;
 if(read!=bytes||QString::fromLatin1(QCryptographicHash::hash(json,QCryptographicHash::Sha256).toHex())!=sha256||!textjob::decodeResult(json,header,options,decoded)){error=textjob::failure("text_incompatible");return {};}
 decoded.spawnMs=spawnMs;decoded.totalMs=clock.elapsed();
 return decoded;
#endif
}
}
