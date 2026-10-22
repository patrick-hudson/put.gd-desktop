#pragma once
// The resident's diagnostics service (docs/diagnostics.md): diagnostics.bundle
// (preview, save, notify, upload with copyLink), diagnostics.uploadTarget,
// diagnostics.reveal, and the settings it applies: log retention, the 24-hour
// debug-logging limit and the crash-dump opt-in. The package's
// contents and redaction are core/DiagnosticsPackage.h; this gathers the inputs
// on the owner thread, builds and writes the package on a background thread,
// and uploads it through OperationService::uploadDiagnostics.
#include "core/DiagnosticsPackage.h"
#include "resident/OperationService.h"
#include "resident/SettingsService.h"
#include "resident/SetupService.h"
#include "resident/Tray.h"
#include <map>
#include <set>

namespace putgd {
class DiagnosticsService {
public:
    DiagnosticsService(std::string dataRoot, SettingsService&, OperationService&, SetupService&, Tray&);
    // The full `diagnostics` method output, for diagnostics.json.
    std::function<json::Object()> document;
    void bundle(const json::Object& params, JsonReply);
    void uploadTarget(const json::Object& params, JsonReply);
    void reveal(const json::Object& params, JsonReply);
    // A Test connection passed: an earlier failed upload no longer blocks this destination.
    void destinationTested(const std::string& profileId);
    // Retention, debug-logging expiry and crash-dump registration from the settings.
    void applySettings();
    json::Object crashDumps() const;
    bool trayAvailable() const;
private:
    void check(const std::string& profileId, std::function<void(Error, json::Object)>);
    void upload(json::Object result, const diagnostics::Request&, JsonReply);
    void notifySaved(const json::Object& result);
    std::string root_;
    SettingsService& settings_;
    OperationService& operations_;
    SetupService& setup_;
    Tray& tray_;
    struct Check { std::string revision; long long at = 0; };
    std::map<std::string, Check> checked_;
    std::map<std::string, long long> tested_;
    std::set<std::string> saved_; // lower-case paths saved by this resident
    bool running_ = false, debugOffPending_ = false;
    UINT_PTR debugTimer_ = 0;
};
}
