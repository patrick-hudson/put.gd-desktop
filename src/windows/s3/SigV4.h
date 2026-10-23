#pragma once
// AWS Signature Version 4 for S3 requests, as pure functions over strings
// (https://docs.aws.amazon.com/AmazonS3/latest/API/sig-v4-header-based-auth.html).
// Portable: no Windows headers, so tests/test_sigv4.cpp builds on any host.
// Nothing here logs; callers must never log credentials, signatures or the
// Authorization value.
#include <chrono>
#include <string>
#include <utility>
#include <vector>

namespace putgd::sigv4 {
using Pairs = std::vector<std::pair<std::string, std::string>>;

// Hex SHA-256 of the empty payload (bodyless requests).
inline constexpr const char* emptyPayloadHash = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";

struct Credentials {
    std::string accessKey, secretKey, sessionToken;
    // Overwrites the secret material in place before the strings are released.
    void wipe();
};

struct Request {
    std::string method;      // GET, HEAD, PUT, DELETE
    std::string host;        // exact Host header value sent (host[:port])
    std::string path;        // canonical URI, already percent-encoded once; signed as-is ("" signs "/")
    Pairs query;             // decoded names/values; encoded and sorted by canonicalQuery()
    Pairs headers;           // extra headers to sign and send (content-type, range, x-amz-*, if-match ...);
                             // never content-length. host/x-amz-date/x-amz-content-sha256/x-amz-security-token
                             // are added by sign().
    std::string payloadHash; // hex SHA-256 of the body (emptyPayloadHash when bodyless)
};

struct Signed {
    Pairs headers;            // every header to send, including Authorization, in signed order
    std::string amzDate;      // YYYYMMDDTHHMMSSZ
    std::string signedHeaders;
    std::string canonicalRequest;
    std::string stringToSign;
    std::string signature;    // hex
    std::string authorization;
    std::string query;        // wire query string (canonical form) without '?', "" when none
};

// "20130524T000000Z" for a UTC instant.
std::string amzDate(std::chrono::system_clock::time_point);
// RFC 3986 encoding of every name and value, sorted by encoded name then value,
// joined as name=value&...; an empty value keeps the '='.
std::string canonicalQuery(const Pairs&);
// Header value as canonicalized: trimmed, runs of spaces collapsed.
std::string canonicalHeaderValue(const std::string&);
// HMAC chain: kSecret -> date -> region -> service -> "aws4_request" (raw 32 bytes).
std::string signingKey(const std::string& secretKey, const std::string& date, const std::string& region, const std::string& service);
Signed sign(const Request&, const Credentials&, const std::string& region, const std::string& service, const std::string& amzDateValue);
}
