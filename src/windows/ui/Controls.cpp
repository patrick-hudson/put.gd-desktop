#include "ui/Controls.h"
#include "ui/Theme.h"
#include "common/Files.h"
#include "common/Paths.h"
#include <algorithm>
#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <uxtheme.h>

namespace putgd::ui {
namespace {
// Resolved at run time so the helper also builds against older SDK headers and
// degrades on Windows versions without per-monitor DPI APIs.
template <class F> F user32(const char* name) {
    static HMODULE module = GetModuleHandleW(L"user32.dll");
    return reinterpret_cast<F>(reinterpret_cast<void (*)()>(GetProcAddress(module, name)));
}
using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
using GetDpiForSystemFn = UINT(WINAPI*)();
using SystemParametersInfoForDpiFn = BOOL(WINAPI*)(UINT, UINT, PVOID, UINT, UINT);
using AdjustWindowRectExForDpiFn = BOOL(WINAPI*)(LPRECT, DWORD, BOOL, DWORD, UINT);
using SetProcessDpiAwarenessContextFn = BOOL(WINAPI*)(HANDLE);

constexpr wchar_t fontProperty[] = L"putgd.font";
constexpr wchar_t panelClass[] = L"putgd.Panel";
constexpr DWORD darkModeAttribute = 20;    // DWMWA_USE_IMMERSIVE_DARK_MODE (Windows 10 2004+)
constexpr DWORD darkModeAttributeOld = 19; // Windows 10 1809-1909

const GUID clsidFileOpenDialog{0xDC1C5A9C, 0xE88A, 0x4DDE, {0xA5, 0xA1, 0x60, 0xF8, 0x2A, 0x20, 0xAE, 0xF7}};
const GUID iidFileOpenDialog{0xD57C7288, 0xD4AD, 0x4768, {0xBE, 0x02, 0x9D, 0x96, 0x95, 0x32, 0xD9, 0x60}};
const GUID iidShellItem{0x43826D1E, 0xE718, 0x42EE, {0xBC, 0x55, 0xA1, 0xE2, 0x61, 0xC3, 0x7B, 0xFE}};

std::map<HWND, std::vector<std::string>>& comboValues() { static std::map<HWND, std::vector<std::string>> values; return values; }

HWND make(DWORD exStyle, const wchar_t* cls, const std::wstring& text, DWORD style, HWND parent, int id, FontRole role = FontRole::Normal) {
    HWND hwnd = CreateWindowExW(exStyle, cls, text.c_str(), WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, parent,
                                reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
    if (hwnd) setFont(hwnd, role, windowDpi(parent));
    return hwnd;
}
}

std::wstring wide(std::string_view utf8) { return paths::wide(std::string(utf8)); }
std::string narrow(std::wstring_view utf16) { return paths::narrow(std::wstring(utf16)); }

UINT windowDpi(HWND hwnd) {
    static auto get = user32<GetDpiForWindowFn>("GetDpiForWindow");
    if (get && hwnd) { const UINT dpi = get(hwnd); if (dpi) return dpi; }
    return systemDpi();
}
UINT systemDpi() {
    static auto get = user32<GetDpiForSystemFn>("GetDpiForSystem");
    if (get) return get();
    HDC dc = GetDC(nullptr);
    const UINT dpi = UINT(GetDeviceCaps(dc, LOGPIXELSY));
    ReleaseDC(nullptr, dc);
    return dpi ? dpi : 96;
}
bool adjustWindowRectForDpi(RECT& rect, DWORD style, DWORD exStyle, UINT dpi) {
    static auto adjust = user32<AdjustWindowRectExForDpiFn>("AdjustWindowRectExForDpi");
    if (adjust) return adjust(&rect, style, FALSE, exStyle, dpi);
    return AdjustWindowRectEx(&rect, style, FALSE, exStyle);
}
void enablePerMonitorDpi() {
    static auto set = user32<SetProcessDpiAwarenessContextFn>("SetProcessDpiAwarenessContext");
    // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 == (HANDLE)-4; fails harmlessly when the manifest already set it.
    if (set) set(reinterpret_cast<HANDLE>(static_cast<INT_PTR>(-4)));
}

namespace {
// Segoe UI Variable on Windows 11 (its GDI named instances, e.g. "Segoe UI Variable
// Display Semib"), else the message font's face (Segoe UI).
std::vector<std::wstring>& families() {
    static std::vector<std::wstring> names = [] {
        std::vector<std::wstring> found;
        LOGFONTW query{};
        query.lfCharSet = DEFAULT_CHARSET;
        HDC dc = GetDC(nullptr);
        EnumFontFamiliesExW(dc, &query, [](const LOGFONTW* font, const TEXTMETRICW*, DWORD, LPARAM data) -> int {
            reinterpret_cast<std::vector<std::wstring>*>(data)->push_back(font->lfFaceName);
            return 1;
        }, reinterpret_cast<LPARAM>(&found), 0);
        ReleaseDC(nullptr, dc);
        return found;
    }();
    return names;
}
std::wstring familyStarting(const std::wstring& prefix) {
    for (const auto& name : families()) if (name.rfind(prefix, 0) == 0) return name;
    return {};
}
}

HFONT font(FontRole role, UINT dpi) {
    static std::map<std::pair<int, UINT>, HFONT> cache;
    const auto key = std::make_pair(int(role), dpi);
    if (auto it = cache.find(key); it != cache.end()) return it->second;
    NONCLIENTMETRICSW metrics{};
    metrics.cbSize = sizeof metrics;
    static auto forDpi = user32<SystemParametersInfoForDpiFn>("SystemParametersInfoForDpi");
    if (!(forDpi && forDpi(SPI_GETNONCLIENTMETRICS, sizeof metrics, &metrics, 0, dpi))) SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof metrics, &metrics, 0);
    LOGFONTW logical = metrics.lfMessageFont;
    int points10 = 90;
    bool semibold = false, display = false;
    switch (role) {
    case FontRole::Normal: break;
    case FontRole::Bold: semibold = true; break;
    case FontRole::Heading: points10 = 120; semibold = display = true; break;
    case FontRole::Title: points10 = 200; semibold = display = true; break;
    case FontRole::Subtitle: points10 = 140; semibold = display = true; break;
    case FontRole::Large: points10 = 105; break;
    case FontRole::Small: points10 = 80; break;
    case FontRole::SmallBold: points10 = 80; semibold = true; break;
    }
    logical.lfHeight = -MulDiv(points10, int(dpi), 720);
    logical.lfWeight = semibold ? FW_SEMIBOLD : FW_NORMAL;
    logical.lfQuality = CLEARTYPE_QUALITY;
    const std::wstring base = display ? L"Segoe UI Variable Display" : L"Segoe UI Variable Text";
    std::wstring face;
    if (semibold) {
        face = familyStarting(base + L" Semib");
        if (!face.empty()) logical.lfWeight = FW_NORMAL; // the named instance carries the weight
    }
    if (face.empty() && !semibold) for (const auto& name : families()) if (name == base) face = name;
    if (face.empty() && semibold) face = familyStarting(L"Segoe UI Semibold");
    if (!face.empty()) {
        if (face == L"Segoe UI Semibold") logical.lfWeight = FW_NORMAL;
        lstrcpynW(logical.lfFaceName, face.c_str(), LF_FACESIZE);
    }
    HFONT created = CreateFontIndirectW(&logical);
    cache[key] = created;
    return created;
}
void setFont(HWND hwnd, FontRole role, UINT dpi) {
    SetPropW(hwnd, fontProperty, reinterpret_cast<HANDLE>(static_cast<INT_PTR>(role)));
    SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(font(role, dpi)), TRUE);
}
void refreshFonts(HWND root, UINT dpi) {
    struct Context { UINT dpi; };
    Context context{dpi};
    EnumChildWindows(root, [](HWND child, LPARAM data) -> BOOL {
        const auto role = static_cast<int>(reinterpret_cast<INT_PTR>(GetPropW(child, fontProperty)));
        if (role) SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(font(FontRole(role), reinterpret_cast<Context*>(data)->dpi)), TRUE);
        return TRUE;
    }, reinterpret_cast<LPARAM>(&context));
}
int fontHeight(HFONT f) {
    HDC dc = GetDC(nullptr);
    HGDIOBJ old = SelectObject(dc, f);
    TEXTMETRICW metrics{};
    GetTextMetricsW(dc, &metrics);
    SelectObject(dc, old);
    ReleaseDC(nullptr, dc);
    return metrics.tmHeight + metrics.tmExternalLeading;
}
int lineHeight(UINT dpi) { return fontHeight(font(FontRole::Normal, dpi)); }
int textHeight(HWND hwnd, int width) {
    HFONT f = reinterpret_cast<HFONT>(SendMessageW(hwnd, WM_GETFONT, 0, 0));
    if (!f) f = font(FontRole::Normal, windowDpi(hwnd));
    const auto value = textW(hwnd);
    if (value.empty()) return fontHeight(f);
    HDC dc = GetDC(hwnd);
    HGDIOBJ old = SelectObject(dc, f);
    RECT r{0, 0, std::max(1, width), 0};
    DrawTextW(dc, value.c_str(), int(value.size()), &r, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX | DT_EDITCONTROL);
    SelectObject(dc, old);
    ReleaseDC(hwnd, dc);
    return std::max<int>(r.bottom, fontHeight(f));
}
int buttonWidth(HWND hwnd, int minimum) {
    const UINT dpi = windowDpi(hwnd);
    HFONT f = reinterpret_cast<HFONT>(SendMessageW(hwnd, WM_GETFONT, 0, 0));
    if (!f) f = font(FontRole::Normal, dpi);
    const auto value = textW(hwnd);
    HDC dc = GetDC(hwnd);
    HGDIOBJ old = SelectObject(dc, f);
    RECT r{0, 0, 0, 0};
    DrawTextW(dc, value.c_str(), int(value.size()), &r, DT_CALCRECT | DT_SINGLELINE);
    SelectObject(dc, old);
    ReleaseDC(hwnd, dc);
    return std::max(scale(minimum, dpi), int(r.right) + scale(40, dpi));
}

