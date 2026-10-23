#include "common/Text.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace putgd::text {
namespace {
bool decode(std::string_view s, size_t& i, uint32_t& cp) {
    const auto b = static_cast<unsigned char>(s[i]);
    int extra = 0; uint32_t min = 0;
    if (b < 0x80) { cp = b; ++i; return true; }
    if ((b & 0xE0) == 0xC0) { extra = 1; cp = b & 0x1F; min = 0x80; }
    else if ((b & 0xF0) == 0xE0) { extra = 2; cp = b & 0x0F; min = 0x800; }
    else if ((b & 0xF8) == 0xF0) { extra = 3; cp = b & 0x07; min = 0x10000; }
    else return false;
    for (int k = 1; k <= extra; ++k) {
        if (i + size_t(k) >= s.size()) return false;
        const auto c = static_cast<unsigned char>(s[i + size_t(k)]);
        if ((c & 0xC0) != 0x80) return false;
        cp = (cp << 6) | (c & 0x3F);
    }
    if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
    i += size_t(extra) + 1;
    return true;
}
}
bool validUtf8(std::string_view s) { return codePoints(s) >= 0; }
long codePoints(std::string_view s) {
    long count = 0; size_t i = 0; uint32_t cp;
    while (i < s.size()) { if (!decode(s, i, cp)) return -1; ++count; }
    return count;
}
bool startsWith(std::string_view s, std::string_view p) { return s.size() >= p.size() && s.compare(0, p.size(), p) == 0; }
bool endsWith(std::string_view s, std::string_view p) { return s.size() >= p.size() && s.compare(s.size() - p.size(), p.size(), p) == 0; }
bool contains(std::string_view s, std::string_view n) { return s.find(n) != std::string_view::npos; }
bool containsChar(std::string_view s, char c) { return s.find(c) != std::string_view::npos; }
std::string lower(std::string_view s) { std::string r(s); for (auto& c : r) if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a'); return r; }
std::string upper(std::string_view s) { std::string r(s); for (auto& c : r) if (c >= 'a' && c <= 'z') c = char(c - 'a' + 'A'); return r; }
std::string trim(std::string_view s) {
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\n' || s[b] == '\r')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\n' || s[e - 1] == '\r')) --e;
    return std::string(s.substr(b, e - b));
}
std::vector<std::string> split(std::string_view s, char sep) {
    std::vector<std::string> r; size_t start = 0;
    while (true) {
        const auto pos = s.find(sep, start);
        if (pos == std::string_view::npos) { r.emplace_back(s.substr(start)); break; }
        r.emplace_back(s.substr(start, pos - start));
        start = pos + 1;
    }
    return r;
}
std::string join(const std::vector<std::string>& parts, std::string_view sep) {
    std::string r;
    for (size_t i = 0; i < parts.size(); ++i) { if (i) r += sep; r += parts[i]; }
    return r;
}
std::string replace(std::string s, std::string_view from, std::string_view to) {
    if (from.empty()) return s;
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) { s.replace(pos, from.size(), to); pos += to.size(); }
    return s;
}
std::string hex(const uint8_t* data, size_t size) {
    static const char digits[] = "0123456789abcdef";
    std::string r; r.reserve(size * 2);
    for (size_t i = 0; i < size; ++i) { r += digits[data[i] >> 4]; r += digits[data[i] & 15]; }
    return r;
}
std::string hex(const std::vector<uint8_t>& v) { return hex(v.data(), v.size()); }
std::string hex(std::string_view s) { return hex(reinterpret_cast<const uint8_t*>(s.data()), s.size()); }
std::optional<std::vector<uint8_t>> fromHex(std::string_view s) {
    if (s.size() % 2) return std::nullopt;
    std::vector<uint8_t> r; r.reserve(s.size() / 2);
    auto nibble = [](char c, int& v) { if (c >= '0' && c <= '9') v = c - '0'; else if (c >= 'a' && c <= 'f') v = c - 'a' + 10; else if (c >= 'A' && c <= 'F') v = c - 'A' + 10; else return false; return true; };
    for (size_t i = 0; i < s.size(); i += 2) { int a, b; if (!nibble(s[i], a) || !nibble(s[i + 1], b)) return std::nullopt; r.push_back(uint8_t(a * 16 + b)); }
    return r;
}
std::string base64(const uint8_t* data, size_t size) {
    static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string r; r.reserve((size + 2) / 3 * 4);
    for (size_t i = 0; i < size; i += 3) {
        uint32_t n = uint32_t(data[i]) << 16;
        if (i + 1 < size) n |= uint32_t(data[i + 1]) << 8;
        if (i + 2 < size) n |= data[i + 2];
        r += table[(n >> 18) & 63]; r += table[(n >> 12) & 63];
        r += i + 1 < size ? table[(n >> 6) & 63] : '=';
        r += i + 2 < size ? table[n & 63] : '=';
    }
    return r;
}
std::string base64(std::string_view s) { return base64(reinterpret_cast<const uint8_t*>(s.data()), s.size()); }
std::optional<std::string> fromBase64(std::string_view s) {
    std::string r; uint32_t acc = 0; int bits = 0;
    for (const char c : s) {
        int v;
        if (c >= 'A' && c <= 'Z') v = c - 'A'; else if (c >= 'a' && c <= 'z') v = c - 'a' + 26; else if (c >= '0' && c <= '9') v = c - '0' + 52;
        else if (c == '+') v = 62; else if (c == '/') v = 63; else if (c == '=' || c == '\n' || c == '\r') continue; else return std::nullopt;
        acc = (acc << 6) | uint32_t(v); bits += 6;
        if (bits >= 8) { bits -= 8; r += char((acc >> bits) & 0xFF); }
    }
    return r;
}
std::string percentEncode(std::string_view s, bool keepSlash) {
    static const char digits[] = "0123456789ABCDEF";
    std::string r;
    for (const unsigned char c : s) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~' || (keepSlash && c == '/')) r += char(c);
        else { r += '%'; r += digits[c >> 4]; r += digits[c & 15]; }
    }
    return r;
}
std::string percentDecode(std::string_view s) {
    std::string r;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            auto h = fromHex(s.substr(i + 1, 2));
            if (h) { r += char((*h)[0]); i += 2; continue; }
        }
        r += s[i];
    }
    return r;
}
bool printable(std::string_view s) {
    if (!validUtf8(s)) return false;
    for (const unsigned char c : s) if (c < 0x20 || c == 0x7F) return false;
    return true;
}
bool matchesIdentifier(std::string_view s) {
    if (s.empty() || s.size() > 64) return false;
    auto alnum = [](char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'); };
    if (!alnum(s[0])) return false;
    for (const char c : s.substr(1)) if (!alnum(c) && c != '.' && c != '_' && c != '-') return false;
    return true;
}
bool matchesColor(std::string_view s) {
    if (s.size() != 7 || s[0] != '#') return false;
    for (const char c : s.substr(1)) if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}
std::string Url::authority() const { return port >= 0 ? host + ":" + std::to_string(port) : host; }
std::string Url::origin() const { return scheme + "://" + authority(); }
Url parseUrl(std::string_view s) {
    Url u;
    const auto schemeEnd = s.find("://");
    if (schemeEnd == std::string_view::npos || schemeEnd == 0) return u;
    u.scheme = lower(s.substr(0, schemeEnd));
    for (const char c : u.scheme) if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.')) return u;
    auto rest = s.substr(schemeEnd + 3);
    const auto fragmentPos = rest.find('#');
    if (fragmentPos != std::string_view::npos) { u.hasFragment = true; u.fragment = std::string(rest.substr(fragmentPos + 1)); rest = rest.substr(0, fragmentPos); }
    const auto queryPos = rest.find('?');
    if (queryPos != std::string_view::npos) { u.hasQuery = true; u.query = std::string(rest.substr(queryPos + 1)); rest = rest.substr(0, queryPos); }
    const auto pathPos = rest.find('/');
    std::string_view authority = pathPos == std::string_view::npos ? rest : rest.substr(0, pathPos);
    u.path = pathPos == std::string_view::npos ? std::string() : std::string(rest.substr(pathPos));
    const auto at = authority.rfind('@');
    if (at != std::string_view::npos) { u.userInfo = std::string(authority.substr(0, at)); authority = authority.substr(at + 1); }
    if (!authority.empty() && authority[0] == '[') {
        const auto close = authority.find(']');
        if (close == std::string_view::npos) return u;
        u.host = lower(authority.substr(0, close + 1));
        authority = authority.substr(close + 1);
        if (!authority.empty()) { if (authority[0] != ':') return u; authority = authority.substr(1); }
        else authority = {};
        if (!authority.empty()) { for (const char c : authority) if (c < '0' || c > '9') return u; u.port = std::stoi(std::string(authority)); }
    } else {
        const auto colon = authority.find(':');
        if (colon != std::string_view::npos) {
            const auto portText = authority.substr(colon + 1);
            if (portText.empty() || portText.size() > 5) return u;
            for (const char c : portText) if (c < '0' || c > '9') return u;
            u.port = std::stoi(std::string(portText));
            if (u.port > 65535) return u;
            authority = authority.substr(0, colon);
        }
        u.host = lower(authority);
        for (const char c : u.host) if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_')) return u;
    }
    if (u.host.empty()) return u;
    u.valid = true;
    return u;
}
std::string format(std::string_view pattern, const std::vector<std::string>& args) {
    std::string r;
    for (size_t i = 0; i < pattern.size(); ++i) {
        if (pattern[i] == '%' && i + 1 < pattern.size() && pattern[i + 1] >= '1' && pattern[i + 1] <= '9') {
            const size_t index = size_t(pattern[i + 1] - '1');
            if (index < args.size()) r += args[index];
            ++i;
        } else r += pattern[i];
    }
    return r;
}
std::string byteSize(long long bytes) {
    static const char* units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    double value = double(bytes); int unit = 0;
    while (value >= 1024 && unit < 4) { value /= 1024; ++unit; }
    char buf[64];
    if (unit == 0) std::snprintf(buf, sizeof buf, "%lld B", bytes);
    else std::snprintf(buf, sizeof buf, value >= 100 ? "%.0f %s" : "%.1f %s", value, units[unit]);
    return buf;
}
std::string toString(long long v) { return std::to_string(v); }
std::string toString(double d) { std::string out; char buf[32]; if (std::floor(d) == d && std::fabs(d) < 9007199254740992.0) std::snprintf(buf, sizeof buf, "%lld", (long long)d); else std::snprintf(buf, sizeof buf, "%.17g", d); return buf; }
}
