#pragma once
// Global shortcuts through RegisterHotKey on the resident's message window.
// Desired bindings come from settings; effective registrations are reported
// separately (a conflict with another app never takes its binding).
#include "common/Json.h"
#include <functional>
#include <map>
#include <string>
#include <windows.h>

namespace putgd {
class Hotkeys {
public:
    explicit Hotkeys(HWND owner);
    ~Hotkeys();
    // action name -> PortableText (empty = unbound). Re-registers everything.
    void apply(const std::map<std::string, std::string>& desired);
    // Shortcut recording pauses every registration; resume restores the latest desired set.
    void pause();
    void resume();
    bool paused() const { return paused_; }
    // WM_HOTKEY: returns the action name, or empty.
    std::string actionFor(WPARAM id) const;
    // capabilities.shortcuts.bindings: {action: {configured, bound, error}}
    json::Object status() const;
    bool available() const { return true; }
private:
    void unregisterAll();
    void registerAll();
    HWND owner_;
    bool paused_ = false;
    struct Binding { std::string text; bool bound = false; std::string error; int id = 0; };
    std::map<std::string, Binding> bindings_;
};
}