HWND label(HWND parent, const std::wstring& text, int id, DWORD extraStyle) { return make(0, WC_STATICW, text, SS_LEFT | SS_NOPREFIX | extraStyle, parent, id); }
HWND heading(HWND parent, const std::wstring& text) { return make(0, WC_STATICW, text, SS_LEFT | SS_NOPREFIX, parent, 0, FontRole::Heading); }
HWND edit(HWND parent, int id, DWORD extraStyle) {
    HWND hwnd = make(0, WC_EDITW, L"", WS_TABSTOP | ES_AUTOHSCROLL | extraStyle, parent, id);
    theme::setFramed(hwnd, true);
    return hwnd;
}
HWND multilineReadOnly(HWND parent, int id) {
    return make(0, WC_EDITW, L"", WS_TABSTOP | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL, parent, id);
}
HWND password(HWND parent, int id) {
    HWND hwnd = make(0, WC_EDITW, L"", WS_TABSTOP | ES_AUTOHSCROLL | ES_PASSWORD, parent, id);
    theme::setFramed(hwnd, true);
    return hwnd;
}
HWND combo(HWND parent, int id) {
    HWND hwnd = make(0, WC_COMBOBOXW, L"", WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST, parent, id);
    theme::styleCombo(hwnd);
    return hwnd;
}
HWND check(HWND parent, int id, const std::wstring& text) { return make(0, WC_BUTTONW, text, WS_TABSTOP | BS_AUTOCHECKBOX, parent, id); }
HWND radio(HWND parent, int id, const std::wstring& text, bool firstInGroup) {
    return make(0, WC_BUTTONW, text, BS_AUTORADIOBUTTON | (firstInGroup ? WS_GROUP | WS_TABSTOP : 0), parent, id);
}
HWND button(HWND parent, int id, const std::wstring& text, DWORD extraStyle) { return make(0, WC_BUTTONW, text, WS_TABSTOP | BS_PUSHBUTTON | extraStyle, parent, id); }
HWND listBox(HWND parent, int id, bool ownerDrawn) {
    HWND hwnd = make(0, WC_LISTBOXW, L"", WS_TABSTOP | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | (ownerDrawn ? LBS_OWNERDRAWFIXED | LBS_HASSTRINGS : 0), parent, id);
    theme::setCard(hwnd, true);
    return hwnd;
}
HWND listView(HWND parent, int id, DWORD extraStyle) {
    HWND list = make(0, WC_LISTVIEWW, L"", WS_TABSTOP | LVS_REPORT | LVS_SHOWSELALWAYS | extraStyle, parent, id);
    ListView_SetExtendedListViewStyle(list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
    SetWindowTheme(list, L"Explorer", nullptr);
    theme::setCard(list, true);
    return list;
}
HWND imageBox(HWND parent, int id) { return make(0, WC_STATICW, L"", SS_BITMAP | SS_CENTERIMAGE | SS_REALSIZECONTROL, parent, id); }

std::wstring textW(HWND hwnd) {
    const int length = GetWindowTextLengthW(hwnd);
    std::wstring value(size_t(length) + 1, L'\0');
    GetWindowTextW(hwnd, value.data(), length + 1);
    value.resize(size_t(length));
    return value;
}
std::string text(HWND hwnd) { return narrow(textW(hwnd)); }
void setText(HWND hwnd, std::string_view utf8) { SetWindowTextW(hwnd, wide(utf8).c_str()); }
void setTextW(HWND hwnd, const std::wstring& value) { SetWindowTextW(hwnd, value.c_str()); }
void syncText(HWND hwnd, std::string_view utf8) { if (text(hwnd) != utf8) setText(hwnd, utf8); }
bool checked(HWND hwnd) { return SendMessageW(hwnd, BM_GETCHECK, 0, 0) == BST_CHECKED; }
void setChecked(HWND hwnd, bool value) { SendMessageW(hwnd, BM_SETCHECK, value ? BST_CHECKED : BST_UNCHECKED, 0); }
void setCue(HWND hwnd, const std::wstring& cue) { SendMessageW(hwnd, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(cue.c_str())); }
void enable(HWND hwnd, bool on) { if (hwnd && bool(IsWindowEnabled(hwnd)) != on) EnableWindow(hwnd, on); }
void show(HWND hwnd, bool on) { if (hwnd) ShowWindow(hwnd, on ? SW_SHOWNA : SW_HIDE); }
bool visible(HWND hwnd) { return hwnd && (GetWindowLongW(hwnd, GWL_STYLE) & WS_VISIBLE); }
void wipe(HWND hwnd) {
    const int length = GetWindowTextLengthW(hwnd);
    if (length > 0) {
        std::wstring filler(size_t(length), L'x');
        SetWindowTextW(hwnd, filler.c_str());
        SecureZeroMemory(filler.data(), filler.size() * sizeof(wchar_t));
    }
    SetWindowTextW(hwnd, L"");
    SendMessageW(hwnd, EM_EMPTYUNDOBUFFER, 0, 0);
}

void setChoices(HWND hwnd, const std::vector<Choice>& choices) {
    const auto previous = selected(hwnd);
    SendMessageW(hwnd, CB_RESETCONTENT, 0, 0);
    auto& values = comboValues()[hwnd];
    values.clear();
    for (const auto& choice : choices) {
        const auto index = SendMessageW(hwnd, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(choice.label.c_str()));
        SendMessageW(hwnd, CB_SETITEMDATA, WPARAM(index), LPARAM(values.size()));
        values.push_back(choice.value);
    }
    if (!select(hwnd, previous) && !choices.empty()) SendMessageW(hwnd, CB_SETCURSEL, 0, 0);
}
std::string selected(HWND hwnd) {
    const auto index = SendMessageW(hwnd, CB_GETCURSEL, 0, 0);
    if (index == CB_ERR) return {};
    const auto slot = size_t(SendMessageW(hwnd, CB_GETITEMDATA, WPARAM(index), 0));
    const auto& values = comboValues()[hwnd];
    return slot < values.size() ? values[slot] : std::string();
}
bool select(HWND hwnd, const std::string& value) {
    const auto& values = comboValues()[hwnd];
    const auto count = SendMessageW(hwnd, CB_GETCOUNT, 0, 0);
    for (LRESULT index = 0; index < count; ++index) {
        const auto slot = size_t(SendMessageW(hwnd, CB_GETITEMDATA, WPARAM(index), 0));
        if (slot < values.size() && values[slot] == value) { SendMessageW(hwnd, CB_SETCURSEL, WPARAM(index), 0); return true; }
    }
    return false;
}

void addColumn(HWND list, int index, const std::wstring& title, int width) {
    LVCOLUMNW column{};
    column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
    column.pszText = const_cast<wchar_t*>(title.c_str());
    column.cx = width;
    column.iSubItem = index;
    SendMessageW(list, LVM_INSERTCOLUMNW, WPARAM(index), reinterpret_cast<LPARAM>(&column));
}
int addRow(HWND list, const std::vector<std::wstring>& cells, LPARAM data) {
    LVITEMW item{};
    item.mask = LVIF_TEXT | LVIF_PARAM;
    item.iItem = int(SendMessageW(list, LVM_GETITEMCOUNT, 0, 0));
    item.pszText = const_cast<wchar_t*>(cells.empty() ? L"" : cells[0].c_str());
    item.lParam = data;
    const int row = int(SendMessageW(list, LVM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&item)));
    for (size_t column = 1; column < cells.size(); ++column) {
        LVITEMW sub{};
        sub.iSubItem = int(column);
        sub.pszText = const_cast<wchar_t*>(cells[column].c_str());
        SendMessageW(list, LVM_SETITEMTEXTW, WPARAM(row), reinterpret_cast<LPARAM>(&sub));
    }
    return row;
}
std::vector<int> selectedRows(HWND list) {
    std::vector<int> rows;
    int row = -1;
    while ((row = int(SendMessageW(list, LVM_GETNEXTITEM, WPARAM(row), LVNI_SELECTED))) != -1) rows.push_back(row);
    return rows;
}
LPARAM rowData(HWND list, int row) {
    LVITEMW item{};
    item.mask = LVIF_PARAM;
    item.iItem = row;
    SendMessageW(list, LVM_GETITEMW, 0, reinterpret_cast<LPARAM>(&item));
    return item.lParam;
}

