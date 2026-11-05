#include "PosterJob.h"
#include "ImageFormat.h"
#include <QBuffer>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QProcess>
#include <algorithm>
#ifdef Q_OS_UNIX
#include <fcntl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define PUTGD_POSTER_SANITIZED 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) || __has_feature(memory_sanitizer)
#define PUTGD_POSTER_SANITIZED 1
#endif
#endif

namespace putgd {
namespace {
// A 256 px poster is a few hundred KiB at most; anything larger is not one.
constexpr qint64 maximumOutput=2*1024*1024;
#if defined(Q_OS_UNIX)&&!defined(PUTGD_POSTER_SANITIZED)
// Address space for the helper. A 4K H.265 poster peaked at 570 MiB of
// address space and 186 MiB resident (#112, docs/recording.md); 2 GiB leaves
// room for longer GOPs. Sanitizer shadow memory cannot work under a limit, so
// sanitizer builds leave it off.
constexpr rlim_t addressSpace=rlim_t(2)*1024*1024*1024;
#endif
Error failure(const QString& code){
 if(code=="cancelled")return {code,"Video preview cancelled",{}};
 if(code=="input_excessive")return {code,"The video exceeds the preview limits",{}};
 if(code=="poster_timeout")return {code,"Video preview timed out",{}};
 if(code=="poster_helper_missing")return {code,"Video previews need the putgd-record helper",{}};
 return {code,"The video preview could not be made",{}};
}
}
PosterJob::PosterJob(QString path,int timeout):helperPath_(path.isEmpty()?defaultHelper():std::move(path)),timeoutMs_(std::clamp(timeout,100,30000)){}
QString PosterJob::defaultHelper(){return QCoreApplication::applicationDirPath()+"/putgd-record";}
QByteArray PosterJob::render(const QString& file,qint64 maxPixels,qint64 budget,Error& error,Cancel cancelled) const {
 error={};
#ifndef Q_OS_UNIX
 Q_UNUSED(file);Q_UNUSED(maxPixels);Q_UNUSED(budget);Q_UNUSED(cancelled);
 error=failure("poster_helper_missing");return {};
#else
 if(maxPixels<1||budget<1){error=failure("input_excessive");return {};}
 const QFileInfo helper(helperPath_);if(!helper.isFile()||!helper.isExecutable()){error=failure("poster_helper_missing");return {};}
 // The child reads exactly the file opened here: never a symlink, a FIFO or a
 // device, and never whatever the name points at by the time it starts.
 const int fd=::open(QFile::encodeName(file).constData(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);
 struct stat s{};if(fd<0||::fstat(fd,&s)||!S_ISREG(s.st_mode)||s.st_size<1||::fcntl(fd,F_SETFL,0)<0){if(fd>=0)::close(fd);error=failure("poster_failed");return {};}
 const qint64 limit=std::min(budget,maximumOutput);const int cpuSeconds=timeoutMs_*2/1000+1;
 QProcess process;process.setStandardErrorFile(QProcess::nullDevice());
 process.setChildProcessModifier([fd,cpuSeconds]{
  // Only async-signal-safe calls between fork and exec. dup2 clears close-on-exec.
  if(::dup2(fd,STDIN_FILENO)<0)::_exit(126);
  const rlimit none{0,0},cpu{rlim_t(cpuSeconds),rlim_t(cpuSeconds)},files{64,64};
  ::setrlimit(RLIMIT_CORE,&none);::setrlimit(RLIMIT_FSIZE,&none);::setrlimit(RLIMIT_CPU,&cpu);::setrlimit(RLIMIT_NOFILE,&files);
#ifndef PUTGD_POSTER_SANITIZED
  const rlimit memory{addressSpace,addressSpace};::setrlimit(RLIMIT_AS,&memory);
#endif
  ::setpriority(PRIO_PROCESS,0,10);
 });
 QElapsedTimer clock;clock.start();
 const auto stopped=[&]{if(cancelled&&cancelled->load()){error=failure("cancelled");return true;}if(clock.elapsed()>=timeoutMs_){error=failure("poster_timeout");return true;}return false;};
 process.start(helperPath_,{"--poster",QString::number(maxPixels)});
 const bool started=process.waitForStarted(std::max(1,int(timeoutMs_-clock.elapsed())));::close(fd);
 if(!started){error=failure("poster_helper_missing");return {};}
 process.closeWriteChannel();
 QByteArray png;
 while(process.state()!=QProcess::NotRunning&&!stopped()){
  process.waitForReadyRead(int(std::clamp<qint64>(timeoutMs_-clock.elapsed(),1,50)));
  png+=process.readAllStandardOutput();if(png.size()>limit){error=failure("poster_protocol");break;}
 }
 if(process.state()!=QProcess::NotRunning){process.kill();process.waitForFinished(1500);}
 if(error)return {};
 png+=process.readAllStandardOutput();
 if(process.exitStatus()!=QProcess::NormalExit||process.exitCode()!=0){error=failure(process.exitStatus()==QProcess::NormalExit&&process.exitCode()==3?"input_excessive":"poster_failed");return {};}
 if(png.size()>limit||imageFormatHint(png)!="png"){error=failure("poster_protocol");return {};}
 // Qt 6.4.2 and 6.8.2 instantiate every installed image plugin for a named
 // format unless detection is off; the PNG signature was checked above.
 QBuffer buffer(&png);buffer.open(QIODevice::ReadOnly);QImageReader reader(&buffer,"png");reader.setAutoDetectImageFormat(false);
 const auto size=reader.size();if(!size.isValid()||size.width()<1||size.height()<1||size.width()>256||size.height()>256){error=failure("poster_protocol");return {};}
 return png;
#endif
}
}
