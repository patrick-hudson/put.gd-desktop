#pragma once
// GDI acquisition for the capture helper: virtual-screen geometry, window
// enumeration and pixel grabs into 32-bpp top-down DIB sections. All coordinates
// are physical pixels in virtual-screen space; the process must be per-monitor
// DPI aware (v2) before any function here runs.
#include "common/Error.h"
#include <cstdint>
#include <string>
#include <vector>
#include <windows.h>

namespace putgd::grab {
struct Rect {
    int x = 0, y = 0, w = 0, h = 0;
    bool empty() const { return w <= 0 || h <= 0; }
    bool contains(int px, int py) const { return px >= x && py >= y && px < x + w && py < y + h; }
    bool operator==(const Rect&) const = default;
};
Rect intersect(const Rect&, const Rect&);
Rect fromRECT(const RECT&);

Rect virtualScreen();
std::vector<Rect> monitors();

struct Limits { long long pixels = 0, bytes = 0; };
// input_excessive when w*h > pixels or w*h*4 > bytes.
Error checkLimits(int width, int height, const Limits&);

// An owned 32-bpp top-down DIB section selected into a memory DC.
class Image {
public:
    Image() = default;
    ~Image();
    Image(const Image&) = delete;
    Image& operator=(const Image&) = delete;
    bool create(int width, int height);
    void release();
    int width() const { return width_; }
    int height() const { return height_; }
    size_t stride() const { return size_t(width_) * 4; }
    uint8_t* bits() const { return bits_; }
    HDC dc() const { return dc_; }
private:
    int width_ = 0, height_ = 0;
    uint8_t* bits_ = nullptr;
    HBITMAP bitmap_ = nullptr, previous_ = nullptr;
    HDC dc_ = nullptr;
};

// A sub-rectangle of an Image to encode (bits points at its first pixel).
struct View { const uint8_t* bits = nullptr; int width = 0, height = 0; size_t stride = 0; };

// BitBlt(SRCCOPY | CAPTUREBLT) of `area` from the screen; virtual-screen areas no
// monitor covers are filled black. Errors: input_excessive, capture_failed.
Error captureScreen(const Rect& area, bool includeCursor, const Limits&, Image& out, View& view);
// PrintWindow(PW_RENDERFULLCONTENT) cropped to `frame` (the DWM extended frame
// bounds); falls back to a screen grab of `frame` when PrintWindow fails or
// renders only black. Errors: input_excessive, capture_failed.
Error captureWindow(HWND window, const Rect& frame, bool includeCursor, const Limits&, Image& out, View& view);

struct WindowInfo {
    HWND hwnd = nullptr;
    Rect frame;
    DWORD pid = 0;
};
// Top-level capture candidates in z-order (topmost first): visible, not minimized,
// not cloaked, not WS_EX_TOOLWINDOW, non-empty extended frame bounds, not the
// desktop (Progman/WorkerW), not owned by this process or any excluded pid.
std::vector<WindowInfo> candidates(const std::vector<DWORD>& excludePids);
// The first candidate whose frame contains the point.
const WindowInfo* windowAt(const std::vector<WindowInfo>&, int x, int y);
bool frameBounds(HWND, Rect&);
// Window title as UTF-8, at most 256 code points, control characters replaced.
std::string windowTitle(HWND);
}
