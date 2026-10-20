#include "CrashHandler.h"
#include "BuildInfo.h"
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <dlfcn.h>
#include <elf.h>
#include <fcntl.h>
#include <link.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>
#include <unwind.h>
// The sanitizers own the fatal signals and print better reports.
#if defined(__SANITIZE_ADDRESS__)||defined(__SANITIZE_THREAD__)||defined(PUTGD_CRASH_SANITIZED)
#define PUTGD_CRASH_DISABLED 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)||__has_feature(thread_sanitizer)
#define PUTGD_CRASH_DISABLED 1
#endif
#endif
namespace putgd::crash {
#ifndef PUTGD_CRASH_DISABLED
namespace {
constexpr int signals[]={SIGSEGV,SIGBUS,SIGILL,SIGFPE,SIGABRT};
constexpr int maximumFrames=128,maximumModules=512,poolBytes=96*1024,maximumBuildId=32;
// Everything the handler reads is static: nothing is allocated while crashing.
struct Module {std::uintptr_t start=0,end=0,bias=0;std::uint32_t path=0;std::uint8_t buildIdBytes=0;std::uint8_t buildId[maximumBuildId]{};};
struct ModuleMap {Module modules[maximumModules];int count=0;char pool[poolBytes];std::uint32_t used=0;};
// Two maps: a refresh fills the inactive one, then publishes it.
ModuleMap maps[2];
std::atomic<int> activeMap{-1};
std::mutex refreshMutex;
std::atomic<int> output{STDERR_FILENO};
std::atomic<bool> enabled{false};
std::atomic<int> handling{0}; // 0 idle, 1 writing, 2 written
struct sigaction previous[NSIG];
char processName[64]="putgd";
char versionText[128]="";
std::uintptr_t pageSize=4096;
constexpr std::size_t alternateStackBytes=64*1024;
alignas(16) char alternateStack[alternateStackBytes];

// ---- Async-signal-safe output -------------------------------------------
struct Line {
 char text[640];std::size_t length=0;
 Line& add(const char* value) {while(*value&&length<sizeof text-1)text[length++]=*value++;return *this;}
 Line& add(const char* value,std::size_t count) {for(std::size_t i=0;i<count&&value[i]&&length<sizeof text-1;++i)text[length++]=value[i];return *this;}
 Line& tab() {return add("\t");}
 Line& decimal(long long value) {
  char digits[24];int count=0;const bool negative=value<0;unsigned long long magnitude=negative?0ULL-static_cast<unsigned long long>(value):static_cast<unsigned long long>(value);
  do{digits[count++]=char('0'+magnitude%10);magnitude/=10;}while(magnitude&&count<23);
  if(negative)add("-");
  while(count>0&&length<sizeof text-1)text[length++]=digits[--count];
  return *this;
 }
 Line& hex(std::uintptr_t value) {
  add("0x");char digits[2*sizeof value];int count=0;
  do{digits[count++]="0123456789abcdef"[value&15];value>>=4;}while(value&&count<int(sizeof digits));
  while(count>0&&length<sizeof text-1)text[length++]=digits[--count];
  return *this;
 }
 Line& bytes(const std::uint8_t* value,int count) {for(int i=0;i<count&&length+2<sizeof text;++i){text[length++]="0123456789abcdef"[value[i]>>4];text[length++]="0123456789abcdef"[value[i]&15];}return *this;}
 void write(int fd) {
  text[length++]='\n';
  for(std::size_t done=0;done<length;) {
   const auto written=::write(fd,text+done,length-done);
   if(written>0)done+=std::size_t(written);else if(written<0&&errno==EINTR)continue;else break;
  }
  length=0;
 }
};
Line& begin(Line& line,const char* kind) {return line.add(linePrefix).add(kind);}
const char* signalName(int signal) {
 switch(signal){case SIGSEGV:return "SIGSEGV";case SIGBUS:return "SIGBUS";case SIGILL:return "SIGILL";case SIGFPE:return "SIGFPE";case SIGABRT:return "SIGABRT";default:return "signal";}
}

// ---- Modules --------------------------------------------------------------
// The GNU build ID from a module's PT_NOTE segments, read from memory.
int buildId(std::uintptr_t bias,const ElfW(Phdr)* headers,int count,std::uint8_t* out) {
 for(int i=0;i<count;++i) {
  if(headers[i].p_type!=PT_NOTE)continue;
  const std::uintptr_t align=headers[i].p_align==8?8:4;
  auto at=bias+headers[i].p_vaddr;const auto end=at+headers[i].p_memsz;
  while(at+sizeof(ElfW(Nhdr))<=end) {
   const auto* note=reinterpret_cast<const ElfW(Nhdr)*>(at);
   const auto name=at+sizeof(ElfW(Nhdr)),desc=name+((note->n_namesz+align-1)&~(align-1)),next=desc+((note->n_descsz+align-1)&~(align-1));
   if(next>end||next<=at)break;
   if(note->n_type==NT_GNU_BUILD_ID&&note->n_namesz==4&&std::memcmp(reinterpret_cast<const char*>(name),"GNU",4)==0) {
    const int bytes=note->n_descsz<maximumBuildId?int(note->n_descsz):maximumBuildId;
    std::memcpy(out,reinterpret_cast<const void*>(desc),std::size_t(bytes));return bytes;
   }
   at=next;
  }
 }
 return 0;
}
void describe(Module& module,std::uintptr_t bias,const ElfW(Phdr)* headers,int count) {
 module.bias=bias;module.start=~std::uintptr_t(0);module.end=0;
 for(int i=0;i<count;++i)if(headers[i].p_type==PT_LOAD) {
  const auto start=bias+headers[i].p_vaddr,end=start+headers[i].p_memsz;
  if(start<module.start)module.start=start;
  if(end>module.end)module.end=end;
 }
 if(module.end==0)module.start=0;
 module.buildIdBytes=std::uint8_t(buildId(bias,headers,count,module.buildId));
}
int collect(dl_phdr_info* info,std::size_t,void* data) {
 auto& map=*static_cast<ModuleMap*>(data);
 if(map.count>=maximumModules)return 1;
 auto& module=map.modules[map.count];module=Module{};
 describe(module,info->dlpi_addr,info->dlpi_phdr,info->dlpi_phnum);
 if(module.end==0)return 0;
 char executable[512];const char* path=info->dlpi_name;
 if(!path||!*path) {
  // The main program has no name here; the kernel knows its file.
  const auto length=::readlink("/proc/self/exe",executable,sizeof executable-1);
  executable[length>0?length:0]=0;path=length>0?executable:"<main>";
 }
 const auto length=std::strlen(path);
 if(map.used+length+1>poolBytes)return 1;
 module.path=map.used;std::memcpy(map.pool+map.used,path,length+1);map.used+=std::uint32_t(length+1);
 ++map.count;
 return 0;
}
const Module* findModule(const ModuleMap& map,std::uintptr_t address) {
 for(int i=0;i<map.count;++i)if(address>=map.modules[i].start&&address<map.modules[i].end)return &map.modules[i];
 return nullptr;
}
// A frame outside the collected map (a library loaded later): find its file in
// /proc/self/maps and read the build ID from the ELF headers it mapped.
struct Fallback {Module module;char path[256];bool found=false;};
bool hexValue(const char*& at,const char* end,std::uintptr_t& value) {
 value=0;bool any=false;
 for(;at<end;++at) {const char c=*at;int digit=c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:-1;if(digit<0)break;value=value*16+std::uintptr_t(digit);any=true;}
 return any;
}
void lookupMapped(std::uintptr_t address,Fallback& result) {
 result.found=false;
 const int fd=::open("/proc/self/maps",O_RDONLY|O_CLOEXEC);
 if(fd<0)return;
 char buffer[4096];char line[512];std::size_t lineLength=0;
 std::uintptr_t lastBase=0;char lastPath[256]="";
 bool done=false;
 while(!done) {
  const auto count=::read(fd,buffer,sizeof buffer);
  if(count<0&&errno==EINTR)continue;
  if(count<=0)break;
  for(ssize_t i=0;i<count&&!done;++i) {
   if(buffer[i]!='\n') {if(lineLength<sizeof line-1)line[lineLength++]=buffer[i];continue;}
   line[lineLength]=0;
   // start-end perms offset dev inode path
   const char* at=line;const char* end=line+lineLength;std::uintptr_t start=0,stop=0,offset=0;
   bool ok=hexValue(at,end,start)&&at<end&&*at=='-';
   if(ok){++at;ok=hexValue(at,end,stop);}
   while(ok&&at<end&&*at==' ')++at;
   while(ok&&at<end&&*at!=' ')++at; // permissions
   while(ok&&at<end&&*at==' ')++at;
   if(ok)ok=hexValue(at,end,offset);
   for(int field=0;ok&&field<2;++field){while(at<end&&*at==' ')++at;while(at<end&&*at!=' ')++at;}
   while(ok&&at<end&&*at==' ')++at;
   const char* path=at;
   if(ok&&*path=='/') {
    if(offset==0||std::strcmp(path,lastPath)!=0) {
     if(offset==0){lastBase=start;std::strncpy(lastPath,path,sizeof lastPath-1);lastPath[sizeof lastPath-1]=0;}
     else lastPath[0]=0;
    }
    if(address>=start&&address<stop&&lastPath[0]) {
     const auto* header=reinterpret_cast<const ElfW(Ehdr)*>(lastBase);
     if(std::memcmp(header->e_ident,ELFMAG,SELFMAG)==0&&header->e_phentsize==sizeof(ElfW(Phdr))) {
      const auto* headers=reinterpret_cast<const ElfW(Phdr)*>(lastBase+header->e_phoff);
      std::uintptr_t lowest=~std::uintptr_t(0);
      for(int h=0;h<header->e_phnum;++h)if(headers[h].p_type==PT_LOAD&&headers[h].p_vaddr<lowest)lowest=headers[h].p_vaddr;
      const std::uintptr_t page=pageSize;
      if(lowest!=~std::uintptr_t(0)) {
       describe(result.module,lastBase-(lowest&~(page-1)),headers,header->e_phnum);
       std::strncpy(result.path,lastPath,sizeof result.path-1);result.path[sizeof result.path-1]=0;
       result.found=address>=result.module.start&&address<result.module.end;
      }
     }
     done=true;
    }
   }
   lineLength=0;
  }
 }
 ::close(fd);
}

// ---- The handler -------------------------------------------------------------
struct Frames {std::uintptr_t address[maximumFrames];int count=0;};
_Unwind_Reason_Code unwind(_Unwind_Context* context,void* data) {
 auto& frames=*static_cast<Frames*>(data);
 if(frames.count>=maximumFrames)return _URC_END_OF_STACK;
 int beforeInstruction=0;const auto ip=_Unwind_GetIPInfo(context,&beforeInstruction);
 if(ip==0)return _URC_END_OF_STACK;
 frames.address[frames.count++]=ip;
 return _URC_NO_REASON;
}
std::uintptr_t programCounter(void* context) {
 if(!context)return 0;
 const auto* uc=static_cast<const ucontext_t*>(context);
#if defined(__x86_64__)
 return std::uintptr_t(uc->uc_mcontext.gregs[REG_RIP]);
#elif defined(__i386__)
 return std::uintptr_t(uc->uc_mcontext.gregs[REG_EIP]);
#elif defined(__aarch64__)
 return std::uintptr_t(uc->uc_mcontext.pc);
#else
 (void)uc;return 0;
#endif
}
void writeRecord(int signal,siginfo_t* info,void* context) {
 const int fd=output.load();Line line;
 timespec now{};::clock_gettime(CLOCK_REALTIME,&now);
 begin(line,"begin").tab().decimal(recordVersion).tab().add(processName).tab().decimal(::getpid()).tab().decimal(long(::syscall(SYS_gettid))).write(fd);
 begin(line,"version").tab().add(versionText).write(fd);
 begin(line,"signal").tab().decimal(signal).tab().add(signalName(signal)).tab().decimal(info?info->si_code:0).tab().hex(info&&info->si_code>0?std::uintptr_t(info->si_addr):0).write(fd); // a sent signal has no fault address
 begin(line,"time").tab().decimal(now.tv_sec).write(fd);
 const auto pc=programCounter(context);
 begin(line,"pc").tab().hex(pc).write(fd);
 static Frames frames;frames.count=0;
 _Unwind_Backtrace(unwind,&frames);
 // Drop the handler's own frames: the walk reaches the interrupted code
 // through the signal frame, whose first address is the faulting PC.
 int first=0;
 for(int i=0;i<frames.count;++i)if(frames.address[i]==pc){first=i;break;}
 if(pc&&(frames.count==0||frames.address[first]!=pc)) {
  // The unwinder could not cross the signal frame; keep the PC at least.
  for(int i=frames.count<maximumFrames?frames.count:maximumFrames-1;i>0;--i)frames.address[i]=frames.address[i-1];
  frames.address[0]=pc;first=0;if(frames.count<maximumFrames)++frames.count;
 }
 const int mapIndex=activeMap.load();const ModuleMap* map=mapIndex>=0?&maps[mapIndex]:nullptr;
 // The modules the frames fall in, in first-seen order.
 static const Module* used[64];static Fallback fallbacks[16];int usedCount=0,fallbackCount=0;
 for(int i=first;i<frames.count;++i) {
  const auto address=frames.address[i];const Module* module=map?findModule(*map,address):nullptr;
  for(int f=0;!module&&f<fallbackCount;++f)if(address>=fallbacks[f].module.start&&address<fallbacks[f].module.end)module=&fallbacks[f].module;
  if(!module&&fallbackCount<16) {lookupMapped(address,fallbacks[fallbackCount]);if(fallbacks[fallbackCount].found)module=&fallbacks[fallbackCount++].module;}
  int index=-1;
  for(int u=0;module&&u<usedCount;++u)if(used[u]==module)index=u;
  if(module&&index<0&&usedCount<64){used[usedCount]=module;index=usedCount++;}
  begin(line,"frame").tab().decimal(i-first).tab().hex(address).tab();
  if(index>=0)line.decimal(index).tab().hex(address-used[index]->bias);else line.add("-\t-");
  line.write(fd);
 }
 for(int u=0;u<usedCount;++u) {
  const auto* module=used[u];const char* path="<unknown>";
  if(map&&module>=map->modules&&module<map->modules+maximumModules)path=map->pool+module->path;
  for(int f=0;f<fallbackCount;++f)if(module==&fallbacks[f].module)path=fallbacks[f].path;
  begin(line,"module").tab().decimal(u).tab().hex(module->bias).tab();
  if(module->buildIdBytes)line.bytes(module->buildId,module->buildIdBytes);else line.add("-");
  line.tab().add(path).write(fd);
 }
 begin(line,"end").tab().decimal(frames.count-first).write(fd);
}
void handler(int signal,siginfo_t* info,void* context) {
 const int saved=errno;
 int expected=0;
 if(handling.compare_exchange_strong(expected,1)) {writeRecord(signal,info,context);handling.store(2);}
 else {
  // Another thread is writing its record; give it a moment before this
  // signal's default action ends the process.
  for(int i=0;i<200&&handling.load()==1;++i){timespec pause{0,10*1000*1000};::nanosleep(&pause,nullptr);}
 }
 ::sigaction(signal,&previous[signal],nullptr);
 // A fault re-executes the instruction and faults again under the restored
 // disposition; a sent signal (abort, kill) has to be raised again.
 if(!info||info->si_code<=0||signal==SIGABRT)::raise(signal);
 errno=saved;
}
// ---- Alternate stacks for the other threads (#156) ------------------------
// sigaltstack is per thread and a new thread starts without one, so a stack
// overflow on a worker left the handler no stack to run on. pthread_create
// below (exported from every executable that links this library) wraps each
// thread's start routine, and the thread maps its own alternate stack, with a
// guard page below it, before running it. QThread, std::thread and the helper
// threads of Qt, GStreamer or the AWS SDK all start there. The pages stay
// untouched unless the thread crashes; the mapping goes when the thread ends.
using CreateThread=int(*)(pthread_t*,const pthread_attr_t*,void*(*)(void*),void*);
std::atomic<CreateThread> nextCreateThread{nullptr};
struct ThreadStart {void*(*routine)(void*);void* argument;};
struct ThreadStack {
 void* mapping=nullptr;std::size_t bytes=0;
 ~ThreadStack() {
  if(!mapping)return;
  stack_t disabled{};disabled.ss_flags=SS_DISABLE;::sigaltstack(&disabled,nullptr);
  ::munmap(mapping,bytes);
 }
};
thread_local ThreadStack threadStack;
void giveThreadAStack() {
 stack_t existing{};
 if(!enabled.load()||::sigaltstack(nullptr,&existing)!=0||!(existing.ss_flags&SS_DISABLE))return;
 const std::size_t bytes=pageSize+alternateStackBytes;
 void* mapping=::mmap(nullptr,bytes,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE|MAP_STACK,-1,0);
 if(mapping==MAP_FAILED)return;
 stack_t stack{};stack.ss_sp=static_cast<char*>(mapping)+pageSize;stack.ss_size=alternateStackBytes;
 if(::mprotect(mapping,pageSize,PROT_NONE)!=0||::sigaltstack(&stack,nullptr)!=0){::munmap(mapping,bytes);return;}
 threadStack.mapping=mapping;threadStack.bytes=bytes;
}
void* startWithStack(void* boxed) {
 const ThreadStart start=*static_cast<ThreadStart*>(boxed);std::free(boxed);
 giveThreadAStack();
 return start.routine(start.argument);
}
void warmUp() {
 // Load and bind the unwinder now, so the handler never resolves a symbol.
 Frames frames;_Unwind_Backtrace(unwind,&frames);
}
#ifdef PUTGD_CRASH_TRIGGER
volatile std::uintptr_t nowhere=0;
volatile int zero=0;
char triggerKind[16]="";
pthread_t mainThread;
void onTrigger(int) {crashNow(triggerKind);}
#endif
bool environmentDisabled() {const char* value=std::getenv("PUTGD_CRASH_HANDLER");return value&&std::strcmp(value,"0")==0;}
void install() {
 if(environmentDisabled())return;
 if(const auto page=::sysconf(_SC_PAGESIZE);page>0)pageSize=std::uintptr_t(page);
 if(program_invocation_short_name&&*program_invocation_short_name){std::strncpy(processName,program_invocation_short_name,sizeof processName-1);processName[sizeof processName-1]=0;}
 std::strncpy(versionText,putgd::buildVersion(),sizeof versionText-1);
 refreshModules();
 warmUp();
 stack_t existing{};
 if(::sigaltstack(nullptr,&existing)==0&&(existing.ss_flags&SS_DISABLE)) {
  stack_t stack{};stack.ss_sp=alternateStack;stack.ss_size=alternateStackBytes;stack.ss_flags=0;::sigaltstack(&stack,nullptr);
 }
 struct sigaction action{};action.sa_sigaction=handler;action.sa_flags=SA_SIGINFO|SA_ONSTACK;
 sigemptyset(&action.sa_mask);for(const int signal:signals)sigaddset(&action.sa_mask,signal);
 for(const int signal:signals) {
  // Keep an ignored signal ignored; anything else is chained to afterwards.
  if(::sigaction(signal,nullptr,&previous[signal])==0&&previous[signal].sa_handler!=SIG_IGN)::sigaction(signal,&action,nullptr);
 }
 enabled.store(true);
#ifdef PUTGD_CRASH_TRIGGER
 if(const char* request=std::getenv("PUTGD_TEST_CRASH")) {
  // <process>:<kind>[:<delay ms>[:worker]]
  const char* colon=std::strchr(request,':');
  if(colon&&std::size_t(colon-request)==std::strlen(processName)&&std::strncmp(request,processName,std::size_t(colon-request))==0) {
   const char* kind=colon+1;const char* delayText=std::strchr(kind,':');
   const auto kindLength=std::size_t((delayText?delayText:kind+std::strlen(kind))-kind);
   std::strncpy(triggerKind,kind,kindLength<sizeof triggerKind-1?kindLength:sizeof triggerKind-1);
   const long delay=delayText?std::strtol(delayText+1,nullptr,10):0;
   const char* threadText=delayText?std::strchr(delayText+1,':'):nullptr;
   if(threadText&&std::strcmp(threadText+1,"worker")==0) {
    // On a thread of its own, started like any other worker.
    std::thread([delay]{timespec pause{delay/1000,(delay%1000)*1000*1000};::nanosleep(&pause,nullptr);crashNow(triggerKind);}).detach();
    return;
   }
   if(delay<=0)crashNow(triggerKind);
   // Crash on the main thread's stack, wherever it is, after the delay.
   mainThread=::pthread_self();
   struct sigaction trigger{};trigger.sa_handler=onTrigger;sigemptyset(&trigger.sa_mask);::sigaction(SIGRTMIN+5,&trigger,nullptr);
   std::thread([delay]{timespec pause{delay/1000,(delay%1000)*1000*1000};::nanosleep(&pause,nullptr);::pthread_kill(mainThread,SIGRTMIN+5);}).detach();
  }
 }
#endif
}
// Installed before main in every executable that links the object library.
[[gnu::constructor]] void installAtStartup() {install();}
}
}
extern "C" int pthread_create(pthread_t* thread,const pthread_attr_t* attributes,void*(*routine)(void*),void* argument) {
 using namespace putgd::crash;
 auto create=nextCreateThread.load();
 if(!create){create=reinterpret_cast<CreateThread>(::dlsym(RTLD_NEXT,"pthread_create"));nextCreateThread.store(create);}
 if(!create)return EAGAIN;
 auto* start=enabled.load()?static_cast<ThreadStart*>(std::malloc(sizeof(ThreadStart))):nullptr;
 if(!start)return create(thread,attributes,routine,argument);
 *start={routine,argument};
 const int result=create(thread,attributes,startWithStack,start);
 if(result!=0)std::free(start);
 return result;
}
namespace putgd::crash {
bool installed() {return enabled.load();}
void setOutput(int fd) {output.store(fd>=0?fd:STDERR_FILENO);}
void refreshModules() {
 std::lock_guard lock(refreshMutex);
 const int next=activeMap.load()==0?1:0;
 auto& map=maps[next];map.count=0;map.used=0;
 ::dl_iterate_phdr(collect,&map);
 activeMap.store(next);
}
#else
bool installed() {return false;}
void setOutput(int) {}
void refreshModules() {}
#endif
#ifdef PUTGD_CRASH_TRIGGER
namespace {
#ifdef PUTGD_CRASH_DISABLED
volatile std::uintptr_t nowhere=0;
volatile int zero=0;
#endif
[[gnu::noinline]] void crashSegv() {*reinterpret_cast<volatile int*>(nowhere)=42;asm volatile("");}
[[gnu::noinline]] void crashIll() {__builtin_trap();}
[[gnu::noinline]] void crashFpe() {volatile int value=1;value=value/zero;asm volatile("");::raise(SIGFPE);}
[[gnu::noinline]] void crashBus() {::raise(SIGBUS);asm volatile("");}
volatile int stopRecursion=0;
// Recurses until the thread's stack runs into its guard page.
[[gnu::noinline]] int crashOverflow(int depth) {
 volatile char frame[1024];frame[0]=char(depth);
 if(stopRecursion)return frame[0];
 return crashOverflow(depth+1)+frame[0];
}
}
void crashNow(const char* kind) {
 if(std::strcmp(kind,"abort")==0)std::abort();
 if(std::strcmp(kind,"overflow")==0)crashOverflow(0);
 if(std::strcmp(kind,"ill")==0)crashIll();
 if(std::strcmp(kind,"fpe")==0)crashFpe();
 if(std::strcmp(kind,"bus")==0)crashBus();
 crashSegv();
 std::abort();
}
#endif
}
