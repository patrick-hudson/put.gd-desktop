#include "capture/Overlay.h"
#include <algorithm>
#include <cwchar>
#include <dwmapi.h>
#include <shellscalingapi.h>
#include <windowsx.h>

namespace putgd::overlay {
namespace {
// Premultiplied BGRA as little-endian 0xAARRGGBB.
constexpr uint32_t inputLayer = 0x01000000; // alpha 1: visually transparent, still hit-testable
constexpr uint32_t dim = 0x30000000;        // alpha 48 black outside the selection while dragging
constexpr uint32_t white = 0xFFFFFFFF;
constexpr uint32_t black = 0xFF000000;
constexpr uint8_t labelAlpha = 0xE6;
constexpr int escapeHotkey = 1;

RECT inflated(RECT r, int by) { InflateRect(&r, by, by); return r; }

void bands(const RECT& r, int thickness, std::vector<RECT>& out) {
    out.push_back({r.left, r.top, r.right, r.top + thickness});
    out.push_back({r.left, r.bottom - thickness, r.right, r.bottom});
    out.push_back({r.left, r.top, r.left + thickness, r.bottom});
    out.push_back({r.right - thickness, r.top, r.right, r.bottom});
}

bool emptyRect(const RECT& r) { return r.right <= r.left || r.bottom <= r.top; }
}

Selector::~Selector() { hide(); }

Error Selector::show(Mode mode, const std::vector<DWORD>& excludePids) {
    mode_ = mode;
    exclude_ = excludePids;
    screen_ = grab::virtualScreen();
    if (screen_.empty()) return {"capture_geometry", "No display is available"};
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW cls{};
    cls.cbSize = sizeof cls;
    cls.lpfnWndProc = windowProc;
    cls.hInstance = instance;
    cls.lpszClassName = windowClassName;
    if (!RegisterClassExW(&cls) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return {"capture_failed", "Could not register the overlay"};

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = screen_.w;
    info.bmiHeader.biHeight = -screen_.h;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    HDC screen = GetDC(nullptr);
    void* bits = nullptr;
    bitmap_ = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    dc_ = CreateCompatibleDC(screen);
    ReleaseDC(nullptr, screen);
    if (!bitmap_ || !dc_ || !bits) { hide(); return {"capture_failed", "Could not allocate the overlay surface"}; }
    previous_ = static_cast<HBITMAP>(SelectObject(dc_, bitmap_));
    pixels_ = static_cast<uint32_t*>(bits);

    hwnd_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW, windowClassName, L"put.gd capture", WS_POPUP, screen_.x, screen_.y,
                            screen_.w, screen_.h, nullptr, nullptr, instance, this);
    if (!hwnd_) { hide(); return {"capture_failed", "Could not create the overlay"}; }
    // No WDA_EXCLUDEFROMCAPTURE: it would also hide the overlay from VNC/screen
    // recorders used to drive and document the selector. Hiding plus two
    // DwmFlush passes (hide()) keeps it out of the saved pixels.

    POINT p{};
    GetCursorPos(&p);
    const int x = std::clamp(int(p.x) - screen_.x, 0, screen_.w - 1), y = std::clamp(int(p.y) - screen_.y, 0, screen_.h - 1);
    if (mode_ == Mode::Region) cursor_ = {x, y};
    else hover(x, y);
    const RECT all{0, 0, screen_.w, screen_.h};
    paint(all);
    push(all);
    ShowWindow(hwnd_, SW_SHOWNORMAL);
    SetForegroundWindow(hwnd_);
    SetFocus(hwnd_);
    // Escape must work even when the foreground lock kept focus elsewhere.
    hotkey_ = RegisterHotKey(hwnd_, escapeHotkey, MOD_NOREPEAT, VK_ESCAPE);
    return {};
}

void Selector::hide() {
    if (hwnd_) {
        if (hotkey_) UnregisterHotKey(hwnd_, escapeHotkey);
        hotkey_ = false;
        if (GetCapture() == hwnd_) { releasing_ = true; ReleaseCapture(); releasing_ = false; }
        ShowWindow(hwnd_, SW_HIDE);
        // Two compositor passes plus a short settle so DWM has dropped the overlay.
        DwmFlush();
        DwmFlush();
        Sleep(60);
        SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    if (dc_) { if (previous_) SelectObject(dc_, previous_); DeleteDC(dc_); }
    if (bitmap_) DeleteObject(bitmap_);
    if (font_) DeleteObject(font_);
    dc_ = nullptr; bitmap_ = previous_ = nullptr; pixels_ = nullptr; font_ = nullptr; fontDpi_ = 0;
    label_.clear();
}

void Selector::finish(Result result) {
    if (finished_) return;
    finished_ = true;
    result_ = std::move(result);
    PostQuitMessage(0);
}

void Selector::cancel() {
    Result r;
    r.kind = Result::Kind::Cancelled;
    finish(std::move(r));
}

void Selector::fail(Error error) {
    Result r;
    r.kind = Result::Kind::Failed;
    r.error = std::move(error);
    finish(std::move(r));
}

LRESULT CALLBACK Selector::windowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_NCCREATE) {
        auto* self = static_cast<Selector*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    auto* self = reinterpret_cast<Selector*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!self || !self->hwnd_) return DefWindowProcW(hwnd, message, wParam, lParam);
    return self->handle(message, wParam, lParam);
}

