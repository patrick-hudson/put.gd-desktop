#include "common/Minisign.h"
#include "common/Blake2b.h"
#include "common/Ed25519.h"
#include "common/Text.h"

namespace putgd::minisign {
namespace {
Error invalid(const std::string& message, const std::string& check) { return {"update_signature_invalid", message, {{"check", check}}}; }
bool space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r'; }
std::string_view trimmed(std::string_view s) {
    while (!s.empty() && space(s.front())) s.remove_prefix(1);
    while (!s.empty() && space(s.back())) s.remove_suffix(1);
    return s;
}
// Strict base64 of the trimmed text: the standard alphabet only, '=' only as the
// final padding of a length that is a multiple of 4. Empty when the text isn't.
std::string base64(std::string_view text) {
    text = trimmed(text);
    size_t padding = 0;
    while (padding < 2 && padding < text.size() && text[text.size() - 1 - padding] == '=') ++padding;
    if (padding && text.size() % 4) return {};
    text.remove_suffix(padding);
    if (text.size() % 4 == 1) return {};
    std::string out; uint32_t acc = 0; int bits = 0;
    for (const char c : text) {
        uint32_t v;
        if (c >= 'A' && c <= 'Z') v = uint32_t(c - 'A'); else if (c >= 'a' && c <= 'z') v = uint32_t(c - 'a' + 26);
        else if (c >= '0' && c <= '9') v = uint32_t(c - '0' + 52); else if (c == '+') v = 62; else if (c == '/') v = 63; else return {};
        acc = (acc << 6) | v; bits += 6;
        if (bits >= 8) { bits -= 8; out += char((acc >> bits) & 0xFF); }
    }
    return out;
}
// The first base64 line that decodes to size bytes starting with prefix.
std::string decodedLine(const std::string& text, size_t size, std::string_view prefix) {
    for (const auto& raw : text::split(text, '\n')) {
        const auto line = trimmed(raw);
        if (line.empty() || text::startsWith(line, "untrusted comment:") || text::startsWith(line, "trusted comment:")) continue;
        auto decoded = base64(line);
        if (decoded.size() == size && text::startsWith(decoded, prefix)) return decoded;
    }
    return {};
}
}
Error verify(const std::string& message, const std::string& signatureFile, const std::string& publicKeyText, std::string* trustedComment) {
    // Public key: "Ed" + key id (8) + Ed25519 key (32).
    const auto publicKey = decodedLine(publicKeyText, 42, "Ed");
    if (publicKey.empty()) return invalid("This build has no usable release key", "key");
    // Signature: algorithm ("ED" prehashed, "Ed" legacy) + key id (8) + signature (64),
    // then the trusted comment and its global signature (64).
    const auto lines = text::split(signatureFile, '\n');
    if (lines.size() < 4 || !text::startsWith(lines[0], "untrusted comment:") || !text::startsWith(lines[2], "trusted comment: "))
        return invalid("The signature file is malformed", "format");
    const auto signature = base64(lines[1]), global = base64(lines[3]);
    if (signature.size() != 74 || global.size() != 64) return invalid("The signature file is malformed", "format");
    const auto algorithm = signature.substr(0, 2);
    if (algorithm != "ED" && algorithm != "Ed") return invalid("The signature uses an unknown algorithm", "algorithm");
    if (signature.compare(2, 8, publicKey, 2, 8) != 0) return invalid("The signature was made with another key", "keyId");
    const auto key = std::string_view(publicKey).substr(10);
    const auto sig = std::string_view(signature).substr(10);
    bool valid;
    if (algorithm == "ED") {
        const auto digest = crypto::Blake2b::digest(message);
        valid = crypto::ed25519Verify(key, sig, std::string_view(reinterpret_cast<const char*>(digest.data()), digest.size()));
    } else valid = crypto::ed25519Verify(key, sig, message);
    if (!valid) return invalid("The signature doesn't match", "signature");
    const auto comment = lines[2].substr(std::string_view("trusted comment: ").size());
    if (!crypto::ed25519Verify(key, global, std::string(sig) + comment)) return invalid("The signature's trusted comment doesn't match", "trustedComment");
    if (trustedComment) *trustedComment = comment;
    return {};
}
}