void place(HWND hwnd, int x, int y, int width, int height) {
    if (!hwnd) return;
    if (theme::framed(hwnd) || theme::card(hwnd)) { theme::placeInside(hwnd, x, y, width, height); return; }
    wchar_t cls[32]{};
    GetClassNameW(hwnd, cls, 32);
    if (lstrcmpiW(cls, WC_COMBOBOXW) == 0) {
        // The face takes the row's height; the drop-down list gets roomier items.
        const UINT dpi = windowDpi(hwnd);
        SetPropW(hwnd, L"putgd.faceHeight", reinterpret_cast<HANDLE>(static_cast<INT_PTR>(height - scale(6, dpi))));
        SetWindowPos(hwnd, nullptr, x, y, std::max(0, width), std::max(0, height + scale(260, dpi)), SWP_NOZORDER | SWP_NOACTIVATE);
        // After sizing: a resize recomputes the face from the font.
        if (SendMessageW(hwnd, CB_GETITEMHEIGHT, WPARAM(-1), 0) != height - scale(6, dpi)) {
            SendMessageW(hwnd, CB_SETITEMHEIGHT, WPARAM(-1), height - scale(6, dpi));
            SendMessageW(hwnd, CB_SETITEMHEIGHT, 0, scale(26, dpi));
        }
        RECT face{};
        GetWindowRect(hwnd, &face);
        if (face.bottom - face.top != height) SetWindowPos(hwnd, nullptr, x, y + (height - int(face.bottom - face.top)) / 2, 0, 0, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOSIZE);
        return;
    }
    SetWindowPos(hwnd, nullptr, x, y, std::max(0, width), std::max(0, height), SWP_NOZORDER | SWP_NOACTIVATE);
}