LRESULT Selector::handle(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_NCHITTEST: return HTCLIENT;
    case WM_MOUSEACTIVATE: return MA_ACTIVATE;
    case WM_SETCURSOR:
        SetCursor(LoadCursorW(nullptr, mode_ == Mode::Region ? IDC_CROSS : IDC_HAND));
        return TRUE;
    case WM_ERASEBKGND: return 1;
    case WM_MOUSEMOVE:
        if (!finished_) pointerMoved(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        return 0;
    case WM_LBUTTONDOWN:
        if (!finished_) pressed(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        return 0;
    case WM_LBUTTONUP:
        if (!finished_) released(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        return 0;
    case WM_RBUTTONDOWN:
        cancel();
        return 0;
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) cancel();
        return 0;
    case WM_HOTKEY:
        if (wParam == escapeHotkey) cancel();
        return 0;
    case WM_CAPTURECHANGED:
        if (dragging_ && !releasing_ && !finished_) { dragging_ = false; renderLabel(); if (mode_ == Mode::Region) redrawAll(); }
        return 0;
    case WM_DISPLAYCHANGE:
        fail({"capture_geometry", "The display layout changed during selection"});
        return 0;
    case WM_CLOSE:
        cancel();
        return 0;
    default: return DefWindowProcW(hwnd_, message, wParam, lParam);
    }
}

RECT Selector::selectionRect() const {
    return {std::min(anchor_.x, current_.x), std::min(anchor_.y, current_.y), std::max(anchor_.x, current_.x), std::max(anchor_.y, current_.y)};
}

void Selector::pointerMoved(int x, int y) {
    if (mode_ == Mode::Window) { hover(x, y); return; }
    const POINT oldCursor = cursor_;
    const RECT oldSelection = selectionRect(), oldLabel = labelRect_;
    const bool oldDim = dragging_ && !emptyRect(oldSelection);
    cursor_ = {std::clamp(x, 0, screen_.w - 1), std::clamp(y, 0, screen_.h - 1)};
    if (dragging_) current_ = {std::clamp(x, 0, screen_.w), std::clamp(y, 0, screen_.h)};
    const RECT selection = selectionRect();
    const bool newDim = dragging_ && !emptyRect(selection);
    renderLabel();
    if (oldDim != newDim) { redrawAll(); return; }
    std::vector<RECT> dirty;
    if (oldCursor.x != cursor_.x || oldCursor.y != cursor_.y) {
        if (oldCursor.y >= 0) dirty.push_back({0, oldCursor.y, screen_.w, oldCursor.y + 1});
        if (oldCursor.x >= 0) dirty.push_back({oldCursor.x, 0, oldCursor.x + 1, screen_.h});
        dirty.push_back({0, cursor_.y, screen_.w, cursor_.y + 1});
        dirty.push_back({cursor_.x, 0, cursor_.x + 1, screen_.h});
    }
    if (newDim && !EqualRect(&oldSelection, &selection)) {
        // Pixels whose dim/outline state changed lie in four strips between the old
        // and new edges (outline included by the margin).
        constexpr int m = 4;
        RECT u{};
        UnionRect(&u, &oldSelection, &selection);
        u = inflated(u, m);
        const RECT& o = oldSelection;
        const RECT& n = selection;
        dirty.push_back({std::min(o.left, n.left) - m, u.top, std::max(o.left, n.left) + m, u.bottom});
        dirty.push_back({std::min(o.right, n.right) - m, u.top, std::max(o.right, n.right) + m, u.bottom});
        dirty.push_back({u.left, std::min(o.top, n.top) - m, u.right, std::max(o.top, n.top) + m});
        dirty.push_back({u.left, std::min(o.bottom, n.bottom) - m, u.right, std::max(o.bottom, n.bottom) + m});
    }
    if (!EqualRect(&oldLabel, &labelRect_) || newDim) { dirty.push_back(oldLabel); dirty.push_back(labelRect_); }
    redraw(std::move(dirty));
}

void Selector::pressed(int x, int y) {
    // Both modes complete on button release while holding capture, so the
    // release never reaches the window underneath once the overlay is hidden.
    dragging_ = true;
    anchor_ = current_ = {std::clamp(x, 0, screen_.w), std::clamp(y, 0, screen_.h)};
    SetCapture(hwnd_);
}

void Selector::released(int x, int y) {
    if (!dragging_) return;
    current_ = {std::clamp(x, 0, screen_.w), std::clamp(y, 0, screen_.h)};
    releasing_ = true;
    ReleaseCapture();
    releasing_ = false;
    if (mode_ == Mode::Window) {
        dragging_ = false;
        const auto windows = grab::candidates(exclude_);
        const auto* hit = grab::windowAt(windows, x + screen_.x, y + screen_.y);
        if (!hit) return;
        Result r;
        r.kind = Result::Kind::Selected;
        r.rect = hit->frame;
        r.window = hit->hwnd;
        r.pid = hit->pid;
        r.title = grab::windowTitle(hit->hwnd);
        finish(std::move(r));
        return;
    }
    const RECT s = selectionRect();
    if (emptyRect(s)) {
        dragging_ = false;
        renderLabel();
        redrawAll();
        return;
    }
    Result r;
    r.kind = Result::Kind::Selected;
    r.rect = {int(s.left) + screen_.x, int(s.top) + screen_.y, int(s.right - s.left), int(s.bottom - s.top)};
    r.rect = grab::intersect(r.rect, screen_);
    finish(std::move(r));
}

void Selector::hover(int x, int y) {
    const auto windows = grab::candidates(exclude_);
    const auto* hit = grab::windowAt(windows, x + screen_.x, y + screen_.y);
    RECT frame{};
    if (hit) frame = {hit->frame.x - screen_.x, hit->frame.y - screen_.y, hit->frame.x - screen_.x + hit->frame.w, hit->frame.y - screen_.y + hit->frame.h};
    if (bool(hit) == hasHover_ && (!hit || EqualRect(&frame, &hoverFrame_))) return;
    std::vector<RECT> dirty;
    if (hasHover_) bands(hoverFrame_, 3, dirty);
    hasHover_ = hit != nullptr;
    hoverFrame_ = frame;
    if (hasHover_) bands(hoverFrame_, 3, dirty);
    if (pixels_ && hwnd_) redraw(std::move(dirty));
}

void Selector::fill(const RECT& clip, const RECT& area, uint32_t pixel) {
    RECT r;
    if (!IntersectRect(&r, &clip, &area)) return;
    for (LONG y = r.top; y < r.bottom; ++y) std::fill_n(pixels_ + size_t(y) * size_t(screen_.w) + size_t(r.left), size_t(r.right - r.left), pixel);
}

void Selector::ring(const RECT& clip, const RECT& outer, int thickness, uint32_t pixel) {
    if (emptyRect(outer)) return;
    std::vector<RECT> parts;
    bands(outer, thickness, parts);
    for (const auto& part : parts) fill(clip, part, pixel);
}

void Selector::crosshair(const RECT& clip) {
    if (cursor_.x < 0 || cursor_.y < 0) return;
    // A 1-px dashed line (4 px white, 4 px black) stays visible on any background.
    if (cursor_.y >= clip.top && cursor_.y < clip.bottom) {
        uint32_t* row = pixels_ + size_t(cursor_.y) * size_t(screen_.w);
        for (LONG x = std::max<LONG>(clip.left, 0); x < std::min<LONG>(clip.right, screen_.w); ++x) row[x] = ((x >> 2) & 1) ? black : white;
    }
    if (cursor_.x >= clip.left && cursor_.x < clip.right) {
        for (LONG y = std::max<LONG>(clip.top, 0); y < std::min<LONG>(clip.bottom, screen_.h); ++y)
            pixels_[size_t(y) * size_t(screen_.w) + size_t(cursor_.x)] = ((y >> 2) & 1) ? black : white;
    }
}

void Selector::blitLabel(const RECT& clip) {
    RECT r;
    if (label_.empty() || !IntersectRect(&r, &clip, &labelRect_)) return;
    for (LONG y = r.top; y < r.bottom; ++y) {
        const uint32_t* source = label_.data() + size_t(y - labelRect_.top) * size_t(labelWidth_) + size_t(r.left - labelRect_.left);
        std::copy_n(source, size_t(r.right - r.left), pixels_ + size_t(y) * size_t(screen_.w) + size_t(r.left));
    }
}

void Selector::renderLabel() {
    label_.clear();
    labelRect_ = {};
    labelWidth_ = labelHeight_ = 0;
    const RECT s = selectionRect();
    if (mode_ != Mode::Region || !dragging_ || emptyRect(s) || !dc_) return;
    wchar_t text[64];
    const int length = std::swprintf(text, 64, L"%ld × %ld", long(s.right - s.left), long(s.bottom - s.top));
    if (length <= 0) return;
    const POINT at{cursor_.x + screen_.x, cursor_.y + screen_.y};
    HMONITOR monitor = MonitorFromPoint(at, MONITOR_DEFAULTTONEAREST);
    UINT dpiX = 96, dpiY = 96;
    if (FAILED(GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY)) || !dpiX) dpiX = 96;
    if (!font_ || fontDpi_ != dpiX) {
        if (font_) DeleteObject(font_);
        font_ = CreateFontW(-MulDiv(9, int(dpiX), 72), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
        fontDpi_ = dpiX;
    }
    HDC dc = CreateCompatibleDC(dc_);
    if (!dc) return;
    HGDIOBJ oldFont = SelectObject(dc, font_);
    SIZE extent{};
    GetTextExtentPoint32W(dc, text, length, &extent);
    const int padX = MulDiv(6, int(dpiX), 96), padY = MulDiv(3, int(dpiX), 96);
    const int w = int(extent.cx) + 2 * padX, h = int(extent.cy) + 2 * padY;
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = w;
    info.bmiHeader.biHeight = -h;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bitmap = w > 0 && h > 0 ? CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0) : nullptr;
    if (bitmap && bits) {
        HGDIOBJ oldBitmap = SelectObject(dc, bitmap);
        auto* px = static_cast<uint32_t*>(bits);
        std::fill_n(px, size_t(w) * size_t(h), 0x00202020u);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(255, 255, 255));
        TextOutW(dc, padX, padY, text, length);
        GdiFlush();
        label_.resize(size_t(w) * size_t(h));
        for (size_t i = 0; i < label_.size(); ++i) {
            const uint32_t c = px[i];
            const uint32_t r = ((c >> 16) & 0xFF) * labelAlpha / 255, g = ((c >> 8) & 0xFF) * labelAlpha / 255, b = (c & 0xFF) * labelAlpha / 255;
            label_[i] = uint32_t(labelAlpha) << 24 | r << 16 | g << 8 | b;
        }
        SelectObject(dc, oldBitmap);
        labelWidth_ = w;
        labelHeight_ = h;
        // Near the cursor, kept on the cursor's monitor.
        MONITORINFO mi{};
        mi.cbSize = sizeof mi;
        RECT bounds{0, 0, screen_.w, screen_.h};
        if (GetMonitorInfoW(monitor, &mi)) bounds = {mi.rcMonitor.left - screen_.x, mi.rcMonitor.top - screen_.y, mi.rcMonitor.right - screen_.x, mi.rcMonitor.bottom - screen_.y};
        const int offset = MulDiv(16, int(dpiX), 96);
        int x = cursor_.x + offset, y = cursor_.y + offset;
        if (x + w > bounds.right) x = cursor_.x - offset - w;
        if (y + h > bounds.bottom) y = cursor_.y - offset - h;
        x = std::max<int>(x, bounds.left);
        y = std::max<int>(y, bounds.top);
        labelRect_ = {x, y, x + w, y + h};
    }
    if (bitmap) DeleteObject(bitmap);
    SelectObject(dc, oldFont);
    DeleteDC(dc);
}

