#include "ui/Theme.h"
#include "ui/Controls.h"
#include <algorithm>
#include <cmath>
#include <cwctype>
#include <map>
#include <tuple>
#include <uxtheme.h>
#include <vector>

namespace putgd::ui::theme {
namespace {
// Light: Windows 11 neutrals (mica #F3F3F3, layer #F9F9F9, cards white) with the
// redesign's blue (#1F64D6) and red (#B4312A). Dark: #202020 / #272727 / #2B2B2B
// with the redesign's dark accent (#6EA8FE) and danger (#F07A6A).
const Palette lightPalette{
    RGB(0xF3, 0xF3, 0xF3), RGB(0xF9, 0xF9, 0xF9), RGB(0xFF, 0xFF, 0xFF), RGB(0xE5, 0xE5, 0xE5), RGB(0xFF, 0xFF, 0xFF), RGB(0xEE, 0xEE, 0xEE),
    RGB(0xFF, 0xF4, 0xCE), RGB(0xEB, 0xD9, 0x9A),
    RGB(0xEA, 0xEA, 0xEA), RGB(0xE3, 0xE3, 0xE3), RGB(0xE6, 0xE6, 0xE6), RGB(0xE5, 0xE5, 0xE5),
    RGB(0xFE, 0xFE, 0xFE), RGB(0xF5, 0xF5, 0xF5), RGB(0xF0, 0xF0, 0xF0), RGB(0xDE, 0xDE, 0xDE), RGB(0xC8, 0xC8, 0xC8), RGB(0xDE, 0xDE, 0xDE), RGB(0x8A, 0x8A, 0x8A),
    RGB(0x1A, 0x1A, 0x1A), RGB(0x5C, 0x5C, 0x5C), RGB(0x80, 0x80, 0x80), RGB(0xA3, 0xA3, 0xA3),
    RGB(0x1F, 0x64, 0xD6), RGB(0x17, 0x53, 0xB5), RGB(0x14, 0x4A, 0x9F), RGB(0xFF, 0xFF, 0xFF), RGB(0x17, 0x54, 0xBD),
    RGB(0xB4, 0x31, 0x2A), RGB(0x8A, 0x53, 0x00), RGB(0x14, 0x6C, 0x42),
    RGB(0x27, 0x59, 0xB8), RGB(0xB4, 0xC8, 0xEE),
};
const Palette darkPalette{
    RGB(0x20, 0x20, 0x20), RGB(0x27, 0x27, 0x27), RGB(0x2B, 0x2B, 0x2B), RGB(0x1C, 0x1C, 0x1C), RGB(0x31, 0x31, 0x31), RGB(0x32, 0x32, 0x32),
    RGB(0x43, 0x35, 0x19), RGB(0x5E, 0x4B, 0x24),
    RGB(0x2D, 0x2D, 0x2D), RGB(0x29, 0x29, 0x29), RGB(0x33, 0x33, 0x33), RGB(0x3A, 0x3A, 0x3A),
    RGB(0x37, 0x37, 0x37), RGB(0x3D, 0x3D, 0x3D), RGB(0x30, 0x30, 0x30), RGB(0x42, 0x42, 0x42), RGB(0x4A, 0x4A, 0x4A), RGB(0x40, 0x40, 0x40), RGB(0x9A, 0x9A, 0x9A),
    RGB(0xFF, 0xFF, 0xFF), RGB(0xC8, 0xC8, 0xC8), RGB(0x9D, 0x9D, 0x9D), RGB(0x6E, 0x6E, 0x6E),
    RGB(0x6E, 0xA8, 0xFE), RGB(0x8D, 0xBB, 0xFF), RGB(0x5E, 0x95, 0xE6), RGB(0x11, 0x13, 0x17), RGB(0x8D, 0xBB, 0xFF),
    RGB(0xF0, 0x7A, 0x6A), RGB(0xE5, 0xA4, 0x43), RGB(0x4C, 0xC3, 0x8A),
    RGB(0x9B, 0xBC, 0xFF), RGB(0x52, 0x63, 0x86),
};
bool darkScheme = false;
bool schemeApplied = false;

constexpr wchar_t surfaceProperty[] = L"putgd.surface";
constexpr wchar_t roleProperty[] = L"putgd.role";
constexpr wchar_t glyphProperty[] = L"putgd.glyph";
constexpr wchar_t iconOnlyProperty[] = L"putgd.icononly";
constexpr wchar_t selectedProperty[] = L"putgd.selected";
constexpr wchar_t framedProperty[] = L"putgd.framed";
constexpr wchar_t cardProperty[] = L"putgd.card";
constexpr wchar_t padXProperty[] = L"putgd.padx";
constexpr wchar_t padYProperty[] = L"putgd.pady";
constexpr wchar_t pivotClass[] = L"putgd.Pivot";

template <class T> T prop(HWND h, const wchar_t* name) { return static_cast<T>(reinterpret_cast<INT_PTR>(GetPropW(h, name))); }
void setProp(HWND h, const wchar_t* name, INT_PTR value) {
    if (value) SetPropW(h, name, reinterpret_cast<HANDLE>(value));
    else RemovePropW(h, name);
}
std::wstring className(HWND h) {
    wchar_t name[64]{};
    GetClassNameW(h, name, 64);
    return name;
}

// ---- undocumented dark-mode switches (uxtheme ordinals, Windows 10 1903+) ----
HMODULE uxtheme() {
    static HMODULE module = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    return module;
}
DWORD windowsBuild() {
    static DWORD build = [] {
        using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
        OSVERSIONINFOW info{};
        info.dwOSVersionInfoSize = sizeof info;
        auto get = reinterpret_cast<RtlGetVersionFn>(reinterpret_cast<void (*)()>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion")));
        return get && get(&info) == 0 ? info.dwBuildNumber : 0UL;
    }();
    return build;
}
template <class F> F ordinal(int number) {
    if (!uxtheme() || windowsBuild() < 18362) return nullptr;
    return reinterpret_cast<F>(reinterpret_cast<void (*)()>(GetProcAddress(uxtheme(), MAKEINTRESOURCEA(number))));
}
void allowDark(HWND h, bool on) {
    using AllowDarkModeForWindowFn = BOOL(WINAPI*)(HWND, BOOL);
    static auto allow = ordinal<AllowDarkModeForWindowFn>(133);
    if (allow) allow(h, on);
}
void preferAppMode(bool on) {
    using SetPreferredAppModeFn = int(WINAPI*)(int);
    using VoidFn = void(WINAPI*)();
    static auto set = ordinal<SetPreferredAppModeFn>(135);
    static auto refresh = ordinal<VoidFn>(104);
    static auto flush = ordinal<VoidFn>(136);
    if (set) set(on ? 2 : 3); // ForceDark : ForceLight
    if (refresh) refresh();
    if (flush) flush();
}

// ---- glyph rasterizer -----------------------------------------------------------------
// Shapes in IconArt's 24-unit space; later shapes cover earlier ones. Layer 0 is
// the main colour, 1 the secondary colour, 2 the brand tile.
struct Shape {
    enum Kind { Polygon, Circle, Rounded, Segment } kind;
    int layer;
    float a = 0, b = 0, c = 0, d = 0, e = 0;
    std::vector<float> points;
};
struct Glyph {
    std::vector<Shape> shapes;
    void circle(int layer, float x, float y, float r) { shapes.push_back({Shape::Circle, layer, x, y, r}); }
    void rounded(int layer, float x, float y, float w, float h, float r) { shapes.push_back({Shape::Rounded, layer, x, y, w, h, r}); }
    void segment(int layer, float x1, float y1, float x2, float y2, float width) { shapes.push_back({Shape::Segment, layer, x1, y1, x2, y2, width / 2}); }
    void polyline(int layer, std::initializer_list<float> xy, float width) {
        std::vector<float> p(xy);
        for (size_t i = 2; i + 1 < p.size(); i += 2) segment(layer, p[i - 2], p[i - 1], p[i], p[i + 1], width);
    }
    void polygon(int layer, std::vector<float> xy) { Shape s{Shape::Polygon, layer}; s.points = std::move(xy); shapes.push_back(std::move(s)); }
    void arc(int layer, float cx, float cy, float r, float fromDegrees, float spanDegrees, float width) {
        const int steps = std::max(4, int(std::fabs(spanDegrees) / 12));
        float px = 0, py = 0;
        for (int i = 0; i <= steps; ++i) {
            const float angle = (fromDegrees + spanDegrees * float(i) / float(steps)) * 3.14159265f / 180.0f;
            const float x = cx + r * std::cos(angle), y = cy - r * std::sin(angle);
            if (i) segment(layer, px, py, x, y, width);
            px = x;
            py = y;
        }
    }
};
void quad(std::vector<float>& p, float cx, float cy, float x1, float y1) {
    const float x0 = p[p.size() - 2], y0 = p[p.size() - 1];
    for (int i = 1; i <= 4; ++i) {
        const float t = float(i) / 4.0f, u = 1 - t;
        p.push_back(u * u * x0 + 2 * t * u * cx + t * t * x1);
        p.push_back(u * u * y0 + 2 * t * u * cy + t * t * y1);
    }
}
std::vector<float> trayPath(float scale = 1, float dx = 0, float dy = 0) {
    std::vector<float> p{2, 13, 6, 13, 6, 18};
    quad(p, 6, 19, 7, 19);
    p.insert(p.end(), {17, 19});
    quad(p, 18, 19, 18, 18);
    p.insert(p.end(), {18, 13, 22, 13, 22, 18});
    quad(p, 22, 23, 17, 23);
    p.insert(p.end(), {7, 23});
    quad(p, 2, 23, 2, 18);
    for (size_t i = 0; i < p.size(); i += 2) { p[i] = p[i] * scale + dx; p[i + 1] = p[i + 1] * scale + dy; }
    return p;
}
std::vector<float> arrowPath(float scale = 1, float dx = 0, float dy = 0) {
    std::vector<float> p{12, 2, 20, 10, 15, 10, 15, 16, 9, 16, 9, 10, 4, 10};
    for (size_t i = 0; i < p.size(); i += 2) { p[i] = p[i] * scale + dx; p[i + 1] = p[i + 1] * scale + dy; }
    return p;
}
Glyph define(const std::string& name) {
    Glyph g;
    if (name == "brand") {
        g.rounded(2, 0, 0, 24, 24, 5);
        g.polygon(1, trayPath(0.8f, 2.4f, 2));
        g.polygon(0, arrowPath(0.8f, 2.4f, 2));
    } else if (name == "history") {
        g.circle(1, 12, 12, 8.5f);
        g.arc(0, 12, 12, 10, 10, 245, 2);
        g.polygon(0, {1, 5, 7, 7, 3, 11});
        g.polyline(0, {12, 7, 12, 12, 16, 14}, 2.2f);
    } else if (name == "destinations") {
        g.rounded(1, 2, 4, 9, 6, 2);
        g.rounded(1, 2, 7, 20, 14, 3);
        g.polyline(0, {9, 14, 13, 10, 17, 14}, 2.5f);
        g.segment(0, 13, 10, 13, 18, 2.5f);
    } else if (name == "settings") {
        for (float y : {6.0f, 12.0f, 18.0f}) g.segment(1, 3, y, 21, y, 3);
        g.circle(0, 8, 6, 3);
        g.circle(0, 16, 12, 3);
        g.circle(0, 10, 18, 3);
    } else if (name == "region" || name == "edit") {
        g.polyline(1, {8, 3, 3, 3, 3, 8}, 3.5f);
        g.polyline(1, {16, 3, 21, 3, 21, 8}, 3.5f);
        g.polyline(1, {3, 16, 3, 21, 8, 21}, 3.5f);
        if (name == "region") {
            g.polyline(1, {16, 21, 21, 21, 21, 16}, 3.5f);
            g.segment(0, 8, 12, 16, 12, 2.2f);
            g.segment(0, 12, 8, 12, 16, 2.2f);
        } else {
            g.polygon(0, {19, 10, 22, 13, 13, 22, 9, 23, 10, 19});
        }
    } else if (name == "window") {
        g.rounded(1, 2, 4, 20, 17, 3);
        g.rounded(0, 2, 4, 20, 7, 3);
        g.rounded(0, 2, 8, 20, 3, 0);
        for (float x : {6.0f, 10.0f, 14.0f}) g.circle(1, x, 7.5f, 1.1f);
    } else if (name == "desktop") {
        g.rounded(1, 2, 3, 20, 15, 2);
        g.rounded(1, 10, 17, 4, 5, 1);
        g.rounded(0, 4, 5, 16, 10, 1);
        g.rounded(0, 6, 20.5f, 12, 2.5f, 1.2f);
    } else if (name == "recordScreen") {
        g.rounded(1, 2, 3, 20, 15, 2);
        g.rounded(1, 10, 17, 4, 5, 1);
        g.rounded(0, 6, 20.5f, 12, 2.5f, 1.2f);
        g.circle(0, 12, 10.5f, 4.2f);
    } else if (name == "clipboard") {
        g.rounded(1, 4, 4, 16, 18, 3);
        g.rounded(0, 8, 2, 8, 5, 2);
        g.segment(0, 8, 11.5f, 16, 11.5f, 2.2f);
        g.segment(0, 8, 15.5f, 14, 15.5f, 2.2f);
    } else if (name == "info") {
        g.circle(1, 12, 12, 10);
        g.circle(0, 12, 7.5f, 1.5f);
        g.rounded(0, 10.8f, 10.5f, 2.4f, 7.5f, 1.2f);
    } else if (name == "warning") {
        g.circle(0, 12, 12, 10);
        g.rounded(1, 10.7f, 5.5f, 2.6f, 9, 1.3f);
        g.circle(1, 12, 17.6f, 1.6f);
    } else if (name == "power") {
        g.arc(1, 12, 13, 8, 125, 290, 2.6f);
        g.segment(0, 12, 3, 12, 12, 2.6f);
    } else if (name == "images") {
        g.rounded(1, 2, 3, 20, 18, 3);
        g.circle(0, 8, 8, 2);
        g.polygon(0, {3, 19, 10, 12, 14, 16, 18, 11, 21, 15, 21, 19});
    } else if (name == "chevron") {
        g.polyline(0, {6, 9, 12, 15, 18, 9}, 2.2f);
    } else if (name == "check") {
        g.polyline(0, {5.5f, 12.5f, 10, 17, 18.5f, 7.5f}, 2.6f);
    // Recording (#400): the bar's and the review window's controls.
    } else if (name == "play") {
        g.polygon(0, {7, 4.5f, 19.5f, 12, 7, 19.5f});
    } else if (name == "pause") {
        g.rounded(0, 6, 4.5f, 4.5f, 15, 1.2f);
        g.rounded(0, 13.5f, 4.5f, 4.5f, 15, 1.2f);
    } else if (name == "record") {
        g.circle(1, 12, 12, 9.5f);
        g.circle(0, 12, 12, 5.5f);
    } else if (name == "stop") {
        g.rounded(0, 5.5f, 5.5f, 13, 13, 2.5f);
    } else if (name == "trimIn" || name == "maskIn") {
        g.rounded(name == "maskIn" ? 1 : 0, 4, 4, 2.6f, 16, 1);
        g.polyline(0, {4, 5.3f, 9, 5.3f}, 2.6f);
        g.polyline(0, {4, 18.7f, 9, 18.7f}, 2.6f);
        g.polygon(1, {11, 7, 19, 12, 11, 17});
    } else if (name == "trimOut" || name == "maskOut") {
        g.rounded(name == "maskOut" ? 1 : 0, 17.4f, 4, 2.6f, 16, 1);
        g.polyline(0, {15, 5.3f, 20, 5.3f}, 2.6f);
        g.polyline(0, {15, 18.7f, 20, 18.7f}, 2.6f);
        g.polygon(1, {13, 7, 5, 12, 13, 17});
    } else if (name == "reset") {
        g.arc(0, 12, 12, 7.5f, 100, 290, 2.4f);
        g.polygon(0, {6, 2.5f, 12, 4.8f, 7.4f, 9.4f});
    } else if (name == "blur") {
        g.rounded(1, 3, 3, 18, 18, 3);
        for (float y : {7.0f, 12.0f, 17.0f}) for (float x : {7.0f, 12.0f, 17.0f}) g.circle(0, x, y, (int(x + y) % 2) ? 1.3f : 2.0f);
    } else if (name == "solid") {
        g.rounded(1, 3, 3, 18, 18, 3);
        g.rounded(0, 7, 7, 10, 10, 1.5f);
    } else if (name == "remove") {
        g.polygon(1, {5.5f, 8, 18.5f, 8, 17.2f, 21, 6.8f, 21});
        g.rounded(0, 3, 5, 18, 2.5f, 1);
        g.rounded(0, 9, 2, 6, 3.5f, 1);
    }
    return g;
}
bool covers(const Shape& s, float x, float y) {
    switch (s.kind) {
    case Shape::Circle: return (x - s.a) * (x - s.a) + (y - s.b) * (y - s.b) <= s.c * s.c;
    case Shape::Rounded: {
        if (x < s.a || y < s.b || x > s.a + s.c || y > s.b + s.d) return false;
        const float r = s.e;
        const float cx = std::clamp(x, s.a + r, s.a + s.c - r), cy = std::clamp(y, s.b + r, s.b + s.d - r);
        return (x - cx) * (x - cx) + (y - cy) * (y - cy) <= r * r;
    }
    case Shape::Segment: {
        const float dx = s.c - s.a, dy = s.d - s.b, length = dx * dx + dy * dy;
        const float t = length > 0 ? std::clamp(((x - s.a) * dx + (y - s.b) * dy) / length, 0.0f, 1.0f) : 0.0f;
        const float px = s.a + t * dx - x, py = s.b + t * dy - y;
        return px * px + py * py <= s.e * s.e;
    }
    case Shape::Polygon: {
        bool inside = false;
        const auto& p = s.points;
        for (size_t i = 0, j = p.size() - 2; i < p.size(); j = i, i += 2)
            if ((p[i + 1] > y) != (p[j + 1] > y) && x < (p[j] - p[i]) * (y - p[i + 1]) / (p[j + 1] - p[i + 1]) + p[i]) inside = !inside;
        return inside;
    }
    }
    return false;
}
HICON makeIcon(const std::vector<unsigned>& argb, int size) {
    BITMAPV5HEADER header{};
    header.bV5Size = sizeof header;
    header.bV5Width = size;
    header.bV5Height = -size;
    header.bV5Planes = 1;
    header.bV5BitCount = 32;
    header.bV5Compression = BI_BITFIELDS;
    header.bV5RedMask = 0x00FF0000;
    header.bV5GreenMask = 0x0000FF00;
    header.bV5BlueMask = 0x000000FF;
    header.bV5AlphaMask = 0xFF000000;
    void* bits = nullptr;
    HDC screen = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(screen, reinterpret_cast<BITMAPINFO*>(&header), DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (!color || !bits) return nullptr;
    std::copy(argb.begin(), argb.end(), static_cast<unsigned*>(bits));
    HBITMAP mask = CreateBitmap(size, size, 1, 1, nullptr);
    ICONINFO info{};
    info.fIcon = TRUE;
    info.hbmColor = color;
    info.hbmMask = mask;
    HICON icon = CreateIconIndirect(&info);
    DeleteObject(color);
    DeleteObject(mask);
    return icon;
}

// ---- double-buffered drawing ----------------------------------------------------------
struct Buffer {
    HDC target, dc;
    RECT rect;
    HBITMAP bitmap, old;
    Buffer(HDC t, const RECT& r) : target(t), rect(r) {
        dc = CreateCompatibleDC(t);
        bitmap = CreateCompatibleBitmap(t, std::max(1, int(r.right - r.left)), std::max(1, int(r.bottom - r.top)));
        old = static_cast<HBITMAP>(SelectObject(dc, bitmap));
        SetWindowOrgEx(dc, r.left, r.top, nullptr);
    }
    ~Buffer() {
        BitBlt(target, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top, dc, rect.left, rect.top, SRCCOPY);
        SelectObject(dc, old);
        DeleteObject(bitmap);
        DeleteDC(dc);
    }
};
void focusRing(HDC dc, RECT r, int radius, UINT dpi) {
    HPEN pen = CreatePen(PS_SOLID, std::max(1, scale(2, dpi)), colors().text);
    HGDIOBJ oldPen = SelectObject(dc, pen), oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    InflateRect(&r, -scale(1, dpi), -scale(1, dpi));
    RoundRect(dc, r.left, r.top, r.right, r.bottom, radius * 2, radius * 2);
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    DeleteObject(pen);
}
HFONT fontOf(HWND h) {
    HFONT f = reinterpret_cast<HFONT>(SendMessageW(h, WM_GETFONT, 0, 0));
    return f ? f : font(FontRole::Normal, windowDpi(h));
}
int textWidthOf(HDC dc, HFONT f, const std::wstring& text) {
    HGDIOBJ old = SelectObject(dc, f);
    RECT r{0, 0, 0, 0};
    DrawTextW(dc, text.c_str(), int(text.size()), &r, DT_CALCRECT | DT_SINGLELINE);
    SelectObject(dc, old);
    return r.right;
}

std::map<HWND, std::wstring>& hints() { static std::map<HWND, std::wstring> table; return table; }
std::wstring hintOf(HWND h) { const auto it = hints().find(h); return it == hints().end() ? std::wstring() : it->second; }
// ---- custom-drawn buttons -------------------------------------------------------------
void paintPush(HWND h, HDC target, UINT state) {
    const auto& c = colors();
    const UINT dpi = windowDpi(h);
    RECT rc{};
    GetClientRect(h, &rc);
    Buffer buffer(target, rc);
    HDC dc = buffer.dc;
    const COLORREF bg = backgroundOf(h);
    fill(dc, rc, bg);
    const Role role = prop<Role>(h, roleProperty);
    const bool disabled = (state & CDIS_DISABLED) || !IsWindowEnabled(h);
    const bool pressed = !disabled && ((state & CDIS_SELECTED) || (SendMessageW(h, BM_GETSTATE, 0, 0) & BST_PUSHED));
    const bool hot = !disabled && (state & CDIS_HOT);
    const bool focus = (state & CDIS_FOCUS) && (state & CDIS_SHOWKEYBOARDCUES);
    const int radius = scale(4, dpi);
    const std::wstring text = textW(h);
    UINT format = DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS;
    if (!(state & CDIS_SHOWKEYBOARDCUES)) format |= DT_HIDEPREFIX;
    COLORREF ink = disabled ? c.textDisabled : c.text;
    if (role == Role::Nav || role == Role::Action) {
        const bool selected = prop<int>(h, selectedProperty) != 0;
        if (pressed) roundRect(dc, rc, radius, c.pressed);
        else if (selected) roundRect(dc, rc, radius, hot ? c.hover : c.selected);
        else if (hot) roundRect(dc, rc, radius, c.hover);
        if (selected) {
            const int pillHeight = scale(16, dpi), pillWidth = scale(3, dpi);
            RECT pill{rc.left, (rc.top + rc.bottom - pillHeight) / 2, rc.left + pillWidth, (rc.top + rc.bottom + pillHeight) / 2};
            roundRect(dc, pill, pillWidth / 2 + 1, c.accent);
        }
        const auto* name = reinterpret_cast<const char*>(GetPropW(h, glyphProperty));
        const int icon = scale(20, dpi), left = rc.left + scale(12, dpi);
        if (name) drawGlyph(dc, name, left, (rc.top + rc.bottom - icon) / 2, icon, disabled ? c.textDisabled : c.icon, disabled ? c.divider : c.iconSecondary);
        RECT label{left + icon + scale(12, dpi), rc.top, rc.right - scale(8, dpi), rc.bottom};
        const auto hint = hintOf(h);
        if (!hint.empty()) {
            HFONT small = font(FontRole::Small, dpi);
            const int hintWidth = textWidthOf(dc, small, hint);
            const int labelWidth = textWidthOf(dc, fontOf(h), text);
            if (labelWidth + hintWidth + scale(16, dpi) < label.right - label.left) {
                RECT hintRect{label.right - hintWidth - scale(6, dpi), rc.top, label.right - scale(2, dpi), rc.bottom};
                drawText(dc, hint, hintRect, disabled ? c.textDisabled : c.textMuted, small, DT_SINGLELINE | DT_VCENTER | DT_RIGHT | DT_NOPREFIX);
                label.right = hintRect.left - scale(8, dpi);
            }
        }
        drawText(dc, text, label, ink, fontOf(h), format | DT_LEFT);
    } else {
        COLORREF face = c.button, border = c.buttonBorder, bottom = c.buttonBottom;
        if (role == Role::Primary && !disabled) {
            face = pressed ? c.accentPressed : hot ? c.accentHover : c.accent;
            border = bottom = face;
            ink = c.onAccent;
        } else if (role == Role::Subtle || role == Role::Link) {
            face = pressed ? c.pressed : hot && role == Role::Subtle ? c.hover : bg;
            border = bottom = face;
            if (role == Role::Link && !disabled) ink = hot ? c.accentHover : c.link;
        } else {
            if (pressed) face = c.buttonPressed;
            else if (hot) face = c.buttonHover;
            if (disabled) { face = blend(bg, c.button, 128); bottom = border; }
            if (role == Role::Danger && !disabled) ink = c.danger;
            if (pressed && role != Role::Danger) ink = c.textSecondary;
        }
        roundRect(dc, rc, radius, face, border);
        if (bottom != border) {
            RECT line{rc.left + radius, rc.bottom - 1, rc.right - radius, rc.bottom};
            fill(dc, line, bottom);
        }
        RECT label = rc;
        InflateRect(&label, -scale(role == Role::Link ? 4 : 10, dpi), 0);
        // A glyph on a push button: alone and centred for an icon button (its text
        // stays the accessible name), else before the text, centred as a group.
        if (const auto* name = reinterpret_cast<const char*>(GetPropW(h, glyphProperty))) {
            const int icon = scale(16, dpi), gap = scale(6, dpi);
            const COLORREF main = disabled ? c.textDisabled : role == Role::Primary ? c.onAccent : role == Role::Danger ? c.danger : c.icon;
            const COLORREF second = disabled ? c.divider : role == Role::Primary ? blend(c.accent, c.onAccent, 110) : c.iconSecondary;
            const bool iconOnly = prop<int>(h, iconOnlyProperty) != 0;
            const int textWidth = iconOnly ? 0 : textWidthOf(dc, fontOf(h), text);
            const int group = icon + (iconOnly ? 0 : gap + textWidth);
            const int x = std::max(int(label.left), int(rc.left + rc.right - group) / 2);
            drawGlyph(dc, name, x, (rc.top + rc.bottom - icon) / 2, icon, main, second);
            if (!iconOnly) drawText(dc, text, {x + icon + gap, rc.top, label.right, rc.bottom}, ink, fontOf(h), format | DT_LEFT);
        } else drawText(dc, text, label, ink, fontOf(h), format | (role == Role::Link ? DT_LEFT : DT_CENTER));
    }
    if (focus) focusRing(dc, rc, radius + scale(1, dpi), dpi);
}
void paintCheck(HWND h, HDC target, UINT state, bool radio) {
    const auto& c = colors();
    const UINT dpi = windowDpi(h);
    RECT rc{};
    GetClientRect(h, &rc);
    Buffer buffer(target, rc);
    HDC dc = buffer.dc;
    fill(dc, rc, backgroundOf(h));
    const bool disabled = (state & CDIS_DISABLED) || !IsWindowEnabled(h);
    const bool hot = !disabled && (state & CDIS_HOT);
    const bool pressed = !disabled && (SendMessageW(h, BM_GETSTATE, 0, 0) & BST_PUSHED);
    const bool on = SendMessageW(h, BM_GETCHECK, 0, 0) == BST_CHECKED;
    const int box = scale(18, dpi);
    RECT square{rc.left + scale(1, dpi), (rc.top + rc.bottom - box) / 2, rc.left + scale(1, dpi) + box, (rc.top + rc.bottom - box) / 2 + box};
    const int radius = radio ? box / 2 : scale(4, dpi);
    if (on) {
        const COLORREF face = disabled ? c.textDisabled : pressed ? c.accentPressed : hot ? c.accentHover : c.accent;
        roundRect(dc, square, radius, face);
        if (radio) {
            const int dot = box * 4 / 9;
            RECT inner{square.left + (box - dot) / 2, square.top + (box - dot) / 2, square.left + (box - dot) / 2 + dot, square.top + (box - dot) / 2 + dot};
            roundRect(dc, inner, dot / 2, c.onAccent);
        } else {
            drawGlyph(dc, "check", square.left, square.top, box, c.onAccent, c.onAccent);
        }
    } else {
        roundRect(dc, square, radius, disabled ? backgroundOf(h) : pressed ? c.pressed : hot ? c.hover : c.input, disabled ? c.divider : c.inputBottom);
    }
    RECT label{square.right + scale(8, dpi), rc.top, rc.right, rc.bottom};
    UINT format = DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS;
    if (!(state & CDIS_SHOWKEYBOARDCUES)) format |= DT_HIDEPREFIX;
    drawText(dc, textW(h), label, disabled ? c.textDisabled : c.text, fontOf(h), format);
    if ((state & CDIS_FOCUS) && (state & CDIS_SHOWKEYBOARDCUES)) {
        HDC measure = dc;
        const int width = textWidthOf(measure, fontOf(h), textW(h));
        RECT ring{rc.left, rc.top, std::min<LONG>(rc.right, label.left + width + scale(4, dpi)), rc.bottom};
        focusRing(dc, ring, scale(4, dpi), dpi);
    }
}
bool headerDraw(NMCUSTOMDRAW* cd, LRESULT& result) {
    const auto& c = colors();
    HWND header = cd->hdr.hwndFrom;
    const UINT dpi = windowDpi(header);
    RECT client{};
    GetClientRect(header, &client);
    switch (cd->dwDrawStage) {
    case CDDS_PREPAINT:
        fill(cd->hdc, client, backgroundOf(header));
        result = CDRF_NOTIFYITEMDRAW | CDRF_NOTIFYPOSTPAINT;
        return true;
    case CDDS_ITEMPREPAINT: {
        RECT r = cd->rc;
        fill(cd->hdc, r, backgroundOf(header));
        wchar_t text[128]{};
        HDITEMW item{};
        item.mask = HDI_TEXT;
        item.pszText = text;
        item.cchTextMax = 128;
        SendMessageW(header, HDM_GETITEMW, cd->dwItemSpec, reinterpret_cast<LPARAM>(&item));
        RECT label{r.left + scale(10, dpi), r.top, r.right - scale(6, dpi), r.bottom};
        drawText(cd->hdc, text, label, c.textSecondary, font(FontRole::SmallBold, dpi), DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS | DT_NOPREFIX);
        RECT divider{r.right - 1, r.top + scale(7, dpi), r.right, r.bottom - scale(7, dpi)};
        fill(cd->hdc, divider, c.divider);
        result = CDRF_SKIPDEFAULT;
        return true;
    }
    case CDDS_POSTPAINT: {
        const int count = int(SendMessageW(header, HDM_GETITEMCOUNT, 0, 0));
        int right = 0;
        for (int i = 0; i < count; ++i) {
            RECT r{};
            SendMessageW(header, HDM_GETITEMRECT, WPARAM(i), reinterpret_cast<LPARAM>(&r));
            right = std::max<int>(right, r.right);
        }
        RECT rest{right, client.top, client.right, client.bottom};
        fill(cd->hdc, rest, backgroundOf(header));
        RECT line{client.left, client.bottom - 1, client.right, client.bottom};
        fill(cd->hdc, line, c.divider);
        result = CDRF_DODEFAULT;
        return true;
    }
    }
    result = CDRF_DODEFAULT;
    return true;
}
// List view rows: a rounded selection with the accent pill, the first column in
// primary text and the others in secondary text; no column separators.
bool rowDraw(NMLVCUSTOMDRAW* cd, LRESULT& result) {
    HWND list = cd->nmcd.hdr.hwndFrom;
    if (cd->nmcd.dwDrawStage == CDDS_PREPAINT) {
        // Cover the theme's column separators below the header before rows are drawn.
        RECT client{}, header{};
        GetClientRect(list, &client);
        if (HWND h = ListView_GetHeader(list)) { GetWindowRect(h, &header); client.top += header.bottom - header.top; }
        if (!SendMessageW(list, LVM_GETITEMCOUNT, 0, 0)) {
            fill(cd->nmcd.hdc, client, backgroundOf(list));
            result = CDRF_DODEFAULT; // the empty-state text follows
            return true;
        }
        // No background fill here: the list erases with its own background colour, and
        // every row fills its rectangle below. Filling at this stage reached the whole
        // double buffer while only the dirty rows were drawn again, which wiped the rest.
        result = CDRF_NOTIFYITEMDRAW;
        return true;
    }
    if (cd->nmcd.dwDrawStage != CDDS_ITEMPREPAINT) { result = CDRF_DODEFAULT; return true; }
    const auto& c = colors();
    const UINT dpi = windowDpi(list);
    HDC dc = cd->nmcd.hdc;
    const int item = int(cd->nmcd.dwItemSpec);
    RECT row{};
    row.left = LVIR_BOUNDS;
    SendMessageW(list, LVM_GETITEMRECT, WPARAM(item), reinterpret_cast<LPARAM>(&row));
    const COLORREF bg = backgroundOf(list);
    fill(dc, row, bg);
    const bool selected = ListView_GetItemState(list, item, LVIS_SELECTED) & LVIS_SELECTED;
    const bool hot = int(SendMessageW(list, LVM_GETHOTITEM, 0, 0)) == item;
    RECT face = row;
    InflateRect(&face, -scale(2, dpi), -scale(2, dpi));
    if (selected) roundRect(dc, face, scale(4, dpi), hot ? blend(c.selected, c.text, 12) : c.selected);
    else if (hot) roundRect(dc, face, scale(4, dpi), c.hover);
    if (selected) {
        const int pillHeight = scale(16, dpi), pillWidth = scale(3, dpi);
        RECT pill{face.left, (face.top + face.bottom - pillHeight) / 2, face.left + pillWidth, (face.top + face.bottom + pillHeight) / 2};
        roundRect(dc, pill, pillWidth / 2 + 1, c.accent);
    }
    HWND header = ListView_GetHeader(list);
    const int columns = header ? int(SendMessageW(header, HDM_GETITEMCOUNT, 0, 0)) : 1;
    HFONT f = reinterpret_cast<HFONT>(SendMessageW(list, WM_GETFONT, 0, 0));
    for (int column = 0; column < columns; ++column) {
        RECT cell{};
        cell.top = column;
        cell.left = LVIR_BOUNDS;
        SendMessageW(list, LVM_GETSUBITEMRECT, WPARAM(item), reinterpret_cast<LPARAM>(&cell));
        if (column == 0 && columns > 1) {
            RECT next{};
            next.top = 1;
            next.left = LVIR_BOUNDS;
            SendMessageW(list, LVM_GETSUBITEMRECT, WPARAM(item), reinterpret_cast<LPARAM>(&next));
            cell.right = next.left;
        }
        wchar_t text[1024]{};
        LVITEMW get{};
        get.iSubItem = column;
        get.pszText = text;
        get.cchTextMax = 1024;
        SendMessageW(list, LVM_GETITEMTEXTW, WPARAM(item), reinterpret_cast<LPARAM>(&get));
        RECT label{cell.left + scale(column == 0 ? 12 : 10, dpi), cell.top, cell.right - scale(6, dpi), cell.bottom};
        if (column == 0) {
            // A thumbnail or type glyph from the small image list, vertically centred.
            HIMAGELIST images = ListView_GetImageList(list, LVSIL_SMALL);
            LVITEMW probe{};
            probe.mask = LVIF_IMAGE; probe.iItem = item;
            if (images && SendMessageW(list, LVM_GETITEMW, 0, reinterpret_cast<LPARAM>(&probe)) && probe.iImage >= 0) {
                int cx = 0, cy = 0;
                ImageList_GetIconSize(images, &cx, &cy);
                ImageList_Draw(images, probe.iImage, dc, label.left, (cell.top + cell.bottom - cy) / 2, ILD_NORMAL);
                label.left += cx + scale(10, dpi);
            }
        }
        if (column + 1 == int(reinterpret_cast<INT_PTR>(GetPropW(list, L"putgd.stateColumn"))) && text[0]) {
            std::wstring word(text);
            for (auto& ch : word) ch = wchar_t(towlower(ch));
            auto has = [&word](const wchar_t* part) { return word.find(part) != std::wstring::npos; };
            const COLORREF tone = has(L"fail") ? c.danger
                                  : has(L"unknown") ? c.warning
                                  : has(L"cancel") || has(L"local capture") || has(L"no upload") ? c.textMuted
                                  : has(L"uploaded") || has(L"completed") || has(L"saved") || has(L"succeeded") ? c.success
                                  : has(L"uploading") || has(L"waiting") || has(L"running") || has(L"preparing") || has(L"finaliz") || has(L"pending") || has(L"accepted") ||
                                            has(L"queued") || has(L"acquir")
                                      ? c.accent
                                      : c.textMuted;
            const int dot = scale(8, dpi);
            RECT mark{label.left, (cell.top + cell.bottom - dot) / 2, label.left + dot, (cell.top + cell.bottom - dot) / 2 + dot};
            roundRect(dc, mark, dot / 2, tone);
            label.left += dot + scale(8, dpi);
            drawText(dc, text, label, c.text, f, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS | DT_NOPREFIX);
            continue;
        }
        drawText(dc, text, label, column == 0 ? c.text : c.textSecondary, f, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS | DT_NOPREFIX);
    }
    result = CDRF_SKIPDEFAULT;
    return true;
}
LRESULT CALLBACK listViewProc(HWND h, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR) {
    if (message == WM_NOTIFY) {
        auto* header = reinterpret_cast<NMHDR*>(lParam);
        LRESULT result = 0;
        if (header->code == NM_CUSTOMDRAW && header->hwndFrom == ListView_GetHeader(h) && headerDraw(reinterpret_cast<NMCUSTOMDRAW*>(header), result)) return result;
    }
    return DefSubclassProc(h, message, wParam, lParam);
}

// ---- combo boxes: a button-like face drawn over the native control -----------------------
LRESULT CALLBACK comboProc(HWND h, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR) {
    if (message == WM_ERASEBKGND) return 1;
    if (message == WM_PAINT || message == WM_PRINTCLIENT) {
        PAINTSTRUCT ps{};
        HDC target = message == WM_PAINT ? BeginPaint(h, &ps) : reinterpret_cast<HDC>(wParam);
        RECT rc{};
        GetClientRect(h, &rc);
        {
            Buffer buffer(target, rc);
            HDC dc = buffer.dc;
            const auto& c = colors();
            const UINT dpi = windowDpi(h);
            const COLORREF bg = backgroundOf(GetParent(h));
            fill(dc, rc, bg);
            const bool enabled = IsWindowEnabled(h);
            COMBOBOXINFO info{};
            info.cbSize = sizeof info;
            GetComboBoxInfo(h, &info);
            const bool dropped = SendMessageW(h, CB_GETDROPPEDSTATE, 0, 0) != 0;
            POINT cursor{};
            GetCursorPos(&cursor);
            RECT screen{};
            GetWindowRect(h, &screen);
            const bool hot = enabled && PtInRect(&screen, cursor);
            COLORREF face = !enabled ? blend(bg, c.button, 128) : dropped ? c.buttonPressed : hot ? c.buttonHover : c.button;
            const int radius = scale(4, dpi);
            roundRect(dc, rc, radius, face, c.buttonBorder);
            if (enabled) fill(dc, {rc.left + radius, rc.bottom - 1, rc.right - radius, rc.bottom}, c.buttonBottom);
            const int index = int(SendMessageW(h, CB_GETCURSEL, 0, 0));
            std::wstring text;
            if (index >= 0) {
                const int length = int(SendMessageW(h, CB_GETLBTEXTLEN, WPARAM(index), 0));
                if (length > 0) {
                    text.resize(size_t(length) + 1);
                    SendMessageW(h, CB_GETLBTEXT, WPARAM(index), reinterpret_cast<LPARAM>(text.data()));
                    text.resize(size_t(length));
                }
            }
            const int chevron = scale(12, dpi);
            RECT label{rc.left + scale(11, dpi), rc.top, rc.right - chevron - scale(20, dpi), rc.bottom};
            drawText(dc, text, label, enabled ? c.text : c.textDisabled, fontOf(h), DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS | DT_NOPREFIX);
            drawGlyph(dc, "chevron", rc.right - chevron - scale(12, dpi), (rc.top + rc.bottom - chevron) / 2, chevron, enabled ? c.textSecondary : c.textDisabled,
                      c.textSecondary);
            const bool focusShown = GetFocus() == h && !(SendMessageW(h, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS);
            if (focusShown && !dropped) focusRing(dc, rc, radius + scale(1, dpi), dpi);
        }
        if (message == WM_PAINT) EndPaint(h, &ps);
        return 0;
    }
    if (message == WM_MOUSEMOVE) {
        // Repaint on hover changes; the native control only tracks its arrow button.
        if (!GetPropW(h, L"putgd.tracking")) {
            TRACKMOUSEEVENT track{sizeof track, TME_LEAVE, h, 0};
            TrackMouseEvent(&track);
            SetPropW(h, L"putgd.tracking", reinterpret_cast<HANDLE>(1));
            InvalidateRect(h, nullptr, FALSE);
        }
    } else if (message == WM_MOUSELEAVE) {
        RemovePropW(h, L"putgd.tracking");
        InvalidateRect(h, nullptr, FALSE);
    } else if (message == WM_SETFOCUS || message == WM_KILLFOCUS || message == WM_ENABLE) {
        const LRESULT result = DefSubclassProc(h, message, wParam, lParam);
        InvalidateRect(h, nullptr, FALSE);
        return result;
    } else if (message == WM_NCDESTROY) {
        RemovePropW(h, L"putgd.tracking");
    }
    return DefSubclassProc(h, message, wParam, lParam);
}

// ---- pivot ------------------------------------------------------------------------------
struct PivotData {
    std::vector<std::wstring> items;
    int selected = 0, hot = -1;
    bool keyboard = false, tracking = false, segmented = false;
    HFONT font = nullptr;
};
std::vector<RECT> pivotItems(HWND h, PivotData& data) {
    std::vector<RECT> rects;
    RECT client{};
    GetClientRect(h, &client);
    const UINT dpi = windowDpi(h);
    HDC dc = GetDC(h);
    int x = 0;
    for (const auto& item : data.items) {
        const int width = textWidthOf(dc, data.font ? data.font : font(FontRole::Large, dpi), item) + 2 * scale(data.segmented ? 16 : 12, dpi);
        rects.push_back({x, client.top, x + width, client.bottom});
        x += width + (data.segmented ? 0 : scale(4, dpi));
    }
    ReleaseDC(h, dc);
    return rects;
}
void pivotSelect(HWND h, PivotData& data, int index, bool notify) {
    index = std::clamp(index, 0, int(data.items.size()) - 1);
    if (index == data.selected) return;
    data.selected = index;
    InvalidateRect(h, nullptr, FALSE);
    if (notify) SendMessageW(GetParent(h), WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(h), LBN_SELCHANGE), reinterpret_cast<LPARAM>(h));
}
LRESULT CALLBACK pivotProc(HWND h, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* data = reinterpret_cast<PivotData*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        data = new PivotData;
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(data));
    }
    if (!data) return DefWindowProcW(h, message, wParam, lParam);
    switch (message) {
    case WM_NCDESTROY:
        delete data;
        SetWindowLongPtrW(h, GWLP_USERDATA, 0);
        break;
    case WM_SETFONT:
        data->font = reinterpret_cast<HFONT>(wParam);
        if (lParam) InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_GETFONT:
        return reinterpret_cast<LRESULT>(data->font);
    case LB_SETCURSEL:
        pivotSelect(h, *data, int(wParam), false);
        return 0;
    case LB_GETCURSEL:
        return data->selected;
    case WM_GETDLGCODE:
        return DLGC_WANTARROWS;
    case WM_SETFOCUS: case WM_KILLFOCUS:
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_KEYDOWN:
        data->keyboard = true;
        if (wParam == VK_LEFT || wParam == VK_UP) pivotSelect(h, *data, data->selected - 1, true);
        else if (wParam == VK_RIGHT || wParam == VK_DOWN) pivotSelect(h, *data, data->selected + 1, true);
        else if (wParam == VK_HOME) pivotSelect(h, *data, 0, true);
        else if (wParam == VK_END) pivotSelect(h, *data, int(data->items.size()) - 1, true);
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_MOUSEMOVE: {
        const POINT at{static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam))};
        int hot = -1;
        const auto rects = pivotItems(h, *data);
        for (size_t i = 0; i < rects.size(); ++i) if (PtInRect(&rects[i], at)) hot = int(i);
        if (!data->tracking) {
            TRACKMOUSEEVENT track{sizeof track, TME_LEAVE, h, 0};
            TrackMouseEvent(&track);
            data->tracking = true;
        }
        if (hot != data->hot) { data->hot = hot; InvalidateRect(h, nullptr, FALSE); }
        return 0;
    }
    case WM_MOUSELEAVE:
        data->tracking = false;
        data->hot = -1;
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_LBUTTONDOWN: {
        data->keyboard = false;
        SetFocus(h);
        const POINT at{static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam))};
        const auto rects = pivotItems(h, *data);
        for (size_t i = 0; i < rects.size(); ++i) if (PtInRect(&rects[i], at)) pivotSelect(h, *data, int(i), true);
        return 0;
    }
    case WM_ENABLE:
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC target = BeginPaint(h, &ps);
        RECT client{};
        GetClientRect(h, &client);
        {
            Buffer buffer(target, client);
            const auto& c = colors();
            const UINT dpi = windowDpi(h);
            fill(buffer.dc, client, backgroundOf(h));
            const auto rects = pivotItems(h, *data);
            const bool enabled = IsWindowEnabled(h);
            if (data->segmented && !rects.empty()) {
                RECT outer{0, client.top, rects.back().right, client.bottom};
                roundRect(buffer.dc, outer, scale(4, dpi), c.button, c.buttonBorder);
                for (size_t i = 0; i < rects.size(); ++i) {
                    RECT r = rects[i];
                    const bool selected = int(i) == data->selected;
                    RECT face = r;
                    InflateRect(&face, -scale(3, dpi), -scale(3, dpi));
                    if (selected) roundRect(buffer.dc, face, scale(3, dpi), enabled ? c.accent : c.textDisabled);
                    else if (int(i) == data->hot && enabled) roundRect(buffer.dc, face, scale(3, dpi), c.buttonHover);
                    const COLORREF ink = !enabled ? c.textDisabled : selected ? c.onAccent : c.text;
                    drawText(buffer.dc, data->items[i], r, ink, font(selected ? FontRole::Bold : FontRole::Normal, dpi), DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX);
                    if (selected && GetFocus() == h && data->keyboard) focusRing(buffer.dc, r, scale(4, dpi), dpi);
                }
            }
            for (size_t i = 0; i < rects.size() && !data->segmented; ++i) {
                RECT r = rects[i];
                const bool selected = int(i) == data->selected;
                if (int(i) == data->hot && enabled) {
                    RECT face = r;
                    InflateRect(&face, 0, -scale(4, dpi));
                    roundRect(buffer.dc, face, scale(4, dpi), c.hover);
                }
                RECT label = r;
                label.bottom -= scale(2, dpi);
                const COLORREF ink = !enabled ? c.textDisabled : selected || int(i) == data->hot ? c.text : c.textSecondary;
                drawText(buffer.dc, data->items[i], label, ink, data->font ? data->font : font(FontRole::Large, dpi), DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX);
                if (selected) {
                    const int width = scale(18, dpi), height = scale(3, dpi);
                    RECT pill{(r.left + r.right - width) / 2, r.bottom - height - scale(2, dpi), (r.left + r.right + width) / 2, r.bottom - scale(2, dpi)};
                    roundRect(buffer.dc, pill, height / 2 + 1, enabled ? c.accent : c.textDisabled);
                    if (GetFocus() == h && data->keyboard) focusRing(buffer.dc, r, scale(4, dpi), dpi);
                }
            }
        }
        EndPaint(h, &ps);
        return 0;
    }
    }
    return DefWindowProcW(h, message, wParam, lParam);
}
}

