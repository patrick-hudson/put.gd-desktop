#pragma once
#include "common/Json.h"
#include <string>
#include <utility>

namespace putgd {
// The versioned error envelope shared with the Linux resident: a stable code, a
// fixed message and a details object. An empty code means success.
struct Error {
    std::string code;
    std::string message;
    json::Object details;
    Error() = default;
    Error(std::string c, std::string m, json::Object d = {}) : code(std::move(c)), message(std::move(m)), details(std::move(d)) {}
    explicit operator bool() const { return !code.empty(); }
    json::Object toJson() const { return {{"code", code}, {"message", message}, {"details", details}}; }
    static Error fromJson(const json::Object& o) {
        return {o.value("code").toString(), o.value("message").toString(), o.value("details").toObject()};
    }
};
struct Result {
    Error error;
    bool changed = false;
    explicit operator bool() const { return !error; }
};
}
