#include "resident/RunMarker.h"
#include "common/Crash.h"
#include "common/Files.h"
#include "common/Log.h"
#include "common/Paths.h"
#include "common/Text.h"
#include "core/UncleanExit.h"
#include <mutex>
#include <windows.h>

namespace putgd::runmarker {
namespace {
std::mutex mutex;
std::string root;
json::Object current;
HANDLE crashFile = INVALID_HANDLE_VALUE;
std::string path(const char* name) { return paths::join(root, name); }
std::string readSmall(const std::string& p, size_t limit) { return files::read(p, limit).value_or(std::string()); }
// The marker's pid is a running putgd.exe that started when the marker says.
bool alive(const json::Object& marker) {
    const auto pid = DWORD(marker.value("pid").toInteger(0));
    if (!pid || pid == GetCurrentProcessId()) return false;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return false;
    wchar_t image[MAX_PATH]; DWORD length = MAX_PATH;
    bool same = QueryFullProcessImageNameW(process, 0, image, &length) && text::lower(paths::fileName(paths::narrow(std::wstring(image, length)))) == "putgd.exe";
    FILETIME created, exited, kernel, user;
    if (same && GetProcessTimes(process, &created, &exited, &kernel, &user)) {
        const auto createdMs = (long long)((((unsigned long long)created.dwHighDateTime << 32) | created.dwLowDateTime) - 116444736000000000ULL) / 10000;
        const auto started = files::parseIsoUtcMs(marker.value("started").toString());
        // A reused pid belongs to a process created at another time.
        same = started < 0 || (createdMs <= started + 1000 && createdMs >= started - 120000);
    }
    CloseHandle(process);
    return same;
}
}
std::string bootId() {
    DWORD value = 0, size = sizeof value;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Memory Management\\PrefetchParameters", L"BootId", RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS)
        return "boot-" + std::to_string(value);
    const auto booted = log::nowMs() - (long long)GetTickCount64();
    return "booted-" + files::isoUtcFromMs(booted / 60000 * 60000).substr(0, 16);
}
void begin(const std::string& dataRoot, const std::string& version) {
    std::lock_guard lock(mutex);
    root = dataRoot;
    files::makePrivateDirectory(root);
    unclean::Previous previous;
    previous.present = files::isRegularFile(path("running.json"));
    previous.marker = json::parseObject(readSmall(path("running.json"), 64 * 1024)).value_or(json::Object{});
    previous.note = readSmall(path("running.signal"), 64 * 1024);
    previous.crashText = readSmall(path("running.crash"), 256 * 1024);
    previous.alive = previous.present && alive(previous.marker);
    auto recent = json::parse(readSmall(path("unclean-exits.json"), 256 * 1024)).toArray();
    const auto boot = bootId();
    const auto record = unclean::evaluate(previous, boot, files::nowIsoUtc());
    current = {{"detected", !record.empty()}, {"last", record.empty() ? json::Value() : json::Value(record)}};
    if (!record.empty()) {
        recent = unclean::remember(recent, record);
        files::writeAtomically(path("unclean-exits.json"), json::serialize(recent));
        log::write(unclean::level(record) == logformat::Level::Error ? log::Level::Error : unclean::level(record) == logformat::Level::Info ? log::Level::Info : log::Level::Warning,
                   "app", unclean::message(record), record);
    }
    current["recent"] = recent;
    // A clean start: no stale note or crash record survives into this run.
    files::remove(path("running.signal"));
    files::writeAtomically(path("running.json"), json::serialize(unclean::marker(GetCurrentProcessId(), files::nowIsoUtc(), version, boot)));
    // Opened now, so the crash filter only ever calls WriteFile.
    if (crashFile != INVALID_HANDLE_VALUE) CloseHandle(crashFile);
    crashFile = CreateFileW(paths::wide(path("running.crash")).c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    crash::setOutput(crashFile);
}
void note(const std::string& event, const std::string& detail) {
    std::lock_guard lock(mutex);
    if (root.empty()) return;
    files::writeAtomically(path("running.signal"), unclean::note(event, detail, files::nowIsoUtc()));
}
void end() {
    std::lock_guard lock(mutex);
    if (root.empty()) return;
    crash::setOutput(INVALID_HANDLE_VALUE);
    if (crashFile != INVALID_HANDLE_VALUE) { CloseHandle(crashFile); crashFile = INVALID_HANDLE_VALUE; }
    files::remove(path("running.json")); files::remove(path("running.signal")); files::remove(path("running.crash"));
    root.clear();
}
json::Object report() {
    std::lock_guard lock(mutex);
    return current.empty() ? json::Object{{"detected", false}, {"last", json::Value()}, {"recent", json::Array{}}} : current;
}
}
