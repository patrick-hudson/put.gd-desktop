#include "resident/Hotkeys.h"
#include "common/Log.h"
#include "common/Shortcut.h"

namespace putgd {
namespace {
const std::map<std::string, int>& ids() {
    static const std::map<std::string, int> value{{"region", 1}, {"window", 2}, {"desktop", 3}, {"clipboard", 4}, {"edit", 5}, {"record", 6}};
    return value;
}
}
Hotkeys::Hotkeys(HWND owner) : owner_(owner) {}
Hotkeys::~Hotkeys() { unregisterAll(); }
void Hotkeys::unregisterAll() {
    for (auto& pair : bindings_) if (pair.second.bound) { UnregisterHotKey(owner_, pair.second.id); pair.second.bound = false; }
}
void Hotkeys::registerAll() {
    for (auto& pair : bindings_) {
        auto& binding = pair.second;
        binding.error.clear();
        if (binding.text.empty()) continue;
        const auto chord = shortcut::parse(binding.text);
        if (!chord) { binding.error = "unsupported_shortcut"; continue; }
        if (RegisterHotKey(owner_, binding.id, chord->modifiers | MOD_NOREPEAT, chord->virtualKey)) binding.bound = true;
        else {
            binding.error = GetLastError() == ERROR_HOTKEY_ALREADY_REGISTERED ? "shortcut_conflict" : "shortcut_rejected";
            log::warning("shortcuts", "Global shortcut not registered", {{"action", pair.first}, {"error", binding.error}});
        }
    }
}
void Hotkeys::apply(const std::map<std::string, std::string>& desired) {
    unregisterAll();
    bindings_.clear();
    for (const auto& pair : desired) {
        const auto id = ids().find(pair.first);
        if (id == ids().end()) continue;
        bindings_[pair.first] = {pair.second, false, {}, id->second};
    }
    if (!paused_) registerAll();
}
void Hotkeys::pause() { if (paused_) return; paused_ = true; unregisterAll(); }
void Hotkeys::resume() { if (!paused_) return; paused_ = false; registerAll(); }
std::string Hotkeys::actionFor(WPARAM id) const {
    for (const auto& pair : bindings_) if (pair.second.bound && WPARAM(pair.second.id) == id) return pair.first;
    return {};
}
json::Object Hotkeys::status() const {
    json::Object result;
    for (const auto& pair : bindings_)
        result[pair.first] = json::Object{{"configured", pair.second.text}, {"bound", pair.second.bound}, {"error", pair.second.error}, {"paused", paused_}};
    return result;
}
}