Rows::Rows(UINT dpi, int x, int y, int width, int labelWidth) : dpi_(dpi), x_(x), y_(y), width_(width), label_(scale(labelWidth, dpi)) {
    if (label_ > width_ / 2) label_ = width_ / 2;
}
int Rows::fieldHeight() const { return std::max(px(32), lineHeight(dpi_) + px(14)); }
int Rows::buttonHeight() const { return std::max(px(32), lineHeight(dpi_) + px(14)); }
void Rows::heading(HWND hwnd) {
    const int height = textHeight(hwnd, width_);
    if (!first_) y_ += px(30);
    first_ = false;
    place(hwnd, x_, y_, width_, height + px(2));
    y_ += height + px(14);
}
void Rows::text(HWND hwnd, int lines) {
    (void)lines;
    const int height = textHeight(hwnd, width_) + px(2);
    place(hwnd, x_, y_, width_, height);
    y_ += height + px(10);
}
void Rows::field(HWND labelControl, HWND control, int controlWidth, HWND trailing, int trailingWidth) {
    const int height = fieldHeight();
    const int textHeightPx = lineHeight(dpi_);
    if (labelControl) place(labelControl, x_, y_ + (height - textHeightPx) / 2, label_ - px(12), textHeightPx + px(2));
    int available = width_ - label_;
    const int gap = px(8);
    const int trailingPx = trailing ? (trailingWidth < 0 ? buttonWidth(trailing) : scale(trailingWidth, dpi_)) : 0;
    const int trailingSpace = trailing ? trailingPx + gap : 0;
    int width = controlWidth > 0 ? std::min(scale(controlWidth, dpi_), available - trailingSpace) : available - trailingSpace;
    if (control) place(control, x_ + label_, y_, width, height);
    if (trailing) place(trailing, x_ + label_ + width + gap, y_ + (height - buttonHeight()) / 2, trailingPx, buttonHeight());
    y_ += std::max(height, trailing ? buttonHeight() : 0) + px(8);
}
void Rows::error(HWND hwnd) {
    if (!hwnd) return;
    if (GetWindowTextLengthW(hwnd) == 0) { show(hwnd, false); return; }
    const int height = textHeight(hwnd, width_ - label_) + px(2);
    y_ -= px(4);
    place(hwnd, x_ + label_, y_, width_ - label_, height);
    show(hwnd, true);
    y_ += height + px(8);
}
void Rows::line(const std::vector<std::pair<HWND, int>>& items, bool controlColumn, int height) {
    if (!height) height = buttonHeight();
    int x = controlColumn ? x_ + label_ : x_;
    for (const auto& [hwnd, width] : items) {
        if (!hwnd) continue;
        const int w = width > 0 ? scale(width, dpi_) : width < 0 ? buttonWidth(hwnd) : x_ + width_ - x;
        place(hwnd, x, y_, w, height);
        x += w + px(8);
    }
    y_ += height + px(8);
}
void Rows::full(HWND hwnd, int height) {
    place(hwnd, x_, y_, width_, height);
    y_ += height + px(8);
}
void Rows::block(HWND hwnd, int heightPx, bool controlColumn) {
    place(hwnd, controlColumn ? x_ + label_ : x_, y_, controlColumn ? width_ - label_ : width_, heightPx);
    y_ += heightPx + px(8);
}
void Rows::space(int value) { y_ += px(value); }

