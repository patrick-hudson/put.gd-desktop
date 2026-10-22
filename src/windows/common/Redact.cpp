#include "common/Redact.h"
#include <algorithm>
#include <array>
#include <set>

namespace putgd::redact {
namespace {
bool word(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; }
bool space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }
bool base64(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '/' || c == '+' || c == '='; }
char lower(char c) { return c >= 'A' && c <= 'Z' ? char(c + 32) : c; }
// `name` is lowercase ASCII.
bool at(std::string_view s, size_t pos, std::string_view name) {
    if (pos + name.size() > s.size()) return false;
    for (size_t i = 0; i < name.size(); ++i) if (lower(s[pos + i]) != name[i]) return false;
    return true;
}
bool boundary(std::string_view s, size_t pos) { return pos == 0 || !word(s[pos - 1]); }
size_t skipSpaces(std::string_view s, size_t pos) { while (pos < s.size() && space(s[pos])) ++pos; return pos; }
// \s*[:=]\s* from pos; npos when absent.
size_t separator(std::string_view s, size_t pos) {
    pos = skipSpaces(s, pos);
    if (pos >= s.size() || (s[pos] != ':' && s[pos] != '=')) return std::string_view::npos;
    return skipSpaces(s, pos + 1);
}
// One global replacement: `match(s, i, end, replacement)` claims [i, end).
template <class Match> std::string pass(std::string_view s, Match match) {
    std::string out; out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        size_t end = i; std::string replacement;
        if (match(s, i, end, replacement) && end > i) { out += replacement; i = end; }
        else out += s[i++];
    }
    return out;
}
bool privateKeyHeader(std::string_view s, size_t pos, const char* opening, size_t& end) {
    const std::string_view open(opening);
    if (s.compare(pos, open.size(), open) != 0) return false;
    size_t k = pos + open.size();
    while (k < s.size() && ((s[k] >= 'A' && s[k] <= 'Z') || s[k] == ' ')) ++k;
    const std::string_view tail("PRIVATE KEY");
    if (k - (pos + open.size()) < tail.size() || s.compare(k - tail.size(), tail.size(), tail) != 0 || s.compare(k, 5, "-----") != 0) return false;
    end = k + 5;
    return true;
}
std::string privateKeys(std::string_view s) {
    return pass(s, [](std::string_view t, size_t i, size_t& end, std::string& r) {
        size_t header = 0;
        if (!privateKeyHeader(t, i, "-----BEGIN ", header)) return false;
        end = t.size();
        for (size_t found = t.find("-----END ", header); found != std::string_view::npos; found = t.find("-----END ", found + 1)) {
            size_t footer = 0;
            if (privateKeyHeader(t, found, "-----END ", footer)) { end = footer; break; }
        }
        r = "<redacted-private-key>";
        return true;
    });
}
std::string authorization(std::string_view s) {
    return pass(s, [](std::string_view t, size_t i, size_t& end, std::string& r) {
        if (!boundary(t, i)) return false;
        size_t name = at(t, i, "proxy-authorization") ? 19 : at(t, i, "authorization") ? 13 : 0;
        if (!name) return false;
        const size_t value = separator(t, i + name);
        if (value == std::string_view::npos) return false;
        end = value; while (end < t.size() && t[end] != '\r' && t[end] != '\n') ++end;
        r = std::string(t.substr(i, value - i)) + "<redacted>";
        return true;
    });
}
std::string signedParameters(std::string_view s) {
    static const std::array<std::string_view, 7> names{"x-amz-security-token", "x-amz-signature", "x-amz-credential", "x-goog-signature", "signature", "credential", "sig"};
    return pass(s, [](std::string_view t, size_t i, size_t& end, std::string& r) {
        if (!boundary(t, i)) return false;
        for (const auto name : names) {
            if (!at(t, i, name)) continue;
            const size_t value = separator(t, i + name.size());
            if (value == std::string_view::npos) continue;
            size_t k = value;
            while (k < t.size() && !space(t[k]) && std::string_view("&,;\"'<>").find(t[k]) == std::string_view::npos) ++k;
            if (k == value) continue;
            end = k; r = std::string(t.substr(i, value - i)) + "<redacted>";
            return true;
        }
        return false;
    });
}
// Presigned queries and share-link fragments (Proton keeps the share password
// in the fragment). The scheme, host and path stay.
std::string urlQueries(std::string_view s) {
    static const std::array<std::string_view, 5> schemes{"https://", "http://", "wss://", "ws://", "s3://"};
    return pass(s, [](std::string_view t, size_t i, size_t& end, std::string& r) {
        if (!boundary(t, i)) return false;
        for (const auto scheme : schemes) {
            if (!at(t, i, scheme)) continue;
            size_t k = i + scheme.size();
            const size_t start = k;
            while (k < t.size() && !space(t[k]) && std::string_view("?#\"'<>").find(t[k]) == std::string_view::npos) ++k;
            if (k == start || k >= t.size() || (t[k] != '?' && t[k] != '#')) return false;
            size_t m = k + 1;
            while (m < t.size() && !space(t[m]) && std::string_view("\"'<>").find(t[m]) == std::string_view::npos) ++m;
            end = m; r = std::string(t.substr(i, k - i)) + "?<redacted>";
            return true;
        }
        return false;
    });
}
std::string secretValues(std::string_view s) {
    // The Linux pattern's alternation order, with each optional underscore expanded.
    static const std::array<std::string_view, 32> names{
        "aws_secret_access_key", "aws_session_token", "secret_access_key", "secret_accesskey", "secretaccess_key", "secretaccesskey",
        "secret_key", "secretkey", "session_token", "sessiontoken", "security_token", "securitytoken", "access_token", "accesstoken",
        "refresh_token", "refreshtoken", "id_token", "idtoken", "share_password", "sharepassword", "password", "passphrase", "passwd",
        "api_key", "apikey", "client_secret", "clientsecret", "private_key", "privatekey", "cookie", "token", "secret"};
    return pass(s, [](std::string_view t, size_t i, size_t& end, std::string& r) {
        if (!boundary(t, i)) return false;
        for (const auto name : names) {
            if (!at(t, i, name)) continue;
            size_t k = i + name.size();
            if (k < t.size() && t[k] == '"') ++k;
            k = separator(t, k);
            if (k == std::string_view::npos) continue;
            if (k < t.size() && t[k] == '"') ++k;
            size_t v = k;
            while (v < t.size() && !space(t[v]) && std::string_view("\",;&}]").find(t[v]) == std::string_view::npos) ++v;
            if (v == k) continue;
            end = v; r = std::string(t.substr(i, k - i)) + "<redacted>";
            return true;
        }
        return false;
    });
}
std::string bearer(std::string_view s) {
    return pass(s, [](std::string_view t, size_t i, size_t& end, std::string& r) {
        if (!boundary(t, i)) return false;
        const size_t name = at(t, i, "bearer") ? 6 : at(t, i, "basic") ? 5 : 0;
        if (!name) return false;
        size_t k = i + name;
        if (k >= t.size() || !space(t[k])) return false;
        k = skipSpaces(t, k);
        size_t v = k;
        while (v < t.size() && (word(t[v]) || std::string_view(".~+/=-").find(t[v]) != std::string_view::npos)) ++v;
        if (v - k < 8) return false;
        end = v; r = std::string(t.substr(i, name)) + " <redacted>";
        return true;
    });
}
std::string accessKeyIds(std::string_view s) {
    static const std::array<std::string_view, 10> prefixes{"AKIA", "ASIA", "AGPA", "AIDA", "AROA", "AIPA", "ANPA", "ANVA", "ABIA", "ACCA"};
    return pass(s, [](std::string_view t, size_t i, size_t& end, std::string& r) {
        if (!boundary(t, i) || i + 20 > t.size()) return false;
        if (std::find(prefixes.begin(), prefixes.end(), t.substr(i, 4)) == prefixes.end()) return false;
        for (size_t k = i + 4; k < i + 20; ++k) if (!((t[k] >= 'A' && t[k] <= 'Z') || (t[k] >= '0' && t[k] <= '9'))) return false;
        if (i + 20 < t.size() && word(t[i + 20])) return false;
        end = i + 20; r = "<access-key-id>";
        return true;
    });
}
// Base64 runs of 120 or more characters, then 40 characters of mixed-case
// base64 alphabet (the shape of an AWS secret key). Lowercase hex never matches.
std::string base64Runs(std::string_view s, bool shapes) {
    std::string out; out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        if (!base64(s[i]) || (i > 0 && base64(s[i - 1]))) { out += s[i++]; continue; }
        size_t k = i; while (k < s.size() && base64(s[k])) ++k;
        const auto run = s.substr(i, k - i);
        if (!shapes && run.size() >= 120) out += "<redacted-blob>";
        else if (shapes && run.size() == 40 && run.find('=') == std::string_view::npos) {
            bool upper = false, low = false, other = false;
            for (const char c : run) { if (c >= 'A' && c <= 'Z') upper = true; else if (c >= 'a' && c <= 'z') low = true; else other = true; }
            out += upper && low && other ? std::string("<redacted>") : std::string(run);
        } else out += run;
        i = k;
    }
    return out;
}
}
std::string text(std::string_view input) {
    // Each rule needs a character the text may not have: skip those passes.
    std::string s(input);
    const bool assignment = s.find_first_of(":=") != std::string::npos;
    if (s.find("-----BEGIN ") != std::string::npos) s = privateKeys(s);
    if (assignment) { s = authorization(s); s = signedParameters(s); }
    if (s.find("://") != std::string::npos) s = urlQueries(s);
    if (assignment) s = secretValues(s);
    if (s.find_first_of("bB") != std::string::npos) s = bearer(s);
    if (s.find('A') != std::string::npos) s = accessKeyIds(s);
    if (s.size() >= 40) { s = base64Runs(s, false); s = base64Runs(s, true); }
    return s;
}
bool sensitiveKey(std::string_view key) {
    static const std::set<std::string, std::less<>> keys{"password", "passphrase", "passwd", "secret", "secretkey", "secretaccesskey", "awssecretaccesskey", "accesskey", "accesskeyid",
        "awsaccesskeyid", "sessiontoken", "awssessiontoken", "securitytoken", "token", "accesstoken", "refreshtoken", "authorization", "credentials", "cookie",
        "sharepassword", "privatekey", "clipboard", "clipboardtext", "text", "content", "contents", "payload"};
    std::string normalized;
    for (const char c : key) if (c != '_' && c != '-') normalized += lower(c);
    return keys.count(normalized) > 0;
}
json::Value value(const json::Value& v) {
    if (v.isString()) return text(v.stringRef());
    if (v.isArray()) { json::Array a; for (const auto& entry : v.arrayRef()) a.push_back(value(entry)); return a; }
    if (v.isObject()) return object(v.objectRef());
    return v;
}
json::Object object(const json::Object& o) {
    json::Object result;
    for (const auto& pair : o) result[pair.first] = sensitiveKey(pair.first) ? json::Value("<redacted>") : value(pair.second);
    return result;
}
}
