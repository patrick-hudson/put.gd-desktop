#include "common/Files.h"
#include "common/Paths.h"
#include "common/Sha256.h"
#include "common/Text.h"
#include <aclapi.h>
#include <cstdio>
#include <ctime>
#include <sddl.h>
#include <windows.h>

namespace putgd::files {
namespace {
std::wstring w(const std::string& s) { return paths::wide(s); }
DWORD attributes(const std::string& path) { return GetFileAttributesW(w(path).c_str()); }
bool applyOwnerOnlyDacl(const std::string& path, bool directory) {
    // D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;FA;;;<owner>) - protected, no inheritance from the parent.
    const auto sid = paths::userSidString();
    if (sid.empty()) return false;
    const std::string sddl = directory ? "D:PAI(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;FA;;;" + sid + ")" : "D:PAI(A;;FA;;;SY)(A;;FA;;;BA)(A;;FA;;;" + sid + ")";
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(w(sddl).c_str(), SDDL_REVISION_1, &descriptor, nullptr)) return false;
    PACL dacl = nullptr; BOOL present = FALSE, defaulted = FALSE;
    bool ok = GetSecurityDescriptorDacl(descriptor, &present, &dacl, &defaulted) && present;
    if (ok) ok = SetNamedSecurityInfoW(const_cast<LPWSTR>(w(path).c_str()), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr, nullptr, dacl, nullptr) == ERROR_SUCCESS;
    LocalFree(descriptor);
    return ok;
}
}
bool exists(const std::string& path) { return attributes(path) != INVALID_FILE_ATTRIBUTES; }
bool isDirectory(const std::string& path) { const auto a = attributes(path); return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY); }
bool isRegularFile(const std::string& path) { const auto a = attributes(path); return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY) && !(a & FILE_ATTRIBUTE_REPARSE_POINT); }
long long size(const std::string& path) {
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (!GetFileAttributesExW(w(path).c_str(), GetFileExInfoStandard, &data)) return -1;
    return (static_cast<long long>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
}
bool makeDirectories(const std::string& path) {
    if (isDirectory(path)) return true;
    const auto up = paths::parent(path);
    if (!up.empty() && up != path && !isDirectory(up) && !makeDirectories(up)) return false;
    return CreateDirectoryW(w(path).c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
}
bool makePrivateDirectory(const std::string& path) { return makeDirectories(path) && applyOwnerOnlyDacl(path, true); }
bool restrictToOwner(const std::string& path) { return applyOwnerOnlyDacl(path, isDirectory(path)); }
std::optional<std::string> read(const std::string& path, size_t maximumBytes) {
    HANDLE h = CreateFileW(w(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return std::nullopt;
    LARGE_INTEGER length; std::string out;
    if (GetFileSizeEx(h, &length) && length.QuadPart >= 0 && (unsigned long long)length.QuadPart <= maximumBytes) {
        out.resize(size_t(length.QuadPart));
        size_t offset = 0;
        while (offset < out.size()) {
            DWORD got = 0;
            if (!ReadFile(h, out.data() + offset, DWORD(std::min<size_t>(out.size() - offset, 1 << 20)), &got, nullptr) || got == 0) { CloseHandle(h); return std::nullopt; }
            offset += got;
        }
    } else { CloseHandle(h); return std::nullopt; }
    CloseHandle(h);
    return out;
}
std::string temporaryName(const std::string& directory, const std::string& prefix) {
    char suffix[32]; std::snprintf(suffix, sizeof suffix, "%08lx%08lx", (unsigned long)GetCurrentProcessId(), (unsigned long)GetTickCount());
    static unsigned counter = 0;
    return paths::join(directory, prefix + suffix + std::to_string(++counter) + ".tmp");
}
Error writeAtomically(const std::string& path, const std::string& bytes, bool ownerOnly) {
    const auto directory = paths::parent(path);
    const auto temporary = temporaryName(directory, ".putgd-");
    HANDLE h = CreateFileW(w(temporary).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return {"file_io", "Cannot create a temporary file beside the target", {}};
    size_t offset = 0; bool ok = true;
    while (ok && offset < bytes.size()) { DWORD written = 0; ok = WriteFile(h, bytes.data() + offset, DWORD(bytes.size() - offset), &written, nullptr) && written > 0; offset += written; }
    ok = ok && FlushFileBuffers(h);
    CloseHandle(h);
    if (ok && ownerOnly) applyOwnerOnlyDacl(temporary, false);
    if (!ok || !MoveFileExW(w(temporary).c_str(), w(path).c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) { DeleteFileW(w(temporary).c_str()); return {"file_io", "Cannot commit the file", {}}; }
    return {};
}
bool remove(const std::string& path) { return DeleteFileW(w(path).c_str()) || GetLastError() == ERROR_FILE_NOT_FOUND; }
bool removeDirectoryRecursively(const std::string& path) {
    if (!isDirectory(path)) return !exists(path);
    for (const auto& name : list(path)) {
        const auto child = paths::join(path, name);
        if (isDirectory(child) && !(attributes(child) & FILE_ATTRIBUTE_REPARSE_POINT)) removeDirectoryRecursively(child);
        else { SetFileAttributesW(w(child).c_str(), FILE_ATTRIBUTE_NORMAL); DeleteFileW(w(child).c_str()); }
    }
    return RemoveDirectoryW(w(path).c_str());
}
bool rename(const std::string& from, const std::string& to, bool replace) { return MoveFileExW(w(from).c_str(), w(to).c_str(), replace ? MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH : MOVEFILE_WRITE_THROUGH); }
std::string sha256(const std::string& path, long long* bytes) {
    HANDLE h = CreateFileW(w(path).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return {};
    crypto::Sha256 hash; std::string chunk(1 << 20, '\0'); long long total = 0;
    while (true) { DWORD got = 0; if (!ReadFile(h, chunk.data(), DWORD(chunk.size()), &got, nullptr)) { CloseHandle(h); return {}; } if (got == 0) break; hash.update(chunk.data(), got); total += got; }
    CloseHandle(h);
    if (bytes) *bytes = total;
    return crypto::hex(hash.finish());
}
std::vector<std::string> list(const std::string& directory) {
    std::vector<std::string> names;
    WIN32_FIND_DATAW data;
    HANDLE h = FindFirstFileW(w(paths::join(directory, "*")).c_str(), &data);
    if (h == INVALID_HANDLE_VALUE) return names;
    do { const std::wstring name = data.cFileName; if (name != L"." && name != L"..") names.push_back(paths::narrow(name)); } while (FindNextFileW(h, &data));
    FindClose(h);
    return names;
}
FileIdentity identity(const std::string& path) {
    FileIdentity id;
    HANDLE h = CreateFileW(w(path).c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) return id;
    BY_HANDLE_FILE_INFORMATION info;
    if (GetFileInformationByHandle(h, &info)) { id.volume = info.dwVolumeSerialNumber; id.index = (static_cast<unsigned long long>(info.nFileIndexHigh) << 32) | info.nFileIndexLow; id.valid = true; }
    CloseHandle(h);
    return id;
}
long long freeBytes(const std::string& path) {
    ULARGE_INTEGER available; std::string probe = path;
    while (!probe.empty() && !isDirectory(probe)) probe = paths::parent(probe);
    if (probe.empty() || !GetDiskFreeSpaceExW(w(probe).c_str(), &available, nullptr, nullptr)) return -1;
    return (long long)available.QuadPart;
}
Error copyFile(const std::string& from, const std::string& to) {
    const auto temporary = temporaryName(paths::parent(to), ".putgd-");
    if (!CopyFileW(w(from).c_str(), w(temporary).c_str(), TRUE)) return {"file_io", "Cannot copy the file", {}};
    if (!MoveFileExW(w(temporary).c_str(), w(to).c_str(), MOVEFILE_WRITE_THROUGH)) { DeleteFileW(w(temporary).c_str()); return {"file_io", "Cannot place the copied file", {}}; }
    return {};
}
std::string isoUtcFromFileTime(unsigned long long ft) {
    // FILETIME: 100 ns since 1601-01-01.
    const long long ms = (long long)(ft / 10000ULL) - 11644473600000LL;
    return isoUtcFromMs(ms);
}
std::string isoUtcFromMs(long long ms) {
    const time_t seconds = ms / 1000; const int millis = int(ms % 1000);
    std::tm t; gmtime_s(&t, &seconds);
    char buf[40]; std::snprintf(buf, sizeof buf, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec, millis);
    return buf;
}
std::string nowIsoUtc() {
    FILETIME ft; GetSystemTimePreciseAsFileTime(&ft);
    return isoUtcFromFileTime((static_cast<unsigned long long>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime);
}
long long parseIsoUtcMs(const std::string& s) {
    int y, mo, d, h, mi, sec, ms = 0;
    if (std::sscanf(s.c_str(), "%4d-%2d-%2dT%2d:%2d:%2d.%3d", &y, &mo, &d, &h, &mi, &sec, &ms) < 6) return -1;
    std::tm t{}; t.tm_year = y - 1900; t.tm_mon = mo - 1; t.tm_mday = d; t.tm_hour = h; t.tm_min = mi; t.tm_sec = sec;
    const time_t seconds = _mkgmtime(&t);
    if (seconds < 0) return -1;
    return (long long)seconds * 1000 + ms;
}
LocalFields localFields(const std::string& iso) {
    long long ms = iso.empty() ? -1 : parseIsoUtcMs(iso);
    if (ms < 0) { FILETIME ft; GetSystemTimeAsFileTime(&ft); ms = static_cast<long long>(((static_cast<unsigned long long>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime) / 10000ULL) - 11644473600000LL; }
    const time_t seconds = ms / 1000; std::tm t; localtime_s(&t, &seconds);
    return {t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec};
}
}