void Selector::paint(const RECT& dirty) {
    const RECT all{0, 0, screen_.w, screen_.h};
    RECT clip;
    if (!pixels_ || !IntersectRect(&clip, &dirty, &all)) return;
    if (mode_ == Mode::Region) {
        const RECT s = selectionRect();
        const bool dimmed = dragging_ && !emptyRect(s);
        fill(clip, all, dimmed ? dim : inputLayer);
        if (dimmed) {
            fill(clip, s, inputLayer);
            ring(clip, inflated(s, 3), 1, black);
            ring(clip, inflated(s, 2), 2, white);
        }
        crosshair(clip);
        blitLabel(clip);
    } else {
        fill(clip, all, inputLayer);
        if (hasHover_) {
            // 3 px inside the frame bounds: 1 px black outside 2 px white.
            ring(clip, hoverFrame_, 1, black);
            ring(clip, inflated(hoverFrame_, -1), 2, white);
        }
    }
}

void Selector::push(const RECT& dirty) {
    if (!hwnd_ || !dc_) return;
    POINT destination{screen_.x, screen_.y}, source{0, 0};
    SIZE size{screen_.w, screen_.h};
    BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UPDATELAYEREDWINDOWINFO info{};
    info.cbSize = sizeof info;
    info.pptDst = &destination;
    info.psize = &size;
    info.hdcSrc = dc_;
    info.pptSrc = &source;
    info.pblend = &blend;
    info.dwFlags = ULW_ALPHA;
    info.prcDirty = &dirty;
    GdiFlush();
    UpdateLayeredWindowIndirect(hwnd_, &info);
}

void Selector::redraw(std::vector<RECT> dirty) {
    const RECT all{0, 0, screen_.w, screen_.h};
    for (const auto& d : dirty) {
        RECT r;
        if (!IntersectRect(&r, &d, &all)) continue;
        paint(r);
        push(r);
    }
}

void Selector::redrawAll() {
    const RECT all{0, 0, screen_.w, screen_.h};
    paint(all);
    push(all);
}
}
