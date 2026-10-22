#include "core/FileDialogState.h"

namespace putgd {
namespace {
constexpr const char* backend = "native";
}
void FileDialogState::begin(std::string chooserMode, std::string chooserTitle) {
    open = true;
    mode = std::move(chooserMode);
    title = std::move(chooserTitle);
}
void FileDialogState::finish(const char* status, const std::string& path, const json::Value& error) {
    last = {{"status", status}, {"backend", backend}, {"mode", mode}, {"path", path.empty() ? json::Value() : json::Value(path)}, {"fallbackReason", json::Value()}, {"error", error}};
    open = false;
}
void FileDialogState::chosen(const std::string& path) { finish("chosen", path, json::Value()); }
void FileDialogState::cancelled() { finish("cancelled", {}, json::Value()); }
void FileDialogState::failed(const std::string& code, const std::string& message) { finish("failed", {}, json::Object{{"code", code}, {"message", message}}); }
json::Object FileDialogState::toJson() const {
    auto when = [this](const std::string& value) { return open ? json::Value(value) : json::Value(); };
    return {{"open", open}, {"mode", when(mode)}, {"title", when(title)}, {"backend", open ? json::Value(backend) : json::Value()},
            {"parentWindow", json::Value()}, {"fallbackReason", json::Value()}, {"last", last.empty() ? json::Value() : json::Value(last)}};
}
}
