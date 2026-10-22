#pragma once
// Setup jobs (docs/ui-process-protocol.md): credential storage and unlock,
// destination tests, credential hints and the explicit secret reveal. Jobs are
// process-lifetime status records that never contain credential inputs.
#include "resident/CredentialStore.h"
#include "resident/OperationService.h"
#include <deque>

namespace putgd {
class SetupService {
public:
    SetupService(SettingsService&, CredentialStore&, ProviderRegistry&, OperationService&);
    void start(const std::string& kind, const std::string& profileId, const std::string& expectedRevision, const json::Object& input, JsonReply);
    void inspect(const std::string& id, JsonReply);
    void credentialHint(const std::string& profileId, const std::string& expectedRevision, JsonReply);
    void revealCredentialSecret(const std::string& profileId, const std::string& expectedRevision, JsonReply);
    std::vector<std::string> unlockSavedCredentials();
    // Test connection with the current settings and the credentials already loaded,
    // outside the setup jobs (the diagnostics upload's working check).
    void testNow(const std::string& profileId, std::function<void(Error)> done);
    std::function<void(const std::string& setupId)> onChanged;
    // A Test connection passed for this destination (Settings or testNow).
    std::function<void(const std::string& profileId)> onDestinationTested;
private:
    Error resolveProfile(const std::string& profileId, const std::string& expectedRevision, json::Object& profile) const;
    void finish(const std::string& id, Error, json::Object result);
    SettingsService& settings_;
    CredentialStore& credentials_;
    ProviderRegistry& providers_;
    OperationService& operations_;
    std::map<std::string, json::Object> jobs_;
    std::deque<std::string> completed_;
    std::set<std::string> active_;
    bool startupUnlockDone_ = false;
};
}
