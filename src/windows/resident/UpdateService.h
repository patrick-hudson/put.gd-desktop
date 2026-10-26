#pragma once
// The daily update check (docs/updates.md): one GET of the channel feed in a
// transient child (`putgd --check-update CHANNEL`), a saved result and a
// one-time notification per newer version. Windows has no package to install,
// so update.install reports update_check_unavailable and the result links to
// the release page.
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
    std::function<void(const json::Object& result)> onUpdateAvailable; // owner thread, once per version
    static int childMain(const std::vector<std::string>& args); // --check-update CHANNEL
    static int compareVersions(const std::string& a, const std::string& b);
    static std::string buildChannel();
private:
    std::string channel() const;
    void load();
    void save() const;
    void schedule();
    std::string root_, statePath_;
    SettingsService& settings_;
    json::Object result_, notified_;
    Error lastError_;
    std::string attemptedAt_, checkedAt_, nextCheckAt_;
    bool checking_ = false;
    unsigned long long timer_ = 0;
};
}
