#pragma once
// The authoritative configuration: one config.json under the data root, a
// content-hash revision, cooperative lock file, atomic replacement, a directory
// watcher with a 75 ms debounce and a 2 s rescan fallback (docs/configuration.md).
#include "core/Settings.h"
#include <functional>
#include <memory>
#include <string>
#include <thread>

namespace putgd {
class SettingsService {
public:
    SettingsService(std::string path, ProfileValidator, CredentialValidator);
    ~SettingsService();
    // Loads the file (or writes defaults on first start). Invalid files keep defaults in memory.
    Result load();
    const json::Object& document() const { return document_; }
    Settings snapshot() const { return settings_; }
    std::string revision() const { return acceptedRevision_; }
    std::string observedRevision() const { return observedRevision_; }
    json::Object metadata() const;
    Error lastError() const { return lastError_; }
    bool firstStart() const { return firstStart_; }
    const std::string& path() const { return path_; }
    // Mutations: a stale expected revision returns config_conflict without writing.
    Result save(const json::Object& document, const std::string& expectedRevision);
    Result patch(const json::Object& patch, const std::string& expectedRevision);
    Result reload();
    Error validate(const json::Object& document) const;
    // Accepted-document observers (shortcuts, startup, tray) run on the owner thread.
    void onChanged(std::function<void()> callback) { changed_.push_back(std::move(callback)); }
    void startWatching();
    void stopWatching();
private:
    struct Loaded { json::Object candidate; std::string revision; Error error; bool exists = false; };
    Loaded read() const;
    Result accept(const Loaded&);
    Error write(const json::Object& candidate, const std::string& expectedRevision);
    std::string path_;
    ProfileValidator validateProfile_;
    CredentialValidator validateCredentials_;
    json::Object document_;
    Settings settings_;
    std::string acceptedRevision_, observedRevision_;
    Error lastError_;
    bool firstStart_ = false, watching_ = false, stopping_ = false, pendingReload_ = false;
    std::vector<std::function<void()>> changed_;
    std::thread watcher_;
    void* stopEvent_ = nullptr;
    unsigned long long debounceTimer_ = 0;
};
}
