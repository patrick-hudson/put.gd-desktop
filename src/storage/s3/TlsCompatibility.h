#pragma once
#include <charconv>
#include <string_view>
namespace putgd {
// False when explicit groups are configured or the configuration cannot be read.
bool defaultSystemTlsGroups(const char* configuration);
bool defaultSystemTlsGroups();
// OpenSSL 3.5 predicts a hybrid key share that can make the first ClientHello
// exceed a TCP segment. Keep the hybrid advertised but predict the smaller
// classical share first; a server can still select the hybrid via HelloRetryRequest.
inline bool smallInitialTlsShare(unsigned curlVersion, std::string_view backend) {
    constexpr std::string_view prefix = "OpenSSL/";
    if (curlVersion < 0x074900 || !backend.starts_with(prefix)) return false;
    backend.remove_prefix(prefix.size());
    unsigned major = 0, minor = 0, patch = 0;
    const auto end = backend.data() + backend.size();
    const auto first = std::from_chars(backend.data(), end, major);
    if (first.ec != std::errc{} || first.ptr == end || *first.ptr != '.') return false;
    const auto second = std::from_chars(first.ptr + 1, end, minor);
    if (second.ec != std::errc{} || second.ptr == end || *second.ptr != '.') return false;
    const auto third = std::from_chars(second.ptr + 1, end, patch);
    if (third.ec != std::errc{} || third.ptr != end) return false;
    return major > 3 || (major == 3 && minor >= 5);
}
}
