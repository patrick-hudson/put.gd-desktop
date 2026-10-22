#include "resident/SetupService.h"
#include "common/Log.h"
#include "common/Uuid.h"
#include "resident/Resident.h"

namespace putgd {
SetupService::SetupService(SettingsService& settings, CredentialStore& credentials, ProviderRegistry& providers, OperationService& operations)
    : settings_(settings), credentials_(credentials), providers_(providers), operations_(operations) {}
Error SetupService::resolveProfile(const std::string& profileId, const std::string& expectedRevision, json::Object& profile) const {
    if (expectedRevision != settings_.revision() || expectedRevision != settings_.observedRevision())
        return {"config_conflict", "Configuration changed since it was read; reread and retry", {{"observedRevision", settings_.observedRevision()}}};
    const auto snapshot = settings_.snapshot();
    const auto it = snapshot.profiles.find(profileId);
    if (it == snapshot.profiles.end()) return {"profile_not_found", "No configured destination has this identifier", {{"profileId", profileId}}};
    profile = it->second;
    return {};
}
void SetupService::finish(const std::string& id, Error error, json::Object result) {
    auto found = jobs_.find(id);
    if (found == jobs_.end() || found->second.value("state") != "running") return;
    auto& record = found->second;
    record.remove("event");
    record["result"] = result; record["state"] = error ? "failed" : "completed"; record["error"] = error.toJson();
    active_.erase(record.value("profileId").toString() + "/" + record.value("kind").toString());
    if (!error && record.value("kind") == "testDestination" && onDestinationTested) onDestinationTested(record.value("profileId").toString());
    completed_.push_back(id);
    while (completed_.size() > 64) { jobs_.erase(completed_.front()); completed_.pop_front(); }
    if (onChanged) onChanged(id);
}
void SetupService::start(const std::string& kind, const std::string& profileId, const std::string& expectedRevision, const json::Object& input, JsonReply reply) {
    json::Object profile;
    if (auto e = resolveProfile(profileId, expectedRevision, profile)) { reply(e, {}); return; }
    const auto provider = providers_.find(profile.value("provider").toString());
    if (!provider) { reply({"unknown_provider", "No registered storage provider has this identifier", {}}, {}); return; }
    const bool accountSetup = kind == "connectAccount" || kind == "inspectAccount" || kind == "createFolder" || kind == "storeSharePassword";
    if (!accountSetup && kind != "storeCredentials" && kind != "unlockCredentials" && kind != "testDestination") { reply({"invalid_request", "Unknown setup kind", {}}, {}); return; }
    if (kind == "createFolder" && (input.size() != 1 || !input.value("name").isString() || input.value("name").stringRef().empty() || input.value("name").stringRef().size() > 255)) { reply({"invalid_request", "Invalid setup fields or parameter types", {}}, {}); return; }
    if (kind == "storeSharePassword" && (input.size() != 2 || !input.value("password").isString() || input.value("password").stringRef().size() > 4096 || !input.value("reference").isString() || input.value("reference").stringRef().empty() || input.value("reference").stringRef().size() > 256)) { reply({"invalid_request", "Invalid setup fields or parameter types", {}}, {}); return; }
    if ((kind == "connectAccount" || kind == "inspectAccount") && !input.empty()) { reply({"invalid_request", "Invalid setup fields or parameter types", {}}, {}); return; }
    if (accountSetup && !provider->available()) { reply({"setup_unavailable", "Account setup is unavailable: " + provider->unavailableReason(), {{"kind", kind}, {"reason", provider->unavailableReason()}}}, {}); return; }
    if (active_.count(profileId + "/" + kind)) { reply({"setup_busy", "A setup job of this kind is already running for this destination", {}}, {}); return; }
    int running = 0; for (const auto& pair : jobs_) if (pair.second.value("state") == "running") ++running;
    if (running >= 32) { reply({"setup_busy", "Too many active setup jobs", {}}, {}); return; }
    const auto id = uuid::create();
    jobs_[id] = {{"setupId", id}, {"kind", kind}, {"profileId", profileId}, {"state", "running"}, {"error", Error{}.toJson()}};
    active_.insert(profileId + "/" + kind);
    reply({}, {{"setupId", id}});
    const auto credentialsField = profile.value("credentials").toObject();
    const bool keychain = credentialsField.value("mode") == "keychain";
    const auto reference = credentialsField.value("reference").toString();
    if (accountSetup) {
        // Provider account actions run off the owner thread; the record only ever holds the safe job id,
        // transient UI events (the sign-in URL) and the provider's non-secret result.
        Destination destination{profile.value("provider").toString(), profileId, expectedRevision, profile.value("settings").toObject(), credentialsField};
        auto event = [this, id](const json::Object& value) {
            Loop::instance().post([this, id, value] { auto found = jobs_.find(id); if (found != jobs_.end() && found->second.value("state") == "running") { found->second["event"] = value; if (onChanged) onChanged(id); } });
        };
        operations_.background([provider, destination, kind, input, event](Error& error) { return provider->setup(kind, destination, input, error, event); },
            [this, id](Error error, json::Object result) { finish(id, error, result); });
        return;
    }
    if (kind == "storeCredentials") {
        Credentials c{input.value("accessKey").toString(), input.value("secretKey").toString(), input.value("sessionToken").toString()};
        if (c.accessKey.empty() || c.secretKey.empty() || c.accessKey.size() > 256 || c.secretKey.size() > 512 || c.sessionToken.size() > 4096) { finish(id, {"credentials_invalid", "Enter an access key ID and secret access key", {}}, {}); return; }
        Error error;
        if (keychain) error = credentials_.save(profileId, reference, c); else credentials_.setSession(profileId, c);
        finish(id, error, {{"stored", !error}, {"mode", keychain ? "keychain" : "session"}});
        return;
    }
    if (kind == "unlockCredentials") {
        if (!keychain) { finish(id, {"credential_setup_required", "Session credentials must be entered again after a restart", {}}, {}); return; }
        Loop::instance().background([this, id, profileId, reference] {
            const auto error = credentials_.unlock(profileId, reference);
            Loop::instance().post([this, id, error] { finish(id, error, {{"unlocked", !error}}); });
        });
        return;
    }
    // testDestination: a synthetic upload, public GET and guarded cleanup in the provider.
    Destination destination{profile.value("provider").toString(), profileId, settings_.revision(), profile.value("settings").toObject(), credentialsField};
    if (auto e = provider->readiness(destination)) { finish(id, e, {}); return; }
    operations_.background([provider, destination, this, id](Error& error) {
        auto checkpoint = [this, id](const json::Object& progress) -> Error {
            // Setup checkpoints are process-lifetime records (docs/ui-process-protocol.md).
            Loop::instance().post([this, id, progress] { if (jobs_.count(id)) jobs_[id]["progress"] = progress; });
            return {};
        };
        return provider->testDestination(destination, checkpoint, error);
    }, [this, id](Error error, json::Object result) {
        if (!error && result.value("status") == "ok") result["testScope"] = "upload-public-get-cleanup";
        finish(id, error, result);
    });
}
void SetupService::testNow(const std::string& profileId, std::function<void(Error)> done) {
    const auto snapshot = settings_.snapshot();
    const auto it = snapshot.profiles.find(profileId);
    if (it == snapshot.profiles.end()) { done({"profile_not_found", "No configured destination has this identifier", {{"profileId", profileId}}}); return; }
    const auto provider = providers_.find(it->second.value("provider").toString());
    if (!provider) { done({"unknown_provider", "No registered storage provider has this identifier", {}}); return; }
    Destination destination{it->second.value("provider").toString(), profileId, settings_.revision(), it->second.value("settings").toObject(), it->second.value("credentials").toObject()};
    if (auto e = provider->readiness(destination)) { done(e); return; }
    operations_.background([provider, destination](Error& error) { return provider->testDestination(destination, [](const json::Object&) -> Error { return {}; }, error); },
        [this, profileId, done](Error error, json::Object) {
            if (!error && onDestinationTested) onDestinationTested(profileId);
            done(error);
        });
}
void SetupService::inspect(const std::string& id, JsonReply reply) {
    const auto it = jobs_.find(id);
    if (it == jobs_.end()) { reply({"setup_not_found", "No setup job has this identifier", {}}, {}); return; }
    reply({}, it->second);
}
void SetupService::credentialHint(const std::string& profileId, const std::string& expectedRevision, JsonReply reply) {
    json::Object profile;
    if (auto e = resolveProfile(profileId, expectedRevision, profile)) { reply(e, {}); return; }
    const auto credentialsField = profile.value("credentials").toObject();
    const bool keychain = credentialsField.value("mode") == "keychain";
    const auto cached = credentials_.cached(profileId);
    std::string prefix;
    if (cached && cached->accessKey.size() > 4) prefix = cached->accessKey.substr(0, 4) + "…";
    else if (cached) prefix = "…";
    reply({}, {{"profileId", profileId}, {"mode", keychain ? "keychain" : "session"}, {"available", cached.has_value()}, {"stored", keychain && credentials_.stored(credentialsField.value("reference").toString())},
        {"accessKeyPrefix", prefix}, {"hasSecret", cached && !cached->secretKey.empty()}, {"hasSessionToken", cached && !cached->sessionToken.empty()}});
}
void SetupService::revealCredentialSecret(const std::string& profileId, const std::string& expectedRevision, JsonReply reply) {
    json::Object profile;
    if (auto e = resolveProfile(profileId, expectedRevision, profile)) { reply(e, {}); return; }
    const auto cached = credentials_.cached(profileId);
    if (!cached) { reply({"credential_setup_required", "Unlock the saved credentials first", {}}, {}); return; }
    reply({}, {{"profileId", profileId}, {"secretKey", cached->secretKey}});
}
std::vector<std::string> SetupService::unlockSavedCredentials() {
    std::vector<std::string> started;
    if (startupUnlockDone_) return started;
    startupUnlockDone_ = true;
    const auto snapshot = settings_.snapshot();
    if (!snapshot.unlockCredentialsAtStartup) return started;
    for (const auto& pair : snapshot.profiles) {
        if (pair.second.value("provider") != "s3" || pair.second.value("credentials").toObject().value("mode") != "keychain" || credentials_.cached(pair.first)) continue;
        start("unlockCredentials", pair.first, settings_.revision(), {}, [&](Error error, json::Object result) { if (!error) started.push_back(result.value("setupId").toString()); });
    }
    return started;
}
}
