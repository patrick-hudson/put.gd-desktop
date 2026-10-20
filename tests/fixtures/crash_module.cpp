// A library the crash handler test loads after startup, so its frames are
// outside the module map collected before main (#124).
#include <cstdint>
namespace {
volatile std::uintptr_t nowhere=0;
}
extern "C" [[gnu::visibility("default"),gnu::noinline]] void putgd_test_crash_in_module() {
 *reinterpret_cast<volatile int*>(nowhere)=11;
 asm volatile("");
}
