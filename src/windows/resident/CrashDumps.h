#pragma once
// Crash dumps (docs/diagnostics.md, "Crash dumps"): with
// preferences.diagnostics.crashDumps on, every mode of putgd.exe writes a
// minidump (thread stacks, thread and module lists, no heap) when it crashes,
// from its own unhandled-exception filter with the system's dbghelp.dll
// (common/Crash.h), as <dataRoot>\crashdumps\putgd.exe.<pid>.dmp. Windows Error
// Reporting's LocalDumps is not used: it honours only an HKLM key, which needs
// administrator rights, and ignores the same key under HKCU. The newest five
// dumps are kept. A diagnostics package lists them (crashdumps.json: name, size,
// time) and never includes one, since a dump holds process memory.
#include "common/Json.h"
#include <string>

namespace putgd::crashdumps {
constexpr int keptDumps = 5;
std::string folder(const std::string& dataRoot);
// Arms or disarms this process' filter and the PUTGD_CRASH_DUMPS variable its
// helpers inherit, and prunes the folder to the newest five; returns status().
json::Object apply(bool enabled, const std::string& dataRoot);
// {enabled, mechanism, dumpType, kept, folder, count, dumps[{file, bytes, time}]}
// with the newest five dumps of put.gd's executables in the folder.
json::Object status(const std::string& dataRoot);
}
