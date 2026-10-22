#include "common/Crash.h"
#include "common/BuildInfo.h"
#include "core/CrashRecord.h"
#include <dbghelp.h>
#include <cstdlib>
#include <cstring>
#ifdef _MSC_VER
#include <intrin.h>
#define PUTGD_NOINLINE __declspec(noinline)
#define PUTGD_TRAP() __ud2()
#else
#define PUTGD_NOINLINE __attribute__((noinline))
#define PUTGD_TRAP() __builtin_trap()
#endif

namespace putgd::crash {
namespace {
LPTOP_LEVEL_EXCEPTION_FILTER previous = nullptr;
HANDLE output = INVALID_HANDLE_VALUE;
char processName[64] = "putgd";
wchar_t dumpFolder[MAX_PATH] = L"";
volatile LONG writing = 0;
// A line buffer on the filter's stack: no heap, no C runtime formatting.
struct Line {
    char data[1200]; size_t length = 0;
    void put(const char* s) { while (*s && length < sizeof data - 2) data[length++] = *s++; }
    void tab() { put("\t"); }
    void hex(unsigned long long v, int minimumDigits = 1) {
        char digits[17]; int n = 0;
        do { digits[n++] = "0123456789abcdef"[v & 0xF]; v >>= 4; } while (v && n < 16);
        while (n < minimumDigits && n < 16) digits[n++] = '0';
        put("0x"); while (n > 0 && length < sizeof data - 2) data[length++] = digits[--n];
    }
    void hexBare(unsigned long long v, int digitsWanted) {
        char digits[17]; int n = 0;
        do { digits[n++] = "0123456789ABCDEF"[v & 0xF]; v >>= 4; } while (v && n < 16);
        while (n < digitsWanted && n < 16) digits[n++] = '0';
        while (n > 0 && length < sizeof data - 2) data[length++] = digits[--n];
    }
    void number(unsigned long long v) {
        char digits[21]; int n = 0;
        do { digits[n++] = char('0' + v % 10); v /= 10; } while (v && n < 20);
        while (n > 0 && length < sizeof data - 2) data[length++] = digits[--n];
    }
    void begin(const char* kind) { length = 0; put("putgd-crash\t"); put(kind); }
    void end() { data[length++] = '\n'; DWORD written = 0; if (output != INVALID_HANDLE_VALUE) WriteFile(output, data, DWORD(length), &written, nullptr); }
};
struct Module { DWORD64 base; };
int moduleIndex(Module* modules, int& count, DWORD64 address) {
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(address), &module) || !module) return -1;
    const auto base = reinterpret_cast<DWORD64>(module);
    for (int i = 0; i < count; ++i) if (modules[i].base == base) return i;
    if (count >= 32) return -1;
    modules[count].base = base;
    return count++;
}
void writeRecord(EXCEPTION_POINTERS* info) {
    Line line;
    line.begin("begin"); line.tab(); line.put("1"); line.tab(); line.put(processName); line.tab(); line.number(GetCurrentProcessId()); line.tab(); line.number(GetCurrentThreadId()); line.end();
    line.begin("version"); line.tab(); line.put(build::version); line.end();
    const auto* exception = info->ExceptionRecord;
    line.begin("exception"); line.tab(); line.put("0x"); line.hexBare(exception->ExceptionCode, 8); line.tab();
    const char* name = exceptionName(exception->ExceptionCode); line.put(*name ? name : "-"); line.tab();
    unsigned long long fault = reinterpret_cast<unsigned long long>(exception->ExceptionAddress);
    const char* access = "-";
    if ((exception->ExceptionCode == EXCEPTION_ACCESS_VIOLATION || exception->ExceptionCode == EXCEPTION_IN_PAGE_ERROR) && exception->NumberParameters >= 2) {
        access = exception->ExceptionInformation[0] == 0 ? "read" : exception->ExceptionInformation[0] == 1 ? "write" : exception->ExceptionInformation[0] == 8 ? "execute" : "-";
        fault = exception->ExceptionInformation[1];
    }
    line.hex(fault, 16); line.tab(); line.put(access); line.end();
    FILETIME now; GetSystemTimeAsFileTime(&now);
    line.begin("time"); line.tab(); line.number(((((unsigned long long)now.dwHighDateTime << 32) | now.dwLowDateTime) - 116444736000000000ULL) / 10000000ULL); line.end();
    Module modules[32]; int moduleCount = 0, frames = 0;
#if defined(_M_X64) || defined(__x86_64__)
    CONTEXT context = *info->ContextRecord;
    line.begin("pc"); line.tab(); line.hex(context.Rip, 16); line.end();
    ULONG_PTR low = 0, high = 0; GetCurrentThreadStackLimits(&low, &high);
    for (; frames < 64 && context.Rip; ++frames) {
        const DWORD64 pc = context.Rip;
        const int index = moduleIndex(modules, moduleCount, pc);
        line.begin("frame"); line.tab(); line.number(frames); line.tab(); line.hex(pc, 16); line.tab();
        if (index < 0) { line.put("-"); line.tab(); line.put("0x0"); } else { line.number(index); line.tab(); line.hex(pc - modules[index].base); }
        line.end();
        DWORD64 imageBase = 0;
        if (auto* function = RtlLookupFunctionEntry(pc, &imageBase, nullptr)) {
            PVOID handlerData = nullptr; DWORD64 establisher = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, pc, function, &context, &handlerData, &establisher, nullptr);
        } else {
            // A leaf function (or a jump to nowhere): the return address is on top of the stack.
            if (context.Rsp < low || context.Rsp + 8 > high) break;
            context.Rip = *reinterpret_cast<DWORD64*>(context.Rsp); context.Rsp += 8;
        }
        if (context.Rsp < low || context.Rsp > high) { ++frames; break; }
    }
