#pragma once
// Secrets live in Windows Credential Manager (CredWrite/CredRead, generic
// credentials under "putgd/<reference>"), the OS secret store; session-mode
// credentials stay only in this process. Configuration holds references only.
#include "common/Error.h"
#include <map>
#include <mutex>
#include <optional>
#include <string>

namespace putgd {
struct Credentials {
    std::string accessKey, secretKey, sessionToken;
    bool empty() const { return accessKey.empty() && secretKey.empty(); }
};
class CredentialStore {
public:
    // Writes the referenced record and caches it only after the store accepted it.
    Error save(const std::string& profileId, const std::string& reference, const Credentials&);
    // Reads the referenced record into the cache. Errors: credential_store_unavailable, credential_missing, credential_denied.
    Error unlock(const std::string& profileId, const std::string& reference);
    void setSession(const std::string& profileId, const Credentials&);
    std::optional<Credentials> cached(const std::string& profileId) const;
    void forget(const std::string& profileId);
    Error remove(const std::string& reference);
    // Non-secret presence check of a stored record, without caching it.
    bool stored(const std::string& reference) const;
    json::Object diagnostics() const;
private:
    mutable std::mutex mutex_;
    std::map<std::string, Credentials> cache_;
    unsigned long long generation_ = 0;
};
}