bool dark() { return darkScheme; }
const Palette& colors() { return darkScheme ? darkPalette : lightPalette; }
bool setDark(bool on) {
    const bool changed = !schemeApplied || on != darkScheme;
    darkScheme = on;
    schemeApplied = true;
    if (changed) preferAppMode(on);
    return changed;
}
bool wantsDark(const std::string& setting) { return setting == "dark" || (setting != "light" && systemPrefersDark()); }

COLORREF blend(COLORREF from, COLORREF to, int alpha) {
    alpha = std::clamp(alpha, 0, 255);
    auto mix = [alpha](int a, int b) { return (a * (255 - alpha) + b * alpha + 127) / 255; };
    return RGB(mix(GetRValue(from), GetRValue(to)), mix(GetGValue(from), GetGValue(to)), mix(GetBValue(from), GetBValue(to)));
}
HBRUSH brush(COLORREF color) {
    static std::map<COLORREF, HBRUSH> cache;
    auto& slot = cache[color];
    if (!slot) slot = CreateSolidBrush(color);
    return slot;
}

COLORREF surfaceColor(Surface surface) {
    const auto& c = colors();
    switch (surface) {
    case Surface::Rail: return c.rail;
    case Surface::Card: return c.card;
    case Surface::Placeholder: return c.placeholder;
    case Surface::Notice: return c.notice;
    case Surface::Content: break;
    }
    return c.content;
}
void setSurface(HWND h, Surface surface) { setProp(h, surfaceProperty, INT_PTR(surface)); }
COLORREF backgroundOf(HWND h) {
    for (HWND w = h; w; w = GetParent(w)) {
        if (const int s = prop<int>(w, surfaceProperty)) return surfaceColor(Surface(s));
        if (GetWindowLongW(w, GWL_STYLE) & WS_POPUP) break;
    }
    return colors().content;
}

