#include "record/Overlay.h"
#include "core/BarPlacement.h"
#include "core/Recording.h"
#include "core/ReviewTimeline.h"
#include "ui/Controls.h"
#include "ui/Theme.h"
#include <dwmapi.h>
#include <shellscalingapi.h>

namespace putgd::record {
namespace theme = ui::theme;
namespace {
constexpr wchar_t edgeClass[] = L"putgd.record.edge";
constexpr wchar_t barClass[] = L"putgd.record.bar";
constexpr UINT_PTR clockTimer = 1;
constexpr int idRecord = 101, idPause = 102, idStop = 103, idCancel = 104, idFormat = 105, idAudio = 106;
constexpr int ringWidth = 3;   // white ring, then a 1 px dark hairline outside it
constexpr BYTE dimAlpha = 110; // everything outside the area while recording

// Edge windows: click-through, never activated, above everything else.
constexpr DWORD edgeStyle = WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
LRESULT CALLBACK edgeProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_NCHITTEST) return HTTRANSPARENT;
    if (message == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT client; GetClientRect(hwnd, &client);
        const auto kind = int(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (kind == 0) FillRect(dc, &client, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        else {
            // kind 1..4: top, bottom, left, right ring edge; the hairline is on the outer side.
            FillRect(dc, &client, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
            RECT line = client;
            if (kind == 1) line.bottom = line.top + 1;
            else if (kind == 2) line.top = line.bottom - 1;
            else if (kind == 3) line.right = line.left + 1;
            else line.left = line.right - 1;
            HBRUSH dark = CreateSolidBrush(RGB(0x20, 0x20, 0x20));
            FillRect(dc, &line, dark);
            DeleteObject(dark);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}
bool registerClasses() {
    static const bool done = [] {
        WNDCLASSEXW edge{sizeof edge};
        edge.lpfnWndProc = edgeProc; edge.hInstance = GetModuleHandleW(nullptr); edge.lpszClassName = edgeClass; edge.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        return RegisterClassExW(&edge) != 0;
    }();
    return done;
}
HWND edgeWindow(int kind, const grab::Rect& r, BYTE alpha) {
    if (!registerClasses() || r.empty()) return nullptr;
    HWND hwnd = CreateWindowExW(edgeStyle, edgeClass, L"put.gd recording frame", WS_POPUP, r.x, r.y, r.w, r.h, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!hwnd) return nullptr;
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, kind);
    SetLayeredWindowAttributes(hwnd, 0, alpha, LWA_ALPHA);
    excludeFromCapture(hwnd);
    return hwnd;
}
}

bool excludeFromCapture(HWND hwnd) {
    constexpr DWORD excludeFromCaptureAffinity = 0x00000011; // WDA_EXCLUDEFROMCAPTURE
    return SetWindowDisplayAffinity(hwnd, excludeFromCaptureAffinity) != FALSE;
}

AreaFrame::~AreaFrame() { hide(); }
bool AreaFrame::show(const grab::Rect& area) {
    hide();
    area_ = area; screen_ = grab::virtualScreen();
    auto& e = edges_;
    const int right = area.x + area.w, bottom = area.y + area.h, sr = screen_.x + screen_.w, sb = screen_.y + screen_.h;
    // Dimming strips (0-3), shown only while recording; the ring (4-7) just outside the area.
    e[0] = edgeWindow(0, {screen_.x, screen_.y, screen_.w, area.y - screen_.y}, dimAlpha);
    e[1] = edgeWindow(0, {screen_.x, bottom, screen_.w, sb - bottom}, dimAlpha);
    e[2] = edgeWindow(0, {screen_.x, area.y, area.x - screen_.x, area.h}, dimAlpha);
    e[3] = edgeWindow(0, {right, area.y, sr - right, area.h}, dimAlpha);
    const int t = ringWidth + 1;
    e[4] = edgeWindow(1, {area.x - t, area.y - t, area.w + 2 * t, t}, 255);
    e[5] = edgeWindow(2, {area.x - t, bottom, area.w + 2 * t, t}, 255);
    e[6] = edgeWindow(3, {area.x - t, area.y, t, area.h}, 255);
    e[7] = edgeWindow(4, {right, area.y, t, area.h}, 255);
    excluded_ = true;
    for (int i = 4; i < 8; ++i) if (e[i]) { ShowWindow(e[i], SW_SHOWNOACTIVATE); excluded_ = excluded_ && excludeFromCapture(e[i]); }
    dimmed_ = false;
    return true;
}
void AreaFrame::setDimmed(bool dimmed) {
    if (dimmed == dimmed_) return;
    dimmed_ = dimmed;
    auto& e = edges_;
    for (int i = 0; i < 4; ++i) if (e[i]) ShowWindow(e[i], dimmed ? SW_SHOWNOACTIVATE : SW_HIDE);
    // The ring stays above the dimming.
    for (int i = 4; i < 8; ++i) if (e[i]) SetWindowPos(e[i], HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}
void AreaFrame::hide() {
    auto& e = edges_;
    for (auto& h : e) if (h) { DestroyWindow(h); h = nullptr; }
}

ControlBar::ControlBar(Callbacks callbacks) : callbacks_(std::move(callbacks)) {}
ControlBar::~ControlBar() { destroy(); }
void ControlBar::destroy() { if (hwnd_) { DestroyWindow(hwnd_); hwnd_ = nullptr; } }
bool ControlBar::create(const grab::Rect& area, const std::wstring& target, const std::vector<std::string>& formats, const std::string& format, bool audioAvailable, bool audio) {
    static const bool registered = [] {
        WNDCLASSEXW wc{sizeof wc};
        wc.lpfnWndProc = &ControlBar::proc; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = barClass; wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1));
        return RegisterClassExW(&wc) != 0;
    }();
    if (!registered) return false;
    area_ = area; target_ = target;
    // Sized for the monitor that will hold it, before it exists.
    UINT dpiX = 96, dpiY = 96;
    if (FAILED(GetDpiForMonitor(MonitorFromPoint({area.x + area.w / 2, area.y + area.h / 2}, MONITOR_DEFAULTTONEAREST), MDT_EFFECTIVE_DPI, &dpiX, &dpiY))) dpiX = 96;
    dpi_ = dpiX;
    hwnd_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, barClass, L"put.gd recording controls", WS_POPUP | WS_CLIPCHILDREN, 0, 0, 10, 10, nullptr, nullptr, GetModuleHandleW(nullptr), this);
    if (!hwnd_) return false;
    dpi_ = GetDpiForWindow(hwnd_);
    theme::setSurface(hwnd_, theme::Surface::Card);
    const DWORD corner = 2; // DWMWCP_ROUND
    DwmSetWindowAttribute(hwnd_, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &corner, sizeof corner);
    record_ = ui::button(hwnd_, idRecord, L"Record"); theme::setRole(record_, theme::Role::Primary, "record");
    pause_ = ui::button(hwnd_, idPause, L"Pause"); theme::setRole(pause_, theme::Role::Standard, "pause");
    stop_ = ui::button(hwnd_, idStop, L"Stop"); theme::setRole(stop_, theme::Role::Danger, "stop");
    cancel_ = ui::button(hwnd_, idCancel, L"Cancel"); theme::setRole(cancel_, theme::Role::Standard);
    // Before Record: the export format and the computer's playback; locked from Record on.
    format_ = ui::combo(hwnd_, idFormat);
    std::vector<ui::Choice> choices;
    for (const auto& key : formats) choices.push_back({ui::wide(recording::formatLabel(key)), key});
    ui::setChoices(format_, choices);
    ui::select(format_, format);
    theme::styleCombo(format_);
    audio_ = ui::check(hwnd_, idAudio, L"System audio");
    audioAvailable_ = audioAvailable;
    ui::setChecked(audio_, audio && audioAvailable && format != "gif");
    tooltip_ = ui::createTooltip(hwnd_);
    ui::setTooltip(tooltip_, record_, L"Start recording");
    ui::setTooltip(tooltip_, pause_, L"Pause or resume the recording");
    ui::setTooltip(tooltip_, stop_, L"Stop and review the recording (or press the recording shortcut)");
    ui::setTooltip(tooltip_, cancel_, L"Discard this recording");
    ui::setTooltip(tooltip_, format_, L"Output format; GIF is silent. It can still change in the review.");
    ui::setTooltip(tooltip_, audio_, audioAvailable ? L"Record what the computer plays" : L"No audio output device to record from");
    for (HWND b : {record_, pause_, stop_, cancel_, format_, audio_}) ui::setFont(b, ui::FontRole::Normal, dpi_);
    excluded_ = excludeFromCapture(hwnd_);
    theme::applyTo(hwnd_);
    setState("ready");
    place();
    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    SetTimer(hwnd_, clockTimer, 100, nullptr);
    return true;
}
void ControlBar::setChoice(const std::string& format, bool audio) {
    if (!hwnd_) return;
    ui::select(format_, format);
    ui::setChecked(audio_, audio && audioAvailable_ && format != "gif");
    InvalidateRect(format_, nullptr, FALSE);
}
void ControlBar::setState(const std::string& state) {
    state_ = state;
    if (!hwnd_) return;
    const bool ready = state == "ready", stopping = state == "stopping";
    ui::show(record_, ready);
    ui::show(pause_, !ready);
    ui::show(stop_, !ready);
    ui::setTextW(pause_, state == "paused" ? L"Resume" : L"Pause");
    theme::setRole(pause_, theme::Role::Standard, state == "paused" ? "play" : "pause");
    for (HWND b : {record_, pause_, stop_, cancel_}) ui::enable(b, !stopping);
    ui::enable(format_, ready && SendMessageW(format_, CB_GETCOUNT, 0, 0) > 1);
    ui::enable(audio_, ready && audioAvailable_ && ui::selected(format_) != "gif");
    layout();
    InvalidateRect(hwnd_, nullptr, TRUE);
}
void ControlBar::setVisible(bool visible) { hidden_ = !visible; if (hwnd_) ShowWindow(hwnd_, visible ? SW_SHOWNOACTIVATE : SW_HIDE); }
void ControlBar::raise() { if (hwnd_ && !hidden_) SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW); }

