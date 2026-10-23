#pragma once
// The live selection overlay of the capture helper: one WS_EX_LAYERED topmost
// tool window covering the virtual screen, drawn with UpdateLayeredWindowIndirect
// and per-pixel alpha. The desktop stays live underneath: every pixel is fully
// transparent apart from an alpha-1 input layer (1/255 black, needed so the
// window receives mouse input), the crosshair, the selection/window outline, the
// size label and a faint dim (alpha 48) outside the selection while dragging.
// The window is hidden and flushed from the compositor before any pixels are
// acquired, so the saved image never contains overlay pixels.
//
// Threading: create, input and hide all happen on the thread that runs the
// message loop. The selector calls PostQuitMessage(0) once it has a result.
#include "capture/Grab.h"
#include "common/Error.h"
#include <cstdint>
#include <string>
#include <vector>
#include <windows.h>

namespace putgd::overlay {
enum class Mode { Region, Window };

struct Result {
    enum class Kind { None, Selected, Cancelled, Failed };
    Kind kind = Kind::None;
    grab::Rect rect;       // virtual-screen physical pixels
    HWND window = nullptr; // Window mode only
    std::string title;     // Window mode only, <= 256 code points
    DWORD pid = 0;         // Window mode only
    Error error;           // Failed only
};

inline constexpr const wchar_t* windowClassName = L"putgd.capture.overlay";

class Selector {
public:
    Selector() = default;
    ~Selector();
    Selector(const Selector&) = delete;
    Selector& operator=(const Selector&) = delete;
    // Creates, paints and shows the overlay and takes the foreground if allowed.
    Error show(Mode mode, const std::vector<DWORD>& excludePids);
    // Ends selection with Cancelled (Escape, right click, parent cancel).
    void cancel();
    void fail(Error error);
    // Hides the overlay, waits until the compositor has dropped it (two
    // DwmFlush calls and ~60 ms), then destroys it and frees its surface.
    void hide();
    const Result& result() const { return result_; }

private:
    static LRESULT CALLBACK windowProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT handle(UINT, WPARAM, LPARAM);
    void finish(Result result);

    // Drawing (client coordinates, physical pixels).
    void paint(const RECT& dirty);
    void push(const RECT& dirty);
    void redraw(std::vector<RECT> dirty);
    void redrawAll();
    void fill(const RECT& clip, const RECT& area, uint32_t pixel);
    void ring(const RECT& clip, const RECT& outer, int thickness, uint32_t pixel);
    void crosshair(const RECT& clip);
    void blitLabel(const RECT& clip);
    void renderLabel();
    RECT selectionRect() const;

    void pointerMoved(int x, int y);
    void pressed(int x, int y);
    void released(int x, int y);
    void hover(int x, int y);

    Mode mode_ = Mode::Region;
    std::vector<DWORD> exclude_;
    HWND hwnd_ = nullptr;
    grab::Rect screen_;
    HDC dc_ = nullptr;
    HBITMAP bitmap_ = nullptr, previous_ = nullptr;
    uint32_t* pixels_ = nullptr;
    bool hotkey_ = false;
    bool finished_ = false;
    Result result_;

    // Region state.
    POINT cursor_{-1, -1};
    bool dragging_ = false, releasing_ = false;
    POINT anchor_{}, current_{};
    std::vector<uint32_t> label_;
    int labelWidth_ = 0, labelHeight_ = 0;
    RECT labelRect_{};
    HFONT font_ = nullptr;
    UINT fontDpi_ = 0;
    // Window state.
    bool hasHover_ = false;
    RECT hoverFrame_{};
};
}