void setRole(HWND h, Role role, const char* glyphName) {
    setProp(h, roleProperty, INT_PTR(role));
    setProp(h, glyphProperty, reinterpret_cast<INT_PTR>(glyphName));
    InvalidateRect(h, nullptr, FALSE);
}
void setSelected(HWND h, bool selected) {
    if ((prop<int>(h, selectedProperty) != 0) == selected) return;
    setProp(h, selectedProperty, selected ? 1 : 0);
    InvalidateRect(h, nullptr, FALSE);
}
void setFramed(HWND h, bool on) { setProp(h, framedProperty, on ? 1 : 0); }
void styleCombo(HWND h) { SetWindowSubclass(h, comboProc, 8, 0); }
bool framed(HWND h) { return prop<int>(h, framedProperty) != 0; }
void setCard(HWND h, bool on) {
    setProp(h, cardProperty, on ? 1 : 0);
    if (on && className(h) == WC_LISTVIEWW) SetWindowSubclass(h, listViewProc, 7, 0);
}
bool card(HWND h) { return prop<int>(h, cardProperty) != 0; }
RECT outerRect(HWND h) {
    RECT r{};
    GetWindowRect(h, &r);
    MapWindowPoints(nullptr, GetParent(h), reinterpret_cast<POINT*>(&r), 2);
    InflateRect(&r, prop<int>(h, padXProperty), prop<int>(h, padYProperty));
    return r;
}
void placeInside(HWND h, int x, int y, int width, int height) {
    const UINT dpi = windowDpi(h);
    int padX = 0, padY = 0;
    if (framed(h)) {
        padX = scale(10, dpi);
        const int inner = lineHeight(dpi) + scale(2, dpi);
        padY = std::max(1, (height - inner) / 2);
    }
    SetPropW(h, padXProperty, reinterpret_cast<HANDLE>(static_cast<INT_PTR>(padX)));
    SetPropW(h, padYProperty, reinterpret_cast<HANDLE>(static_cast<INT_PTR>(padY)));
    SetWindowPos(h, nullptr, x + padX, y + padY, std::max(0, width - 2 * padX), std::max(0, height - 2 * padY), SWP_NOZORDER | SWP_NOACTIVATE);
}