HWND Panel::create(HWND parent, bool scrollable) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW cls{};
        cls.cbSize = sizeof cls;
        cls.lpfnWndProc = proc;
        cls.hInstance = GetModuleHandleW(nullptr);
        cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        cls.hbrBackground = nullptr;
        cls.lpszClassName = panelClass;
        RegisterClassExW(&cls);
        registered = true;
    }
    scrollable_ = scrollable;
    hwnd_ = CreateWindowExW(WS_EX_CONTROLPARENT, panelClass, L"", WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS | (scrollable ? WS_VSCROLL : 0),
                            0, 0, 10, 10, parent, nullptr, GetModuleHandleW(nullptr), this);
    if (hwnd_) theme::setSurface(hwnd_, theme::Surface::Content);
    return hwnd_;
}
void Panel::layout() {
    if (!hwnd_ || laying_) return;
    laying_ = true;
    RECT client{};
    GetClientRect(hwnd_, &client);
    content_ = onLayout ? onLayout(client.right, client.bottom) : 0;
    laying_ = false;
    if (scrollable_) updateScrollBar(client.bottom);
    // Frames and dividers are painted by the panel itself: repaint them where children moved.
    InvalidateRect(hwnd_, nullptr, FALSE);
}
void Panel::updateScrollBar(int height) {
    const int maximum = std::max(0, content_ - height);
    if (scroll_ > maximum) { scroll_ = maximum; laying_ = true; RECT c{}; GetClientRect(hwnd_, &c); if (onLayout) onLayout(c.right, c.bottom); laying_ = false; }
    SCROLLINFO info{};
    info.cbSize = sizeof info;
    info.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    info.nMin = 0;
    info.nMax = std::max(0, content_ - 1);
    info.nPage = UINT(std::max(0, height));
    info.nPos = scroll_;
    SetScrollInfo(hwnd_, SB_VERT, &info, TRUE);
}
void Panel::scrollTo(int y) {
    RECT client{};
    GetClientRect(hwnd_, &client);
    const int target = std::clamp(y, 0, std::max(0, content_ - int(client.bottom)));
    if (target == scroll_) return;
    scroll_ = target;
    layout();
    InvalidateRect(hwnd_, nullptr, TRUE);
}
void Panel::markError(HWND control, bool error) {
    errors_.erase(std::remove(errors_.begin(), errors_.end(), control), errors_.end());
    if (error) errors_.push_back(control);
    InvalidateRect(control, nullptr, TRUE);
}
void Panel::markMuted(HWND control) { muted_.push_back(control); }
void Panel::markFaint(HWND control) { faint_.push_back(control); }
void Panel::paint() {
    PAINTSTRUCT ps;
    HDC target = BeginPaint(hwnd_, &ps);
    RECT client{};
    GetClientRect(hwnd_, &client);
    if (client.right > 0 && client.bottom > 0) {
        HDC dc = CreateCompatibleDC(target);
        HBITMAP bitmap = CreateCompatibleBitmap(target, client.right, client.bottom);
        HGDIOBJ old = SelectObject(dc, bitmap);
        theme::fill(dc, client, theme::backgroundOf(hwnd_));
        theme::paintDecor(hwnd_, dc, scroll_);
        BitBlt(target, ps.rcPaint.left, ps.rcPaint.top, ps.rcPaint.right - ps.rcPaint.left, ps.rcPaint.bottom - ps.rcPaint.top, dc, ps.rcPaint.left, ps.rcPaint.top, SRCCOPY);
        SelectObject(dc, old);
        DeleteObject(bitmap);
        DeleteDC(dc);
    }
    EndPaint(hwnd_, &ps);
}
LRESULT CALLBACK Panel::proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    Panel* self = reinterpret_cast<Panel*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        self = static_cast<Panel*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
    }
    if (!self) return DefWindowProcW(hwnd, message, wParam, lParam);
    return self->handle(message, wParam, lParam);
}
LRESULT Panel::handle(UINT message, WPARAM wParam, LPARAM lParam) {
    if (onMessage) { LRESULT result = 0; if (onMessage(message, wParam, lParam, result)) return result; }
    switch (message) {
    case WM_COMMAND:
        if (lParam && (HIWORD(wParam) == EN_SETFOCUS || HIWORD(wParam) == EN_KILLFOCUS) && theme::framed(reinterpret_cast<HWND>(lParam))) {
            RECT frame = theme::outerRect(reinterpret_cast<HWND>(lParam));
            InvalidateRect(hwnd_, &frame, FALSE);
        }
        if (onCommand && lParam) onCommand(LOWORD(wParam), HIWORD(wParam), reinterpret_cast<HWND>(lParam));
        return 0;
    case WM_NOTIFY: {
        LRESULT result = 0;
        if (theme::customDraw(reinterpret_cast<NMHDR*>(lParam), result)) return result;
        if (onNotify) return onNotify(reinterpret_cast<NMHDR*>(lParam));
        return 0;
    }
    case WM_TIMER:
        if (onTimer) onTimer(wParam);
        return 0;
    case WM_SIZE:
        layout();
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        paint();
        return 0;
    case WM_LBUTTONDOWN: {
        // A click in a framed edit's padding focuses the edit.
        const POINT at{static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam))};
        for (HWND child = GetWindow(hwnd_, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
            if (!theme::framed(child) || !IsWindowVisible(child) || !IsWindowEnabled(child)) continue;
            RECT frame = theme::outerRect(child);
            if (PtInRect(&frame, at)) { SetFocus(child); SendMessageW(child, EM_SETSEL, WPARAM(-1), 0); break; }
        }
        return 0;
    }
    case WM_MEASUREITEM: {
        auto* item = reinterpret_cast<MEASUREITEMSTRUCT*>(lParam);
        if (item->CtlType == ODT_LISTBOX) { theme::measureListItem(GetDlgItem(hwnd_, int(item->CtlID)), item); return TRUE; }
        break;
    }
    case WM_DRAWITEM: {
        auto* item = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
        if (item->CtlType == ODT_LISTBOX) { theme::drawListItem(item); return TRUE; }
        break;
    }
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN: {
        HWND control = reinterpret_cast<HWND>(lParam);
        const auto has = [control](const std::vector<HWND>& list) { return std::find(list.begin(), list.end(), control) != list.end(); };
        const int role = has(errors_) ? 2 : has(muted_) ? 1 : has(faint_) ? 3 : 0;
        return theme::controlColor(message, reinterpret_cast<HDC>(wParam), control, role);
    }
    case WM_VSCROLL: {
        if (!scrollable_) break;
        SCROLLINFO info{};
        info.cbSize = sizeof info;
        info.fMask = SIF_ALL;
        GetScrollInfo(hwnd_, SB_VERT, &info);
        int target = scroll_;
        const int step = scale(40, dpi());
        switch (LOWORD(wParam)) {
        case SB_LINEUP: target -= step; break;
        case SB_LINEDOWN: target += step; break;
        case SB_PAGEUP: target -= int(info.nPage); break;
        case SB_PAGEDOWN: target += int(info.nPage); break;
        case SB_THUMBTRACK: case SB_THUMBPOSITION: target = info.nTrackPos; break;
        case SB_TOP: target = 0; break;
        case SB_BOTTOM: target = content_; break;
        }
        scrollTo(target);
        return 0;
    }
    case WM_MOUSEWHEEL:
        if (!scrollable_) break;
        scrollTo(scroll_ - GET_WHEEL_DELTA_WPARAM(wParam) * scale(48, dpi()) / WHEEL_DELTA);
        return 0;
    }
    return DefWindowProcW(hwnd_, message, wParam, lParam);
}

