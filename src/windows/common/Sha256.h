#pragma once
// Portable SHA-256 (FIPS 180-4) for digests the protocol exposes: settings
// revisions, deletion acknowledgements, artifact hashes and SigV4 signing.
#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace putgd::crypto {
class Sha256 {
public:
    Sha256();
    void update(const void* data, size_t size);
    void update(std::string_view s) { update(s.data(), s.size()); }
    std::array<uint8_t, 32> finish();
    static std::array<uint8_t, 32> digest(std::string_view);
    static std::string hexDigest(std::string_view);
private:
    void block(const uint8_t*);
    uint32_t state_[8];
    uint64_t length_ = 0;
    uint8_t buffer_[64];
    size_t buffered_ = 0;
};
std::array<uint8_t, 32> hmacSha256(std::string_view key, std::string_view message);
std::string hex(const std::array<uint8_t, 32>&);
}
