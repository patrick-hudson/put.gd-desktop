#include "ClipboardKeeper.h"
#include "ClipboardKeeperFormat.h"
#include "core/HostEnvironment.h"
#include "core/Log.h"
#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>
#include <vector>
#if __has_include(<linux/close_range.h>)
#include <linux/close_range.h>
#endif
#ifndef MFD_EXEC
#define MFD_EXEC 0x0010U
#endif
namespace putgd {
namespace {
class Fd {
 int fd_;
public:
 explicit Fd(int fd=-1):fd_(fd){}
 ~Fd(){if(fd_>=0)::close(fd_);}
 Fd(const Fd&)=delete;Fd& operator=(const Fd&)=delete;
 int get()const{return fd_;}
 int release(){const int fd=fd_;fd_=-1;return fd;}
};
bool writeAll(int fd,const void* bytes,size_t size){
 auto* cursor=static_cast<const char*>(bytes);
 while(size>0){
  const auto written=::write(fd,cursor,size);
  if(written<0&&errno==EINTR)continue;
  if(written<=0)return false;
  cursor+=written;size-=size_t(written);
 }
 return true;
}
// An AppImage's mount goes away with the resident, and the image's libraries
// with it. A copy of the keeper in memory, run on the host's libraries, needs
// nothing from the mount. The keeper uses only libc and libwayland-client.
int copyIntoMemory(const QString& path){
 QFile file(path);if(!file.open(QIODevice::ReadOnly))return -1;
 const auto bytes=file.readAll();if(bytes.isEmpty())return -1;
 int fd=::memfd_create("putgd-clipboard-keeper",MFD_CLOEXEC|MFD_EXEC);
 if(fd<0&&errno==EINVAL)fd=::memfd_create("putgd-clipboard-keeper",MFD_CLOEXEC); // Before Linux 6.3.
 if(fd<0)return -1;
 if(!writeAll(fd,bytes.constData(),size_t(bytes.size()))){::close(fd);return -1;}
 ::fchmod(fd,0500);
 return fd;
}
// Only async-signal-safe calls: other threads may hold locks across the fork.
// A byte on started means exec failed; the pipe's close-on-exec end says it worked.
[[noreturn]] void becomeKeeper(int payload,int image,int started,const char* path,char* const argv[],char* const envp[]){
 const auto failed=[started]{const char byte=1;(void)!::write(started,&byte,1);::_exit(127);};
 ::setsid();
 const pid_t daemon=::fork();
 if(daemon<0)::_exit(127);
 if(daemon>0)::_exit(0);
 if(::dup2(payload,STDIN_FILENO)<0||::fcntl(STDIN_FILENO,F_SETFD,0)<0)failed();
 const int null=::open("/dev/null",O_RDWR|O_CLOEXEC);
 if(null<0||::dup2(null,STDOUT_FILENO)<0||::dup2(null,STDERR_FILENO)<0)failed();
 // It outlives this process: no sockets, locks or AppImage mount descriptor.
#if defined(SYS_close_range) && defined(CLOSE_RANGE_CLOEXEC)
 if(::syscall(SYS_close_range,3U,~0U,CLOSE_RANGE_CLOEXEC)!=0)
#endif
 {
  const long maximum=::sysconf(_SC_OPEN_MAX);
  for(int fd=3;fd<maximum;++fd)::fcntl(fd,F_SETFD,FD_CLOEXEC);
 }
 sigset_t none;::sigemptyset(&none);::sigprocmask(SIG_SETMASK,&none,nullptr);
 if(image>=0)::fexecve(image,argv,envp);else ::execve(path,argv,envp);
 failed();
 __builtin_unreachable();
}
}
bool startClipboardKeeper(const QHash<QString,QByteArray>& data,QString helperPath){
 if(helperPath.isEmpty())helperPath=QCoreApplication::applicationDirPath()+QStringLiteral("/putgd-clipboard-keeper");
 if(!QFileInfo(helperPath).isExecutable()||data.isEmpty()||uint(data.size())>PUTGD_KEEPER_MAX_TYPES)return false;
 QByteArray header(PUTGD_KEEPER_MAGIC,PUTGD_KEEPER_MAGIC_BYTES);
 const quint32 count=quint32(data.size());header.append(reinterpret_cast<const char*>(&count),sizeof count);
 std::vector<std::pair<QByteArray,const QByteArray*>> entries;quint64 total=quint64(header.size());
 for(auto it=data.cbegin();it!=data.cend();++it){
  auto name=it.key().toUtf8();name.append('\0');
  total+=2*sizeof(quint32)+sizeof(quint64)+quint64(name.size())+quint64(it.value().size());
  entries.emplace_back(std::move(name),&it.value());
 }
 if(total>PUTGD_KEEPER_MAX_BYTES){logging::info(logClipboard(),"Clipboard result too large to keep after quitting",{{"bytes",qint64(total)}});return false;}
 Fd payload(::memfd_create("putgd-clipboard",MFD_CLOEXEC));
 if(payload.get()<0||!writeAll(payload.get(),header.constData(),size_t(header.size())))return false;
 for(const auto& [name,bytes]:entries){
  const quint32 sizes[2]{quint32(name.size()),0};const quint64 dataBytes=quint64(bytes->size());
  if(!writeAll(payload.get(),sizes,sizeof sizes)||!writeAll(payload.get(),&dataBytes,sizeof dataBytes)
   ||!writeAll(payload.get(),name.constData(),size_t(name.size()))||!writeAll(payload.get(),bytes->constData(),size_t(bytes->size())))return false;
 }
 const bool appImage=host::installationClass()==QStringLiteral("appimage");
 Fd image(appImage?copyIntoMemory(helperPath):-1);
 if(appImage&&image.get()<0)return false;
 const auto program=helperPath.toLocal8Bit();
 char* const argv[]{const_cast<char*>("putgd-clipboard-keeper"),nullptr};
 std::vector<QByteArray> variables;for(const auto& variable:host::environment().toStringList())variables.push_back(variable.toLocal8Bit());
 std::vector<char*> envp;for(auto& variable:variables)envp.push_back(variable.data());envp.push_back(nullptr);
 int pipes[2];if(::pipe2(pipes,O_CLOEXEC)<0)return false;
 Fd readEnd(pipes[0]),writeEnd(pipes[1]);
 const pid_t child=::fork();
 if(child<0)return false;
 if(child==0)becomeKeeper(payload.get(),image.get(),writeEnd.get(),program.constData(),argv,envp.data());
 ::close(writeEnd.release());
 int status=0;while(::waitpid(child,&status,0)<0&&errno==EINTR){}
 if(!WIFEXITED(status)||WEXITSTATUS(status)!=0)return false;
 char failed=0;ssize_t got;do got=::read(readEnd.get(),&failed,1);while(got<0&&errno==EINTR);
 return got==0;
}
}
