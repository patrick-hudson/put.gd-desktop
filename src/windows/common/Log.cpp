#include "common/Log.h"
#include "common/Files.h"
#include "common/Paths.h"
#include <algorithm>
#include <cstdio>
#include <mutex>
#include <windows.h>

namespace putgd::log {
namespace {
std::mutex mutex_;
std::string directory_, process_, file_;
bool debug_ = false;
const char* name(Level l) { switch (l) { case Level::Debug: return "debug"; case Level::Info: return "info"; case Level::Warning: return "warning"; default: return "error"; } }
std::string fileFor(const std::string& stamp) { return paths::join(directory_, "putgd-" + stamp.substr(0, 4) + stamp.substr(5, 2) + stamp.substr(8, 2) + ".log"); }
}
void initialize(const std::string& directory, const std::string& process) { std::lock_guard lock(mutex_); directory_ = directory; process_ = process; if (!directory.empty()) files::makePrivateDirectory(directory); }
void setDebugEnabled(bool enabled) { debug_ = enabled; }
bool debugEnabled() { return debug_; }
std::string currentFile() { std::lock_guard lock(mutex_); return file_; }
void write(Level level, const std::string& category, const std::string& message, const json::Object& fields) {
    if (level == Level::Debug && !debug_) return;
    const auto stamp = files::nowIsoUtc();
    json::Object record{{"time", stamp}, {"level", name(level)}, {"process", process_}, {"category", category}, {"message", message}};
    if (!fields.empty()) record["fields"] = fields;
    const auto line = json::serialize(record) + "\n";
    std::lock_guard lock(mutex_);
    std::fputs(line.c_str(), stderr); std::fflush(stderr);
    if (directory_.empty()) return;
    file_ = fileFor(stamp);
    HANDLE h = CreateFileW(paths::wide(file_).c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD written = 0; WriteFile(h, line.data(), DWORD(line.size()), &written, nullptr); CloseHandle(h);
}
void prune(const std::string& directory, int days, long long bytes) {
    auto names = files::list(directory);
    std::vector<std::string> logs;
    for (const auto& n : names) if (n.rfind("putgd-", 0) == 0 && n.size() == 18 && n.substr(14) == ".log") logs.push_back(n);
    std::sort(logs.begin(), logs.end());
    const auto today = files::nowIsoUtc();
    const auto cutoffMs = files::parseIsoUtcMs(today) - (long long)days * 86400000LL;
    long long total = 0;
    std::vector<std::pair<std::string, long long>> kept;
    for (auto it = logs.rbegin(); it != logs.rend(); ++it) {
        const auto path = paths::join(directory, *it);
        const auto stamp = it->substr(6, 4) + "-" + it->substr(10, 2) + "-" + it->substr(12, 2) + "T00:00:00.000Z";
        const auto size = files::size(path);
        if (files::parseIsoUtcMs(stamp) < cutoffMs || total + size > bytes) { files::remove(path); continue; }
        total += size;
    }
}
void clearAll(const std::string& directory) {
    for (const auto& n : files::list(directory)) if (n.rfind("putgd-", 0) == 0 && n.size() > 4 && n.substr(n.size() - 4) == ".log") files::remove(paths::join(directory, n));
}
}
