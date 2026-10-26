#include "resident/SettingsService.h"
#include "common/Files.h"
#include "common/Log.h"
#include "common/Paths.h"
#include "common/Sha256.h"
#include "core/SettingsPatch.h"
#include "resident/Resident.h"
#include <windows.h>

namespace putgd {
namespace {
constexpr size_t maximumBytes = 1024 * 1024;
std::string revisionOf(const std::string& bytes) { return crypto::Sha256::hexDigest(bytes); }
// A cooperative lock beside the file: created exclusively, holds our pid, and
// counts as stale once that process is gone or the file is older than 30 s.
class LockFile {
public:
    explicit LockFile(const std::string& path) : path_(path) {}
    bool acquire(int timeoutMs) {
        const ULONGLONG start = GetTickCount64();
        while (true) {
            HANDLE h = CreateFileW(paths::wide(path_).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
            if (h != INVALID_HANDLE_VALUE) { const auto pid = std::to_string(GetCurrentProcessId()) + "\n"; DWORD n; WriteFile(h, pid.data(), DWORD(pid.size()), &n, nullptr); handle_ = h; return true; }
            if (stale()) { files::remove(path_); continue; }
            if (GetTickCount64() - start > ULONGLONG(timeoutMs)) return false;
            Sleep(25);
        }
    }
    ~LockFile() { if (handle_) CloseHandle(handle_); }
private:
    bool stale() const {
        const auto content = files::read(path_, 64);
        if (!content) return false;
        const auto pid = std::strtoul(content->c_str(), nullptr, 10);
        if (pid == 0) return true;
        HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, DWORD(pid));
        if (!process) return GetLastError() == ERROR_INVALID_PARAMETER;
        DWORD code = 0; const bool running = GetExitCodeProcess(process, &code) && code == STILL_ACTIVE;
        CloseHandle(process);
        return !running;
    }
    std::string path_;
    HANDLE handle_ = nullptr;
};
}
SettingsService::SettingsService(std::string path, ProfileValidator profile, CredentialValidator credentials)
    : path_(std::move(path)), validateProfile_(std::move(profile)), validateCredentials_(std::move(credentials)) {
    document_ = Settings::defaults(paths::documentsFolder());
    settings_ = Settings::fromValidated(document_);
}
SettingsService::~SettingsService() { stopWatching(); }
Error SettingsService::validate(const json::Object& document) const { return Settings::validate(document, validateProfile_, validateCredentials_); }
SettingsService::Loaded SettingsService::read() const {
    Loaded loaded;
    loaded.exists = files::isRegularFile(path_);
    if (!loaded.exists) return loaded;
    const auto bytes = files::read(path_, maximumBytes);
    if (!bytes) { loaded.error = {"config_invalid", "Configuration file cannot be read or exceeds 1 MiB.", {{"path", "/"}}}; return loaded; }
    loaded.revision = revisionOf(*bytes);
    json::ParseError parseError;
    const auto value = json::parse(*bytes, &parseError);
    if (parseError.failed || !value.isObject()) { loaded.error = {"config_invalid", "Configuration is not a JSON object.", {{"path", "/"}}}; return loaded; }
    loaded.candidate = value.toObject();
    loaded.error = validate(loaded.candidate);
    return loaded;
}
Result SettingsService::accept(const Loaded& loaded) {
    Result result;
    observedRevision_ = loaded.revision;
    lastError_ = loaded.error;
    if (loaded.error) { log::warning("settings", "Configuration rejected", {{"path", loaded.error.details.value("path")}, {"code", loaded.error.code}}); result.error = loaded.error; return result; }
    if (loaded.revision != acceptedRevision_) {
        document_ = Settings::normalized(loaded.candidate);
        settings_ = Settings::fromValidated(document_);
        acceptedRevision_ = loaded.revision;
        result.changed = true;
        for (const auto& callback : changed_) callback();
    }
    return result;
}
Result SettingsService::load() {
    auto loaded = read();
    if (!loaded.exists) {
        firstStart_ = true;
        files::makePrivateDirectory(paths::parent(path_));
        const auto bytes = json::pretty(json::Value(Settings::defaults(paths::documentsFolder())));
        if (auto error = files::writeAtomically(path_, bytes)) { lastError_ = error; return {error}; }
        loaded = read();
    }
    return accept(loaded);
}
Result SettingsService::reload() { return accept(read()); }
Error SettingsService::write(const json::Object& candidate, const std::string& expectedRevision) {
    if (auto error = validate(candidate)) return error;
    LockFile lock(path_ + ".lock");
    if (!lock.acquire(2000)) return {"config_busy", "Another writer holds the configuration lock; retry", {}};
    const auto current = read();
    const auto actual = current.exists ? current.revision : std::string();
    if (actual != expectedRevision) return {"config_conflict", "Configuration changed since it was read; reread and retry", {{"observedRevision", actual}}};
    const auto bytes = json::pretty(json::Value(Settings::normalized(candidate)));
    if (bytes.size() > maximumBytes) return {"config_invalid", "Configuration exceeds 1 MiB.", {{"path", "/"}}};
    return files::writeAtomically(path_, bytes);
}
Result SettingsService::save(const json::Object& document, const std::string& expectedRevision) {
    if (auto error = write(document, expectedRevision)) return {error};
    return reload();
}
Result SettingsService::patch(const json::Object& patch, const std::string& expectedRevision) {
    const auto current = read();
    const auto base = current.exists && !current.error ? current.candidate : document_;
    return save(settings::applyPatch(base, patch), expectedRevision);
}
json::Object SettingsService::metadata() const {
    return {{"path", path_}, {"schemaVersion", 1}, {"acceptedRevision", acceptedRevision_}, {"observedRevision", observedRevision_}, {"watching", watching_},
        {"fallbackRescanMs", 2000}, {"validation", json::Object{{"ok", !lastError_}, {"error", lastError_.toJson()}}}};
}
void SettingsService::startWatching() {
    if (watcher_.joinable()) return;
    stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    const auto directory = paths::parent(path_);
    const auto name = paths::wide(paths::fileName(path_));
    watching_ = true;
    watcher_ = std::thread([this, directory, name] {
        HANDLE dir = CreateFileW(paths::wide(directory).c_str(), FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
        OVERLAPPED overlapped{}; overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        std::vector<char> buffer(64 * 1024);
        HANDLE waits[2] = {stopEvent_, overlapped.hEvent};
        bool armed = false;
        while (!stopping_) {
            if (dir != INVALID_HANDLE_VALUE && !armed) {
                ResetEvent(overlapped.hEvent);
                armed = ReadDirectoryChangesW(dir, buffer.data(), DWORD(buffer.size()), FALSE, FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE, nullptr, &overlapped, nullptr) != FALSE;
                if (!armed) Loop::instance().post([this] { watching_ = false; });
            }
            const DWORD waited = WaitForMultipleObjects(armed ? 2 : 1, waits, FALSE, armed ? INFINITE : 2000);
            if (waited == WAIT_OBJECT_0) break;
            bool relevant = !armed; // fallback rescan every two seconds without a watcher
            if (armed && waited == WAIT_OBJECT_0 + 1) {
                DWORD bytes = 0;
                if (GetOverlappedResult(dir, &overlapped, &bytes, FALSE) && bytes > 0) {
                    size_t offset = 0;
                    while (true) {
                        auto* info = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(buffer.data() + offset);
                        const std::wstring changed(info->FileName, info->FileNameLength / sizeof(wchar_t));
                        if (_wcsicmp(changed.c_str(), name.c_str()) == 0 || changed.rfind(L".putgd-", 0) == 0) relevant = true;
                        if (!info->NextEntryOffset) break;
                        offset += info->NextEntryOffset;
                    }
                } else relevant = true;
                armed = false;
            }
            if (relevant) Loop::instance().post([this] {
                if (stopping_) return;
                // Debounce bursts from atomic replacement: one reload 75 ms after the last change.
                if (debounceTimer_) Loop::instance().stopTimer(debounceTimer_);
                debounceTimer_ = Loop::instance().startTimer(75, [this] { debounceTimer_ = 0; reload(); }, false);
            });
        }
        if (dir != INVALID_HANDLE_VALUE) { CancelIoEx(dir, &overlapped); CloseHandle(dir); }
        CloseHandle(overlapped.hEvent);
    });
}
void SettingsService::stopWatching() {
    stopping_ = true;
    if (stopEvent_) SetEvent(stopEvent_);
    if (watcher_.joinable()) watcher_.join();
    if (stopEvent_) { CloseHandle(stopEvent_); stopEvent_ = nullptr; }
    watching_ = false;
}
}
