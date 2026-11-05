// A library the crash handler test loads after startup, so its frames are
// outside the module map collected before main (#124). It also starts a thread
// of its own, the way Qt, GStreamer or the AWS SDK start helper threads (#156).
#include <cstdint>
#include <pthread.h>
namespace {
volatile std::uintptr_t nowhere=0;
volatile int stopRecursion=0;
[[gnu::noinline]] int overflowInModule(int depth) {
 volatile char frame[1024];frame[0]=char(depth);
 if(stopRecursion)return frame[0];
 return overflowInModule(depth+1)+frame[0];
}
void* overflowThread(void*) {overflowInModule(0);return nullptr;}
}
extern "C" [[gnu::visibility("default"),gnu::noinline]] void putgd_test_crash_in_module() {
 *reinterpret_cast<volatile int*>(nowhere)=11;
 asm volatile("");
}
extern "C" [[gnu::visibility("default"),gnu::noinline]] void putgd_test_overflow_in_module_thread() {
 pthread_t thread;
 if(pthread_create(&thread,nullptr,overflowThread,nullptr)==0)pthread_join(thread,nullptr);
}
