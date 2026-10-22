#pragma once
// The recording helper's on-screen furniture while a recording is ready or
// running (docs/recording.md, "Overlay"): a ring just outside the recorded area,
// translucent dimming of everything else while recording or paused, and the
// floating control bar. All of them are excluded from screen capture
// (SetWindowDisplayAffinity WDA_EXCLUDEFROMCAPTURE, Windows 10 2004 and later), so
// they never reach the clip even where the bar has to sit inside the area; the
// bar is still placed outside the area when there is room, so it covers nothing.
#include "capture/Grab.h"
#include <functional>
#include <string>
#include <vector>
#include <windows.h>

namespace putgd::record {
// Applies capture exclusion; false on Windows versions without it.
bool excludeFromCapture(HWND);

// One click-through layered window over the virtual screen: the ring (ready) and
// the dimming with a hole for the area (recording, paused).
class AreaFrame {
public:
    ~AreaFrame();
    bool show(const grab::Rect& area);
    void setDimmed(bool dimmed);
    void hide();
    bool excluded() const { return excluded_; }
private:
    HWND edges_[8] = {}; // dimming strips 0-3 (shown while recording), ring edges 4-7
    grab::Rect area_, screen_;
    bool dimmed_ = false, excluded_ = false;
};

class ControlBar {
public:
    struct Callbacks {
        std::function<void()> record, pause, resume, stop, cancel;
        std::function<long long()> elapsedUs;
        std::function<void(bool)> audioChanged;               // the System audio box (ready only)
        std::function<void(const std::string&)> formatChanged; // format or format/codec (ready only)
    };
    explicit ControlBar(Callbacks);
    ~ControlBar();
    // Creates the bar for `area` and places it (outside the area when there is room).
    // `formats` are the menu keys (mp4/h264, gif …); audio is offered when available.
    bool create(const grab::Rect& area, const std::wstring& target, const std::vector<std::string>& formats, const std::string& format, bool audioAvailable, bool audio);
    // Shows the resident's answer to a format or audio change.
    void setChoice(const std::string& format, bool audio);
    // ready | recording | paused | stopping
    void setState(const std::string& state);
    void setVisible(bool visible);
    void raise();
    void destroy();
    HWND hwnd() const { return hwnd_; }
    bool outside() const { return outside_; }
    bool excluded() const { return excluded_; }
    // outside | hidden (inside the area, excluded from capture) | inside (in the clip)
    std::string placement() const { return outside_ ? "outside" : excluded_ ? "hidden" : "inside"; }
private:
    static LRESULT CALLBACK proc(HWND, UINT, WPARAM, LPARAM);
    LRESULT handle(UINT, WPARAM, LPARAM);
    void layout();
    void place();
    void paint(HDC);
    Callbacks callbacks_;
    HWND hwnd_ = nullptr, record_ = nullptr, pause_ = nullptr, stop_ = nullptr, cancel_ = nullptr, tooltip_ = nullptr, format_ = nullptr, audio_ = nullptr;
    bool audioAvailable_ = false;
    grab::Rect area_;
    std::wstring target_;
    std::string state_ = "ready";
    UINT dpi_ = 96;
    bool outside_ = false, excluded_ = false, hidden_ = false, moved_ = false;
};
}