void applyTo(HWND root) {
    const bool on = dark();
    const auto& c = colors();
    auto one = [on, &c](HWND h) {
        const auto cls = className(h);
        allowDark(h, on);
        if (cls == WC_BUTTONW) SetWindowTheme(h, on ? L"DarkMode_Explorer" : L"Explorer", nullptr);
        else if (cls == L"ComboLBox") SetWindowTheme(h, on ? L"DarkMode_Explorer" : L"Explorer", nullptr);
        else if (cls == WC_COMBOBOXW) {
            SetWindowTheme(h, on ? L"DarkMode_CFD" : nullptr, nullptr);
            COMBOBOXINFO info{};
            info.cbSize = sizeof info;
            if (GetComboBoxInfo(h, &info) && info.hwndList) { allowDark(info.hwndList, on); SetWindowTheme(info.hwndList, on ? L"DarkMode_Explorer" : L"Explorer", nullptr); }
        } else if (cls == WC_EDITW) {
            const bool multiline = GetWindowLongW(h, GWL_STYLE) & ES_MULTILINE;
            SetWindowTheme(h, multiline ? (on ? L"DarkMode_Explorer" : L"Explorer") : (on ? L"DarkMode_CFD" : nullptr), nullptr);
        } else if (cls == WC_LISTVIEWW) {
            SetWindowTheme(h, on ? L"DarkMode_Explorer" : L"Explorer", nullptr);
            if (HWND header = ListView_GetHeader(h)) { allowDark(header, on); SetWindowTheme(header, on ? L"DarkMode_ItemsView" : L"ItemsView", nullptr); }
            const COLORREF back = backgroundOf(h);
            ListView_SetBkColor(h, back);
            ListView_SetTextBkColor(h, back);
            ListView_SetTextColor(h, c.text);
            // A one-pixel-wide image list sets a roomier row height.
            const int row = scale(34, windowDpi(h));
            HIMAGELIST previous = ListView_GetImageList(h, LVSIL_SMALL);
            int width = 0, height = 0;
            if (!previous || !ImageList_GetIconSize(previous, &width, &height) || height != row) {
                ListView_SetImageList(h, ImageList_Create(1, row, ILC_COLOR32, 1, 0), LVSIL_SMALL);
                if (previous) ImageList_Destroy(previous);
            }
        } else if (cls == WC_LISTBOXW || cls == L"putgd.Panel" || cls == L"putgd.Main" || cls == L"putgd.About") {
            SetWindowTheme(h, on ? L"DarkMode_Explorer" : L"Explorer", nullptr);
        } else if (cls == TOOLTIPS_CLASSW) {
            SetWindowTheme(h, on ? L"DarkMode_Explorer" : nullptr, nullptr);
        }
        SendMessageW(h, WM_THEMECHANGED, 0, 0);
        // A theme change recomputes a combo box's face from its font: restore the row height place() chose.
        if (cls == WC_COMBOBOXW)
            if (const auto face = reinterpret_cast<INT_PTR>(GetPropW(h, L"putgd.faceHeight"))) {
                SendMessageW(h, CB_SETITEMHEIGHT, WPARAM(-1), LPARAM(face));
                SendMessageW(h, CB_SETITEMHEIGHT, 0, scale(26, windowDpi(h)));
            }
    };
    one(root);
    EnumChildWindows(root, [](HWND child, LPARAM data) -> BOOL { (*reinterpret_cast<decltype(one)*>(data))(child); return TRUE; }, reinterpret_cast<LPARAM>(&one));
    RedrawWindow(root, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_FRAME);
}

