#include "common/Paths.h"
#include "common/Sha256.h"
#include <shlobj.h>
#include <sddl.h>
#include <vector>

namespace putgd::paths {
std::wstring wide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}
std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}
std::string executablePath() {
    std::vector<wchar_t> buffer(32768);
    const DWORD n = GetModuleFileNameW(nullptr, buffer.data(), DWORD(buffer.size()));
    return narrow(std::wstring(buffer.data(), n));
}
std::string executableDirectory() { return parent(executablePath()); }
namespace {
std::string knownFolder(REFKNOWNFOLDERID id) {
    PWSTR path = nullptr;
    std::string result;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_CREATE | KF_FLAG_DONT_VERIFY, nullptr, &path)) && path) result = narrow(path);
    if (path) CoTaskMemFree(path);
    return result;
}
}
std::string dataRoot() { return join(knownFolder(FOLDERID_LocalAppData), "put.gd"); }
std::string configPath() { return join(dataRoot(), "config.json"); }
std::string documentsFolder() { return knownFolder(FOLDERID_Documents); }
std::string downloadsFolder() { return knownFolder(FOLDERID_Downloads); }
std::string join(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    const char last = a.back();
    return (last == '\\' || last == '/') ? a + b : a + "\\" + b;
}
std::string parent(const std::string& path) {
    const auto pos = path.find_last_of("\\/");
    return pos == std::string::npos ? std::string() : path.substr(0, pos);
}
std::string fileName(const std::string& path) {
    const auto pos = path.find_last_of("\\/");
    return pos == std::string::npos ? path : path.substr(pos + 1);
}
bool isAbsolute(const std::string& p) {
    if (p.size() >= 3 && ((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z')) && p[1] == ':' && (p[2] == '\\' || p[2] == '/')) return true;
    return p.size() > 2 && p[0] == '\\' && p[1] == '\\';
}
std::string userSidString() {
    HANDLE token = nullptr;
    std::string result;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return result;
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::vector<unsigned char> buffer(size);
    if (GetTokenInformation(token, TokenUser, buffer.data(), size, &size)) {
        LPWSTR text = nullptr;
        if (ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid, &text)) { result = narrow(text); LocalFree(text); }
    }
    CloseHandle(token);
    return result;
}
std::string sessionPipePath() {
    return "\\\\.\\pipe\\putgd-" + crypto::Sha256::hexDigest("putgd-session:" + userSidString()).substr(0, 24);
}
}
