#pragma once
#include <cstdint>
namespace putgd::crash {
// A small async-signal-safe crash handler (#124, docs/diagnostics.md). Every
// product executable links it as an object library, and it installs itself
// before main: SIGSEGV, SIGBUS, SIGILL, SIGFPE and SIGABRT run on an alternate
// signal stack, walk the stack with a warmed-up _Unwind_Backtrace and write the
// raw return addresses plus the modules they fall in (path, load address, GNU
// build ID) with write(2), then hand the signal back to the previous
// disposition, so the process still dies by that signal and systemd-coredump
// or a handler installed earlier still sees it. Records go to stderr, which
// the resident collects from its helpers; the resident sends its own to a file
// it opened in advance (setOutput). Nothing else is recorded: no registers
// beyond the faulting address and PC, no memory, no variables.
//
// Disabled in ASan/TSan builds (the sanitizers own these signals) and when
// PUTGD_CRASH_HANDLER=0 is set.
constexpr const char* linePrefix="putgd-crash\t";
constexpr int recordVersion=1;
// Whether the handler is installed in this process.
bool installed();
// Where records go from now on; -1 restores stderr. The descriptor stays open.
void setOutput(int fd);
// Collects the loaded modules again (dl_iterate_phdr), for modules loaded since
// startup; not async-signal-safe. Frames in modules missing from the map are
// still resolved at crash time from /proc/self/maps.
void refreshModules();
#ifdef PUTGD_CRASH_TRIGGER
// Development builds only: PUTGD_TEST_CRASH=<process>:<segv|abort|ill|fpe|bus>[:<delay ms>]
// crashes the named executable (putgd, putgd-ui, putgd-capture, putgd-record, ...)
// on its main thread's stack after the delay. Compiled out of release builds.
[[noreturn]] void crashNow(const char* kind);
#endif
}
