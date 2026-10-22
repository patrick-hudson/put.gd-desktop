#include "core/Mp4.h"
#include <set>

namespace putgd::mp4 {
namespace {
uint32_t be32(const std::string& s, size_t at) { return (uint32_t(uint8_t(s[at])) << 24) | (uint32_t(uint8_t(s[at + 1])) << 16) | (uint32_t(uint8_t(s[at + 2])) << 8) | uint32_t(uint8_t(s[at + 3])); }
uint64_t be64(const std::string& s, size_t at) { return (uint64_t(be32(s, at)) << 32) | be32(s, at + 4); }
void put32(std::string& s, size_t at, uint32_t v) { for (int i = 0; i < 4; ++i) s[at + size_t(i)] = char((v >> (24 - 8 * i)) & 0xFF); }
void put64(std::string& s, size_t at, uint64_t v) { put32(s, at, uint32_t(v >> 32)); put32(s, at + 4, uint32_t(v)); }
// Walks the boxes in [begin, end) of `data`, descending into the sample-table path.
bool walk(std::string& data, size_t begin, size_t end, uint64_t shift) {
    static const std::set<std::string> containers{"moov", "trak", "mdia", "minf", "stbl"};
    size_t at = begin;
    while (at < end) {
        if (end - at < 8) return false;
        uint64_t size = be32(data, at);
        size_t header = 8;
        const std::string type = data.substr(at + 4, 4);
        if (size == 1) { if (end - at < 16) return false; size = be64(data, at + 8); header = 16; }
        else if (size == 0) size = end - at;
        if (size < header || size > end - at) return false;
        const size_t body = at + header, boxEnd = at + size_t(size);
        if (containers.count(type)) { if (!walk(data, body, boxEnd, shift)) return false; }
        else if (type == "stco" || type == "co64") {
            const size_t entry = type == "stco" ? 4 : 8;
            if (boxEnd - body < 8) return false;
            const uint64_t count = be32(data, body + 4);
            if (count > (boxEnd - body - 8) / entry) return false;
            for (uint64_t i = 0; i < count; ++i) {
                const size_t p = body + 8 + size_t(i) * entry;
                if (entry == 4) { const uint64_t v = uint64_t(be32(data, p)) + shift; if (v > 0xFFFFFFFFull) return false; put32(data, p, uint32_t(v)); }
                else put64(data, p, be64(data, p) + shift);
            }
        }
        at = boxEnd;
    }
    return true;
}
}
std::vector<Box> topLevel(uint64_t fileSize, const std::function<std::string(uint64_t, size_t)>& read) {
    std::vector<Box> boxes;
    uint64_t at = 0;
    while (at < fileSize) {
        const auto head = read(at, 16);
        if (head.size() < 8) return {};
        uint64_t size = be32(head, 0);
        uint64_t header = 8;
        if (size == 1) { if (head.size() < 16) return {}; size = be64(head, 8); header = 16; }
        else if (size == 0) size = fileSize - at;
        if (size < header || size > fileSize - at) return {};
        boxes.push_back({head.substr(4, 4), at, size});
        at += size;
    }
    return boxes;
}
bool shiftChunkOffsets(std::string& moov, uint64_t shift) {
    std::string copy = moov;
    if (copy.size() < 8 || copy.substr(4, 4) != "moov" || !walk(copy, 0, copy.size(), shift)) return false;
    moov.swap(copy);
    return true;
}
}
