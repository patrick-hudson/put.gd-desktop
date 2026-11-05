#pragma once
#include <QString>
namespace putgd::record {
// Appends one line to the file named by PUTGD_RECORD_DEBUG_LOG when that
// variable is set; otherwise a no-op. Never carries frames or audio.
void debugLog(const QString& line);
// Writes a backtrace to the same file on SIGSEGV/SIGABRT/SIGBUS/SIGFPE; a no-op without it.
void installCrashLog();
}
