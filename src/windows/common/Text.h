#pragma once
// UTF-8 string helpers shared by the portable core and the Win32 layer.
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace putgd::text {
bool validUtf8(std::string_view);
// Number of Unicode code points; -1 for invalid UTF-8.
long codePoints(std::string_view);
bool startsWith(std::string_view s, std::string_view prefix);
bool endsWith(std::string_view s, std::string_view suffix);
bool contains(std::string_view s, std::string_view needle);
bool containsChar(std::string_view s, char c);
std::string lower(std::string_view);           // ASCII only
std::string upper(std::string_view);           // ASCII only
std::string trim(std::string_view);
std::vector<std::string> split(std::string_view s, char separator);
std::string join(const std::vector<std::string>&, std::string_view separator);
std::string replace(std::string s, std::string_view from, std::string_view to);
std::string hex(const uint8_t* data, size_t size);
std::string hex(const std::vector<uint8_t>&);
std::string hex(std::string_view bytes);
std::optional<std::vector<uint8_t>> fromHex(std::string_view);
std::string base64(const uint8_t* data, size_t size);
std::string base64(std::string_view bytes);
std::optional<std::string> fromBase64(std::string_view);
// RFC 3986 percent-encoding keeping unreserved characters only (S3 canonical form).
std::string percentEncode(std::string_view, bool keepSlash = false);
std::string percentDecode(std::string_view);
// Printable check for identifiers: no control characters.
bool printable(std::string_view);
bool matchesIdentifier(std::string_view); // ^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$
bool matchesColor(std::string_view);      // ^#[0-9a-f]{6}$
struct Url {
    std::string scheme, host, userInfo, path, query, fragment;
    int port = -1;
    bool valid = false;
    bool hasQuery = false, hasFragment = false;
    std::string authority() const; // host[:port]
    std::string origin() const;    // scheme://host[:port]
};
Url parseUrl(std::string_view);
// Simple string formatting: "%1 is %2" with positional replacements.
std::string format(std::string_view pattern, const std::vector<std::string>& args);
// Human-readable byte sizes: "1.5 MiB".
std::string byteSize(long long bytes);
std::string toString(long long);
std::string toString(double);
}
