#include "resident/Environment.h"
#include "common/BuildInfo.h"
#include "common/Files.h"
#include "common/Paths.h"
#include <windows.h>
#include <shellscalingapi.h>

namespace putgd::environment {
namespace {
const wchar_t* currentVersion = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
std::string registryString(const wchar_t* name) {
    wchar_t buffer[256]; DWORD size = sizeof buffer;
    return RegGetValueW(HKEY_LOCAL_MACHINE, currentVersion, name, RRF_RT_REG_SZ, nullptr, buffer, &size) == ERROR_SUCCESS ? paths::narrow(buffer).substr(0, 128) : std::string();
}
DWORD registryDword(const wchar_t* name) {
    DWORD value = 0, size = sizeof value;
    return RegGetValueW(HKEY_LOCAL_MACHINE, currentVersion, name, RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS ? value : 0;
}
const char* architecture(USHORT machine) {
    switch (machine) {
    case IMAGE_FILE_MACHINE_AMD64: return "x86_64";
    case IMAGE_FILE_MACHINE_ARM64: return "arm64";
    case IMAGE_FILE_MACHINE_I386: return "x86";
    default: return "unknown";
    }
}
std::string localeName(LCID lcid) { wchar_t name[LOCALE_NAME_MAX_LENGTH] = {}; return LCIDToLocaleName(lcid, name, LOCALE_NAME_MAX_LENGTH, 0) ? paths::narrow(name) : std::string(); }
BOOL CALLBACK monitor(HMONITOR handle, HDC, LPRECT, LPARAM data) {
    auto& list = *reinterpret_cast<json::Array*>(data);
    MONITORINFOEXW info{}; info.cbSize = sizeof info;
    GetMonitorInfoW(handle, &info);
    UINT dpiX = 0, dpiY = 0;
    const bool dpi = SUCCEEDED(GetDpiForMonitor(handle, MDT_EFFECTIVE_DPI, &dpiX, &dpiY));
    json::Object m{{"primary", (info.dwFlags & MONITORINFOF_PRIMARY) != 0}, {"width", double(info.rcMonitor.right - info.rcMonitor.left)}, {"height", double(info.rcMonitor.bottom - info.rcMonitor.top)}};
    if (dpi) { m["dpiX"] = double(dpiX); m["dpiY"] = double(dpiY); m["scalePercent"] = double(dpiX * 100 / 96); }
    list.push_back(m);
    return list.size() < 16;
}
}
json::Object collect(const std::string& dataRoot, bool trayAvailable) {
    json::Object os{{"product", registryString(L"ProductName")}, {"edition", registryString(L"EditionID")}, {"displayVersion", registryString(L"DisplayVersion")},
        {"installationType", registryString(L"InstallationType")}};
    // RtlGetVersion reports the real version whatever the manifest says.
    using RtlGetVersion = LONG(WINAPI*)(OSVERSIONINFOEXW*);
    OSVERSIONINFOEXW v{}; v.dwOSVersionInfoSize = sizeof v;
    if (auto fn = reinterpret_cast<RtlGetVersion>(reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"))); fn && fn(&v) == 0) {
        const DWORD ubr = registryDword(L"UBR");
        os["build"] = double(v.dwBuildNumber); os["updateBuildRevision"] = double(ubr);
        os["version"] = std::to_string(v.dwMajorVersion) + "." + std::to_string(v.dwMinorVersion) + "." + std::to_string(v.dwBuildNumber) + "." + std::to_string(ubr);
        // ProductName still says "Windows 10" on Windows 11; build 22000 is the first Windows 11.
        if (v.dwBuildNumber >= 22000 && os.value("product").toString().rfind("Windows 10", 0) == 0) os["product"] = "Windows 11" + os.value("product").stringRef().substr(10);
    }
    USHORT processMachine = 0, nativeMachine = 0;
    json::Object arch{{"process", sizeof(void*) == 8 ? "x86_64" : "x86"}};
    if (IsWow64Process2(GetCurrentProcess(), &processMachine, &nativeMachine)) arch["native"] = architecture(nativeMachine);
    DWORD session = 0; ProcessIdToSessionId(GetCurrentProcessId(), &session);
    wchar_t station[128] = {}; DWORD needed = 0;
    GetUserObjectInformationW(GetProcessWindowStation(), UOI_NAME, station, sizeof station, &needed);
    json::Object sessionInfo{{"type", GetSystemMetrics(SM_REMOTESESSION) ? "remote" : "console"}, {"id", double(session)}, {"windowStation", paths::narrow(station)},
        {"interactive", paths::narrow(station) == "WinSta0"}};
    json::Array monitors;
    EnumDisplayMonitors(nullptr, nullptr, monitor, reinterpret_cast<LPARAM>(&monitors));
    MEMORYSTATUSEX memory{}; memory.dwLength = sizeof memory;
    GlobalMemoryStatusEx(&memory);
    wchar_t locale[LOCALE_NAME_MAX_LENGTH] = {};
    GetUserDefaultLocaleName(locale, LOCALE_NAME_MAX_LENGTH);
    return {{"os", os}, {"architecture", arch}, {"session", sessionInfo},
        {"displays", json::Object{{"count", GetSystemMetrics(SM_CMONITORS)}, {"monitors", monitors}, {"systemDpi", double(GetDpiForSystem())}, {"virtualWidth", GetSystemMetrics(SM_CXVIRTUALSCREEN)}, {"virtualHeight", GetSystemMetrics(SM_CYVIRTUALSCREEN)}}},
        {"locale", paths::narrow(locale)}, {"uiLanguage", localeName(MAKELCID(GetUserDefaultUILanguage(), SORT_DEFAULT))},
        {"memoryTotalBytes", double(memory.ullTotalPhys)}, {"dataFreeBytes", double(files::freeBytes(dataRoot))},
        {"tray", json::Object{{"available", trayAvailable}, {"notificationArea", FindWindowW(L"Shell_TrayWnd", nullptr) != nullptr}}},
        {"residentKind", "win32"}, {"toolkit", "win32"}, {"platform", build::platform},
#if defined(_MSC_VER)
        {"compiler", "msvc " + std::to_string(_MSC_VER)}
#else
        {"compiler", std::string("gcc ") + __VERSION__}
#endif
    };
}
json::Object summary(const json::Object& e) {
    return {{"os", e.value("os")}, {"architecture", e.value("architecture")}, {"session", e.value("session")}, {"displays", e.value("displays")["count"]}, {"tray", e.value("tray")}, {"residentKind", "win32"}};
}
}
