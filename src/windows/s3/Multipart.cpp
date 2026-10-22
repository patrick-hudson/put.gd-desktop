#include "s3/Multipart.h"
#include "common/Md5.h"
#include "common/Text.h"
#include <algorithm>

namespace putgd::s3::multipart {
namespace {
constexpr long long mib = 1024 * 1024;
bool lowerHex(std::string_view s, size_t size) {
    if (s.size() != size) return false;
    for (const char c : s) if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}
std::optional<long long> number(const std::optional<std::string>& s, long long maximum) {
    if (!s || s->empty() || s->size() > 19) return std::nullopt;
    long long value = 0;
    for (const char c : *s) {
        if (c < '0' || c > '9') return std::nullopt;
        value = value * 10 + (c - '0');
        if (value > maximum) return std::nullopt;
    }
    return value;
}
}

long long partBytes(long long bytes) {
    if (bytes <= thresholdBytes || bytes > maximumObjectBytes) return 0;
    const long long spread = ((bytes + maximumParts - 1) / maximumParts + mib - 1) / mib * mib;
    return std::max({defaultPartBytes, minimumPartBytes, spread});
}
long long partCount(long long bytes, long long partBytes) { return partBytes > 0 ? std::max(1LL, (bytes + partBytes - 1) / partBytes) : 0; }
bool validPlan(long long bytes, long long partBytes) {
    return bytes > 0 && bytes <= maximumObjectBytes && partBytes >= minimumPartBytes && partBytes <= maximumPartBytes && partCount(bytes, partBytes) <= maximumParts;
}
Range partRange(int number, long long bytes, long long partBytes) {
    if (number < 1 || partBytes <= 0 || number > partCount(bytes, partBytes)) return {};
    const long long offset = (long long)(number - 1) * partBytes;
    return {offset, std::min(partBytes, bytes - offset)};
}

std::string plainEtag(std::string_view etag) {
    auto s = text::trim(etag);
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"') s = s.substr(1, s.size() - 2);
    return text::lower(s);
}
bool md5Etag(std::string_view plain) { return lowerHex(plain, 32); }
std::string compositeEtag(const std::map<int, std::string>& partEtags) {
    if (partEtags.empty()) return {};
    crypto::Md5 composite;
    for (const auto& [number, etag] : partEtags) {
        const auto plain = plainEtag(etag);
        const auto raw = text::fromHex(plain);
        if (!md5Etag(plain) || !raw || raw->size() != 16) return {};
        composite.update(raw->data(), raw->size());
    }
    const auto digest = composite.finish();
    return text::hex(digest.data(), digest.size()) + "-" + std::to_string(partEtags.size());
}

std::string xmlEscape(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (const char c : s) {
        switch (c) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        case '\'': out += "&apos;"; break;
        default: out += c;
        }
    }
    return out;
}
std::optional<std::string> xmlUnescape(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '&') { out += s[i]; continue; }
        const auto end = s.find(';', i);
        if (end == std::string_view::npos || end - i > 10) return std::nullopt;
        const auto entity = s.substr(i + 1, end - i - 1);
        if (entity == "amp") out += '&';
        else if (entity == "lt") out += '<';
        else if (entity == "gt") out += '>';
        else if (entity == "quot") out += '"';
        else if (entity == "apos") out += '\'';
        else if (entity.size() > 1 && entity[0] == '#') {
            unsigned long code = 0;
            const bool hexadecimal = entity[1] == 'x' || entity[1] == 'X';
            const auto digits = entity.substr(hexadecimal ? 2 : 1);
            if (digits.empty()) return std::nullopt;
            for (const char c : digits) {
                int v = -1;
                if (c >= '0' && c <= '9') v = c - '0';
                else if (hexadecimal && c >= 'a' && c <= 'f') v = c - 'a' + 10;
                else if (hexadecimal && c >= 'A' && c <= 'F') v = c - 'A' + 10;
                if (v < 0) return std::nullopt;
                code = code * (hexadecimal ? 16 : 10) + unsigned(v);
                if (code > 0x10FFFF) return std::nullopt;
            }
            // Identifiers and ETags are ASCII; anything else is not one of ours.
            if (code == 0 || code >= 0x80) return std::nullopt;
            out += char(code);
        } else return std::nullopt;
        i = end;
    }
    return out;
}
std::optional<std::string> xmlText(std::string_view body, std::string_view name, size_t maximum, size_t from) {
    const auto open = "<" + std::string(name) + ">", close = "</" + std::string(name) + ">";
    const auto start = body.find(open, from);
    if (start == std::string_view::npos) return std::nullopt;
    const auto end = body.find(close, start + open.size());
    if (end == std::string_view::npos) return std::nullopt;
    const auto raw = body.substr(start + open.size(), end - start - open.size());
    if (raw.size() > maximum * 6) return std::nullopt;
    auto value = xmlUnescape(raw);
    if (!value || value->size() > maximum) return std::nullopt;
    return value;
}
bool errorDocument(std::string_view body) {
    // The document element, after an optional declaration and whitespace.
    size_t i = 0;
    if (body.substr(0, 5) == "<?xml") { const auto end = body.find("?>"); if (end == std::string_view::npos) return false; i = end + 2; }
    while (i < body.size() && (body[i] == ' ' || body[i] == '\r' || body[i] == '\n' || body[i] == '\t')) ++i;
    return body.substr(i, 7) == "<Error>";
}
std::string completeRequest(const std::map<int, std::string>& partEtags) {
    std::string xml = "<CompleteMultipartUpload xmlns=\"http://s3.amazonaws.com/doc/2006-03-01/\">";
    for (const auto& [number, etag] : partEtags)
        xml += "<Part><PartNumber>" + std::to_string(number) + "</PartNumber><ETag>" + xmlEscape(etag) + "</ETag></Part>";
    xml += "</CompleteMultipartUpload>";
    return xml;
}
bool validUploadId(std::string_view id) {
    if (id.empty() || id.size() > 1024) return false;
    for (const char c : id)
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '+' || c == '/' || c == '=' || c == '.' || c == '_' || c == '~' || c == '-')) return false;
    return true;
}
std::string createdUploadId(std::string_view body) {
    if (errorDocument(body)) return {};
    const auto id = xmlText(body, "UploadId", 1024);
    return id && validUploadId(*id) ? *id : std::string();
}
PartListing parseListParts(std::string_view body) {
    PartListing listing;
    if (errorDocument(body) || body.find("<ListPartsResult") == std::string_view::npos) return listing;
    // Parts follow the header elements; header text such as <Key> is escaped, so
    // "<Part>" only ever opens a part.
    size_t at = 0;
    while (true) {
        const auto start = body.find("<Part>", at);
        if (start == std::string_view::npos) break;
        const auto end = body.find("</Part>", start);
        if (end == std::string_view::npos) return listing;
        const auto part = body.substr(start, end - start);
        ListedPart p;
        const auto n = number(xmlText(part, "PartNumber", 8), maximumParts);
        const auto size = number(xmlText(part, "Size", 19), maximumPartBytes);
        const auto etag = xmlText(part, "ETag", 256);
        if (!n || *n < 1 || !size || !etag) return listing;
        p.number = int(*n);
        p.size = *size;
        p.etag = *etag;
        listing.parts.push_back(std::move(p));
        at = end + 7;
    }
    // Header elements precede the first part.
    const auto header = body.substr(0, body.find("<Part>"));
    const auto truncated = xmlText(header, "IsTruncated", 8);
    listing.truncated = truncated && *truncated == "true";
    if (listing.truncated) {
        const auto next = number(xmlText(header, "NextPartNumberMarker", 8), maximumParts);
        if (!next || *next < 1) return listing;
        listing.nextMarker = int(*next);
    }
    listing.ok = true;
    return listing;
}

