#include "core/SanitizedPng.h"
#include <cstring>
#include <vector>

namespace putgd::png {
namespace {
const std::vector<std::string> allowed{"IHDR", "PLTE", "tRNS", "gAMA", "cHRM", "sRGB", "pHYs", "IDAT", "IEND"};
unsigned long readBE(std::string_view b, size_t at) { return (unsigned long)(unsigned char)b[at] << 24 | (unsigned long)(unsigned char)b[at + 1] << 16 | (unsigned long)(unsigned char)b[at + 2] << 8 | (unsigned char)b[at + 3]; }
}
unsigned long crc32(std::string_view bytes) {
    static unsigned long table[256];
    static bool ready = false;
    if (!ready) { for (unsigned long n = 0; n < 256; ++n) { unsigned long c = n; for (int k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320UL ^ (c >> 1) : c >> 1; table[n] = c; } ready = true; }
    unsigned long c = 0xFFFFFFFFUL;
    for (const unsigned char ch : bytes) c = table[(c ^ ch) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFUL;
}
bool hasSignature(std::string_view b) { return b.size() >= 8 && b.substr(0, 8) == signature; }
bool dimensions(std::string_view b, int& w, int& h) {
    if (!hasSignature(b) || b.size() < 33 || b.substr(12, 4) != "IHDR") return false;
    const auto width = readBE(b, 16), height = readBE(b, 20);
    if (width == 0 || height == 0 || width > 0x7FFFFFFF || height > 0x7FFFFFFF) return false;
    w = int(width); h = int(height);
    return true;
}
Error inspect(std::string_view b, int* width, int* height) {
    auto bad = [](const char* m) { return Error{"png_invalid", m, {}}; };
    if (!hasSignature(b)) return bad("Not a PNG file");
    size_t at = 8; bool first = true, ended = false, sawIdat = false;
    while (at < b.size()) {
        if (ended) return bad("Bytes after IEND");
        if (b.size() - at < 12) return bad("Truncated chunk");
        const auto length = readBE(b, at);
        if (length > 0x7FFFFFFFUL || b.size() - at - 12 < length) return bad("Chunk length exceeds the file");
        const std::string type(b.substr(at + 4, 4));
        bool ok = false; for (const auto& a : allowed) if (a == type) ok = true;
        if (!ok) return {"png_chunk_refused", "PNG contains a refused chunk type", {{"chunk", type}}};
        if (first && type != "IHDR") return bad("IHDR must come first");
        if (!first && type == "IHDR") return bad("Repeated IHDR");
        first = false;
        if (crc32(b.substr(at + 4, 4 + length)) != readBE(b, at + 8 + length)) return bad("Chunk CRC mismatch");
        if (type == "IDAT") sawIdat = true;
        if (type == "IEND") { if (length != 0) return bad("IEND has data"); ended = true; }
        at += 12 + length;
    }
    if (!ended || !sawIdat) return bad("Missing IDAT or IEND");
    int w, h; if (!dimensions(b, w, h)) return bad("Malformed IHDR");
    if (width) *width = w;
    if (height) *height = h;
    return {};
}
std::string stripDisallowedChunks(std::string_view b) {
    if (!hasSignature(b)) return {};
    std::string out(b.substr(0, 8));
    size_t at = 8;
    while (at + 12 <= b.size()) {
        const auto length = readBE(b, at);
        if (b.size() - at - 12 < length) break;
        const std::string type(b.substr(at + 4, 4));
        bool ok = false; for (const auto& a : allowed) if (a == type) ok = true;
        if (ok) out.append(b.substr(at, 12 + length));
        at += 12 + length;
        if (type == "IEND") break;
    }
    return out;
}
}
