#pragma once
// Portable SHA-512 (FIPS 180-4) and Ed25519 signature verification (RFC 8032,
// pure Ed25519) for minisign release signatures. Windows CNG has no Ed25519.
#include <array>
#include <cstdint>
#include <string_view>

namespace putgd::crypto {
class Sha512 {
public:
    Sha512();
    void update(const void* data, size_t size);
    void update(std::string_view s) { update(s.data(), s.size()); }
    std::array<uint8_t, 64> finish();
    static std::array<uint8_t, 64> digest(std::string_view);
private:
    void block(const uint8_t*);
    uint64_t state_[8];
    uint64_t length_ = 0;
    uint8_t buffer_[128];
    size_t buffered_ = 0;
};
// True when signature (R || S, 64 bytes) is publicKey's (32 bytes) over message.
// Refuses non-canonical S (S >= L), undecodable or non-canonical points and
// wrong sizes. Not constant time: it only checks public data.
bool ed25519Verify(std::string_view publicKey, std::string_view signature, std::string_view message);
}
