#include "capture/Grab.h"
#include "common/Paths.h"
#include <algorithm>
#include <dwmapi.h>

#ifndef PW_RENDERFULLCONTENT
#define PW_RENDERFULLCONTENT 0x00000002
#endif

namespace putgd::grab {
namespace {
Error failed(const char* message) { return {"capture_failed", message}; }

BOOL CALLBACK collectMonitor(HMONITOR monitor, HDC, LPRECT, LPARAM data) {
    MONITORINFO info{};
    info.cbSize = sizeof info;
    if (GetMonitorInfoW(monitor, &info)) reinterpret_cast<std::vector<Rect>*>(data)->push_back(fromRECT(info.rcMonitor));
    return TRUE;
}

// Draws the current cursor (if showing) into dc, whose origin is virtual-screen (originX, originY).
void drawCursor(HDC dc, int originX, int originY) {
    CURSORINFO cursor{};
    cursor.cbSize = sizeof cursor;
    if (!GetCursorInfo(&cursor) || !(cursor.flags & CURSOR_SHOWING) || !cursor.hCursor) return;
    ICONINFO icon{};
    if (!GetIconInfo(cursor.hCursor, &icon)) return;
    DrawIconEx(dc, cursor.ptScreenPos.x - int(icon.xHotspot) - originX, cursor.ptScreenPos.y - int(icon.yHotspot) - originY, cursor.hCursor, 0, 0, 0,
               nullptr, DI_NORMAL);
    if (icon.hbmMask) DeleteObject(icon.hbmMask);
    if (icon.hbmColor) DeleteObject(icon.hbmColor);
}

// Paints every part of `area` that no monitor covers black (deterministic gaps).
void blackenGaps(HDC dc, const Rect& area) {
    HRGN gaps = CreateRectRgn(0, 0, area.w, area.h);
    if (!gaps) return;
    for (const auto& m : monitors()) {
        HRGN monitor = CreateRectRgn(m.x - area.x, m.y - area.y, m.x - area.x + m.w, m.y - area.y + m.h);
        if (monitor) { CombineRgn(gaps, gaps, monitor, RGN_DIFF); DeleteObject(monitor); }
    }
    FillRgn(dc, gaps, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
    DeleteObject(gaps);
}

bool allBlack(const uint8_t* bits, int width, int height, size_t stride) {
    for (int y = 0; y < height; ++y) {
        const uint8_t* p = bits + stride * size_t(y);
        for (int x = 0; x < width; ++x, p += 4) if (p[0] | p[1] | p[2]) return false;
    }
    return true;
}

struct EnumState {
    std::vector<WindowInfo>* out;
    const std::vector<DWORD>* exclude;
    DWORD self;
};

BOOL CALLBACK collectWindow(HWND hwnd, LPARAM data) {
    auto& state = *reinterpret_cast<EnumState*>(data);
    if (!IsWindowVisible(hwnd) || IsIconic(hwnd)) return TRUE;
    if (GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) return TRUE;
    DWORD cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof cloaked)) && cloaked) return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == state.self || std::find(state.exclude->begin(), state.exclude->end(), pid) != state.exclude->end()) return TRUE;
    wchar_t name[32] = {};
    GetClassNameW(hwnd, name, 32);
    if (!lstrcmpW(name, L"Progman") || !lstrcmpW(name, L"WorkerW")) return TRUE;
    WindowInfo info;
    if (!frameBounds(hwnd, info.frame)) return TRUE;
    info.hwnd = hwnd;
    info.pid = pid;
    state.out->push_back(info);
    return TRUE;
}
}

Rect intersect(const Rect& a, const Rect& b) {
    const int left = std::max(a.x, b.x), top = std::max(a.y, b.y);
    const int right = std::min(a.x + a.w, b.x + b.w), bottom = std::min(a.y + a.h, b.y + b.h);
    if (right <= left || bottom <= top) return {};
    return {left, top, right - left, bottom - top};
}
Rect fromRECT(const RECT& r) { return {int(r.left), int(r.top), int(r.right - r.left), int(r.bottom - r.top)}; }

Rect virtualScreen() {
    return {GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN), GetSystemMetrics(SM_CXVIRTUALSCREEN),
            GetSystemMetrics(SM_CYVIRTUALSCREEN)};
}

std::vector<Rect> monitors() {
    std::vector<Rect> result;
    EnumDisplayMonitors(nullptr, nullptr, collectMonitor, reinterpret_cast<LPARAM>(&result));
    return result;
}

Error checkLimits(int width, int height, const Limits& limits) {
    if (width <= 0 || height <= 0) return {"capture_geometry", "The capture area is empty"};
    const long long pixels = (long long)width * height;
    if (pixels > limits.pixels || pixels * 4 > limits.bytes) return {"input_excessive", "The capture exceeds the accepted image limits"};
    return {};
}