// Dot, clock, target, then the buttons; the bar is sized for its widest state.
void ControlBar::layout() {
    const int pad = ui::scale(10, dpi_), gap = ui::scale(8, dpi_), height = ui::scale(44, dpi_), buttonHeight = ui::scale(30, dpi_);
    HDC dc = GetDC(hwnd_);
    HGDIOBJ old = SelectObject(dc, ui::font(ui::FontRole::Normal, dpi_));
    SIZE target{}; GetTextExtentPoint32W(dc, target_.c_str(), int(target_.size()), &target);
    SIZE clock{}; GetTextExtentPoint32W(dc, L"00:00.0", 7, &clock);
    SelectObject(dc, old); ReleaseDC(hwnd_, dc);
    int x = pad + ui::scale(12, dpi_) + gap + clock.cx + gap * 2 + target.cx + gap * 2;
    const int y = (height - buttonHeight) / 2;
    const int formatWidth = ui::scale(120, dpi_), audioWidth = ui::buttonWidth(audio_, 60) + ui::scale(36, dpi_); // the box, its gap and the focus rectangle
    ui::place(format_, x, y, formatWidth, buttonHeight); x += formatWidth + gap;
    MoveWindow(audio_, x, y, audioWidth, buttonHeight, TRUE); x += audioWidth + gap;
    const int choices = formatWidth + audioWidth + 2 * gap;
    // Buttons with a glyph need room for it beside the text (16 px and a 6 px gap).
    auto width = [&](HWND b) { return ui::buttonWidth(b, 72) + (b == cancel_ ? 0 : ui::scale(22, dpi_)); };
    auto put = [&](HWND b) { if (!ui::visible(b)) return; const int w = width(b); MoveWindow(b, x, y, w, buttonHeight, TRUE); x += w + gap; };
    put(record_); put(pause_); put(stop_); put(cancel_);
    // Sized for the widest state, so the bar never runs past its screen edge after a state change.
    const std::wstring before = ui::textW(pause_);
    ui::setTextW(pause_, L"Resume");
    const int pauseWidth = width(pause_);
    ui::setTextW(pause_, before);
    const int widest = pad + ui::scale(12, dpi_) + gap + clock.cx + gap * 2 + target.cx + gap * 2 + choices + pauseWidth + width(stop_) + width(cancel_) + gap * 2 + pad;
    RECT r; GetWindowRect(hwnd_, &r);
    if (r.right - r.left != widest || r.bottom - r.top != height) SetWindowPos(hwnd_, nullptr, 0, 0, widest, height, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}
void ControlBar::place() {
    RECT r; GetWindowRect(hwnd_, &r);
    std::vector<RecordingRect> screens;
    for (const auto& m : grab::monitors()) screens.push_back({m.x, m.y, m.w, m.h});
    const auto spot = recording::placeBar(r.right - r.left, r.bottom - r.top, {area_.x, area_.y, area_.w, area_.h}, screens, true, ui::scale(10, dpi_));
    outside_ = spot.outside;
    SetWindowPos(hwnd_, HWND_TOPMOST, int(spot.rect.x), int(spot.rect.y), 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
}
void ControlBar::paint(HDC dc) {
    RECT client; GetClientRect(hwnd_, &client);
    const auto& c = theme::colors();
    theme::fill(dc, client, c.card);
    RECT border = client;
    FrameRect(dc, &border, theme::brush(c.cardBorder));
    const int pad = ui::scale(10, dpi_), gap = ui::scale(8, dpi_), dot = ui::scale(12, dpi_);
    const int mid = (client.bottom - client.top) / 2;
    // The state dot: green ready, red recording, amber paused.
    const COLORREF dotColor = state_ == "ready" ? c.success : state_ == "paused" ? c.warning : c.danger;
    theme::roundRect(dc, {pad, mid - dot / 2, pad + dot, mid + dot / 2}, dot / 2, dotColor);
    HFONT font = ui::font(ui::FontRole::Normal, dpi_);
    const long long elapsed = callbacks_.elapsedUs && state_ != "ready" ? callbacks_.elapsedUs() : 0;
    const auto clock = ui::wide(recording::clockText(elapsed, true));
    SIZE clockSize{};
    HGDIOBJ old = SelectObject(dc, font); GetTextExtentPoint32W(dc, L"00:00.0", 7, &clockSize); SelectObject(dc, old);
    int x = pad + dot + gap;
    theme::drawText(dc, clock, {x, 0, x + clockSize.cx, client.bottom}, c.text, font, DT_SINGLELINE | DT_VCENTER | DT_LEFT);
    x += clockSize.cx + gap * 2;
    theme::drawText(dc, target_, {x, 0, client.right, client.bottom}, c.textSecondary, font, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX);
}
LRESULT CALLBACK ControlBar::proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    ControlBar* self = nullptr;
    if (message == WM_NCCREATE) { self = static_cast<ControlBar*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams); SetWindowLongPtrW(hwnd, GWLP_USERDATA, LONG_PTR(self)); self->hwnd_ = hwnd; }
    else self = reinterpret_cast<ControlBar*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    return self ? self->handle(message, wParam, lParam) : DefWindowProcW(hwnd, message, wParam, lParam);
}
LRESULT ControlBar::handle(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_NCHITTEST: {
        // The background drags the bar; buttons are their own windows.
        const LRESULT hit = DefWindowProcW(hwnd_, message, wParam, lParam);
        return hit == HTCLIENT ? HTCAPTION : hit;
    }
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_EXITSIZEMOVE: {
        // A bar placed outside the area stays outside it.
        RECT r; GetWindowRect(hwnd_, &r);
        RECT area{area_.x, area_.y, area_.x + area_.w, area_.y + area_.h}, overlap;
        if (outside_ && IntersectRect(&overlap, &r, &area)) place();
        moved_ = true;
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd_, &ps);
        RECT client; GetClientRect(hwnd_, &client);
        HDC memory = CreateCompatibleDC(dc);
        HBITMAP bitmap = CreateCompatibleBitmap(dc, client.right, client.bottom);
        HGDIOBJ old = SelectObject(memory, bitmap);
        paint(memory);
        BitBlt(dc, 0, 0, client.right, client.bottom, memory, 0, 0, SRCCOPY);
        SelectObject(memory, old); DeleteObject(bitmap); DeleteDC(memory);
        EndPaint(hwnd_, &ps);
        return 0;
    }
    case WM_TIMER:
        if (wParam == clockTimer && state_ == "recording") {
            RECT clock{0, 0, ui::scale(110, dpi_), ui::scale(44, dpi_)};
            InvalidateRect(hwnd_, &clock, FALSE);
        }
        return 0;
    case WM_NOTIFY: { LRESULT result = 0; if (theme::customDraw(reinterpret_cast<NMHDR*>(lParam), result)) return result; break; }
    case WM_CTLCOLORBTN: case WM_CTLCOLORSTATIC: case WM_CTLCOLORLISTBOX: return theme::controlColor(message, reinterpret_cast<HDC>(wParam), reinterpret_cast<HWND>(lParam), 0);
    case WM_COMMAND:
        if (HIWORD(wParam) == BN_CLICKED) {
            const int id = LOWORD(wParam);
            if (id == idRecord && callbacks_.record) callbacks_.record();
            else if (id == idPause) { if (state_ == "paused") { if (callbacks_.resume) callbacks_.resume(); } else if (callbacks_.pause) callbacks_.pause(); }
            else if (id == idStop && callbacks_.stop) callbacks_.stop();
            else if (id == idCancel && callbacks_.cancel) callbacks_.cancel();
            else if (id == idAudio && callbacks_.audioChanged) callbacks_.audioChanged(ui::checked(audio_));
            return 0;
        }
        if (HIWORD(wParam) == CBN_SELCHANGE && LOWORD(wParam) == idFormat) {
            const auto key = ui::selected(format_);
            // GIF is silent.
            if (key == "gif") { ui::setChecked(audio_, false); if (callbacks_.audioChanged) callbacks_.audioChanged(false); }
            ui::enable(audio_, audioAvailable_ && key != "gif");
            if (callbacks_.formatChanged) callbacks_.formatChanged(key);
            return 0;
        }
        break;
    case WM_DPICHANGED: {
        dpi_ = HIWORD(wParam);
        for (HWND b : {record_, pause_, stop_, cancel_, format_, audio_}) ui::setFont(b, ui::FontRole::Normal, dpi_);
        layout();
        return 0;
    }
    case WM_NCDESTROY: hwnd_ = nullptr; break;
    }
    return DefWindowProcW(hwnd_, message, wParam, lParam);
}
}
