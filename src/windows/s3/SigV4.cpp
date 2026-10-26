#include "s3/SigV4.h"
#include "common/Sha256.h"
#include "common/Text.h"
#include <algorithm>
#include <cstdio>

namespace putgd::sigv4 {
namespace {
void wipeString(std::string& s) {
    // volatile stores keep the compiler from dropping the overwrite.
    volatile char* p = s.empty() ? nullptr : &s[0];
    for (size_t i = 0; i < s.size(); ++i) p[i] = 0;
    s.clear();
    s.shrink_to_fit();
}
std::string raw(const std::array<uint8_t, 32>& digest) { return std::string(reinterpret_cast<const char*>(digest.data()), digest.size()); }
// Days since 1970-01-01 to a proleptic Gregorian date (H. Hinnant's civil_from_days).
void civil(long long days, int& year, int& month, int& day) {
    days += 719468;
    const long long era = (days >= 0 ? days : days - 146096) / 146097;
    const unsigned doe = unsigned(days - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const long long y = (long long)yoe + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    day = int(doy - (153 * mp + 2) / 5 + 1);
    month = int(mp < 10 ? mp + 3 : mp - 9);
    year = int(y + (month <= 2 ? 1 : 0));
}
}

void Credentials::wipe() {
    wipeString(accessKey);
    wipeString(secretKey);
    wipeString(sessionToken);
}

std::string amzDate(std::chrono::system_clock::time_point when) {
    const long long seconds = std::chrono::duration_cast<std::chrono::seconds>(when.time_since_epoch()).count();
    long long days = seconds / 86400, rest = seconds % 86400;
    if (rest < 0) { rest += 86400; --days; }
    int year = 0, month = 0, day = 0;
    civil(days, year, month, day);
    char out[32];
    std::snprintf(out, sizeof out, "%04d%02d%02dT%02d%02d%02dZ", year, month, day, int(rest / 3600), int(rest / 60 % 60), int(rest % 60));
    return out;
}

std::string canonicalQuery(const Pairs& query) {
    std::vector<std::pair<std::string, std::string>> encoded;
    encoded.reserve(query.size());
    for (const auto& [name, value] : query) encoded.emplace_back(text::percentEncode(name), text::percentEncode(value));
    std::sort(encoded.begin(), encoded.end());
    std::string out;
    for (const auto& [name, value] : encoded) {
        if (!out.empty()) out += '&';
        out += name;
        out += '=';
        out += value;
    }
    return out;
}

std::string canonicalHeaderValue(const std::string& value) {
    std::string out;
    bool space = false;
    for (const char c : text::trim(value)) {
        if (c == ' ' || c == '\t') { space = true; continue; }
        if (space && !out.empty()) out += ' ';
        space = false;
        out += c;
    }
    return out;
}

std::string signingKey(const std::string& secretKey, const std::string& date, const std::string& region, const std::string& service) {
    std::string seed = "AWS4" + secretKey;
    auto key = raw(crypto::hmacSha256(seed, date));
    wipeString(seed);
    key = raw(crypto::hmacSha256(key, region));
    key = raw(crypto::hmacSha256(key, service));
    return raw(crypto::hmacSha256(key, "aws4_request"));
}

Signed sign(const Request& request, const Credentials& credentials, const std::string& region, const std::string& service, const std::string& amzDateValue) {
    Signed out;
    out.amzDate = amzDateValue;
    // Lower-cased names; a repeated name joins its values with commas, as SigV4 requires.
    std::vector<std::pair<std::string, std::string>> headers;
    auto add = [&](const std::string& name, const std::string& value) {
        const auto lowered = text::lower(name);
        const auto canonical = canonicalHeaderValue(value);
        for (auto& existing : headers)
            if (existing.first == lowered) { existing.second += "," + canonical; return; }
        headers.emplace_back(lowered, canonical);
    };
    add("host", request.host);
    add("x-amz-content-sha256", request.payloadHash);
    add("x-amz-date", amzDateValue);
    if (!credentials.sessionToken.empty()) add("x-amz-security-token", credentials.sessionToken);
    for (const auto& [name, value] : request.headers) {
        const auto lowered = text::lower(name);
        if (lowered == "content-length" || lowered == "host" || lowered == "x-amz-date" || lowered == "x-amz-content-sha256" ||
            lowered == "x-amz-security-token" || lowered == "authorization") continue;
        add(name, value);
    }
    std::sort(headers.begin(), headers.end());

    std::string canonicalHeaders;
    for (const auto& [name, value] : headers) {
        canonicalHeaders += name + ":" + value + "\n";
        if (!out.signedHeaders.empty()) out.signedHeaders += ';';
        out.signedHeaders += name;
    }
    out.query = canonicalQuery(request.query);
    out.canonicalRequest = request.method + "\n" + (request.path.empty() ? std::string("/") : request.path) + "\n" + out.query + "\n" +
                           canonicalHeaders + "\n" + out.signedHeaders + "\n" + request.payloadHash;
    const auto date = amzDateValue.substr(0, 8);
    const auto scope = date + "/" + region + "/" + service + "/aws4_request";
    out.stringToSign = "AWS4-HMAC-SHA256\n" + amzDateValue + "\n" + scope + "\n" + crypto::Sha256::hexDigest(out.canonicalRequest);
    auto key = signingKey(credentials.secretKey, date, region, service);
    out.signature = crypto::hex(crypto::hmacSha256(key, out.stringToSign));
    wipeString(key);
    out.authorization = "AWS4-HMAC-SHA256 Credential=" + credentials.accessKey + "/" + scope + ",SignedHeaders=" + out.signedHeaders + ",Signature=" + out.signature;

    // Sent values keep the caller's spelling (canonical whitespace is equivalent on the wire).
    for (const auto& [name, value] : headers) out.headers.emplace_back(name, value);
    out.headers.emplace_back("authorization", out.authorization);
    return out;
}
}
