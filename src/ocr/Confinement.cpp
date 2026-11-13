#include "Confinement.h"
#include <QByteArray>
#include <QFile>
#include <cerrno>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <pwd.h>
#include <cstdlib>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sched.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <vector>

#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define PUTGD_OCR_SANITIZED 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) || __has_feature(memory_sanitizer)
#define PUTGD_OCR_SANITIZED 1
#endif
#endif

#if !defined(__x86_64__)
#error "putgd-ocr's seccomp filter is written for x86_64, the only packaged architecture"
#endif

namespace putgd::ocr {
namespace {
// Landlock, from the kernel's UAPI, spelled out so the build does not depend
// on how new the build host's kernel headers are.
#ifndef SYS_landlock_create_ruleset
#define SYS_landlock_create_ruleset 444
#define SYS_landlock_add_rule 445
#define SYS_landlock_restrict_self 446
#endif
constexpr uint32_t createRulesetVersion=1u<<0;
constexpr int rulePathBeneath=1;
constexpr uint64_t fsReadFile=1ull<<2,fsReadDir=1ull<<3;
// Every access right of ABI 1 (execute, write, read, directory changes, special files).
constexpr uint64_t fsAbi1=(1ull<<13)-1,fsRefer=1ull<<13,fsTruncate=1ull<<14,fsIoctlDev=1ull<<15;
constexpr uint64_t netBindTcp=1ull<<0,netConnectTcp=1ull<<1;
constexpr uint64_t scopeAbstractUnixSocket=1ull<<0,scopeSignal=1ull<<1;
struct RulesetAttr { uint64_t handledAccessFs=0,handledAccessNet=0,scoped=0; };
struct __attribute__((packed)) PathBeneathAttr { uint64_t allowedAccess=0;int32_t parentFd=-1; };

bool landlock(const QStringList& readable,int abi){
 if(abi<3)return false;
 RulesetAttr attr;
 attr.handledAccessFs=fsAbi1|fsRefer|fsTruncate|(abi>=5?fsIoctlDev:0);
 size_t size=sizeof(uint64_t);
 if(abi>=4){attr.handledAccessNet=netBindTcp|netConnectTcp;size=2*sizeof(uint64_t);}
 if(abi>=6){attr.scoped=scopeAbstractUnixSocket|scopeSignal;size=3*sizeof(uint64_t);}
 const int ruleset=int(::syscall(SYS_landlock_create_ruleset,&attr,size,0));
 if(ruleset<0)return false;
 bool ok=true;
 for(const auto& path:readable){
  const int fd=::open(QFile::encodeName(path).constData(),O_PATH|O_CLOEXEC);
  if(fd<0)continue;  // a directory this system does not have
  struct stat s{};
  if(::fstat(fd,&s)==0&&(S_ISDIR(s.st_mode)||S_ISREG(s.st_mode))){
   PathBeneathAttr rule;rule.allowedAccess=S_ISDIR(s.st_mode)?(fsReadFile|fsReadDir):fsReadFile;rule.parentFd=fd;
   if(::syscall(SYS_landlock_add_rule,ruleset,rulePathBeneath,&rule,0)!=0)ok=false;
  }
  ::close(fd);
 }
 ok=ok&&::syscall(SYS_landlock_restrict_self,ruleset,0)==0;
 ::close(ruleset);
 return ok;
}

#ifndef SYS_pidfd_getfd
#define SYS_pidfd_getfd 438
#endif
#ifndef SYS_io_uring_setup
#define SYS_io_uring_setup 425
#define SYS_io_uring_enter 426
#define SYS_io_uring_register 427
#endif
#ifndef SYS_pidfd_send_signal
#define SYS_pidfd_send_signal 424
#endif
#ifndef SYS_clone3
#define SYS_clone3 435
#endif
constexpr uint32_t x32Bit=0x40000000;
constexpr uint32_t namespaceFlags=CLONE_NEWNS|CLONE_NEWCGROUP|CLONE_NEWUTS|CLONE_NEWIPC|CLONE_NEWUSER|CLONE_NEWPID|CLONE_NEWNET;
bool seccomp(){
 std::vector<sock_filter> f;
 const auto stmt=[&](uint16_t code,uint32_t k){f.push_back(sock_filter{code,0,0,k});};
 const auto jump=[&](uint16_t code,uint32_t k,uint8_t jt,uint8_t jf){f.push_back(sock_filter{code,jt,jf,k});};
 const auto rule=[&](long nr,uint32_t action){jump(BPF_JMP|BPF_JEQ|BPF_K,uint32_t(nr),0,1);stmt(BPF_RET|BPF_K,action);};
 constexpr uint32_t kill=SECCOMP_RET_KILL_PROCESS,allow=SECCOMP_RET_ALLOW;
 // Only x86_64 system calls; x32 numbers are rejected.
 stmt(BPF_LD|BPF_W|BPF_ABS,offsetof(seccomp_data,arch));
 jump(BPF_JMP|BPF_JEQ|BPF_K,AUDIT_ARCH_X86_64,1,0);
 stmt(BPF_RET|BPF_K,kill);
 stmt(BPF_LD|BPF_W|BPF_ABS,offsetof(seccomp_data,nr));
 jump(BPF_JMP|BPF_JGE|BPF_K,x32Bit,0,1);
 stmt(BPF_RET|BPF_K,kill);
 rule(SYS_socket,SECCOMP_RET_ERRNO|(EPERM&SECCOMP_RET_DATA));
 rule(SYS_connect,SECCOMP_RET_ERRNO|(EPERM&SECCOMP_RET_DATA));
 // glibc falls back to clone, whose flags a filter can read.
 rule(SYS_clone3,SECCOMP_RET_ERRNO|(ENOSYS&SECCOMP_RET_DATA));
 for(const long nr:{long(SYS_execve),long(SYS_execveat),long(SYS_process_vm_readv),long(SYS_process_vm_writev),long(SYS_pidfd_getfd),
                    long(SYS_io_uring_setup),long(SYS_io_uring_enter),long(SYS_io_uring_register),long(SYS_bpf),long(SYS_perf_event_open),
                    long(SYS_userfaultfd),long(SYS_keyctl),long(SYS_unshare),long(SYS_setns),long(SYS_mount),long(SYS_pivot_root),
                    long(SYS_chroot),long(SYS_fork),long(SYS_vfork)})rule(nr,kill);
#ifndef PUTGD_OCR_SANITIZED
 rule(SYS_ptrace,kill);
#endif
 // Signals reach only this process's own thread group: below Landlock ABI 6
 // there is no signal scope, and a helper could otherwise kill its parent.
 // kill, tgkill and the two queueinfo calls name the target process first;
 // tkill and pidfd_send_signal name a thread or a descriptor a filter cannot
 // check, and glibc's raise and pthread_kill use tgkill.
 const uint32_t self=uint32_t(::getpid());
 const uint32_t denied=SECCOMP_RET_ERRNO|(EPERM&SECCOMP_RET_DATA);
 for(const long nr:{long(SYS_kill),long(SYS_tgkill),long(SYS_rt_sigqueueinfo),long(SYS_rt_tgsigqueueinfo)}){
  jump(BPF_JMP|BPF_JEQ|BPF_K,uint32_t(nr),0,4);
  stmt(BPF_LD|BPF_W|BPF_ABS,offsetof(seccomp_data,args));  // the target, a pid_t in the low 32 bits
  jump(BPF_JMP|BPF_JEQ|BPF_K,self,0,1);
  stmt(BPF_RET|BPF_K,allow);
  stmt(BPF_RET|BPF_K,denied);
 }
 rule(SYS_tkill,denied);
 rule(SYS_pidfd_send_signal,denied);
 // clone: a thread (CLONE_THREAD, which the kernel only accepts with a shared
 // address space and signal handlers) and no new namespace. Anything else
 // would be a process.
 jump(BPF_JMP|BPF_JEQ|BPF_K,uint32_t(SYS_clone),1,0);
 stmt(BPF_RET|BPF_K,allow);
 stmt(BPF_LD|BPF_W|BPF_ABS,offsetof(seccomp_data,args));  // the low 32 bits of the flags, all clone reads
 jump(BPF_JMP|BPF_JSET|BPF_K,CLONE_THREAD,1,0);
 stmt(BPF_RET|BPF_K,kill);
 jump(BPF_JMP|BPF_JSET|BPF_K,namespaceFlags,0,1);
 stmt(BPF_RET|BPF_K,kill);
 stmt(BPF_RET|BPF_K,allow);
 sock_fprog program{uint16_t(f.size()),f.data()};
 return ::syscall(SYS_seccomp,SECCOMP_SET_MODE_FILTER,SECCOMP_FILTER_FLAG_TSYNC,&program)==0;
}
QString executablePrefix(){
 char buffer[PATH_MAX+1]{};const auto n=::readlink("/proc/self/exe",buffer,PATH_MAX);
 if(n<=0)return {};
 QString path=QFile::decodeName(QByteArray(buffer,int(n)));
 for(int i=0;i<2;++i){const auto slash=path.lastIndexOf(QLatin1Char('/'));if(slash<=0)return {};path.truncate(slash);}
 return path;
}
// The fully resolved path, or empty when it does not exist.
QString resolvedPath(const QString& path){
 char buffer[PATH_MAX+1]{};
 if(!::realpath(QFile::encodeName(path).constData(),buffer))return {};
 return QFile::decodeName(buffer);
}
QString trimmed(QString path){while(path.size()>1&&path.endsWith(QLatin1Char('/')))path.chop(1);return path;}
// The user's home directory as written in the password database, and resolved.
struct Home { QString written,real; };
Home homeDirectory(){
 Home home;
 if(const auto* user=::getpwuid(::getuid());user&&user->pw_dir&&*user->pw_dir){
  home.written=trimmed(QFile::decodeName(user->pw_dir));home.real=trimmed(resolvedPath(home.written));
 }
 return home;
}
// A grant of r would reach / or the whole home directory.
bool coversHome(const QString& r,const Home& home){
 return r==QLatin1String("/")||(!home.real.isEmpty()&&(r==home.real||home.real.startsWith(r+QLatin1Char('/'))));
}
}
// Landlock grants the directory a rule's path resolves to, symlinks followed,
// so every path a rule names is resolved here first and refused when that
// lands somewhere it must not: /, the home directory or above it.
static QStringList prefixDirectories(){
 // The libraries (the AppImage's bundled engines) and put.gd's own data under
 // the helper's installation prefix. Never the prefix itself, whose tree may
 // be a home directory: a helper run from /home/alice/bin would otherwise read
 // all of /home/alice. Each directory must resolve to exactly itself under the
 // real prefix: a symlinked lib pointing at the home directory grants nothing.
 const auto home=homeDirectory();
 const auto prefix=resolvedPath(executablePrefix());
 if(prefix.isEmpty()||coversHome(prefix,home))return {};
 QStringList out;
 for(const auto* sub:{"lib","lib64","share/putgd"}){
  const auto candidate=prefix+QLatin1Char('/')+QLatin1String(sub);
  const auto real=resolvedPath(candidate);
  if(real.isEmpty()||real!=candidate||coversHome(real,home))continue;
  out.append(real);
 }
 return out;
}
QStringList safeLanguageDirectories(const QStringList& directories){
 // Absolute, no "..", resolved. One that resolves into the home directory must
 // get there without a symlink below the home directory, so a user tessdata
 // that is a symlink to somewhere else grants nothing; a home directory that
 // is itself reached through a symlink (/home -> /var/home) is fine.
 const auto home=homeDirectory();QStringList out;
 for(const auto& d:directories){
  if(!d.startsWith(QLatin1Char('/'))||d.split(QLatin1Char('/')).contains(QLatin1String("..")))continue;
  const auto real=resolvedPath(d);
  if(real.isEmpty()||coversHome(real,home)||out.contains(real))continue;
  if(!home.real.isEmpty()&&real.startsWith(home.real+QLatin1Char('/'))){
   const auto path=trimmed(d);QString below;
   if(!home.written.isEmpty()&&path.startsWith(home.written+QLatin1Char('/')))below=path.mid(home.written.size());
   else if(path.startsWith(home.real+QLatin1Char('/')))below=path.mid(home.real.size());
   else continue;  // into the home directory from outside it
   if(real!=home.real+below)continue;
  }
  out.append(real);
 }
 return out;
}
int landlockAbi(){
 const long abi=::syscall(SYS_landlock_create_ruleset,nullptr,0,createRulesetVersion);
 return abi>0?int(abi):0;
}
bool sanitizedBuild(){
#ifdef PUTGD_OCR_SANITIZED
 return true;
#else
 return false;
#endif
}
QStringList readableDirectories(const QStringList& languageDirectories){
 QStringList out{QStringLiteral("/usr/lib"),QStringLiteral("/usr/lib64"),QStringLiteral("/lib"),QStringLiteral("/lib64"),
                 QStringLiteral("/proc/self"),QStringLiteral("/sys/devices/system/cpu")};
 for(const auto& d:prefixDirectories())if(!out.contains(d))out.append(d);
 for(const auto& d:safeLanguageDirectories(languageDirectories))if(!out.contains(d))out.append(d);
 return out;
}
bool confine(const Confinement& c,QString& failed,int& abi){
 failed.clear();abi=landlockAbi();
 if(::prctl(PR_SET_NO_NEW_PRIVS,1,0,0,0)!=0){failed=QStringLiteral("no_new_privs");return false;}
#ifndef PUTGD_OCR_SANITIZED
 if(::prctl(PR_SET_DUMPABLE,0,0,0,0)!=0){failed=QStringLiteral("dumpable");return false;}
#endif
 const rlimit none{0,0},cpu{rlim_t(c.cpuSeconds),rlim_t(c.cpuSeconds+1)},files{rlim_t(c.descriptors),rlim_t(c.descriptors)},size{rlim_t(c.fileBytes),rlim_t(c.fileBytes)};
 if(::setrlimit(RLIMIT_CORE,&none)||(c.cpuSeconds>0&&::setrlimit(RLIMIT_CPU,&cpu))||(c.descriptors>0&&::setrlimit(RLIMIT_NOFILE,&files))||(c.fileBytes>0&&::setrlimit(RLIMIT_FSIZE,&size))){failed=QStringLiteral("rlimit");return false;}
#ifndef PUTGD_OCR_SANITIZED
 if(c.dataBytes>0){const rlimit data{rlim_t(c.dataBytes),rlim_t(c.dataBytes)};if(::setrlimit(RLIMIT_DATA,&data)){failed=QStringLiteral("rlimit");return false;}}
#endif
 if(!landlock(c.readable,abi)){failed=QStringLiteral("landlock");return false;}
 if(!seccomp()){failed=QStringLiteral("seccomp");return false;}
 return true;
}
bool lowerDataLimit(qint64 bytes){
#ifdef PUTGD_OCR_SANITIZED
 Q_UNUSED(bytes);return true;
#else
 rlimit current{};if(::getrlimit(RLIMIT_DATA,&current))return false;
 if(current.rlim_cur!=RLIM_INFINITY&&rlim_t(bytes)>=current.rlim_cur)return true;
 const rlimit data{rlim_t(bytes),rlim_t(bytes)};return ::setrlimit(RLIMIT_DATA,&data)==0;
#endif
}
}
