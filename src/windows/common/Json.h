#pragma once
// A small JSON model with the semantics the protocol depends on:
//  - objects keep their keys sorted (like QJsonObject), so compact output and
//    digests over it are deterministic;
//  - numbers are doubles; integral values within 2^53 print without a fraction;
//  - parsing rejects anything RFC 8259 rejects, NaN/Infinity, lone surrogates and
//    nesting deeper than 512 levels; duplicate keys keep the last value.
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace putgd::json {
class Value;
class Object;
using Array = std::vector<Value>;
enum class Type { Null, Bool, Number, String, Array, Object };

class Object {
public:
    using Map = std::map<std::string, Value>;
    Object() = default;
    template <class Pair = std::pair<const std::string, Value>> Object(std::initializer_list<Pair> init);
    bool contains(std::string_view key) const;
    // A missing key reads as a null value, as with QJsonObject.
    const Value& value(std::string_view key) const;
    Value& operator[](std::string_view key);
    const Value& operator[](std::string_view key) const { return value(key); }
    bool remove(std::string_view key);
    Value take(std::string_view key);
    bool empty() const;
    size_t size() const;
    std::vector<std::string> keys() const;
    Map::const_iterator begin() const;
    Map::const_iterator end() const;
    Map::iterator begin();
    Map::iterator end();
    bool operator==(const Object&) const;
    bool operator!=(const Object& o) const { return !(*this == o); }
private:
    Map map_;
};

class Value {
public:
    Value() = default;
    Value(std::nullptr_t) {}
    Value(bool b) : type_(Type::Bool), bool_(b) {}
    Value(double d) : type_(Type::Number), number_(d) {}
    Value(int i) : type_(Type::Number), number_(i) {}
    Value(long long i) : type_(Type::Number), number_(double(i)) {}
    Value(unsigned long long i) : type_(Type::Number), number_(double(i)) {}
    Value(const char* s) : type_(Type::String), string_(s) {}
    Value(std::string s) : type_(Type::String), string_(std::move(s)) {}
    Value(std::string_view s) : type_(Type::String), string_(s) {}
    Value(Array a) : type_(Type::Array), array_(std::move(a)) {}
    Value(Object o) : type_(Type::Object), object_(std::move(o)) {}
    static Value null() { return {}; }

    Type type() const { return type_; }
    bool isNull() const { return type_ == Type::Null; }
    bool isBool() const { return type_ == Type::Bool; }
    bool isNumber() const { return type_ == Type::Number; }
    bool isString() const { return type_ == Type::String; }
    bool isArray() const { return type_ == Type::Array; }
    bool isObject() const { return type_ == Type::Object; }
    bool isUndefined() const { return type_ == Type::Null; }

    bool toBool(bool fallback = false) const { return isBool() ? bool_ : fallback; }
    double toDouble(double fallback = 0) const { return isNumber() ? number_ : fallback; }
    // Integral conversion: the value must be a finite integer in range, else fallback.
    long long toInteger(long long fallback = 0) const;
    int toInt(int fallback = 0) const;
    std::string toString(std::string fallback = {}) const { return isString() ? string_ : std::move(fallback); }
    const std::string& stringRef() const { return string_; }
    Array toArray() const { return isArray() ? array_ : Array{}; }
    const Array& arrayRef() const { return array_; }
    Object toObject() const { return isObject() ? object_ : Object{}; }
    const Object& objectRef() const { return object_; }
    Object& objectRef() { return object_; }
    Array& arrayRef() { return array_; }
    // Object member access on a non-object yields null.
    const Value& operator[](std::string_view key) const;
    const Value& operator[](size_t index) const;
    bool operator==(const Value&) const;
    bool operator!=(const Value& v) const { return !(*this == v); }
    bool operator==(const char* s) const { return isString() && string_ == s; }
    bool operator!=(const char* s) const { return !(*this == s); }
    static const Value& nullValue();
private:
    Type type_ = Type::Null;
    bool bool_ = false;
    double number_ = 0;
    std::string string_;
    Array array_;
    Object object_;
};

template <class Pair> Object::Object(std::initializer_list<Pair> init) { for (const auto& pair : init) map_[pair.first] = pair.second; }
inline bool Object::contains(std::string_view key) const { return map_.find(std::string(key)) != map_.end(); }
inline bool Object::remove(std::string_view key) { return map_.erase(std::string(key)) > 0; }
inline bool Object::empty() const { return map_.empty(); }
inline size_t Object::size() const { return map_.size(); }
inline Object::Map::const_iterator Object::begin() const { return map_.begin(); }
inline Object::Map::const_iterator Object::end() const { return map_.end(); }
inline Object::Map::iterator Object::begin() { return map_.begin(); }
inline Object::Map::iterator Object::end() { return map_.end(); }

struct ParseError { bool failed = false; size_t offset = 0; std::string message; };
// Parses one complete document (object or array at the root, as QJsonDocument).
Value parse(std::string_view text, ParseError* error = nullptr);
std::optional<Object> parseObject(std::string_view text);
// Compact serialization with sorted keys and minimal escaping.
std::string serialize(const Value&);
std::string serialize(const Object&);
std::string serialize(const Array&);
// Human-readable form with two-space indentation (settings files).
std::string pretty(const Value&);
// True when the value is a finite integer between min and max (inclusive).
bool integer(const Value&, double min, double max);
Array stringArray(const std::vector<std::string>&);
std::vector<std::string> toStringVector(const Array&);
}
