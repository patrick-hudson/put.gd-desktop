#include "core/ProtonSettings.h"
#include "common/Sha256.h"
#include "common/Text.h"
#include <algorithm>

namespace putgd::proton {
namespace {
bool nodePart(std::string_view part) {
    const bool shortId = part.size() == 22, longId = part.size() >= 88 && part.size() <= 108;
    if (!shortId && !longId) return false;
    for (const char c : part) {
        const bool alnum = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
        if (!alnum && c != '_' && c != '-' && !(longId && c == '=')) return false;
    }
    return true;
}
bool controlCharacters(const std::string& s) { for (const unsigned char c : s) if (c < 0x20 || c == 0x7f) return true; return false; }
}
json::Object defaults() {
    return {{"accountRootUid", ""}, {"folderUid", ""}, {"shareByDefault", true}, {"linkExpirationDays", 0}, {"sharePasswordReference", json::Value()}, {"requestTimeoutMs", defaultRequestTimeoutMs}};
}
bool nodeUid(const std::string& value) {
    const auto tilde = value.find('~');
    if (tilde == std::string::npos || value.find('~', tilde + 1) != std::string::npos) return false;
    return nodePart(std::string_view(value).substr(0, tilde)) && nodePart(std::string_view(value).substr(tilde + 1));
}
Error validateSettings(const json::Object& s) {
    auto required = s; required.remove("folderName");
    if (required.keys() != defaults().keys()) return {"config_invalid", "Proton settings have missing or unsupported fields", {}};
    const auto& rootValue = s.value("accountRootUid"); const auto& folderValue = s.value("folderUid");
    const auto root = rootValue.toString(), folder = folderValue.toString();
    if (!rootValue.isString() || !folderValue.isString() || (!root.empty() && !nodeUid(root)) || (!folder.empty() && !nodeUid(folder)) || (root.empty() && !folder.empty()))
        return {"config_invalid", "Proton account and folder identifiers are invalid", {}};
    // The optional name is a display hint; storage identity remains the folder UID.
    if (s.contains("folderName")) {
        const auto& name = s.value("folderName");
        if (!name.isString() || name.stringRef().empty() || name.stringRef().size() > 255 || controlCharacters(name.stringRef()) || folder.empty())
            return {"config_invalid", "Proton folder display name is invalid", {}};
    }
    const auto& days = s.value("linkExpirationDays"); const auto& timeout = s.value("requestTimeoutMs");
    if (!s.value("shareByDefault").isBool() || !json::integer(days, 0, 365) || !json::integer(timeout, 1000, maximumRequestTimeoutMs))
        return {"config_invalid", "Proton sharing or timeout settings are invalid", {}};
    const auto& password = s.value("sharePasswordReference");
    if (!password.isNull() && (!password.isString() || password.stringRef().empty() || password.stringRef().size() > 256))
        return {"config_invalid", "Use a secret-store reference for the share password", {}};
    return {};
}
Error validateCredentials(const json::Object& reference) {
    if (reference.value("mode") != "keychain" || !reference.value("reference").isString() || reference.value("reference").stringRef().empty())
        return {"config_invalid", "Proton requires a keychain account reference", {}};
    return {};
}
std::string serializationKey(const json::Object& settings, const json::Object& ref, const std::string& candidate) {
    const auto root = ref.value("accountRootUid").toString(settings.value("accountRootUid").toString());
    const auto folder = ref.value("folderUid").toString(settings.value("folderUid").toString());
    const auto name = ref.value("name").toString(candidate);
    return crypto::Sha256::hexDigest(json::serialize(json::Array{root, folder, name}));
}
std::optional<int> retryRequestTimeout(const std::string& profileId, const std::string& providerId, const json::Object& credentials, const json::Object& accepted, const json::Object& profile) {
    if (profile.value("id") != profileId.c_str() || profile.value("provider") != providerId.c_str() || profile.value("credentials").toObject() != credentials) return std::nullopt;
    const auto current = profile.value("settings").toObject();
    if (validateSettings(current)) return std::nullopt;
    for (const char* field : {"accountRootUid", "folderUid"}) if (current.value(field) != accepted.value(field)) return std::nullopt;
    return std::max(accepted.value("requestTimeoutMs").toInt(defaultRequestTimeoutMs), current.value("requestTimeoutMs").toInt(defaultRequestTimeoutMs));
}
}