#else
    line.begin("pc"); line.tab(); line.hex(reinterpret_cast<unsigned long long>(exception->ExceptionAddress), 16); line.end();
#endif
    for (int i = 0; i < moduleCount; ++i) {
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(modules[i].base);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(modules[i].base + dos->e_lfanew);
        wchar_t wide[520]; char path[1024];
        const DWORD length = GetModuleFileNameW(reinterpret_cast<HMODULE>(modules[i].base), wide, 520);
        const int bytes = length ? WideCharToMultiByte(CP_UTF8, 0, wide, int(length), path, int(sizeof path - 1), nullptr, nullptr) : 0;
        path[bytes > 0 ? bytes : 0] = 0;
        line.begin("module"); line.tab(); line.number(i); line.tab(); line.hex(modules[i].base, 16); line.tab();
        line.hexBare(nt->FileHeader.TimeDateStamp, 8); line.hexBare(nt->OptionalHeader.SizeOfImage, 1); line.tab(); line.put(*path ? path : "-"); line.end();
    }
    line.begin("end"); line.tab(); line.number(frames); line.end();
    if (output != INVALID_HANDLE_VALUE) FlushFileBuffers(output);
}
// <folder>\putgd.exe.<pid>.dmp, the name Windows Error Reporting gives its dumps.
void writeDump(EXCEPTION_POINTERS* info) {
    if (!dumpFolder[0]) return;
    wchar_t path[MAX_PATH + 40]; size_t n = 0;
    for (const wchar_t* p = dumpFolder; *p && n < MAX_PATH; ++p) path[n++] = *p;
    for (const wchar_t* p = L"\\putgd.exe."; *p; ++p) path[n++] = *p;
    wchar_t digits[12]; int d = 0; DWORD pid = GetCurrentProcessId();
    do { digits[d++] = wchar_t(L'0' + pid % 10); pid /= 10; } while (pid && d < 11);
    while (d > 0) path[n++] = digits[--d];
    for (const wchar_t* p = L".dmp"; *p; ++p) path[n++] = *p;
    path[n] = 0;
    using Write = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION, PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);
    HMODULE dbghelp = LoadLibraryExW(L"dbghelp.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    auto write = dbghelp ? reinterpret_cast<Write>(reinterpret_cast<void*>(GetProcAddress(dbghelp, "MiniDumpWriteDump"))) : nullptr;
    if (!write) return;
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    MINIDUMP_EXCEPTION_INFORMATION exception{GetCurrentThreadId(), info, FALSE};
    const BOOL ok = write(GetCurrentProcess(), GetCurrentProcessId(), file, MiniDumpNormal, &exception, nullptr, nullptr);
    CloseHandle(file);
    Line line; line.begin("dump"); line.tab(); line.put(ok ? "written" : "failed"); line.end();
}
LONG WINAPI filter(EXCEPTION_POINTERS* info) {
    // The first crashing thread writes; another one meanwhile waits for it.
    if (InterlockedCompareExchange(&writing, 1, 0) == 0) { writeRecord(info); writeDump(info); }
    else Sleep(2000);
    return previous ? previous(info) : EXCEPTION_CONTINUE_SEARCH;
}
#ifdef PUTGD_CRASH_TRIGGER
struct Trigger { char kind[16]; DWORD delay; };
Trigger trigger{};
// Recurses until the stack runs out (the "overflow" trigger).
volatile int recursionGuard = 1;
PUTGD_NOINLINE int overflow(int depth) { volatile char pad[4096]; pad[0] = char(depth); return recursionGuard ? overflow(depth + 1) + pad[0] : 0; }
DWORD WINAPI crashLater(void*) {
    ULONG guarantee = 64 * 1024; SetThreadStackGuarantee(&guarantee);
    Sleep(trigger.delay);
    if (!std::strcmp(trigger.kind, "ill")) PUTGD_TRAP();
    if (!std::strcmp(trigger.kind, "fpe")) { volatile int zero = 0; volatile int r = 1 / zero; (void)r; }
    if (!std::strcmp(trigger.kind, "overflow")) overflow(0);
    *static_cast<volatile int*>(nullptr) = 1;
    return 0;
}
#endif
}
void install(const char* process) {
    size_t i = 0; for (; process[i] && i < sizeof processName - 1; ++i) processName[i] = process[i];
    processName[i] = 0;
    output = GetStdHandle(STD_ERROR_HANDLE);
    if (!output) output = INVALID_HANDLE_VALUE;
    wchar_t folder[MAX_PATH];
    const DWORD length = GetEnvironmentVariableW(L"PUTGD_CRASH_DUMPS", folder, MAX_PATH);
    setDumpFolder(length && length < MAX_PATH ? folder : nullptr);
    previous = SetUnhandledExceptionFilter(filter);
}
void setOutput(HANDLE handle) { output = handle ? handle : INVALID_HANDLE_VALUE; }
void setDumpFolder(const wchar_t* folder) {
    size_t i = 0;
    if (folder) for (; folder[i] && i < MAX_PATH - 1; ++i) dumpFolder[i] = folder[i];
    dumpFolder[i] = 0;
}
void armTestTrigger(const char* process) {
#ifdef PUTGD_CRASH_TRIGGER
    char value[128]; const DWORD length = GetEnvironmentVariableA("PUTGD_TEST_CRASH", value, sizeof value);
    if (!length || length >= sizeof value) return;
    // <process>:<kind>[:<delay ms>]
    char* first = std::strchr(value, ':'); if (!first) return;
    *first = 0;
    if (std::strcmp(value, process) != 0) return;
    char* second = std::strchr(first + 1, ':');
    if (second) { *second = 0; trigger.delay = DWORD(std::strtoul(second + 1, nullptr, 10)); }
    std::strncpy(trigger.kind, first + 1, sizeof trigger.kind - 1);
    if (HANDLE thread = CreateThread(nullptr, 0, crashLater, nullptr, 0, nullptr)) CloseHandle(thread);
#else
    (void)process;
#endif
}
}
