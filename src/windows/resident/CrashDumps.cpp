#include "resident/CrashDumps.h"
#include "common/Crash.h"
#include "common/Files.h"
#include "common/Log.h"
#include "common/Paths.h"
#include "common/Text.h"
#include <algorithm>
#include <windows.h>

namespace putgd::crashdumps {
namespace {
bool enabledSetting = false, armed = false;
struct Dump { std::string name; long long bytes, modified; };
// put.gd's own dumps, newest first: putgd.exe.<pid>.dmp.
std::vector<Dump> list(const std::string& dumps) {
    std::vector<Dump> found;
    WIN32_FIND_DATAW data;
    HANDLE find = FindFirstFileExW(paths::wide(paths::join(dumps, "putgd.exe.*.dmp")).c_str(), FindExInfoBasic, &data, FindExSearchNameMatch, nullptr, 0);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            if (data.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) continue;
            const long long modified = (long long)((((unsigned long long)data.ftLastWriteTime.dwHighDateTime << 32) | data.ftLastWriteTime.dwLowDateTime) - 116444736000000000ULL) / 10000;
            found.push_back({paths::narrow(data.cFileName), ((long long)data.nFileSizeHigh << 32) | data.nFileSizeLow, modified});
        } while (FindNextFileW(find, &data));
        FindClose(find);
    }
    std::sort(found.begin(), found.end(), [](const Dump& a, const Dump& b) { return a.modified > b.modified; });
    return found;
}
}
std::string folder(const std::string& dataRoot) { return paths::join(dataRoot, "crashdumps"); }
json::Object apply(bool enabled, const std::string& dataRoot) {
    enabledSetting = enabled;
    const auto dumps = folder(dataRoot);
    if (enabled && !files::makePrivateDirectory(dumps)) { log::warning("app", "Crash dumps unavailable", {{"reason", "folder_unavailable"}}); enabled = false; }
    const auto wide = paths::wide(dumps);
    crash::setDumpFolder(enabled ? wide.c_str() : nullptr);
    SetEnvironmentVariableW(L"PUTGD_CRASH_DUMPS", enabled ? wide.c_str() : nullptr);
    if (enabled != armed) log::info("app", enabled ? "Crash dumps turned on" : "Crash dumps turned off", {{"dumpType", "minidump"}, {"kept", keptDumps}});
    armed = enabled;
    const auto found = list(dumps);
    for (size_t i = keptDumps; i < found.size(); ++i) files::remove(paths::join(dumps, found[i].name));
    return status(dataRoot);
}
json::Object status(const std::string& dataRoot) {
    const auto dumps = folder(dataRoot);
    const auto found = list(dumps);
    json::Array newest;
    for (size_t i = 0; i < found.size() && i < size_t(keptDumps); ++i) newest.push_back(json::Object{{"file", paths::join(dumps, found[i].name)}, {"bytes", double(found[i].bytes)}, {"time", files::isoUtcFromMs(found[i].modified)}});
    return {{"enabled", enabledSetting}, {"armed", armed}, {"mechanism", "minidump"}, {"writer", "dbghelp MiniDumpWriteDump"}, {"dumpType", "MiniDumpNormal"},
        {"kept", keptDumps}, {"folder", dumps}, {"count", double(found.size())}, {"dumps", newest}};
}
}
