#include "common/Ed25519.h"
#include <algorithm>
#include <cstring>

namespace putgd::crypto {
namespace {
constexpr uint64_t K[80] = {
    0x428a2f98d728ae22,0x7137449123ef65cd,0xb5c0fbcfec4d3b2f,0xe9b5dba58189dbbc,0x3956c25bf348b538,0x59f111f1b605d019,0x923f82a4af194f9b,0xab1c5ed5da6d8118,
    0xd807aa98a3030242,0x12835b0145706fbe,0x243185be4ee4b28c,0x550c7dc3d5ffb4e2,0x72be5d74f27b896f,0x80deb1fe3b1696b1,0x9bdc06a725c71235,0xc19bf174cf692694,
    0xe49b69c19ef14ad2,0xefbe4786384f25e3,0x0fc19dc68b8cd5b5,0x240ca1cc77ac9c65,0x2de92c6f592b0275,0x4a7484aa6ea6e483,0x5cb0a9dcbd41fbd4,0x76f988da831153b5,
    0x983e5152ee66dfab,0xa831c66d2db43210,0xb00327c898fb213f,0xbf597fc7beef0ee4,0xc6e00bf33da88fc2,0xd5a79147930aa725,0x06ca6351e003826f,0x142929670a0e6e70,
    0x27b70a8546d22ffc,0x2e1b21385c26c926,0x4d2c6dfc5ac42aed,0x53380d139d95b3df,0x650a73548baf63de,0x766a0abb3c77b2a8,0x81c2c92e47edaee6,0x92722c851482353b,
    0xa2bfe8a14cf10364,0xa81a664bbc423001,0xc24b8b70d0f89791,0xc76c51a30654be30,0xd192e819d6ef5218,0xd69906245565a910,0xf40e35855771202a,0x106aa07032bbd1b8,
    0x19a4c116b8d2d0c8,0x1e376c085141ab53,0x2748774cdf8eeb99,0x34b0bcb5e19b48a8,0x391c0cb3c5c95a63,0x4ed8aa4ae3418acb,0x5b9cca4f7763e373,0x682e6ff3d6b2b8a3,
    0x748f82ee5defb2fc,0x78a5636f43172f60,0x84c87814a1f0ab72,0x8cc702081a6439ec,0x90befffa23631e28,0xa4506cebde82bde9,0xbef9a3f7b2c67915,0xc67178f2e372532b,
    0xca273eceea26619c,0xd186b8c721c0c207,0xeada7dd6cde0eb1e,0xf57d4f7fee6ed178,0x06f067aa72176fba,0x0a637dc5a2c898a6,0x113f9804bef90dae,0x1b710b35131c471b,
    0x28db77f523047d84,0x32caab7b40c72493,0x3c9ebe0a15c9bebc,0x431d67c49c100d4c,0x4cc5d4becb3e42b6,0x597f299cfc657e2a,0x5fcb6fab3ad6faec,0x6c44198c4a475817};
inline uint64_t rotr(uint64_t x, int n) { return (x >> n) | (x << (64 - n)); }

// Field and group arithmetic after TweetNaCl (public domain): GF(2^255-19) as
// 16 signed 16-bit limbs, points in extended coordinates (X, Y, Z, T).
using Gf = int64_t[16];
constexpr int64_t gf0[16] = {0};
constexpr int64_t gf1[16] = {1};
constexpr int64_t D[16] = {0x78a3,0x1359,0x4dca,0x75eb,0xd8ab,0x4141,0x0a4d,0x0070,0xe898,0x7779,0x4079,0x8cc7,0xfe73,0x2b6f,0x6cee,0x5203};
constexpr int64_t D2[16] = {0xf159,0x26b2,0x9b94,0xebd6,0xb156,0x8283,0x149a,0x00e0,0xd130,0xeef3,0x80f2,0x198e,0xfce7,0x56df,0xd9dc,0x2406};
constexpr int64_t BX[16] = {0xd51a,0x8f25,0x2d60,0xc956,0xa7b2,0x9525,0xc760,0x692c,0xdc5c,0xfdd6,0xe231,0xc0a4,0x53fe,0xcd6e,0x36d3,0x2169};
constexpr int64_t BY[16] = {0x6658,0x6666,0x6666,0x6666,0x6666,0x6666,0x6666,0x6666,0x6666,0x6666,0x6666,0x6666,0x6666,0x6666,0x6666,0x6666};
constexpr int64_t SQRTM1[16] = {0xa0b0,0x4a0e,0x1b27,0xc4ee,0xe478,0xad2f,0x1806,0x2f43,0xd7a7,0x3dfb,0x0099,0x2b4d,0xdf0b,0x4fc1,0x2480,0x2b83};
// The group order L, little-endian.
constexpr int64_t L[32] = {0xed,0xd3,0xf5,0x5c,0x1a,0x63,0x12,0x58,0xd6,0x9c,0xf7,0xa2,0xde,0xf9,0xde,0x14,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0x10};

void set(Gf r, const int64_t* a) { std::memcpy(r, a, sizeof(Gf)); }
void carry(Gf o) {
    for (int i = 0; i < 16; ++i) {
        o[i] += int64_t(1) << 16;
        const int64_t c = o[i] >> 16;
        if (i < 15) o[i+1] += c - 1; else o[0] += 38 * (c - 1);
        o[i] -= c * 65536;
    }
}
void cswap(Gf p, Gf q, int64_t b) {
    const int64_t mask = ~(b - 1);
    for (int i = 0; i < 16; ++i) { const int64_t t = mask & (p[i] ^ q[i]); p[i] ^= t; q[i] ^= t; }
}
void pack(uint8_t* o, const int64_t* n) {
    Gf m, t; set(t, n);
    carry(t); carry(t); carry(t);
    for (int j = 0; j < 2; ++j) {
        m[0] = t[0] - 0xffed;
        for (int i = 1; i < 15; ++i) { m[i] = t[i] - 0xffff - ((m[i-1] >> 16) & 1); m[i-1] &= 0xffff; }
        m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
        const int64_t b = (m[15] >> 16) & 1;
        m[14] &= 0xffff;
        cswap(t, m, 1 - b);
    }
    for (int i = 0; i < 16; ++i) { o[2*i] = uint8_t(t[i] & 0xff); o[2*i+1] = uint8_t((t[i] >> 8) & 0xff); }
}
bool equal(const int64_t* a, const int64_t* b) { uint8_t c[32], d[32]; pack(c, a); pack(d, b); return std::memcmp(c, d, 32) == 0; }
int parity(const int64_t* a) { uint8_t d[32]; pack(d, a); return d[0] & 1; }
void unpack(Gf o, const uint8_t* n) {
    for (int i = 0; i < 16; ++i) o[i] = int64_t(n[2*i]) + (int64_t(n[2*i+1]) << 8);
    o[15] &= 0x7fff;
}
void add(Gf o, const int64_t* a, const int64_t* b) { for (int i = 0; i < 16; ++i) o[i] = a[i] + b[i]; }
void sub(Gf o, const int64_t* a, const int64_t* b) { for (int i = 0; i < 16; ++i) o[i] = a[i] - b[i]; }
void mul(Gf o, const int64_t* a, const int64_t* b) {
    int64_t t[31] = {0};
    for (int i = 0; i < 16; ++i) for (int j = 0; j < 16; ++j) t[i+j] += a[i] * b[j];
    for (int i = 0; i < 15; ++i) t[i] += 38 * t[i+16];
    for (int i = 0; i < 16; ++i) o[i] = t[i];
    carry(o); carry(o);
}
void square(Gf o, const int64_t* a) { mul(o, a, a); }
void pow2523(Gf o, const int64_t* i) {
    Gf c; set(c, i);
    for (int a = 250; a >= 0; --a) { square(c, c); if (a != 1) mul(c, c, i); }
    set(o, c);
}
void invert(Gf o, const int64_t* i) {
    Gf c; set(c, i);
    for (int a = 253; a >= 0; --a) { square(c, c); if (a != 2 && a != 4) mul(c, c, i); }
    set(o, c);
}

using Point = Gf[4];
void add(Point p, Point q) {
    Gf a, b, c, d, t, e, f, g, h;
    sub(a, p[1], p[0]); sub(t, q[1], q[0]); mul(a, a, t);
    add(b, p[0], p[1]); add(t, q[0], q[1]); mul(b, b, t);
    mul(c, p[3], q[3]); mul(c, c, D2);
    mul(d, p[2], q[2]); add(d, d, d);
    sub(e, b, a); sub(f, d, c); add(g, d, c); add(h, b, a);
    mul(p[0], e, f); mul(p[1], h, g); mul(p[2], g, f); mul(p[3], e, h);
}
void cswapPoint(Point p, Point q, int64_t b) { for (int i = 0; i < 4; ++i) cswap(p[i], q[i], b); }
void pack(uint8_t* r, Point p) {
    Gf tx, ty, zi;
    invert(zi, p[2]); mul(tx, p[0], zi); mul(ty, p[1], zi);
    pack(r, ty);
    r[31] = uint8_t(r[31] ^ (parity(tx) << 7));
}
// p = s * q (q is clobbered); s is 32 bytes little-endian.
void scalarmult(Point p, Point q, const uint8_t* s) {
    set(p[0], gf0); set(p[1], gf1); set(p[2], gf1); set(p[3], gf0);
    for (int i = 255; i >= 0; --i) {
        const int64_t b = (s[i/8] >> (i & 7)) & 1;
        cswapPoint(p, q, b); add(q, p); add(p, p); cswapPoint(p, q, b);
    }
}
void scalarbase(Point p, const uint8_t* s) {
    Point q; set(q[0], BX); set(q[1], BY); set(q[2], gf1); mul(q[3], BX, BY);
    scalarmult(p, q, s);
}
// Decodes a point and negates it (RFC 8032 5.1.3), refusing y >= p, x = 0 with
// the sign bit set and y with no square root for x.
bool unpackNegated(Point r, const uint8_t* p) {
    Gf t, check, num, den, den2, den4, den6;
    set(r[2], gf1);
    unpack(r[1], p);
    uint8_t canonical[32]; pack(canonical, r[1]);
    canonical[31] = uint8_t(canonical[31] | (p[31] & 0x80));
    if (std::memcmp(canonical, p, 32) != 0) return false;
    square(num, r[1]); mul(den, num, D); sub(num, num, r[2]); add(den, r[2], den);
    square(den2, den); square(den4, den2); mul(den6, den4, den2); mul(t, den6, num); mul(t, t, den);
    pow2523(t, t); mul(t, t, num); mul(t, t, den); mul(t, t, den); mul(r[0], t, den);
    square(check, r[0]); mul(check, check, den);
    if (!equal(check, num)) mul(r[0], r[0], SQRTM1);
    square(check, r[0]); mul(check, check, den);
    if (!equal(check, num)) return false;
    if (equal(r[0], gf0) && (p[31] >> 7)) return false;
    if (parity(r[0]) == (p[31] >> 7)) sub(r[0], gf0, r[0]);
    mul(r[3], r[0], r[1]);
    return true;
}
// r = x mod L for a 64-byte little-endian x.
void reduce(uint8_t* r, const uint8_t* bytes) {
    int64_t x[64];
    for (int i = 0; i < 64; ++i) x[i] = bytes[i];
    for (int i = 63; i >= 32; --i) {
        int64_t c = 0;
        int j = i - 32;
        for (; j < i - 12; ++j) {
            x[j] += c - 16 * x[i] * L[j - (i - 32)];
            c = (x[j] + 128) >> 8;
            x[j] -= c * 256;
        }
        x[j] += c;
        x[i] = 0;
    }
    int64_t c = 0;
    for (int j = 0; j < 32; ++j) { x[j] += c - (x[31] >> 4) * L[j]; c = x[j] >> 8; x[j] &= 255; }
    for (int j = 0; j < 32; ++j) x[j] -= c * L[j];
    for (int i = 0; i < 32; ++i) { x[i+1] += x[i] >> 8; r[i] = uint8_t(x[i] & 255); }
}
bool canonicalScalar(const uint8_t* s) {
    for (int i = 31; i >= 0; --i) {
        if (s[i] < L[i]) return true;
        if (s[i] > L[i]) return false;
    }
    return false;
}
}

Sha512::Sha512() {
    const uint64_t init[8] = {0x6a09e667f3bcc908,0xbb67ae8584caa73b,0x3c6ef372fe94f82b,0xa54ff53a5f1d36f1,
                              0x510e527fade682d1,0x9b05688c2b3e6c1f,0x1f83d9abfb41bd6b,0x5be0cd19137e2179};
    std::memcpy(state_, init, sizeof state_);
}
void Sha512::block(const uint8_t* p) {
    uint64_t w[80];
    for (int i = 0; i < 16; ++i) { w[i] = 0; for (int j = 0; j < 8; ++j) w[i] = w[i] << 8 | p[i*8+j]; }
    for (int i = 16; i < 80; ++i) {
        const uint64_t s0 = rotr(w[i-15], 1) ^ rotr(w[i-15], 8) ^ (w[i-15] >> 7);
        const uint64_t s1 = rotr(w[i-2], 19) ^ rotr(w[i-2], 61) ^ (w[i-2] >> 6);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    uint64_t a = state_[0], b = state_[1], c = state_[2], d = state_[3], e = state_[4], f = state_[5], g = state_[6], h = state_[7];
    for (int i = 0; i < 80; ++i) {
        const uint64_t S1 = rotr(e, 14) ^ rotr(e, 18) ^ rotr(e, 41);
        const uint64_t ch = (e & f) ^ (~e & g);
        const uint64_t t1 = h + S1 + ch + K[i] + w[i];
        const uint64_t S0 = rotr(a, 28) ^ rotr(a, 34) ^ rotr(a, 39);
        const uint64_t maj = (a & b) ^ (a & c) ^ (b & c);
        const uint64_t t2 = S0 + maj;
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    state_[0] += a; state_[1] += b; state_[2] += c; state_[3] += d; state_[4] += e; state_[5] += f; state_[6] += g; state_[7] += h;
}
void Sha512::update(const void* data, size_t size) {
    const auto* p = static_cast<const uint8_t*>(data);
    length_ += size;
    while (size > 0) {
        const size_t take = std::min(size, sizeof buffer_ - buffered_);
        std::memcpy(buffer_ + buffered_, p, take);
        buffered_ += take; p += take; size -= take;
        if (buffered_ == sizeof buffer_) { block(buffer_); buffered_ = 0; }
    }
}
std::array<uint8_t, 64> Sha512::finish() {
    const uint64_t bytes = length_;
    uint8_t pad = 0x80; update(&pad, 1);
    const uint8_t zero = 0;
    while (buffered_ != 112) update(&zero, 1);
    uint8_t len[16];
    for (int i = 0; i < 8; ++i) len[i] = uint8_t(i == 7 ? bytes >> 61 : 0);
    for (int i = 0; i < 8; ++i) len[8+i] = uint8_t((bytes << 3) >> (56 - 8 * i));
    update(len, 16);
    std::array<uint8_t, 64> out;
    for (int i = 0; i < 64; ++i) out[size_t(i)] = uint8_t(state_[i / 8] >> (56 - 8 * (i % 8)));
    return out;
}
std::array<uint8_t, 64> Sha512::digest(std::string_view s) { Sha512 h; h.update(s); return h.finish(); }

bool ed25519Verify(std::string_view publicKey, std::string_view signature, std::string_view message) {
    if (publicKey.size() != 32 || signature.size() != 64) return false;
    const auto* key = reinterpret_cast<const uint8_t*>(publicKey.data());
    const auto* sig = reinterpret_cast<const uint8_t*>(signature.data());
    if (!canonicalScalar(sig + 32)) return false;
    Point p, q;
    if (!unpackNegated(q, key)) return false;
    // R == [S]B - [k]A, k = SHA-512(R || A || M) mod L.
    Sha512 hash; hash.update(sig, 32); hash.update(key, 32); hash.update(message);
    const auto digest = hash.finish();
    uint8_t k[32]; reduce(k, digest.data());
    scalarmult(p, q, k);
    scalarbase(q, sig + 32);
    add(p, q);
    uint8_t r[32]; pack(r, p);
    return std::memcmp(r, sig, 32) == 0;
}
}
