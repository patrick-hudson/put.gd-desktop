#pragma once
// Qt PortableText shortcut strings ("Ctrl+Shift+1", "Meta+Print") are the
// configuration format shared with the Linux app, the CLI and the MCP helper.
// This maps them to Windows hotkey modifiers and virtual-key codes.
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace putgd::shortcut {
struct Chord {
    uint32_t modifiers = 0; // MOD_ALT=1, MOD_CONTROL=2, MOD_SHIFT=4, MOD_WIN=8 (RegisterHotKey values)
    uint32_t virtualKey = 0;
};
inline constexpr uint32_t modAlt = 1, modControl = 2, modShift = 4, modWin = 8;
// One combination with exactly one known key; modifiers may repeat or appear in
// any order. Empty strings are "unbound" and parse to nothing.
std::optional<Chord> parse(std::string_view portableText);
// Canonical PortableText for a chord ("Ctrl+Alt+F4"); empty for unknown keys.
std::string toPortableText(const Chord&);
bool valid(std::string_view portableText);
}