void fill(HDC dc, const RECT& r, COLORREF color) {
    if (r.right <= r.left || r.bottom <= r.top) return;
    FillRect(dc, &r, brush(color));
}
void roundRect(HDC dc, const RECT& r, int radius, COLORREF face, COLORREF border) {
    const int width = r.right - r.left, height = r.bottom - r.top;
    if (width <= 0 || height <= 0) return;
    radius = std::max(0, std::min({radius, width / 2, height / 2}));
    const bool outline = border != CLR_INVALID;
    if (radius == 0) {
        fill(dc, r, outline ? border : face);
        if (outline) { RECT in = r; InflateRect(&in, -1, -1); fill(dc, in, face); }
        return;
    }
    if (outline) {
        fill(dc, {r.left + radius, r.top, r.right - radius, r.top + 1}, border);
        fill(dc, {r.left + radius, r.bottom - 1, r.right - radius, r.bottom}, border);
        fill(dc, {r.left, r.top + radius, r.left + 1, r.bottom - radius}, border);
        fill(dc, {r.right - 1, r.top + radius, r.right, r.bottom - radius}, border);
        fill(dc, {r.left + 1, r.top + radius, r.right - 1, r.bottom - radius}, face);
        fill(dc, {r.left + radius, r.top + 1, r.right - radius, r.top + radius}, face);
        fill(dc, {r.left + radius, r.bottom - radius, r.right - radius, r.bottom - 1}, face);
    } else {
        fill(dc, {r.left, r.top + radius, r.right, r.bottom - radius}, face);
        fill(dc, {r.left + radius, r.top, r.right - radius, r.top + radius}, face);
        fill(dc, {r.left + radius, r.bottom - radius, r.right - radius, r.bottom}, face);
    }
    const float R = float(radius);
    const struct { int x0, y0; float cx, cy; } corners[] = {
        {r.left, r.top, float(r.left) + R, float(r.top) + R},
        {r.right - radius, r.top, float(r.right) - R, float(r.top) + R},
        {r.left, r.bottom - radius, float(r.left) + R, float(r.bottom) - R},
        {r.right - radius, r.bottom - radius, float(r.right) - R, float(r.bottom) - R},
    };
    for (const auto& corner : corners)
        for (int y = corner.y0; y < corner.y0 + radius; ++y)
            for (int x = corner.x0; x < corner.x0 + radius; ++x) {
                // 4x4 supersampled coverage of the outer shape and of the inner (inside the border) shape.
                int outer = 0, inner = 0;
                for (int sy = 0; sy < 4; ++sy)
                    for (int sx = 0; sx < 4; ++sx) {
                        const float px = float(x) + (float(sx) + 0.5f) / 4 - corner.cx, py = float(y) + (float(sy) + 0.5f) / 4 - corner.cy;
                        const float d2 = px * px + py * py;
                        if (d2 <= R * R) ++outer;
                        if (d2 <= (R - 1) * (R - 1)) ++inner;
                    }
                if (!outer) continue;
                COLORREF color = GetPixel(dc, x, y);
                if (outline) {
                    color = blend(color, border, outer * 255 / 16);
                    color = blend(color, face, inner * 255 / 16);
                } else {
                    color = blend(color, face, outer * 255 / 16);
                }
                SetPixelV(dc, x, y, color);
            }
}
void drawText(HDC dc, const std::wstring& text, RECT r, COLORREF color, HFONT f, UINT format) {
    HGDIOBJ old = SelectObject(dc, f);
    SetTextColor(dc, color);
    SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, text.c_str(), int(text.size()), &r, format);
    SelectObject(dc, old);
}
void setIconOnly(HWND button, bool iconOnly) { setProp(button, iconOnlyProperty, iconOnly ? 1 : 0); InvalidateRect(button, nullptr, FALSE); }
void setHint(HWND button, const std::wstring& hint) {
    if (hint.empty()) hints().erase(button); else hints()[button] = hint;
    InvalidateRect(button, nullptr, FALSE);
}
HICON glyph(const std::string& name, int size, COLORREF main, COLORREF secondary) {
    static std::map<std::tuple<std::string, int, COLORREF, COLORREF>, HICON> cache;
    const auto key = std::make_tuple(name, size, main, secondary);
    if (auto it = cache.find(key); it != cache.end()) return it->second;
    const auto g = define(name);
    const COLORREF layers[3] = {main, secondary, RGB(0x1B, 0x1E, 0x25)};
    std::vector<unsigned> argb(size_t(size) * size, 0);
    const float unit = 24.0f / float(size);
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x) {
            unsigned r = 0, gr = 0, b = 0, hits = 0;
            for (int sy = 0; sy < 4; ++sy)
                for (int sx = 0; sx < 4; ++sx) {
                    const float px = (float(x) + (float(sx) + 0.5f) / 4) * unit, py = (float(y) + (float(sy) + 0.5f) / 4) * unit;
                    for (auto it = g.shapes.rbegin(); it != g.shapes.rend(); ++it)
                        if (covers(*it, px, py)) {
                            const COLORREF c = layers[it->layer];
                            r += GetRValue(c); gr += GetGValue(c); b += GetBValue(c); ++hits;
                            break;
                        }
                }
            if (!hits) continue;
            const unsigned alpha = hits * 255 / 16;
            argb[size_t(y) * size + x] = (alpha << 24) | ((r / hits) << 16) | ((gr / hits) << 8) | (b / hits);
        }
    HICON icon = makeIcon(argb, size);
    cache[key] = icon;
    return icon;
}
void drawGlyph(HDC dc, const std::string& name, int x, int y, int size, COLORREF main, COLORREF secondary) {
    if (HICON icon = glyph(name, size, main, secondary)) DrawIconEx(dc, x, y, icon, size, size, 0, nullptr, DI_NORMAL);
}

