// Release signature checks: BLAKE2b-512 (RFC 7693), SHA-512, Ed25519 (RFC 8032
// section 7.1) and minisign files signed by a throwaway key and by the nightly
// release key. Usage: test_win_minisign FIXTURE_DIR (tests/fixtures/minisign).
#include "common/Blake2b.h"
#include "common/Ed25519.h"
#include "common/Minisign.h"
#include "common/Text.h"
#include <cstdio>
#include <fstream>
#include <sstream>

using namespace putgd;
static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

static std::string fixtures;
static std::string readFixture(const std::string& name) {
    std::ifstream in(fixtures + "/" + name, std::ios::binary);
    if (!in) { std::fprintf(stderr, "FAIL cannot read fixture %s\n", name.c_str()); ++failures; return {}; }
    std::ostringstream s; s << in.rdbuf(); return s.str();
}
static std::string bytes(std::string_view hex) {
    const auto v = text::fromHex(hex);
    return v ? std::string(v->begin(), v->end()) : std::string();
}
template <size_t N> static std::string hexOf(const std::array<uint8_t, N>& d) { return text::hex(d.data(), d.size()); }

static void blake2bTests() {
    using crypto::Blake2b;
    CHECK(hexOf(Blake2b::digest("")) == "786a02f742015903c6c6fd852552d272912f4740e15847618a86e217f71f5419d25e1031afee585313896444934eb04b903a685b1448b755d56f701afe9be2ce");
    CHECK(hexOf(Blake2b::digest("abc")) == "ba80a53f981c4d0d6a2797b69f12f6e94c212f14685ac4b74b12bb6fdbffa2d17d87c5392aab792dc252d5de4533cc9518d38aa8dbf1925ab92386edd4009923");
    // Exactly one block: the last block is only compressed by finish().
    CHECK(hexOf(Blake2b::digest(std::string(128, 'a'))) == "fc6c71f688f43ea7d60817478808f3cac753e61571865c95adbc2d9122c943a76b92c2cb1047ef3fe7bf6e436ec1d0a99a9e5b216780bf7fed9d7ca91d3a8f3b");
    const std::string thousand(1000, 'a');
    const auto expected = "d6a69459fe93fc6b9537ed4336e5099e0dcca3e97290a412500ed7a0daffb03d80cf3650a20e0591f748e10c3c534945ee83d5f2c9722f1a68d98b8c01af23fd";
    CHECK(hexOf(Blake2b::digest(thousand)) == expected);
    for (size_t step : {1u, 127u, 128u, 129u}) {
        Blake2b h; for (size_t i = 0; i < thousand.size(); i += step) h.update(std::string_view(thousand).substr(i, step));
        CHECK(hexOf(h.finish()) == expected);
    }
}
static void sha512Tests() {
    using crypto::Sha512;
    CHECK(hexOf(Sha512::digest("")) == "cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce47d0d13c5d85f2b0ff8318d2877eec2f63b931bd47417a81a538327af927da3e");
    CHECK(hexOf(Sha512::digest("abc")) == "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f");
    const std::string thousand(1000, 'a');
    Sha512 h; for (size_t i = 0; i < thousand.size(); i += 111) h.update(std::string_view(thousand).substr(i, 111));
    CHECK(hexOf(h.finish()) == "67ba5535a46e3f86dbfbed8cbbaf0125c76ed549ff8b0b9e03e0c88cf90fa634fa7b12b47d77b694de488ace8d9a65967dc96df599727d3292a8d9d447709c97");
}
static void ed25519Tests() {
    using crypto::ed25519Verify;
    struct Vector { const char* key; const char* message; const char* signature; };
    const Vector vectors[] = {
        {"d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a", "",
         "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b"},
        {"3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c", "72",
         "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00"},
        {"fc51cd8e6218a1a38da47ed00230f0580816ed13ba3303ac5deb911548908025", "af82",
         "6291d657deec24024827e69c3abe01a30ce548a284743a445e3680d7db5ac3ac18ff9b538d16f290ae67f760984dc6594a7c15e9716ed28dc027beceea1ec40a"},
        {"ec172b93ad5e563bf4932c70e1245034c35467ef2efd4d64ebf819683467e2bf",
         "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f",
         "dc2a4459e7369633a52b1bf277839a00201009a3efbf3ecb69bea2186c26b58909351fc9ac90b3ecfdfbc7c66431e0303dca179c138ac17ad9bef1177331a704"}};
    for (const auto& v : vectors) {
        const auto key = bytes(v.key), message = bytes(v.message), signature = bytes(v.signature);
        CHECK(ed25519Verify(key, signature, message));
        CHECK(!ed25519Verify(key, signature, message + "x"));
        for (size_t bit : {0u, 255u, 256u, 511u}) {
            auto flipped = signature; flipped[bit / 8] = char(flipped[bit / 8] ^ (1 << (bit % 8)));
            CHECK(!ed25519Verify(key, flipped, message));
        }
        auto otherKey = key; otherKey[0] = char(otherKey[0] ^ 1);
        CHECK(!ed25519Verify(otherKey, signature, message));
    }
    const auto key = bytes(vectors[0].key), signature = bytes(vectors[0].signature);
    // S + L is the same scalar mod L but not canonical.
    const uint8_t order[32] = {0xed,0xd3,0xf5,0x5c,0x1a,0x63,0x12,0x58,0xd6,0x9c,0xf7,0xa2,0xde,0xf9,0xde,0x14,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0x10};
    auto malleated = signature; unsigned carry = 0;
    for (size_t i = 0; i < 32; ++i) { carry += unsigned(uint8_t(malleated[32+i])) + order[i]; malleated[32+i] = char(carry & 0xff); carry >>= 8; }
    CHECK(!ed25519Verify(key, malleated, ""));
    auto high = signature; high[63] = char(high[63] | 0xe0); CHECK(!ed25519Verify(key, high, ""));
    // Points: y = 2 has no x on the curve; y = p is non-canonical; x = 0 with the sign bit set.
    auto y2 = std::string(32, '\0'); y2[0] = 2; CHECK(!ed25519Verify(y2, signature, ""));
    auto yp = bytes("edffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f"); CHECK(!ed25519Verify(yp, signature, ""));
    // With the identity (y = 1) as key, R = [S]B for any message: S = 1 and R = B
    // verify under its canonical encoding but not under y = p + 1 or x = -0.
    const auto identitySignature = bytes("5866666666666666666666666666666666666666666666666666666666666666") + bytes("01") + std::string(31, '\0');
    auto identity = std::string(32, '\0'); identity[0] = 1;
    CHECK(ed25519Verify(identity, identitySignature, "any"));
    CHECK(!ed25519Verify(bytes("eeffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f"), identitySignature, "any"));
    auto x0 = identity; x0[31] = char(0x80); CHECK(!ed25519Verify(x0, identitySignature, "any"));
    CHECK(!ed25519Verify(key.substr(1), signature, ""));
    CHECK(!ed25519Verify(key, signature.substr(1), ""));
    CHECK(!ed25519Verify(key, signature + "x", ""));
}
static std::string check(const Error& e) { return e.details.value("check").toString(); }
static void minisignTests() {
    const auto message = readFixture("message.txt"), publicKey = readFixture("test.pub"), otherKey = readFixture("other.pub");
    const auto prehashed = readFixture("prehashed.minisig"), legacy = readFixture("legacy.minisig");
    std::string comment;
    CHECK(!minisign::verify(message, prehashed, publicKey, &comment) && comment == "put.gd fixture prehashed");
    CHECK(prehashed.substr(prehashed.find('\n') + 1, 2) == "RU"); // "ED"
    CHECK(!minisign::verify(message, legacy, publicKey, &comment) && comment == "put.gd fixture legacy");
    CHECK(legacy.substr(legacy.find('\n') + 1, 2) == "RW");       // "Ed"
    // The base64 line alone is a key too.
    const auto keyLines = text::split(publicKey, '\n');
    CHECK(keyLines.size() >= 2 && !minisign::verify(message, prehashed, keyLines[1]));
    for (const auto* signature : {&prehashed, &legacy}) {
        auto e = minisign::verify(message + " ", *signature, publicKey);
        CHECK(e.code == "update_signature_invalid" && check(e) == "signature" && e.message == "The signature doesn't match");
        CHECK(check(minisign::verify(message, *signature, otherKey)) == "keyId");
        auto lines = text::split(*signature, '\n');
        auto tampered = lines; tampered[2] += "!";
        e = minisign::verify(message, text::join(tampered, "\n"), publicKey);
        CHECK(e.code == "update_signature_invalid" && check(e) == "trustedComment");
        auto global = *text::fromBase64(lines[3]); global[5] = char(global[5] ^ 0x10);
        tampered = lines; tampered[3] = text::base64(global);
        CHECK(check(minisign::verify(message, text::join(tampered, "\n"), publicKey)) == "trustedComment");
        auto raw = *text::fromBase64(lines[1]); raw[0] = 'X';
        tampered = lines; tampered[1] = text::base64(raw);
        CHECK(check(minisign::verify(message, text::join(tampered, "\n"), publicKey)) == "algorithm");
        tampered = lines; tampered[1].insert(10, "*");
        CHECK(check(minisign::verify(message, text::join(tampered, "\n"), publicKey)) == "format");
        tampered = lines; tampered[1] += "=";
        CHECK(check(minisign::verify(message, text::join(tampered, "\n"), publicKey)) == "format");
        tampered = lines; tampered[0] = "comment";
        CHECK(check(minisign::verify(message, text::join(tampered, "\n"), publicKey)) == "format");
        tampered = lines; tampered[2] = "trusted comment:x";
        CHECK(check(minisign::verify(message, text::join(tampered, "\n"), publicKey)) == "format");
    }
    CHECK(check(minisign::verify(message, "garbage", publicKey)) == "format");
    CHECK(check(minisign::verify(message, "", publicKey)) == "format");
    CHECK(check(minisign::verify(message, prehashed, "garbage")) == "key");
    CHECK(check(minisign::verify(message, prehashed, "")) == "key");
    CHECK(check(minisign::verify(message, prehashed, prehashed)) == "key");
    comment = "unchanged";
    CHECK(minisign::verify(message + "x", prehashed, publicKey, &comment) && comment == "unchanged");
}
// A published nightly's SHA256SUMS and the release signer's .minisig for it.
static void releaseTests() {
    // packaging/linux/repo/keys/putgd-nightly.minisign.pub
    const std::string nightlyKey = "RWS7rjnkpQ9EcSKAfhPmpsB9HLqrzXJx13H3nB/u0Z208V3JOU9+C0n7";
    const auto sums = readFixture("SHA256SUMS"), signature = readFixture("SHA256SUMS.minisig");
    std::string comment;
    CHECK(!minisign::verify(sums, signature, nightlyKey, &comment));
    CHECK(comment == "put.gd v26.1022.983-nightly nightly");
    CHECK(check(minisign::verify(sums, signature, readFixture("test.pub"))) == "keyId");
    auto changed = sums; changed[0] = changed[0] == '0' ? '1' : '0';
    CHECK(check(minisign::verify(changed, signature, nightlyKey)) == "signature");
}
int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: %s FIXTURE_DIR\n", argv[0]); return 2; }
    fixtures = argv[1];
    if (readFixture("test.pub").empty()) return 1;
    blake2bTests(); sha512Tests(); ed25519Tests(); minisignTests(); releaseTests();
    if (failures) { std::fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    std::printf("windows minisign: ok\n");
    return 0;
}
