// A library the crash handler test links, so ld.so runs its constructor before
// the executable's own (#211). The constructor starts a thread, the way a
// library that starts helper threads from its constructor would. The thread
// notes the alternate signal stack it started with, then waits; the test asks
// it for that stack, or tells it to overflow its stack.
#include <cstdint>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>
namespace {
pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t changed=PTHREAD_COND_INITIALIZER;
bool started=false,overflow=false;
long long stackBytes=0;
int stackEnabled=0;
volatile int stopRecursion=0;
[[gnu::noinline]] int overflowInEarlyThread(int depth) {
 volatile char frame[1024];frame[0]=char(depth);
 if(stopRecursion)return frame[0];
 return overflowInEarlyThread(depth+1)+frame[0];
}
void* earlyThread(void*) {
 stack_t stack{};
 const bool known=::sigaltstack(nullptr,&stack)==0;
 pthread_mutex_lock(&lock);
 stackBytes=known?static_cast<long long>(stack.ss_size):0;
 stackEnabled=known&&!(stack.ss_flags&SS_DISABLE);
 started=true;pthread_cond_broadcast(&changed);
 while(!overflow)pthread_cond_wait(&changed,&lock);
 pthread_mutex_unlock(&lock);
 overflowInEarlyThread(0);
 return nullptr;
}
[[gnu::constructor]] void startEarlyThread() {
 pthread_t thread;
 if(pthread_create(&thread,nullptr,earlyThread,nullptr)==0)pthread_detach(thread);
}
}
// The alternate stack the constructor's thread started with.
extern "C" [[gnu::visibility("default")]] void putgd_test_early_thread_stack(long long* bytes,int* enabled) {
 pthread_mutex_lock(&lock);
 while(!started)pthread_cond_wait(&changed,&lock);
 *bytes=stackBytes;*enabled=stackEnabled;
 pthread_mutex_unlock(&lock);
}
// Overflows the constructor's thread; the process dies by SIGSEGV.
extern "C" [[gnu::visibility("default")]] void putgd_test_overflow_early_thread() {
 pthread_mutex_lock(&lock);
 overflow=true;pthread_cond_broadcast(&changed);
 pthread_mutex_unlock(&lock);
 for(;;)::pause();
}
