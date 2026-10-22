#include "core/Zip.h"
#include "core/LogFormat.h"
#include "core/SanitizedPng.h"

namespace putgd {
namespace {
void u16(std::string& s, uint16_t v) { s += char(v & 0xFF); s += char(v >> 8); }
void u32(std::string& s, uint32_t v) { for (int i = 0; i < 4; ++i) s += char((v >> (8 * i)) & 0xFF); }
}
Zip::Zip(long long createdMs) {
    const auto stamp = logformat::iso(createdMs); // yyyy-mm-ddThh:mm:ss.mmmZ
    auto n = [&](size_t at, size_t count) { return std::stoi(stamp.substr(at, count)); };
    const int year = n(0, 4) < 1980 ? 1980 : n(0, 4);
    time_ = uint16_t((n(11, 2) << 11) | (n(14, 2) << 5) | (n(17, 2) / 2));
    date_ = uint16_t(((year - 1980) << 9) | (n(5, 2) << 5) | n(8, 2));
}
void Zip::add(const std::string& name, const std::string& bytes) {
    const auto crc = uint32_t(png::crc32(bytes));
    const uint32_t offset = uint32_t(data_.size());
    u32(data_, 0x04034b50); u16(data_, 20); u16(data_, 0x0800); u16(data_, 0); u16(data_, time_); u16(data_, date_); u32(data_, crc); u32(data_, uint32_t(bytes.size())); u32(data_, uint32_t(bytes.size()));
    u16(data_, uint16_t(name.size())); u16(data_, 0); data_ += name; data_ += bytes;
    u32(central_, 0x02014b50); u16(central_, 20); u16(central_, 20); u16(central_, 0x0800); u16(central_, 0); u16(central_, time_); u16(central_, date_); u32(central_, crc); u32(central_, uint32_t(bytes.size())); u32(central_, uint32_t(bytes.size()));
    u16(central_, uint16_t(name.size())); u16(central_, 0); u16(central_, 0); u16(central_, 0); u16(central_, 0); u32(central_, 0); u32(central_, offset); central_ += name;
    ++entries_;
}
std::string Zip::finish() const {
    std::string out = data_ + central_;
    u32(out, 0x06054b50); u16(out, 0); u16(out, 0); u16(out, uint16_t(entries_)); u16(out, uint16_t(entries_)); u32(out, uint32_t(central_.size())); u32(out, uint32_t(data_.size())); u16(out, 0);
    return out;
}
}