bool customDraw(NMHDR* header, LRESULT& result) {
    if (header->code != NM_CUSTOMDRAW) return false;
    auto* cd = reinterpret_cast<NMCUSTOMDRAW*>(header);
    const auto cls = className(header->hwndFrom);
    if (cls == WC_HEADERW) return headerDraw(cd, result);
    if (cls == WC_LISTVIEWW) return rowDraw(reinterpret_cast<NMLVCUSTOMDRAW*>(cd), result);
    if (cls != WC_BUTTONW) return false;
    const LONG type = GetWindowLongW(header->hwndFrom, GWL_STYLE) & BS_TYPEMASK;
    if (cd->dwDrawStage != CDDS_PREPAINT) { result = CDRF_DODEFAULT; return true; }
    if (type == BS_PUSHBUTTON || type == BS_DEFPUSHBUTTON) paintPush(header->hwndFrom, cd->hdc, cd->uItemState);
    else if (type == BS_AUTOCHECKBOX || type == BS_CHECKBOX) paintCheck(header->hwndFrom, cd->hdc, cd->uItemState, false);
    else if (type == BS_AUTORADIOBUTTON || type == BS_RADIOBUTTON) paintCheck(header->hwndFrom, cd->hdc, cd->uItemState, true);
    else { result = CDRF_DODEFAULT; return true; }
    result = CDRF_SKIPDEFAULT;
    return true;
}

