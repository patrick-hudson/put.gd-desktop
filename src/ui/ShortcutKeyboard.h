#pragma once
#include <QKeyCombination>
#include <cstdint>
#include <memory>
class QKeyEvent;
struct xkb_keymap;

namespace putgd {
// The chord the shortcut recorder stores for a Shift press. Qt reports the
// Shift-level symbol (Shift+4 is Key_Dollar); the configuration form names
// the physical key's unshifted digit or punctuation (Ctrl+Shift+4), matching
// the defaults. Letters, special keys and non-ASCII base symbols (French é on
// the 2 key) keep Qt's key. Any mismatch with the keyboard map keeps Qt's key.
QKeyCombination unshiftedChord(QKeyCombination pressed,uint32_t baseSymbol,uint32_t shiftedSymbol);
// The same decision from an xkb map, active layout and key code.
QKeyCombination unshiftedChord(QKeyCombination pressed,xkb_keymap*,uint32_t layout,uint32_t keycode);

// Reads the session's keyboard map while recording: a private wl_keyboard
// on Qt's Wayland seat (Qt 6.5+), or the X11 core key symbols. Without one,
// recorded chords keep Qt's key.
class ShortcutKeyboard final {
public:
 ShortcutKeyboard();
 ~ShortcutKeyboard();
 ShortcutKeyboard(const ShortcutKeyboard&)=delete;
 ShortcutKeyboard& operator=(const ShortcutKeyboard&)=delete;
 QKeyCombination chord(const QKeyEvent&);
private:
 struct Private;
 std::unique_ptr<Private> d_;
};
}