HWND createTooltip(HWND owner) {
    HWND tip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, CW_USEDEFAULT, CW_USEDEFAULT,
                               CW_USEDEFAULT, CW_USEDEFAULT, owner, nullptr, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(tip, TTM_SETMAXTIPWIDTH, 0, scale(420, windowDpi(owner)));
    SendMessageW(tip, TTM_SETDELAYTIME, TTDT_AUTOPOP, 20000);
    return tip;
}
void setTooltip(HWND tip, HWND control, const std::wstring& value) {
    if (!tip || !control) return;
    TTTOOLINFOW info{};
    info.cbSize = sizeof info;
    info.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
    info.hwnd = GetParent(control);
    info.uId = reinterpret_cast<UINT_PTR>(control);
    info.lpszText = const_cast<wchar_t*>(value.c_str());
    if (SendMessageW(tip, TTM_GETTOOLINFOW, 0, reinterpret_cast<LPARAM>(&info))) {
        info.lpszText = const_cast<wchar_t*>(value.c_str());
        SendMessageW(tip, TTM_UPDATETIPTEXTW, 0, reinterpret_cast<LPARAM>(&info));
    } else {
        SendMessageW(tip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&info));
    }
}

bool systemPrefersDark() {
    DWORD value = 1, size = sizeof value;
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"AppsUseLightTheme",
                     RRF_RT_REG_DWORD, nullptr, &value, &size) != ERROR_SUCCESS) return false;
    return value == 0;
}
void applyTitleBarTheme(HWND hwnd, const std::string& themeSetting) {
    const BOOL dark = themeSetting == "dark" || (themeSetting != "light" && systemPrefersDark());
    if (FAILED(DwmSetWindowAttribute(hwnd, darkModeAttribute, &dark, sizeof dark)))
        DwmSetWindowAttribute(hwnd, darkModeAttributeOld, &dark, sizeof dark);
}

