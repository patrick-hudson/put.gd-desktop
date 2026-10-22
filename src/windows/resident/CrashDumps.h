#pragma once
// Crash dumps through Windows Error Reporting's LocalDumps (docs/diagnostics.md):
// with preferences.diagnostics.crashDumps on, the resident registers
//   HKCU\Software\Microsoft\Windows\Windows Error Reporting\LocalDumps\putgd.exe
// with DumpFolder <dataRoot>\crashdumps, DumpType 1 (a minidump: thread stacks,
// module list, no heap) and DumpCount 5, so WER writes a dump when any putgd.exe
// mode crashes. No code of put.gd's runs to make the dump. A key put.gd did not
// create (one the user or an administrator set up) is never changed or removed.
// Dumps stay in that folder: a diagnostics package lists them (crashdumps.json:
// name, size, time) and never includes one, since a dump holds process memory.
#include "common/Json.h"
#include <string>

namespace putgd::crashdumps {
std::string folder(const std::string& dataRoot);
// Registers or removes put.gd's key; returns status().
json::Object apply(bool enabled, const std::string& dataRoot);
// {enabled, registered, managedByPutgd, folder, dumpType, dumpCount, reason?, dumps[{file, bytes, time}]}
// with the newest five dumps of put.gd executables in the folder.
json::Object status(const std::string& dataRoot);
}