Image::~Image() { release(); }
void Image::release() {
    if (dc_) { if (previous_) SelectObject(dc_, previous_); DeleteDC(dc_); }
    if (bitmap_) DeleteObject(bitmap_);
    dc_ = nullptr; bitmap_ = previous_ = nullptr; bits_ = nullptr; width_ = height_ = 0;
}
bool Image::create(int width, int height) {
    release();
    if (width <= 0 || height <= 0) return false;
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height; // top-down
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC screen = GetDC(nullptr);
    bitmap_ = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    dc_ = CreateCompatibleDC(screen);
    ReleaseDC(nullptr, screen);
    if (!bitmap_ || !dc_ || !bits) { release(); return false; }
    previous_ = static_cast<HBITMAP>(SelectObject(dc_, bitmap_));
    bits_ = static_cast<uint8_t*>(bits);
    width_ = width;
    height_ = height;
    return true;
}

Error captureScreen(const Rect& area, bool includeCursor, const Limits& limits, Image& out, View& view) {
    if (auto e = checkLimits(area.w, area.h, limits)) return e;
    if (!out.create(area.w, area.h)) return failed("Could not allocate the capture bitmap");
    HDC screen = GetDC(nullptr);
    if (!screen) return failed("Could not open the screen");
    const BOOL copied = BitBlt(out.dc(), 0, 0, area.w, area.h, screen, area.x, area.y, SRCCOPY | CAPTUREBLT);
    ReleaseDC(nullptr, screen);
    if (!copied) return failed("The screen could not be copied");
    blackenGaps(out.dc(), area);
    if (includeCursor) drawCursor(out.dc(), area.x, area.y);
    GdiFlush();
    view = {out.bits(), area.w, area.h, out.stride()};
    return {};
}

Error captureWindow(HWND window, const Rect& frame, bool includeCursor, const Limits& limits, Image& out, View& view) {
    if (auto e = checkLimits(frame.w, frame.h, limits)) return e;
    if (!IsWindow(window)) return {"capture_geometry", "The selected window closed"};
    RECT windowRect{};
    if (GetWindowRect(window, &windowRect)) {
        const Rect bounds = fromRECT(windowRect);
        const Rect crop = intersect(bounds, frame);
        // The window rectangle includes invisible resize borders; it is at most a
        // few pixels larger than the frame but must still fit the limits.
        if (!crop.empty() && !checkLimits(bounds.w, bounds.h, limits) && out.create(bounds.w, bounds.h)) {
            if (PrintWindow(window, out.dc(), PW_RENDERFULLCONTENT)) {
                if (includeCursor) drawCursor(out.dc(), bounds.x, bounds.y);
                GdiFlush();
                const uint8_t* first = out.bits() + out.stride() * size_t(crop.y - bounds.y) + size_t(crop.x - bounds.x) * 4;
                if (!allBlack(first, crop.w, crop.h, out.stride())) {
                    view = {first, crop.w, crop.h, out.stride()};
                    return {};
                }
            }
        }
        out.release();
    }
    // Occluded content cannot be recovered this way, but a visible window is.
    return captureScreen(frame, includeCursor, limits, out, view);
}

std::vector<WindowInfo> candidates(const std::vector<DWORD>& excludePids) {
    std::vector<WindowInfo> result;
    EnumState state{&result, &excludePids, GetCurrentProcessId()};
    EnumWindows(collectWindow, reinterpret_cast<LPARAM>(&state));
    return result;
}

const WindowInfo* windowAt(const std::vector<WindowInfo>& windows, int x, int y) {
    for (const auto& w : windows) if (w.frame.contains(x, y)) return &w;
    return nullptr;
}

bool frameBounds(HWND hwnd, Rect& out) {
    RECT r{};
    if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof r))) {
        if (!GetWindowRect(hwnd, &r)) return false;
    }
    out = fromRECT(r);
    return !out.empty();
}

std::string windowTitle(HWND hwnd) {
    wchar_t buffer[1024] = {};
    const int length = GetWindowTextW(hwnd, buffer, 1024);
    if (length <= 0) return {};
    std::wstring title(buffer, size_t(length));
    // At most 256 code points; never split a surrogate pair.
    size_t units = 0;
    for (int points = 0; units < title.size() && points < 256; ++points) {
        const wchar_t c = title[units];
        units += (c >= 0xD800 && c <= 0xDBFF && units + 1 < title.size()) ? 2 : 1;
    }
    title.resize(units);
    for (auto& c : title) if (c < 0x20) c = L' ';
    return paths::narrow(title);
}
}