LRESULT controlColor(UINT message, HDC dc, HWND control, int textRole) {
    const auto& c = colors();
    const bool enabled = IsWindowEnabled(control);
    COLORREF back = backgroundOf(control), ink = c.text;
    const auto cls = className(control);
    if (message == WM_CTLCOLOREDIT || (message == WM_CTLCOLORSTATIC && cls == WC_EDITW)) {
        if (framed(control)) back = enabled ? c.input : blend(backgroundOf(GetParent(control)), c.input, 110);
        ink = enabled ? c.text : c.textDisabled;
        SetBkMode(dc, OPAQUE);
        SetBkColor(dc, back);
        SetTextColor(dc, ink);
        return reinterpret_cast<LRESULT>(brush(back));
    }
    if (message == WM_CTLCOLORLISTBOX) {
        back = card(control) ? backgroundOf(control) : c.input;
        SetBkColor(dc, back);
        SetTextColor(dc, c.text);
        return reinterpret_cast<LRESULT>(brush(back));
    }
    if (!enabled) ink = c.textDisabled;
    else if (textRole == 2) ink = c.danger;
    else if (textRole == 1) ink = c.textSecondary;
    else if (textRole == 3) ink = c.textMuted;
    SetBkMode(dc, TRANSPARENT);
    SetBkColor(dc, back);
    SetTextColor(dc, ink);
    return reinterpret_cast<LRESULT>(brush(back));
}

