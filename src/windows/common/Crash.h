#pragma once
// The crash record writer (docs/diagnostics.md, "Crash records"; the format is
// core/CrashRecord.h). An unhandled-exception filter, installed first thing in
// every putgd.exe mode, writes the exception, the faulting thread's frames
// (unwound with RtlVirtualUnwind from the exception context) and the modules
// they fall in, with WriteFile only and no heap. Then it hands the exception
// on to the filter that was there before and to Windows Error Reporting, so
// the exit code, WER's report and any LocalDumps dump (resident/CrashDumps.h)
// are unchanged. Nothing else is recorded: no registers beyond the PC, no
// memory and no variables. A stack overflow may leave too little stack for the
// record; WER still sees it.
//
// With crash dumps turned on (preferences.diagnostics.crashDumps, resident/
// CrashDumps.h) the filter also writes a minidump (MiniDumpNormal: thread stacks,
// thread and module lists, no heap) with the system's dbghelp.dll, loaded only
// then, as <folder>\putgd.exe.<pid>.dmp. Windows Error Reporting's LocalDumps
// would need an HKLM key (administrator rights); its HKCU key is not honoured.
#include <windows.h>

namespace putgd::crash {
// `process` names the executable in records (putgd, putgd-ui, putgd-capture ...).
void install(const char* process);
// Where records go: the resident's running.crash; stderr (the default) for
// helpers, whose stderr the resident reads. INVALID_HANDLE_VALUE writes nothing.
void setOutput(HANDLE);
// The folder for minidumps, or null for none. Helpers inherit it from the
// resident's PUTGD_CRASH_DUMPS environment variable, read by install().
void setDumpFolder(const wchar_t* folder);
// Development builds only (PUTGD_CRASH_TRIGGER): PUTGD_TEST_CRASH=
// <process>:<segv|ill|fpe|overflow>[:<delay ms>] crashes that process on a
// thread of its own after the delay. Release builds compile this to nothing.
void armTestTrigger(const char* process);
}
