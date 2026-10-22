#include "common/Log.h"
#include "common/Files.h"
#include "common/Paths.h"
#include "common/Text.h"
#include <algorithm>
#include <cstdio>
#include <mutex>
#include <windows.h>

namespace putgd::log {
namespace {
struct State {
    std::mutex mutex;
    std::string directory, process, current, oldest, reason, debugExpiry;
    HANDLE file = INVALID_HANDLE_VALUE;
    long long fileBytes = 0, total = 0, dropped = 0, failedOpenMs = 0, retentionBytes = 52428800LL;
    int files = 0, sequence = 0, retentionDays = 14;
    bool degraded = false, opened = false, closed = false, debug = false;
};
State& state() { static State s; return s; }
logformat::Level level(Level l) {
    switch (l) { case Level::Debug: return logformat::Level::Debug; case Level::Info: return logformat::Level::Info; case Level::Warning: return logformat::Level::Warning; default: return logformat::Level::Error; }
}
long long fileTimeMs(const FILETIME& ft) { return (long long)((((unsigned long long)ft.dwHighDateTime << 32) | ft.dwLowDateTime) - 116444736000000000ULL) / 10000; }
// The earlier Windows build's daily files (putgd-yyyymmdd.log): pruned and cleared with the rest.
bool legacyName(const std::string& n) { return n.size() == 18 && text::startsWith(n, "putgd-") && text::endsWith(n, ".log"); }
void fail(State& s, const char* reason) { ++s.dropped; s.degraded = true; s.reason = reason; }
// The folder must be a real directory, owner-only; files inherit its protected DACL.
bool prepareDirectory(State& s) {
    if (!files::makePrivateDirectory(s.directory)) return false;
    const DWORD attributes = GetFileAttributesW(paths::wide(s.directory).c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) && !(attributes & FILE_ATTRIBUTE_REPARSE_POINT);
}
std::string firstRecordTime(const std::string& path) {
    HANDLE h = CreateFileW(paths::wide(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return {};
    char buffer[32768]; DWORD got = 0;
    const BOOL ok = ReadFile(h, buffer, sizeof buffer, &got, nullptr);
    CloseHandle(h);
    if (!ok) return {};
    std::string line(buffer, got);
    line = line.substr(0, line.find('\n'));
    const auto record = json::parseObject(line);
    return record ? record->value("time").toString() : std::string();
}
void enforce(State& s) {
    if (s.directory.empty()) return;
    std::vector<logformat::FileEntry> entries;
    WIN32_FIND_DATAW data;
    HANDLE find = FindFirstFileExW(paths::wide(paths::join(s.directory, "putgd-*")).c_str(), FindExInfoBasic, &data, FindExSearchNameMatch, nullptr, 0);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            if (data.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) continue;
            const auto name = paths::narrow(data.cFileName);
            const long long bytes = ((long long)data.nFileSizeHigh << 32) | data.nFileSizeLow, modified = fileTimeMs(data.ftLastWriteTime);
            if (legacyName(name)) { if (modified < nowMs() - (long long)s.retentionDays * 86400000LL) files::remove(paths::join(s.directory, name)); continue; }
            if (logformat::isFileName(name)) entries.push_back({name, bytes, modified});
        } while (FindNextFileW(find, &data));
        FindClose(find);
    }
    for (const auto& name : logformat::expired(entries, nowMs(), s.retentionDays, s.retentionBytes, s.current)) files::remove(paths::join(s.directory, name));
    // Recount what is left, so status() never reads the folder.
    std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) { return a.name < b.name; });
    s.files = 0; s.total = 0; std::string oldest;
    for (const auto& e : entries) {
        if (!files::isRegularFile(paths::join(s.directory, e.name))) continue;
        ++s.files; s.total += e.name == s.current ? s.fileBytes : e.bytes;
        if (oldest.empty()) oldest = e.name;
    }
    s.oldest = oldest.empty() ? std::string() : firstRecordTime(paths::join(s.directory, oldest));
}
bool openFile(State& s) {
    // After a failed open, retry at most every 5 s rather than on every record.
    const auto now = nowMs();
    if (s.failedOpenMs > 0 && now - s.failedOpenMs < 5000) return false;
    s.failedOpenMs = now;
    if (!prepareDirectory(s)) return false;
    const auto name = logformat::fileName(now, GetCurrentProcessId(), ++s.sequence);
    // CREATE_NEW: never append to, or through, a file that is already there.
    HANDLE h = CreateFileW(paths::wide(paths::join(s.directory, name)).c_str(), FILE_APPEND_DATA | SYNCHRONIZE, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    s.file = h; s.fileBytes = 0; s.failedOpenMs = 0; s.current = name; s.opened = true;
    return true;
}
void closeFile(State& s) { if (s.file != INVALID_HANDLE_VALUE) { FlushFileBuffers(s.file); CloseHandle(s.file); s.file = INVALID_HANDLE_VALUE; } }
bool append(State& s, const std::string& line) {
    if (s.closed) return false;
    if (s.file == INVALID_HANDLE_VALUE && !openFile(s)) { fail(s, "unavailable"); return false; }
    if (s.fileBytes > 0 && s.fileBytes + (long long)line.size() > logformat::rotationLimit(s.retentionBytes)) {
        closeFile(s);
        if (!openFile(s)) { fail(s, "unavailable"); return false; }
        enforce(s);
    }
    DWORD written = 0;
    const BOOL ok = WriteFile(s.file, line.data(), DWORD(line.size()), &written, nullptr);
    if (!ok || written != line.size()) {
        const DWORD error = GetLastError();
        // Never leave half a record: the next one must start on its own line.
        if (written > 0) { FILE_END_OF_FILE_INFO eof{}; eof.EndOfFile.QuadPart = s.fileBytes; if (!SetFileInformationByHandle(s.file, FileEndOfFileInfo, &eof, sizeof eof)) closeFile(s); }
        fail(s, error == ERROR_DISK_FULL || error == ERROR_HANDLE_DISK_FULL ? "disk_full" : "write_failed");
        return false;
    }
    s.fileBytes += (long long)line.size(); s.total += (long long)line.size(); s.degraded = false;
    if (s.files == 0) s.files = 1;
    return true;
}
void put(State& s, const logformat::Record& r) {
    const auto line = logformat::encode(r);
    if (s.directory.empty()) { std::fputs(line.c_str(), stderr); std::fflush(stderr); return; }
    if (append(s, line) && s.oldest.empty()) s.oldest = logformat::iso(r.timeMs);
}
}
long long nowMs() { FILETIME ft; GetSystemTimePreciseAsFileTime(&ft); return fileTimeMs(ft); }
void initialize(const std::string& directory, const std::string& process, int days, long long bytes) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    s.directory = directory; s.process = process; s.retentionDays = days; s.retentionBytes = bytes;
    if (directory.empty()) return;
    if (!openFile(s)) fail(s, "unavailable");
    enforce(s);
}
void setRetention(int days, long long bytes) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    s.retentionDays = days; s.retentionBytes = bytes;
    enforce(s);
}
void cleanup() { auto& s = state(); std::lock_guard lock(s.mutex); enforce(s); }
void setDebugEnabled(bool enabled, const std::string& expiresAt) { auto& s = state(); std::lock_guard lock(s.mutex); s.debug = enabled; s.debugExpiry = enabled ? expiresAt : std::string(); }
bool debugEnabled() { auto& s = state(); std::lock_guard lock(s.mutex); return s.debug; }
std::string directory() { auto& s = state(); std::lock_guard lock(s.mutex); return s.directory; }
std::string currentFile() { auto& s = state(); std::lock_guard lock(s.mutex); return s.current.empty() ? std::string() : paths::join(s.directory, s.current); }
void write(Level l, const std::string& category, const std::string& message, const json::Object& fields) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    if (l == Level::Debug && !s.debug) return;
    logformat::Record r; r.timeMs = nowMs(); r.level = level(l); r.category = category; r.process = s.process; r.pid = GetCurrentProcessId(); r.message = message; r.fields = fields;
    put(s, r);
}
void writeRecord(logformat::Record r) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    if (r.level == logformat::Level::Debug && !s.debug) return;
    put(s, r);
}
json::Object clearAll() {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    closeFile(s);
    int removed = 0; long long bytes = 0;
    for (const auto& name : files::list(s.directory)) {
        if (!logformat::isFileName(name) && !legacyName(name)) continue;
        const auto path = paths::join(s.directory, name);
        const auto size = files::size(path);
        if (files::remove(path)) { ++removed; bytes += size > 0 ? size : 0; }
    }
    s.current.clear(); s.files = 0; s.total = 0; s.oldest.clear(); s.failedOpenMs = 0;
    if (openFile(s)) {
        logformat::Record r; r.timeMs = nowMs(); r.category = "putgd.app"; r.process = s.process; r.pid = GetCurrentProcessId(); r.message = "Logs cleared";
        r.fields = {{"removedFiles", removed}, {"removedBytes", double(bytes)}};
        put(s, r);
        enforce(s);
    } else fail(s, "unavailable");
    return {{"removedFiles", removed}, {"removedBytes", double(bytes)}};
}
json::Object status() {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    return {{"available", !s.directory.empty() && s.opened}, {"directory", s.directory}, {"files", s.files}, {"bytes", double(s.total)},
        {"oldestRecord", s.oldest.empty() ? json::Value() : json::Value(s.oldest)}, {"degraded", s.degraded}, {"degradedReason", s.degraded ? json::Value(s.reason) : json::Value()},
        {"droppedRecords", double(s.dropped)}, {"retention", json::Object{{"days", s.retentionDays}, {"bytes", double(s.retentionBytes)}}},
        {"rotationBytes", double(logformat::rotationLimit(s.retentionBytes))}, {"debugLogging", s.debug},
        {"debugLoggingExpiresAt", s.debugExpiry.empty() ? json::Value() : json::Value(s.debugExpiry)}};
}
void close() { auto& s = state(); std::lock_guard lock(s.mutex); closeFile(s); s.closed = true; }
}