void message(HWND owner, const std::wstring& title, const std::wstring& instruction, const std::wstring& content, bool error) {
    TASKDIALOGCONFIG config{};
    config.cbSize = sizeof config;
    config.hwndParent = owner;
    config.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW;
    config.dwCommonButtons = TDCBF_OK_BUTTON;
    config.pszWindowTitle = title.c_str();
    config.pszMainIcon = error ? TD_ERROR_ICON : TD_INFORMATION_ICON;
    config.pszMainInstruction = instruction.c_str();
    config.pszContent = content.empty() ? nullptr : content.c_str();
    TaskDialogIndirect(&config, nullptr, nullptr, nullptr);
}
bool confirm(HWND owner, const std::wstring& title, const std::wstring& instruction, const std::wstring& content, const std::wstring& accept,
             const std::wstring& footer, bool warning) {
    constexpr int acceptId = 1001;
    TASKDIALOG_BUTTON buttons[] = {{acceptId, accept.c_str()}};
    TASKDIALOGCONFIG config{};
    config.cbSize = sizeof config;
    config.hwndParent = owner;
    config.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW;
    config.dwCommonButtons = TDCBF_CANCEL_BUTTON;
    config.pszWindowTitle = title.c_str();
    config.pszMainIcon = warning ? TD_WARNING_ICON : TD_INFORMATION_ICON;
    config.pszMainInstruction = instruction.c_str();
    config.pszContent = content.empty() ? nullptr : content.c_str();
    config.pszFooter = footer.empty() ? nullptr : footer.c_str();
    if (!footer.empty()) config.pszFooterIcon = TD_INFORMATION_ICON;
    config.cButtons = 1;
    config.pButtons = buttons;
    config.nDefaultButton = IDCANCEL;
    int pressed = 0;
    if (FAILED(TaskDialogIndirect(&config, &pressed, nullptr, nullptr))) return false;
    return pressed == acceptId;
}

