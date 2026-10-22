#include "common/Blake2b.h"
#include <algorithm>
#include <cstring>

namespace putgd::crypto {
namespace {
constexpr uint64_t IV[8] = {
    0x6a09e667f3bcc908ULL,0xbb67ae8584caa73bULL,0x3c6ef372fe94f82bULL,0xa54ff53a5f1d36f1ULL,
    0x510e527fade682d1ULL,0x9b05688c2b3e6c1fULL,0x1f83d9abfb41bd6bULL,0x5be0cd19137e2179ULL};
constexpr uint8_t SIGMA[10][16] = {
    {0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15},{14,10,4,8,9,15,13,6,1,12,0,2,11,7,5,3},
    {11,8,12,0,5,2,15,13,10,14,3,6,7,1,9,4},{7,9,3,1,13,12,11,14,2,6,5,10,4,0,15,8},
    {9,0,5,7,2,4,10,15,14,1,11,12,6,8,3,13},{2,12,6,10,0,11,8,3,4,13,7,5,15,14,1,9},
    {12,5,1,15,14,13,4,10,0,7,6,3,9,2,8,11},{13,11,7,14,12,1,3,9,5,0,15,4,8,6,2,10},
    {6,15,14,9,11,3,0,8,12,2,13,7,1,4,10,5},{10,2,8,4,7,6,1,5,15,11,9,14,3,12,13,0}};
inline uint64_t rotr(uint64_t x, int n) { return (x >> n) | (x << (64 - n)); }
inline void mix(uint64_t* v, int a, int b, int c, int d, uint64_t x, uint64_t y) {
    v[a] += v[b] + x; v[d] = rotr(v[d] ^ v[a], 32);
    v[c] += v[d];     v[b] = rotr(v[b] ^ v[c], 24);
    v[a] += v[b] + y; v[d] = rotr(v[d] ^ v[a], 16);
    v[c] += v[d];     v[b] = rotr(v[b] ^ v[c], 63);
}
}
Blake2b::Blake2b() {
    std::memcpy(h_, IV, sizeof h_);
    h_[0] ^= 0x01010000ULL ^ 64; // no key, 64-byte digest
}
void Blake2b::compress(bool last) {
    uint64_t m[16], v[16];
    for (int i = 0; i < 16; ++i) { m[i] = 0; for (int j = 7; j >= 0; --j) m[i] = m[i] << 8 | buffer_[i*8+j]; }
    for (int i = 0; i < 8; ++i) { v[i] = h_[i]; v[i+8] = IV[i]; }
    v[12] ^= t_[0]; v[13] ^= t_[1];
    if (last) v[14] = ~v[14];
    for (int r = 0; r < 12; ++r) {
        const uint8_t* s = SIGMA[r % 10];
        mix(v, 0, 4, 8, 12, m[s[0]], m[s[1]]);
        mix(v, 1, 5, 9, 13, m[s[2]], m[s[3]]);
        mix(v, 2, 6, 10, 14, m[s[4]], m[s[5]]);
        mix(v, 3, 7, 11, 15, m[s[6]], m[s[7]]);
        mix(v, 0, 5, 10, 15, m[s[8]], m[s[9]]);
        mix(v, 1, 6, 11, 12, m[s[10]], m[s[11]]);
        mix(v, 2, 7, 8, 13, m[s[12]], m[s[13]]);
        mix(v, 3, 4, 9, 14, m[s[14]], m[s[15]]);
    }
    for (int i = 0; i < 8; ++i) h_[i] ^= v[i] ^ v[i+8];
}
void Blake2b::update(const void* data, size_t size) {
    const auto* p = static_cast<const uint8_t*>(data);
    while (size > 0) {
        // The final block is compressed by finish(), so a full buffer waits for more input.
        if (buffered_ == sizeof buffer_) {
            t_[0] += sizeof buffer_; if (t_[0] < sizeof buffer_) ++t_[1];
            compress(false); buffered_ = 0;
        }
        const size_t take = std::min(size, sizeof buffer_ - buffered_);
        std::memcpy(buffer_ + buffered_, p, take);
        buffered_ += take; p += take; size -= take;
    }
}
std::array<uint8_t, 64> Blake2b::finish() {
    t_[0] += buffered_; if (t_[0] < buffered_) ++t_[1];
    std::memset(buffer_ + buffered_, 0, sizeof buffer_ - buffered_);
    compress(true);
    std::array<uint8_t, 64> out;
    for (int i = 0; i < 64; ++i) out[size_t(i)] = uint8_t(h_[i / 8] >> (8 * (i % 8)));
    return out;
}
std::array<uint8_t, 64> Blake2b::digest(std::string_view s) { Blake2b h; h.update(s); return h.finish(); }
}