bool validJournal(const json::Object& o) {
    if (o.empty()) return true;
    static const std::vector<std::string> allowed{"schemaVersion", "partBytes", "uploadId", "key", "bytes", "sha256", "phase"};
    for (const auto& pair : o) if (std::find(allowed.begin(), allowed.end(), pair.first) == allowed.end()) return false;
    if (!json::integer(o.value("schemaVersion"), 1, 1) || !json::integer(o.value("partBytes"), 1, double(maximumPartBytes))) return false;
    const bool created = o.contains("uploadId");
    for (const auto* key : {"key", "bytes", "sha256", "phase"}) if (o.contains(key) != created) return false;
    if (!created) return true;
    const auto& key = o.value("key");
    const auto& phase = o.value("phase");
    return o.value("uploadId").isString() && validUploadId(o.value("uploadId").stringRef()) && key.isString() && !key.stringRef().empty() &&
           key.stringRef().size() <= 1024 && json::integer(o.value("bytes"), 0, double(maximumObjectBytes)) && o.value("sha256").isString() &&
           lowerHex(o.value("sha256").stringRef(), 64) && phase.isString() && (phase.stringRef() == "parts" || phase.stringRef() == "completing");
}
bool journalCreated(const json::Object& o) { return o.value("uploadId").isString() && !o.value("uploadId").stringRef().empty(); }
bool journalMatches(const json::Object& o, const std::string& key, long long bytes, const std::string& sha256, long long partBytes) {
    return journalCreated(o) && o.value("key").toString() == key && o.value("bytes").toInteger(-1) == bytes && o.value("sha256").toString() == sha256 &&
           o.value("partBytes").toInteger(-1) == partBytes;
}
}
