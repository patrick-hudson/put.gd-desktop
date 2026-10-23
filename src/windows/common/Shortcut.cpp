#include "common/Shortcut.h"
#include "common/Text.h"
#include <vector>

namespace putgd::shortcut {
namespace {
struct Key { const char* name; uint32_t vk; };
// Names follow Qt's PortableText spelling; aliases map to the same key.
const Key keys[] = {
    {"Space", 0x20}, {"Return", 0x0D}, {"Enter", 0x0D}, {"Tab", 0x09}, {"Backspace", 0x08}, {"Esc", 0x1B}, {"Escape", 0x1B},
    {"Del", 0x2E}, {"Delete", 0x2E}, {"Ins", 0x2D}, {"Insert", 0x2D}, {"Home", 0x24}, {"End", 0x23}, {"PgUp", 0x21}, {"PageUp", 0x21},
    {"PgDown", 0x22}, {"PageDown", 0x22}, {"Left", 0x25}, {"Up", 0x26}, {"Right", 0x27}, {"Down", 0x28}, {"Print", 0x2C}, {"SysReq", 0x2C},
    {"Pause", 0x13}, {"ScrollLock", 0x91}, {"NumLock", 0x90}, {"CapsLock", 0x14}, {"Menu", 0x5D}, {"Help", 0x2F},
    {"F1", 0x70}, {"F2", 0x71}, {"F3", 0x72}, {"F4", 0x73}, {"F5", 0x74}, {"F6", 0x75}, {"F7", 0x76}, {"F8", 0x77}, {"F9", 0x78}, {"F10", 0x79},
    {"F11", 0x7A}, {"F12", 0x7B}, {"F13", 0x7C}, {"F14", 0x7D}, {"F15", 0x7E}, {"F16", 0x7F}, {"F17", 0x80}, {"F18", 0x81}, {"F19", 0x82},
    {"F20", 0x83}, {"F21", 0x84}, {"F22", 0x85}, {"F23", 0x86}, {"F24", 0x87},
    {"`", 0xC0}, {"QuoteLeft", 0xC0}, {"-", 0xBD}, {"Minus", 0xBD}, {"=", 0xBB}, {"Equal", 0xBB}, {"[", 0xDB}, {"BracketLeft", 0xDB},
    {"]", 0xDD}, {"BracketRight", 0xDD}, {"\\", 0xDC}, {"Backslash", 0xDC}, {";", 0xBA}, {"Semicolon", 0xBA}, {"'", 0xDE}, {"Apostrophe", 0xDE},
    {",", 0xBC}, {"Comma", 0xBC}, {".", 0xBE}, {"Period", 0xBE}, {"/", 0xBF}, {"Slash", 0xBF},
    {"MediaPlay", 0xB3}, {"MediaStop", 0xB2}, {"MediaNext", 0xB0}, {"MediaPrevious", 0xB1}, {"VolumeUp", 0xAF}, {"VolumeDown", 0xAE}, {"VolumeMute", 0xAD},
};
std::optional<uint32_t> keyFor(std::string_view name) {
    if (name.size() == 1) {
        const char c = name[0];
        if (c >= 'A' && c <= 'Z') return uint32_t(c);
        if (c >= 'a' && c <= 'z') return uint32_t(c - 'a' + 'A');
        if (c >= '0' && c <= '9') return uint32_t(c);
    }
    for (const auto& key : keys) if (text::lower(name) == text::lower(key.name)) return key.vk;
    return std::nullopt;
}
const char* nameFor(uint32_t vk) {
    static char letter[2] = {0, 0};
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) { letter[0] = char(vk); return letter; }
    for (const auto& key : keys) if (key.vk == vk) return key.name;
    return "";
}
}
std::optional<Chord> parse(std::string_view s) {
    if (s.empty() || !text::printable(s)) return std::nullopt;
    Chord chord;
    bool haveKey = false;
    // "Ctrl++" means Ctrl and the plus key; split on '+' but keep a trailing '+'.
    std::vector<std::string> parts;
    std::string current;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '+' && !current.empty() && i + 1 < s.size()) { parts.push_back(current); current.clear(); }
        else if (s[i] == '+' && current.empty() && i + 1 == s.size()) { current = "+"; }
        else if (s[i] == '+' && !current.empty() && i + 1 == s.size()) { parts.push_back(current); current = "+"; }
        else current += s[i];
    }
    if (!current.empty()) parts.push_back(current);
    for (const auto& raw : parts) {
        const auto part = text::trim(raw);
        const auto lowered = text::lower(part);
        if (lowered == "ctrl" || lowered == "control") chord.modifiers |= modControl;
        else if (lowered == "shift") chord.modifiers |= modShift;
        else if (lowered == "alt") chord.modifiers |= modAlt;
        else if (lowered == "meta" || lowered == "win" || lowered == "super") chord.modifiers |= modWin;
        else if (lowered == "+") { if (haveKey) return std::nullopt; chord.virtualKey = 0xBB; haveKey = true; }
        else {
            if (haveKey) return std::nullopt;
            const auto vk = keyFor(part);
            if (!vk) return std::nullopt;
            chord.virtualKey = *vk; haveKey = true;
        }
    }
    if (!haveKey) return std::nullopt;
    return chord;
}
std::string toPortableText(const Chord& chord) {
    std::string r;
    if (chord.modifiers & modControl) r += "Ctrl+";
    if (chord.modifiers & modShift) r += "Shift+";
    if (chord.modifiers & modAlt) r += "Alt+";
    if (chord.modifiers & modWin) r += "Meta+";
    const auto* name = nameFor(chord.virtualKey);
    if (!*name) return {};
    return r + name;
}
bool valid(std::string_view s) { return parse(s).has_value(); }
}
