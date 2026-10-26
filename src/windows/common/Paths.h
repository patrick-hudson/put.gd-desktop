#pragma once
// Well-known locations. All paths are UTF-8 std::string; convert at the API edge.
#include <string>
#include <windows.h>

namespace putgd::paths {
std::wstring wide(const std::string& utf8);
std::string narrow(const std::wstring& utf16);
std::string executablePath();      // this process' image
std::string executableDirectory();
std::string dataRoot();            // %LOCALAPPDATA%\put.gd
std::string configPath();          // <dataRoot>\config.json
std::string documentsFolder();     // FOLDERID_Documents
std::string downloadsFolder();
std::string join(const std::string& a, const std::string& b);
std::string parent(const std::string& path);
std::string fileName(const std::string& path);
bool isAbsolute(const std::string& path);
// \\.\pipe\putgd-<hash of the user SID>: one session endpoint per Windows user.
std::string sessionPipePath();
std::string userSidString();
}
