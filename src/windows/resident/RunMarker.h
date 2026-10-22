#pragma once
// The resident's run marker (docs/diagnostics.md, "Unclean exits"; the rules
// are core/UncleanExit.h). begin() records a marker the previous resident left
// behind, writes this one's running.json and opens running.crash for the crash
// filter; end() removes them on a clean shutdown. note() writes running.signal
// when Windows or a user asks the resident to stop (WM_ENDSESSION, a console
// control event, WM_CLOSE from taskkill), so a logoff that ends the process
// before its shutdown finishes reads as "signal" rather than "unknown". Helpers
// need nothing here: they die with the resident through the job object
// (JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE), and their own exits are logged.
#include "common/Json.h"
#include <string>

namespace putgd::runmarker {
void begin(const std::string& dataRoot, const std::string& version);
void note(const std::string& event, const std::string& detail);
void end();
// {detected, last, recent}: whether this start found an unclean exit, that
// record, and the retained records, newest first.
json::Object report();
// This boot: the kernel's boot counter, else the boot time to the minute.
std::string bootId();
}
