#include "resident/Tray.h"
#include "common/Log.h"
#include "common/Paths.h"
#include "resident/Resident.h"
#include <shellapi.h>

namespace putgd {
namespace {
constexpr UINT trayId = 1;
COLORREF tintColor(Tray::Tint t) {
    switch (t) {
    case Tray::Tint::Busy: return RGB(37, 99, 235);
    case Tray::Tint::Uploading: return RGB(22, 163, 74);
    case Tray::Tint::Failed: return RGB(220, 38, 38);
    default: return RGB(38, 38, 38);
    }
}
}
Tray::Tray(HWND owner) : owner_(owner) {
    taskbarCreated_ = RegisterWindowMessageW(L"TaskbarCreated");
    WNDCLASSW wc{}; wc.lpfnWndProc = DefWindowProcW; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"putgd.tray.menu";
    RegisterClassW(&wc);
    menuOwner_ = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"put.gd", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, wc.hInstance, nullptr);
    add();
}
Tray::~Tray() { remove(); if (icon_) DestroyIcon(icon_); if (menuOwner_) DestroyWindow(menuOwner_); }
HICON Tray::makeIcon(Tint tint, int size) {
    // A rounded square with a white "up" arrow: drawn at runtime so the resident
    // needs no image codec or resource loading beyond user32/gdi32.
    BITMAPV5HEADER header{}; header.bV5Size = sizeof header; header.bV5Width = size; header.bV5Height = -size; header.bV5Planes = 1; header.bV5BitCount = 32; header.bV5Compression = BI_BITFIELDS;
    header.bV5RedMask = 0x00FF0000; header.bV5GreenMask = 0x0000FF00; header.bV5BlueMask = 0x000000FF; header.bV5AlphaMask = 0xFF000000;
    void* bits = nullptr;
    HDC screen = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(screen, reinterpret_cast<BITMAPINFO*>(&header), DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (!color || !bits) return nullptr;
    auto* px = static_cast<unsigned*>(bits);
    const COLORREF c = tintColor(tint);
    const unsigned fill = 0xFF000000u | (unsigned(GetRValue(c)) << 16) | (unsigned(GetGValue(c)) << 8) | GetBValue(c);
    const int radius = size / 4;
    for (int y = 0; y < size; ++y) for (int x = 0; x < size; ++x) {
        int dx = x < radius ? radius - x : x >= size - radius ? x - (size - radius - 1) : 0;
        int dy = y < radius ? radius - y : y >= size - radius ? y - (size - radius - 1) : 0;
        const bool inside = dx * dx + dy * dy <= radius * radius;
        px[y * size + x] = inside ? fill : 0;
    }
    // Arrow: a vertical stem and a chevron, in white.
    const int cx = size / 2, top = size / 4, bottom = size * 3 / 4, stem = size >= 32 ? 3 : 2;
    for (int y = top; y <= bottom; ++y) for (int x = cx - stem / 2; x <= cx + stem / 2; ++x) px[y * size + x] = 0xFFFFFFFFu;
    for (int i = 0; i <= size / 4; ++i) for (int t = 0; t < stem; ++t) {
        const int y = top + i + t;
        if (y < size) { if (cx - i >= 0) px[y * size + cx - i] = 0xFFFFFFFFu; if (cx + i < size) px[y * size + cx + i] = 0xFFFFFFFFu; }
    }
    HBITMAP mask = CreateBitmap(size, size, 1, 1, nullptr);
    ICONINFO info{}; info.fIcon = TRUE; info.hbmColor = color; info.hbmMask = mask;
    HICON icon = CreateIconIndirect(&info);
    DeleteObject(color); DeleteObject(mask);
    return icon;
}
void Tray::add() {
    if (icon_) DestroyIcon(icon_);
    icon_ = nullptr;
    if (tint_ == Tint::Idle) {
        // The brand icon from the executable's resources; a drawn tint replaces it while work runs.
        const int size = GetSystemMetrics(SM_CXSMICON);
        icon_ = static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1), IMAGE_ICON, size, size, LR_DEFAULTCOLOR));
    }
    if (!icon_) icon_ = makeIcon(tint_, GetSystemMetrics(SM_CXSMICON));
    NOTIFYICONDATAW data{}; data.cbSize = sizeof data; data.hWnd = owner_; data.uID = trayId;
    data.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP; data.uCallbackMessage = WM_APP_TRAY; data.hIcon = icon_;
    wcsncpy_s(data.szTip, paths::wide(tooltip_).c_str(), _TRUNCATE);
    added_ = Shell_NotifyIconW(added_ ? NIM_MODIFY : NIM_ADD, &data) != FALSE;
    if (added_) { data.uVersion = NOTIFYICON_VERSION_4; Shell_NotifyIconW(NIM_SETVERSION, &data); }
    else log::warning("tray", "Notification area icon unavailable");
}
void Tray::remove() {
    if (!added_) return;
    NOTIFYICONDATAW data{}; data.cbSize = sizeof data; data.hWnd = owner_; data.uID = trayId;
    Shell_NotifyIconW(NIM_DELETE, &data);
    added_ = false;
}
void Tray::setTint(Tint tint) { if (tint == tint_) return; tint_ = tint; add(); }
void Tray::setTooltip(const std::string& utf8) { tooltip_ = utf8; if (added_) add(); }
void Tray::notify(const std::string& title, const std::string& message, bool warning, const std::string& link) {
    if (!added_) return;
    pendingLink_ = link; pendingAction_ = nullptr;
    static unsigned long long shown = 0;
    log::info("ui", "Notification shown", {{"notificationId", double(++shown)}, {"title", title}, {"notificationText", message}, {"warning", warning}});
    NOTIFYICONDATAW data{}; data.cbSize = sizeof data; data.hWnd = owner_; data.uID = trayId; data.uFlags = NIF_INFO | NIF_SHOWTIP;
    data.dwInfoFlags = warning ? NIIF_WARNING : NIIF_INFO;
    wcsncpy_s(data.szInfoTitle, paths::wide(title).c_str(), _TRUNCATE);
    wcsncpy_s(data.szInfo, paths::wide(message).c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &data);
}
void Tray::notify(const std::string& title, const std::string& message, bool warning, std::function<void()> clicked) {
    notify(title, message, warning, std::string());
    pendingAction_ = std::move(clicked);
}
void Tray::showMenu() {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, CmdCaptureRegion, L"Capture Region");
    AppendMenuW(menu, MF_STRING, CmdCaptureEdit, L"Capture Region && Edit");
    AppendMenuW(menu, MF_STRING, CmdCaptureWindow, L"Capture Window");
    AppendMenuW(menu, MF_STRING, CmdCaptureDesktop, L"Capture Desktop");
    AppendMenuW(menu, MF_STRING | (recording_ ? 0 : MF_GRAYED), CmdRecord, L"Capture Screen Recording");
    HMENU edit = CreatePopupMenu();
    AppendMenuW(edit, MF_STRING, CmdEditClipboard, L"From clipboard");
    AppendMenuW(edit, MF_STRING, CmdEditFile, L"From a file...");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(edit), L"Edit Image");
    AppendMenuW(menu, MF_STRING, CmdUploadClipboard, L"Upload Clipboard");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, CmdSettings, L"Settings and setup");
    AppendMenuW(menu, MF_STRING, CmdHistory, L"Upload history");
    AppendMenuW(menu, MF_STRING, CmdActivity, L"Recent activity");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, CmdAbout, L"About put.gd");
    AppendMenuW(menu, MF_STRING, CmdQuit, L"Quit");
    POINT point; GetCursorPos(&point);
    const HWND menuOwner = menuOwner_ ? menuOwner_ : owner_;
    SetForegroundWindow(menuOwner); // required for the menu to close when focus leaves
    const UINT chosen = UINT(TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON, point.x, point.y, menuOwner, nullptr));
    PostMessageW(menuOwner, WM_NULL, 0, 0);
    DestroyMenu(menu);
    if (chosen && command_) command_(chosen);
}
bool Tray::handleMessage(UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == taskbarCreated_) { added_ = false; add(); return true; }
    if (message != WM_APP_TRAY) return false;
    switch (LOWORD(lParam)) {
    case WM_CONTEXTMENU: case WM_RBUTTONUP: showMenu(); return true;
    case NIN_SELECT: case NIN_KEYSELECT: case WM_LBUTTONUP: if (command_) command_(CmdActivity); return true;
    case NIN_BALLOONUSERCLICK:
        if (!pendingLink_.empty()) { ShellExecuteW(nullptr, L"open", paths::wide(pendingLink_).c_str(), nullptr, nullptr, SW_SHOWNORMAL); pendingLink_.clear(); }
        if (pendingAction_) { auto action = std::move(pendingAction_); pendingAction_ = nullptr; action(); }
        return true;
    case NIN_BALLOONTIMEOUT: case NIN_BALLOONHIDE: pendingLink_.clear(); pendingAction_ = nullptr; return true;
    default: return true;
    }
}
}
