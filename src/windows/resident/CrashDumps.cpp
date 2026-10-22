#include "resident/CrashDumps.h"
#include "common/Files.h"
#include "common/Log.h"
#include "common/Paths.h"
#include "common/Text.h"
#include <algorithm>
#include <windows.h>

namespace putgd::crashdumps {
namespace {
const wchar_t* keyPath = L"Software\\Microsoft\\Windows\\Windows Error Reporting\\LocalDumps\\putgd.exe";
bool enabledSetting = false;
std::wstring readString(HKEY key, const wchar_t* name) {
    wchar_t buffer[1024]; DWORD size = sizeof buffer;
    return RegGetValueW(key, nullptr, name, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND, nullptr, buffer, &size) == ERROR_SUCCESS ? std::wstring(buffer) : std::wstring();
}
DWORD readDword(HKEY key, const wchar_t* name, DWORD fallback) {
    DWORD value = 0, size = sizeof value;
    return RegGetValueW(key, nullptr, name, RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS ? value : fallback;
}
}
std::string folder(const std::string& dataRoot) { return paths::join(dataRoot, "crashdumps"); }
json::Object apply(bool enabled, const std::string& dataRoot) {
    enabledSetting = enabled;
    HKEY key = nullptr;
    const bool exists = RegOpenKeyExW(HKEY_CURRENT_USER, keyPath, 0, KEY_READ | KEY_WRITE, &key) == ERROR_SUCCESS;
    const bool managed = exists && readDword(key, L"PutgdManaged", 0) == 1;
    if (key) RegCloseKey(key);
    if (exists && !managed) return status(dataRoot); // someone else's configuration
    if (!enabled) {
        if (managed && RegDeleteKeyW(HKEY_CURRENT_USER, keyPath) == ERROR_SUCCESS) log::info("app", "Crash dumps turned off", {});
        return status(dataRoot);
    }
    const auto dumps = folder(dataRoot);
    if (!files::makePrivateDirectory(dumps)) { log::warning("app", "Crash dumps not registered", {{"reason", "folder_unavailable"}}); return status(dataRoot); }
    if (RegCreateKeyExW(HKEY_CURRENT_USER, keyPath, 0, nullptr, 0, KEY_WRITE | KEY_READ, nullptr, &key, nullptr) != ERROR_SUCCESS) { log::warning("app", "Crash dumps not registered", {{"reason", "registry_unavailable"}}); return status(dataRoot); }
    const auto folderW = paths::wide(dumps);
    const DWORD type = 1, count = 5, ours = 1;
    const bool ok = RegSetValueExW(key, L"DumpFolder", 0, REG_EXPAND_SZ, reinterpret_cast<const BYTE*>(folderW.c_str()), DWORD((folderW.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS &&
        RegSetValueExW(key, L"DumpType", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&type), sizeof type) == ERROR_SUCCESS &&
        RegSetValueExW(key, L"DumpCount", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&count), sizeof count) == ERROR_SUCCESS &&
        RegSetValueExW(key, L"PutgdManaged", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&ours), sizeof ours) == ERROR_SUCCESS;
    RegCloseKey(key);
    if (!managed && ok) log::info("app", "Crash dumps turned on", {{"dumpType", "mini"}, {"dumpCount", 5}});
    if (!ok) log::warning("app", "Crash dumps not registered", {{"reason", "registry_write_failed"}});
    return status(dataRoot);
}
json::Object status(const std::string& dataRoot) {
    json::Object result{{"enabled", enabledSetting}, {"registered", false}, {"managedByPutgd", false}, {"mechanism", "wer-localdumps"}};
    HKEY key = nullptr;
    std::string dumps = folder(dataRoot);
    if (RegOpenKeyExW(HKEY_CURRENT_USER, keyPath, 0, KEY_READ, &key) == ERROR_SUCCESS) {
        const bool managed = readDword(key, L"PutgdManaged", 0) == 1;
        const auto configured = readString(key, L"DumpFolder");
        result["registered"] = true; result["managedByPutgd"] = managed;
        result["dumpType"] = double(readDword(key, L"DumpType", 1)); result["dumpCount"] = double(readDword(key, L"DumpCount", 10));
        if (!managed) result["reason"] = "configured_elsewhere";
        if (!configured.empty()) {
            wchar_t expanded[1024];
            const DWORD n = ExpandEnvironmentStringsW(configured.c_str(), expanded, 1024);
            dumps = n && n <= 1024 ? paths::narrow(expanded) : paths::narrow(configured);
        } else dumps = paths::join(paths::parent(dataRoot), "CrashDumps"); // WER's default, %LOCALAPPDATA%\CrashDumps
        RegCloseKey(key);
    }
    result["folder"] = dumps;
    // Only put.gd's executables, newest first: WER names dumps <exe>.<pid>.dmp.
    struct Dump { std::string name; long long bytes, modified; };
    std::vector<Dump> found;
    WIN32_FIND_DATAW data;
    HANDLE find = FindFirstFileExW(paths::wide(paths::join(dumps, "*.dmp")).c_str(), FindExInfoBasic, &data, FindExSearchNameMatch, nullptr, 0);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            const auto name = paths::narrow(data.cFileName);
            const auto lowerName = text::lower(name);
            if (!text::startsWith(lowerName, "putgd.exe.") && !text::startsWith(lowerName, "putgd-cli.exe.")) continue;
            const long long modified = (long long)((((unsigned long long)data.ftLastWriteTime.dwHighDateTime << 32) | data.ftLastWriteTime.dwLowDateTime) - 116444736000000000ULL) / 10000;
            found.push_back({name, ((long long)data.nFileSizeHigh << 32) | data.nFileSizeLow, modified});
        } while (FindNextFileW(find, &data));
        FindClose(find);
    }
    std::sort(found.begin(), found.end(), [](const Dump& a, const Dump& b) { return a.modified > b.modified; });
    json::Array list;
    for (size_t i = 0; i < found.size() && i < 5; ++i) list.push_back(json::Object{{"file", paths::join(dumps, found[i].name)}, {"bytes", double(found[i].bytes)}, {"time", files::isoUtcFromMs(found[i].modified)}});
    result["dumps"] = list; result["count"] = double(found.size());
    return result;
}
}
