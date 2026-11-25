#pragma once
#include <charconv>
#include <string_view>

namespace putgd {
// The SDK's Curl backend prefixes connection failures with this numeric code.
// Never export the following message, which may contain endpoint/input material.
inline int sdkCurlCode(std::string_view message) {
    constexpr std::string_view prefix = "curlCode: ";
    if (!message.starts_with(prefix)) return -1;
    message.remove_prefix(prefix.size());
    const auto comma = message.find(',');
    if (comma == std::string_view::npos || comma == 0 || comma > 3) return -1;
    for (const char c : message.substr(0, comma)) if (c < '0' || c > '9') return -1;
    int code = -1;
    const auto parsed = std::from_chars(message.data(), message.data() + comma, code);
    return parsed.ec == std::errc{} && parsed.ptr == message.data() + comma && code <= 999 ? code : -1;
}
// A request whose connection was never set up, so none of it reached the server (#2051): no
// reply (the SDK's status -1) and libcurl's 7 (couldn't connect) or 35 (the TLS handshake failed,
// as when the server or the network resets it). libcurl writes a request only on an established
// connection, and the SDK never turns on TLS early data, which would send it during the handshake.
// A reused connection that drops its reply fails with another code.
inline bool s3ConnectionNeverSetUp(int httpStatus, std::string_view message) {
    if (httpStatus != -1) return false;
    const int code = sdkCurlCode(message);
    return code == 7 || code == 35;
}
}
