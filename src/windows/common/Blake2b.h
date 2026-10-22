#pragma once
// Portable BLAKE2b-512 (RFC 7693, unkeyed): the prehash of minisign's "ED"
// signatures over release SHA256SUMS.
#include <array>
#include <cstdint>
#include <string_view>

namespace putgd::crypto {
class Blake2b {
public:
    Blake2b();
    void update(const void* data, size_t size);
    void update(std::string_view s) { update(s.data(), s.size()); }
    std::array<uint8_t, 64> finish();
    static std::array<uint8_t, 64> digest(std::string_view);
private:
    void compress(bool last);
    uint64_t h_[8];
    uint64_t t_[2] = {0, 0};
    uint8_t buffer_[128];
    size_t buffered_ = 0;
};
}
