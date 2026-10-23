#include "common/Json.h"
#include <cmath>
#include <cstdio>
#include <cstring>

namespace putgd::json {
const Value& Object::value(std::string_view key) const {
    const auto it = map_.find(std::string(key));
    return it == map_.end() ? Value::nullValue() : it->second;
}
Value& Object::operator[](std::string_view key) { return map_[std::string(key)]; }
Value Object::take(std::string_view key) {
    const auto it = map_.find(std::string(key));
    if (it == map_.end()) return {};
    Value v = std::move(it->second);
    map_.erase(it);
    return v;
}
std::vector<std::string> Object::keys() const {
    std::vector<std::string> result;
    result.reserve(map_.size());
    for (const auto& pair : map_) result.push_back(pair.first);
    return result;
}
bool Object::operator==(const Object& other) const { return map_ == other.map_; }

const Value& Value::nullValue() { static const Value value; return value; }
long long Value::toInteger(long long fallback) const {
    if (!isNumber() || !std::isfinite(number_) || std::floor(number_) != number_ || std::fabs(number_) > 9007199254740992.0) return fallback;
    return static_cast<long long>(number_);
}
int Value::toInt(int fallback) const {
    const auto v = toInteger(static_cast<long long>(fallback));
    if (v > 2147483647LL || v < -2147483648LL) return fallback;
    return static_cast<int>(v);
}
const Value& Value::operator[](std::string_view key) const { return isObject() ? object_.value(key) : nullValue(); }
const Value& Value::operator[](size_t index) const { return isArray() && index < array_.size() ? array_[index] : nullValue(); }
bool Value::operator==(const Value& other) const {
    if (type_ != other.type_) return false;
    switch (type_) {
    case Type::Null: return true;
    case Type::Bool: return bool_ == other.bool_;
    case Type::Number: return number_ == other.number_;
    case Type::String: return string_ == other.string_;
    case Type::Array: return array_ == other.array_;
    case Type::Object: return object_ == other.object_;
    }
    return false;
}

namespace {
struct Parser {
    std::string_view text;
    size_t pos = 0;
    ParseError error;
    int depth = 0;
    void fail(const char* message) { if (!error.failed) { error.failed = true; error.offset = pos; error.message = message; } }
    void skip() { while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\t' || text[pos] == '\n' || text[pos] == '\r')) ++pos; }
    bool consume(char c) { skip(); if (pos < text.size() && text[pos] == c) { ++pos; return true; } return false; }
    static void appendUtf8(std::string& out, uint32_t cp) {
        if (cp < 0x80) out += char(cp);
        else if (cp < 0x800) { out += char(0xC0 | (cp >> 6)); out += char(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) { out += char(0xE0 | (cp >> 12)); out += char(0x80 | ((cp >> 6) & 0x3F)); out += char(0x80 | (cp & 0x3F)); }
        else { out += char(0xF0 | (cp >> 18)); out += char(0x80 | ((cp >> 12) & 0x3F)); out += char(0x80 | ((cp >> 6) & 0x3F)); out += char(0x80 | (cp & 0x3F)); }
    }
    bool hex4(uint32_t& value) {
        if (pos + 4 > text.size()) return false;
        value = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = text[pos++];
            value <<= 4;
            if (c >= '0' && c <= '9') value |= uint32_t(c - '0');
            else if (c >= 'a' && c <= 'f') value |= uint32_t(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') value |= uint32_t(c - 'A' + 10);
            else return false;
        }
        return true;
    }
    bool validUtf8(std::string_view s, size_t& i) {
        const auto b = static_cast<unsigned char>(s[i]);
        int extra = 0; uint32_t cp = 0; uint32_t min = 0;
        if (b < 0x80) { ++i; return true; }
        if ((b & 0xE0) == 0xC0) { extra = 1; cp = b & 0x1F; min = 0x80; }
        else if ((b & 0xF0) == 0xE0) { extra = 2; cp = b & 0x0F; min = 0x800; }
        else if ((b & 0xF8) == 0xF0) { extra = 3; cp = b & 0x07; min = 0x10000; }
        else return false;
        for (int k = 1; k <= extra; ++k) {
            if (i + k >= s.size()) return false;
            const auto c = static_cast<unsigned char>(s[i + k]);
            if ((c & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (c & 0x3F);
        }
        if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
        i += extra + 1;
        return true;
    }
    bool string(std::string& out) {
        if (!consume('"')) { fail("expected string"); return false; }
        while (pos < text.size()) {
            const char c = text[pos];
            if (c == '"') { ++pos; return true; }
            if (c == '\\') {
                ++pos;
                if (pos >= text.size()) break;
                const char e = text[pos++];
                switch (e) {
                case '"': out += '"'; break; case '\\': out += '\\'; break; case '/': out += '/'; break;
                case 'b': out += '\b'; break; case 'f': out += '\f'; break; case 'n': out += '\n'; break;
                case 'r': out += '\r'; break; case 't': out += '\t'; break;
                case 'u': {
                    uint32_t cp;
                    if (!hex4(cp)) { fail("bad unicode escape"); return false; }
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        uint32_t low;
                        if (pos + 6 > text.size() || text[pos] != '\\' || text[pos + 1] != 'u') { fail("lone surrogate"); return false; }
                        pos += 2;
                        if (!hex4(low) || low < 0xDC00 || low > 0xDFFF) { fail("lone surrogate"); return false; }
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                    } else if (cp >= 0xDC00 && cp <= 0xDFFF) { fail("lone surrogate"); return false; }
                    appendUtf8(out, cp);
                    break;
                }
                default: fail("bad escape"); return false;
                }
                continue;
            }
            if (static_cast<unsigned char>(c) < 0x20) { fail("control character in string"); return false; }
            size_t i = pos;
            if (!validUtf8(text, i)) { fail("invalid UTF-8"); return false; }
            out.append(text.substr(pos, i - pos));
            pos = i;
        }
        fail("unterminated string");
        return false;
    }
    bool number(double& out) {
        const size_t start = pos;
        if (pos < text.size() && text[pos] == '-') ++pos;
        if (pos >= text.size()) { fail("bad number"); return false; }
        if (text[pos] == '0') ++pos;
        else if (text[pos] >= '1' && text[pos] <= '9') { while (pos < text.size() && isdigit(static_cast<unsigned char>(text[pos]))) ++pos; }
        else { fail("bad number"); return false; }
        if (pos < text.size() && text[pos] == '.') {
            ++pos;
            if (pos >= text.size() || !isdigit(static_cast<unsigned char>(text[pos]))) { fail("bad number"); return false; }
            while (pos < text.size() && isdigit(static_cast<unsigned char>(text[pos]))) ++pos;
        }
        if (pos < text.size() && (text[pos] == 'e' || text[pos] == 'E')) {
            ++pos;
            if (pos < text.size() && (text[pos] == '+' || text[pos] == '-')) ++pos;
            if (pos >= text.size() || !isdigit(static_cast<unsigned char>(text[pos]))) { fail("bad number"); return false; }
            while (pos < text.size() && isdigit(static_cast<unsigned char>(text[pos]))) ++pos;
        }
        const std::string slice(text.substr(start, pos - start));
        char* end = nullptr;
        out = std::strtod(slice.c_str(), &end);
        if (!std::isfinite(out)) { fail("number out of range"); return false; }
        return true;
    }
    bool value(Value& out) {
        skip();
        if (pos >= text.size()) { fail("unexpected end"); return false; }
        if (++depth > 512) { fail("nesting too deep"); return false; }
        bool ok = false;
        const char c = text[pos];
        if (c == '{') {
            ++pos; Object object;
            if (consume('}')) { out = std::move(object); ok = true; }
            else while (true) {
                std::string key;
                if (!string(key)) break;
                if (!consume(':')) { fail("expected colon"); break; }
                Value member;
                if (!value(member)) break;
                object[key] = std::move(member);
                if (consume(',')) continue;
                if (consume('}')) { out = std::move(object); ok = true; break; }
                fail("expected comma or brace"); break;
            }
        } else if (c == '[') {
            ++pos; Array array;
            if (consume(']')) { out = std::move(array); ok = true; }
            else while (true) {
                Value element;
                if (!value(element)) break;
                array.push_back(std::move(element));
                if (consume(',')) continue;
                if (consume(']')) { out = std::move(array); ok = true; break; }
                fail("expected comma or bracket"); break;
            }
        } else if (c == '"') {
            std::string s;
            if (string(s)) { out = Value(std::move(s)); ok = true; }
        } else if (text.compare(pos, 4, "true") == 0) { pos += 4; out = Value(true); ok = true; }
        else if (text.compare(pos, 5, "false") == 0) { pos += 5; out = Value(false); ok = true; }
        else if (text.compare(pos, 4, "null") == 0) { pos += 4; out = Value(); ok = true; }
        else { double d; if (number(d)) { out = Value(d); ok = true; } }
        --depth;
        return ok;
    }
};

void escape(std::string& out, const std::string& s) {
    out += '"';
    for (const unsigned char c : s) {
        switch (c) {
        case '"': out += "\\\""; break; case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break; case '\f': out += "\\f"; break; case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break; case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) { char buf[8]; std::snprintf(buf, sizeof buf, "\\u%04x", c); out += buf; }
            else out += char(c);
        }
    }
    out += '"';
}
void number(std::string& out, double d) {
    if (std::isfinite(d) && std::floor(d) == d && std::fabs(d) < 9007199254740992.0) {
        char buf[32]; std::snprintf(buf, sizeof buf, "%lld", static_cast<long long>(d)); out += buf; return;
    }
    if (!std::isfinite(d)) { out += "null"; return; }
    char buf[32]; std::snprintf(buf, sizeof buf, "%.17g", d);
    // Shortest round-trip form.
    for (int precision = 1; precision <= 17; ++precision) {
        char probe[32]; std::snprintf(probe, sizeof probe, "%.*g", precision, d);
        if (std::strtod(probe, nullptr) == d) { out += probe; return; }
    }
    out += buf;
}
void write(std::string& out, const Value& v, int indent, int level);
void writeObject(std::string& out, const Object& o, int indent, int level) {
    if (o.empty()) { out += "{}"; return; }
    out += '{';
    bool first = true;
    for (const auto& pair : o) {
        if (!first) out += ',';
        first = false;
        if (indent) { out += '\n'; out.append(size_t(indent * (level + 1)), ' '); }
        escape(out, pair.first);
        out += indent ? ": " : ":";
        write(out, pair.second, indent, level + 1);
    }
    if (indent) { out += '\n'; out.append(size_t(indent * level), ' '); }
    out += '}';
}
void writeArray(std::string& out, const Array& a, int indent, int level) {
    if (a.empty()) { out += "[]"; return; }
    out += '[';
    bool first = true;
    for (const auto& v : a) {
        if (!first) out += ',';
        first = false;
        if (indent) { out += '\n'; out.append(size_t(indent * (level + 1)), ' '); }
        write(out, v, indent, level + 1);
    }
    if (indent) { out += '\n'; out.append(size_t(indent * level), ' '); }
    out += ']';
}
void write(std::string& out, const Value& v, int indent, int level) {
    switch (v.type()) {
    case Type::Null: out += "null"; break;
    case Type::Bool: out += v.toBool() ? "true" : "false"; break;
    case Type::Number: number(out, v.toDouble()); break;
    case Type::String: escape(out, v.stringRef()); break;
    case Type::Array: writeArray(out, v.arrayRef(), indent, level); break;
    case Type::Object: writeObject(out, v.objectRef(), indent, level); break;
    }
}
}

Value parse(std::string_view text, ParseError* error) {
    Parser parser; parser.text = text;
    Value result;
    const bool ok = parser.value(result);
    parser.skip();
    if (ok && parser.pos != text.size()) parser.fail("trailing characters");
    if (ok && !result.isObject() && !result.isArray()) parser.fail("document root must be an object or array");
    if (error) *error = parser.error;
    if (parser.error.failed) return {};
    return result;
}
std::optional<Object> parseObject(std::string_view text) {
    ParseError error;
    auto v = parse(text, &error);
    if (error.failed || !v.isObject()) return std::nullopt;
    return v.toObject();
}
std::string serialize(const Value& v) { std::string out; write(out, v, 0, 0); return out; }
std::string serialize(const Object& o) { std::string out; writeObject(out, o, 0, 0); return out; }
std::string serialize(const Array& a) { std::string out; writeArray(out, a, 0, 0); return out; }
std::string pretty(const Value& v) { std::string out; write(out, v, 2, 0); out += '\n'; return out; }
bool integer(const Value& v, double min, double max) {
    if (!v.isNumber()) return false;
    const double d = v.toDouble();
    return std::isfinite(d) && std::floor(d) == d && d >= min && d <= max;
}
Array stringArray(const std::vector<std::string>& values) { Array a; for (const auto& s : values) a.emplace_back(s); return a; }
std::vector<std::string> toStringVector(const Array& a) { std::vector<std::string> r; for (const auto& v : a) r.push_back(v.toString()); return r; }
}
