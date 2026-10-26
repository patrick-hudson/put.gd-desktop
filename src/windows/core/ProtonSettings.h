#pragma once
// The portable Proton Drive destination rules (docs/proton-drive.md): the exact
// settings shape the Linux provider accepts, node UID syntax, the publication
// identity key and the retry-deadline adoption rule. No IO, so the contract tests
// cover it on Linux.
#include "common/Error.h"
#include <optional>
#include <string>

namespace putgd::proton {
inline constexpr int maximumRequestTimeoutMs = 86400000;
inline constexpr int defaultRequestTimeoutMs = 120000;
json::Object defaults();
// "<node>~<node>": each part 88–108 base64 characters or a 22-character short id.
bool nodeUid(const std::string&);
Error validateSettings(const json::Object& settings);
Error validateCredentials(const json::Object& credentials);
// sha256 over the JSON array [accountRootUid, folderUid, name] (the Linux key).
std::string serializationKey(const json::Object& settings, const json::Object& objectReference, const std::string& candidate);
// Explicit Retry may adopt a longer current deadline when the accepted destination
// and the current profile still name the same provider, account, folder and credentials.
std::optional<int> retryRequestTimeout(const std::string& acceptedProfileId, const std::string& acceptedProviderId, const json::Object& acceptedCredentials,
                                       const json::Object& acceptedSettings, const json::Object& currentProfile);
}
