#include "resident/LoginStartup.h"
#include "common/Paths.h"
#include <windows.h>

namespace putgd::startup {
namespace {
const wchar_t* runKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
const wchar_t* valueName = L"put.gd";
}
Error apply(const std::string& executable, bool enable) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, runKey, 0, nullptr, 0, KEY_SET_VALUE | KEY_QUERY_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        return {"autostart_io", "Cannot open the login startup registry key", {}};
    Error error;
    if (enable) {
        if (!paths::isAbsolute(executable)) error = {"autostart_io", "Login startup requires an absolute executable path", {}};
        else {
            const auto command = L"\"" + paths::wide(executable) + L"\" --background";
            if (RegSetValueExW(key, valueName, 0, REG_SZ, reinterpret_cast<const BYTE*>(command.c_str()), DWORD((command.size() + 1) * sizeof(wchar_t))) != ERROR_SUCCESS)
                error = {"autostart_io", "Cannot write the login startup entry", {}};
        }
    } else {
        const auto status = RegDeleteValueW(key, valueName);
        if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND) error = {"autostart_io", "Cannot remove the login startup entry", {}};
    }
    RegCloseKey(key);
    return error;
}
bool enabled() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, runKey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) return false;
    const bool present = RegQueryValueExW(key, valueName, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
    RegCloseKey(key);
    return present;
}
}
