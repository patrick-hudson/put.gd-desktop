#pragma once
// Unclean-exit detection (docs/diagnostics.md, "Unclean exits"), the part that
// does not touch the file system. The resident writes running.json at startup
// and removes it on a clean shutdown. A request to stop from outside (a logoff
// or shutdown's WM_ENDSESSION, a console control event, WM_CLOSE) is noted in
// running.signal first, the Windows counterpart of the Linux SIGTERM note; the
// unhandled-exception filter writes running.crash. A marker found at the next
// start becomes one of these records.
#include "common/Json.h"
#include "core/LogFormat.h"
#include <string>

namespace putgd::unclean {
constexpr int retainedRecords = 10;
struct Previous {
    bool present = false;   // running.json exists
    json::Object marker;    // its {pid, started, version, bootId}
    std::string note;       // running.signal
    std::string crashText;  // running.crash
    bool alive = false;     // the marker's pid is still a putgd.exe started then
};
json::Object marker(long long pid, const std::string& startedIso, const std::string& version, const std::string& bootId);
// One line for running.signal: {"event":"session_end","detail":"logoff",...}.
std::string note(const std::string& event, const std::string& detail, const std::string& atIso);
// {cause: crash|signal|unknown, previousPid, previousStart, previousVersion,
// sameBoot, detectedAt, and event/detail or exceptionCode/crash}; empty when
// the previous resident shut down cleanly or is still running.
json::Object evaluate(const Previous&, const std::string& bootId, const std::string& nowIso);
// Newest first, at most retainedRecords.
json::Array remember(json::Array recent, const json::Object& record);
logformat::Level level(const json::Object& record);
std::string message(const json::Object& record);
}
