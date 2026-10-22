#pragma once
// Portable MD5 (RFC 1321) for S3's Content-MD5 part digests and multipart ETags
// only. Never a security digest: SHA-256 (Sha256.h) covers integrity and signing.
#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace putgd::crypto {
class Md5 {
public:
    Md5();
    void update(const void* data, size_t size);
    void update(std::string_view s) { update(s.data(), s.size()); }
    std::array<uint8_t, 16> finish();
    static std::array<uint8_t, 16> digest(std::string_view);
    static std::string hexDigest(std::string_view);
private:
    void block(const uint8_t*);
    uint32_t state_[4];
    uint64_t length_ = 0;
    uint8_t buffer_[64];
    size_t buffered_ = 0;
};
}