void paintDecor(HWND parent, HDC dc, int scrollOffset) {
    const auto& c = colors();
    const UINT dpi = windowDpi(parent);
    const HWND focus = GetFocus();
    for (HWND child = GetWindow(parent, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
        if (!(GetWindowLongW(child, GWL_STYLE) & WS_VISIBLE)) continue;
        if (framed(child)) {
            const RECT outer = outerRect(child);
            const bool enabled = IsWindowEnabled(child);
            const bool readOnly = GetWindowLongW(child, GWL_STYLE) & ES_READONLY;
            const COLORREF face = enabled ? c.input : blend(backgroundOf(parent), c.input, 110);
            const int radius = scale(4, dpi);
            roundRect(dc, outer, radius, face, enabled ? c.inputBorder : c.divider);
            if (enabled && child == focus) {
                const int thick = scale(2, dpi);
                RECT line{outer.left + radius / 2, outer.bottom - thick, outer.right - radius / 2, outer.bottom};
                roundRect(dc, line, 1, c.accent);
            } else if (enabled && !readOnly) {
                RECT line{outer.left + radius, outer.bottom - 1, outer.right - radius, outer.bottom};
                fill(dc, line, c.inputBottom);
            }
        } else if (card(child)) {
            // Flat lists: no box, a hairline column divider where the page asks for one.
            if (GetPropW(child, L"putgd.dividerRight")) {
                const RECT o = outerRect(child);
                const int x = o.right + int(reinterpret_cast<INT_PTR>(GetPropW(child, L"putgd.dividerRight")));
                fill(dc, {x, o.top, x + 1, o.bottom}, c.divider);
            }
        } else if (static_cast<int>(reinterpret_cast<INT_PTR>(GetPropW(child, L"putgd.font"))) == int(FontRole::Heading)) {
            RECT r{};
            GetWindowRect(child, &r);
            MapWindowPoints(nullptr, parent, reinterpret_cast<POINT*>(&r), 2);
            if (r.top + scrollOffset > scale(40, dpi)) fill(dc, {r.left, r.top - scale(15, dpi), r.right, r.top - scale(15, dpi) + 1}, c.divider);
        }
    }
}

void measureListItem(HWND list, MEASUREITEMSTRUCT* item) { item->itemHeight = scale(52, list ? windowDpi(list) : systemDpi()); }
void drawListItem(DRAWITEMSTRUCT* item) {
    if (item->CtlType != ODT_LISTBOX) return;
    const auto& c = colors();
    HWND list = item->hwndItem;
    const UINT dpi = windowDpi(list);
    RECT rc = item->rcItem;
    Buffer buffer(item->hDC, rc);
    HDC dc = buffer.dc;
    fill(dc, rc, backgroundOf(list));
    if (item->itemID == UINT(-1)) return;
    const bool selected = item->itemState & ODS_SELECTED;
    RECT face = rc;
    InflateRect(&face, 0, -scale(2, dpi));
    if (selected) {
        roundRect(dc, face, scale(4, dpi), c.selected);
        const int pillHeight = scale(16, dpi), pillWidth = scale(3, dpi);
        RECT pill{face.left, (face.top + face.bottom - pillHeight) / 2, face.left + pillWidth, (face.top + face.bottom + pillHeight) / 2};
        roundRect(dc, pill, pillWidth / 2 + 1, c.accent);
    }
    const int length = int(SendMessageW(list, LB_GETTEXTLEN, item->itemID, 0));
    std::wstring text(size_t(std::max(0, length)) + 1, L'\0');
    SendMessageW(list, LB_GETTEXT, item->itemID, reinterpret_cast<LPARAM>(text.data()));
    text.resize(size_t(std::max(0, length)));
    // "Name (default) — needs attention": the name, then the page's known qualifiers as a muted second line.
    std::wstring name = text, detail;
    auto take = [&name, &detail](const std::wstring& suffix, const wchar_t* shown) {
        if (name.size() > suffix.size() && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0) {
            name.resize(name.size() - suffix.size());
            detail = std::wstring(shown) + (detail.empty() ? L"" : L" \x00B7 " + detail);
            return true;
        }
        return false;
    };
    take(L" \x2014 needs attention", L"Needs attention");
    take(L" (connected)", L"Connected") || take(L" (signed out)", L"Signed out") || take(L" (choose a folder)", L"Choose a folder");
    take(L" (default)", L"Default destination") || take(L" (not saved)", L"Not saved") || take(L" (not available on Windows)", L"Not available on Windows");
    const int left = face.left + scale(14, dpi);
    RECT top{left, face.top, face.right - scale(8, dpi), detail.empty() ? face.bottom : (face.top + face.bottom) / 2 + scale(1, dpi)};
    drawText(dc, name, top, c.text, font(FontRole::Bold, dpi), DT_SINGLELINE | (detail.empty() ? DT_VCENTER : DT_BOTTOM) | DT_LEFT | DT_END_ELLIPSIS | DT_NOPREFIX);
    if (!detail.empty()) {
        RECT bottom{left, (face.top + face.bottom) / 2 + scale(2, dpi), face.right - scale(8, dpi), face.bottom};
        const bool attention = text.find(L"needs attention") != std::wstring::npos;
        drawText(dc, detail, bottom, attention ? c.danger : c.textSecondary, font(FontRole::Small, dpi), DT_SINGLELINE | DT_TOP | DT_LEFT | DT_END_ELLIPSIS | DT_NOPREFIX);
    }
}

HWND pivot(HWND parent, int id, std::initializer_list<const wchar_t*> items) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW cls{};
        cls.cbSize = sizeof cls;
        cls.lpfnWndProc = pivotProc;
        cls.hInstance = GetModuleHandleW(nullptr);
        cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        cls.lpszClassName = pivotClass;
        RegisterClassExW(&cls);
        registered = true;
    }
    HWND h = CreateWindowExW(0, pivotClass, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 10, 10, parent,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
    auto* data = reinterpret_cast<PivotData*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    for (const auto* item : items) data->items.push_back(item);
    setFont(h, FontRole::Large, windowDpi(parent));
    return h;
}
void setSegmented(HWND h) {
    auto* data = reinterpret_cast<PivotData*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (!data) return;
    data->segmented = true;
    setFont(h, FontRole::Bold, windowDpi(h)); // measured in the selected (semibold) weight
    InvalidateRect(h, nullptr, FALSE);
}
void setStateColumn(HWND list, int column) { SetPropW(list, L"putgd.stateColumn", reinterpret_cast<HANDLE>(static_cast<INT_PTR>(column + 1))); }
int pivotWidth(HWND h) {
    auto* data = reinterpret_cast<PivotData*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (!data) return 0;
    const auto rects = pivotItems(h, *data);
    return rects.empty() ? 0 : int(rects.back().right);
}
}
