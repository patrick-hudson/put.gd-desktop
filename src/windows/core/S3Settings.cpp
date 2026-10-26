#include "core/S3Settings.h"
#include "common/Text.h"
#include <algorithm>
#include <vector>

namespace putgd::s3 {
namespace {
bool in(const std::vector<std::string>& list, const std::string& v) { return std::find(list.begin(), list.end(), v) != list.end(); }
bool literalIp(const std::string& host) {
    if (host.empty()) return false;
    if (host[0] == '[') return true;
    int dots = 0; for (const char c : host) { if (c == '.') ++dots; else if (c < '0' || c > '9') return false; }
    return dots == 3;
}
}
bool loopbackHost(const std::string& host) {
    if (host == "localhost" || host == "[::1]") return true;
    return text::startsWith(host, "127.");
}
Error validateCredentials(const json::Object& credentials) {
    if (credentials.value("mode") == "keychain" && !text::startsWith(credentials.value("reference").toString(), "putgd/"))
        return {"credential_reference_invalid", "S3 keychain references start with putgd/", {}};
    return {};
}
Error validateSettings(const json::Object& settings) {
    const std::vector<std::string> fields{"bucket", "region", "endpoint", "objectPrefix", "storageClass", "aclMode", "publicUrlBase", "addressing", "connectTimeoutMs", "requestTimeoutMs", "lowSpeedBytesPerSecond", "lowSpeedTimeSeconds"};
    auto bad = [] { return Error{"s3_settings_invalid", "Invalid S3 settings; use a supported endpoint, addressing, ACL, storage class and positive timeouts", {}}; };
    if (settings.size() != fields.size()) return bad();
    for (const auto& field : fields) if (!settings.contains(field)) return bad();
    for (const auto* field : {"bucket", "region", "endpoint", "objectPrefix", "storageClass", "aclMode", "addressing"}) if (!settings.value(field).isString()) return bad();
    const auto bucket = settings.value("bucket").stringRef();
    if (bucket.size() < 3 || bucket.size() > 63 || text::containsChar(bucket, '/') || text::containsChar(bucket, ':') || text::containsChar(bucket, ' ') ||
        settings.value("region").stringRef().empty() || settings.value("objectPrefix").stringRef().size() > 1022 || text::containsChar(settings.value("objectPrefix").stringRef(), '\0')) return bad();
    const auto endpoint = text::parseUrl(settings.value("endpoint").stringRef());
    if (!endpoint.valid || endpoint.host.empty() || !endpoint.userInfo.empty() || endpoint.hasQuery || endpoint.hasFragment ||
        (endpoint.scheme != "https" && !(endpoint.scheme == "http" && loopbackHost(endpoint.host)))) return bad();
    if (settings.value("publicUrlBase").isString()) {
        const auto base = text::parseUrl(settings.value("publicUrlBase").stringRef());
        if (!base.valid || base.host.empty() || !base.userInfo.empty() || base.hasQuery || base.hasFragment || (base.scheme != "https" && !(base.scheme == "http" && loopbackHost(base.host)))) return bad();
    } else if (!settings.value("publicUrlBase").isNull()) return bad();
    if (!in({"public-read", "omit"}, settings.value("aclMode").stringRef()) || !in({"path", "virtual", "auto"}, settings.value("addressing").stringRef()) ||
        !in({"STANDARD", "REDUCED_REDUNDANCY", "STANDARD_IA", "ONEZONE_IA", "INTELLIGENT_TIERING", "GLACIER", "DEEP_ARCHIVE", "GLACIER_IR"}, settings.value("storageClass").stringRef())) return bad();
    for (const auto* field : {"connectTimeoutMs", "requestTimeoutMs", "lowSpeedBytesPerSecond", "lowSpeedTimeSeconds"}) if (!json::integer(settings.value(field), 1, 2147483647.0)) return bad();
    if (settings.value("addressing") == "virtual" && (text::containsChar(bucket, '.') || literalIp(endpoint.host))) return bad();
    return {};
}
bool virtualAddressing(const json::Object& settings) {
    const auto mode = settings.value("addressing").toString();
    if (mode == "virtual") return true;
    if (mode == "path") return false;
    const auto host = text::parseUrl(settings.value("endpoint").toString()).host;
    return text::endsWith(host, "amazonaws.com") && !text::containsChar(settings.value("bucket").toString(), '.');
}
std::string requestHost(const json::Object& settings) {
    const auto endpoint = text::parseUrl(settings.value("endpoint").toString());
    return virtualAddressing(settings) ? settings.value("bucket").toString() + "." + endpoint.authority() : endpoint.authority();
}
std::string requestPath(const json::Object& settings, const std::string& key) {
    std::string encodedKey;
    for (const auto& segment : text::split(key, '/')) { if (!encodedKey.empty() || true) encodedKey += "/"; encodedKey += text::percentEncode(segment); }
    if (virtualAddressing(settings)) return encodedKey;
    return "/" + text::percentEncode(settings.value("bucket").toString()) + encodedKey;
}
std::string publicObjectUrl(const json::Object& settings, const std::string& key) {
    std::string encoded;
    if (settings.value("publicUrlBase").isString()) {
        encoded = settings.value("publicUrlBase").stringRef();
        while (text::endsWith(encoded, "/")) encoded.pop_back();
    } else {
        const auto endpoint = text::parseUrl(settings.value("endpoint").toString());
        std::string path = endpoint.path; while (text::endsWith(path, "/")) path.pop_back();
        if (virtualAddressing(settings)) encoded = endpoint.scheme + "://" + settings.value("bucket").toString() + "." + endpoint.authority() + path;
        else encoded = endpoint.origin() + path + "/" + text::percentEncode(settings.value("bucket").toString());
    }
    std::vector<std::string> segments;
    for (const auto& segment : text::split(key, '/')) segments.push_back(text::percentEncode(segment));
    return encoded + "/" + text::join(segments, "/");
}
std::string objectKey(const json::Object& settings, const std::string& candidate) {
    auto prefix = settings.value("objectPrefix").toString();
    if (!prefix.empty() && !text::endsWith(prefix, "/")) prefix += "/";
    return prefix + candidate;
}
}