namespace {
bool runOpenDialog(HWND owner, const std::wstring& title, const std::string& initial, bool folders, std::string& chosen) {
    IFileOpenDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(clsidFileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, iidFileOpenDialog, reinterpret_cast<void**>(&dialog))) || !dialog) return false;
    FILEOPENDIALOGOPTIONS options = 0;
    dialog->GetOptions(&options);
    options |= FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | (folders ? FOS_PICKFOLDERS : FOS_FILEMUSTEXIST);
    dialog->SetOptions(options);
    dialog->SetTitle(title.c_str());
    if (!folders) {
        COMDLG_FILTERSPEC filters[] = {{L"Images (PNG, JPEG)", L"*.png;*.jpg;*.jpeg"}};
        dialog->SetFileTypes(1, filters);
    }
    if (!initial.empty() && files::isDirectory(initial)) {
        IShellItem* folder = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(wide(initial).c_str(), nullptr, iidShellItem, reinterpret_cast<void**>(&folder))) && folder) {
            dialog->SetFolder(folder);
            folder->Release();
        }
    }
    bool ok = false;
    if (SUCCEEDED(dialog->Show(owner))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item)) && item) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
                chosen = narrow(path);
                ok = !chosen.empty();
                CoTaskMemFree(path);
            }
            item->Release();
        }
    }
    dialog->Release();
    return ok;
}
}
bool pickFolder(HWND owner, const std::wstring& title, const std::string& initial, std::string& chosen) { return runOpenDialog(owner, title, initial, true, chosen); }
bool pickImageFile(HWND owner, std::string& chosen) {
    std::string pictures;
    PWSTR known = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Pictures, 0, nullptr, &known)) && known) { pictures = narrow(known); CoTaskMemFree(known); }
    return runOpenDialog(owner, L"Edit an image file", pictures, false, chosen);
}
void revealInExplorer(const std::string& path) {
    if (path.empty()) return;
    const std::wstring arguments = L"/select,\"" + wide(path) + L"\"";
    ShellExecuteW(nullptr, L"open", L"explorer.exe", arguments.c_str(), nullptr, SW_SHOWNORMAL);
}
void openUrl(const std::string& url) {
    if (url.rfind("https://", 0) != 0 && url.rfind("http://", 0) != 0) return;
    ShellExecuteW(nullptr, L"open", wide(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}
std::wstring localTime(const std::string& isoUtc) {
    const long long ms = files::parseIsoUtcMs(isoUtc);
    if (ms < 0) return {};
    const unsigned long long ticks = (static_cast<unsigned long long>(ms) + 11644473600000ULL) * 10000ULL;
    FILETIME utc{DWORD(ticks & 0xFFFFFFFFULL), DWORD(ticks >> 32)};
    SYSTEMTIME universal{}, local{};
    if (!FileTimeToSystemTime(&utc, &universal) || !SystemTimeToTzSpecificLocalTime(nullptr, &universal, &local)) return {};
    wchar_t date[64]{}, time[64]{};
    GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_SHORTDATE, &local, nullptr, date, 64, nullptr);
    GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, &local, nullptr, time, 64);
    return std::wstring(date) + L" " + time;
}
}
