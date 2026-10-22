#pragma once
// The daily update check (docs/updates.md): one GET of the channel feed in a
// transient child (`putgd --check-update CHANNEL`), a saved result and a
// one-time notification per newer version. update.install downloads and checks
// a tagged build's package in another child (resident/UpdateInstall.h): a
// portable folder has its files replaced, an MSI install stages the new MSI.
// update.restart then starts the new version (portable) or runs the MSI from a
// copy of putgd.exe that starts put.gd again afterwards.
#include "common/Error.h"
#include "resident/SettingsService.h"
#include <functional>
#include <string>

namespace putgd {
class UpdateService {
public:
    UpdateService(std::string dataRoot, SettingsService&);
    void start();
    json::Object status() const;
    // update.check: manual check now; replies with status() or the check error.
    void check(bool manual, std::function<void(Error, json::Object)> reply);
    // update.install {confirm, channel?, method?} and update.restart {confirm}: reply with status() or the refusal.
    void install(const json::Object& params, std::function<void(Error, json::Object)> reply);
    void restart(const json::Object& params, std::function<void(Error, json::Object)> reply);
    std::function<std::string()> busy;  // why an install or restart must wait (an upload runs), or empty
    std::function<void()> quit;         // quits the resident after a restart has been handed off
    std::function<void()> onChanged;    // install progress and results
    std::function<void(const json::Object& result)> onUpdateAvailable; // owner thread, once per version
    static int childMain(const std::vector<std::string>& args); // --check-update CHANNEL
    static int compareVersions(const std::string& a, const std::string& b);
    static std::string buildChannel();
private:
    std::string channel() const;
    std::string installUnavailable() const;
    bool restartRequired() const;
    json::Object installStatus() const;
    void installFinished(int exitCode, const std::string& outputPath);
    void finishRestartedUpdate();
    void load();
    void save() const;
    void schedule();
    std::string root_, statePath_;
    SettingsService& settings_;
    json::Object result_, notified_;
    Error lastError_;
    std::string attemptedAt_, checkedAt_, nextCheckAt_;
    bool checking_ = false, installing_ = false;
    unsigned long long timer_ = 0, progressTimer_ = 0;
    json::Object install_, installState_; // the running/last install; update-install.json
};
}
