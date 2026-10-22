#include "ui/MainWindow.h"
#include "ui/Theme.h"
#include "common/Log.h"
#include "common/Paths.h"
#include "core/Settings.h"
#include "core/SettingsPatch.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <shellapi.h>

namespace putgd::ui {
namespace {
constexpr wchar_t windowClass[] = L"putgd.Main";
constexpr int idRailFirst = 200, idAbout = 220, idQuit = 221, idNoticeButton = 222, idNavFirst = 230, idStatus = 236, idSaveStatus = 237;
constexpr int idEditClipboard = 240, idEditFile = 241;
constexpr UINT_PTR timerHeartbeat = 1, timerAutosave = 2, timerClose = 3, timerCloseSafety = 4, timerCheckpoint = 5;
constexpr int checkpointDelayMs = 250;
constexpr int noticeNone = 0, noticeReload = 1, noticeRetry = 2;
const std::vector<std::string> railActions{"region", "edit", "window", "desktop", "clipboard"};
const wchar_t* pageTitles[] = {L"History", L"Destinations", L"Settings"};
const char* pageGlyphs[] = {"history", "destinations", "settings"};
// The brand mark's two tones (resident/app.ico).
constexpr COLORREF brandArrow = RGB(176, 204, 255), brandTray = RGB(110, 150, 232);

bool screenAction(const std::string& action) { return action == "region" || action == "edit" || action == "window" || action == "desktop"; }

std::wstring outputText(const json::Object& document, const std::string& action) {
    const auto saved = Settings::effectiveAction(document, action);
    return MainWindow::routeText(document, saved.value("destination") == "upload", saved.value("profileId").toString(), saved.value("publicationMode").toString("default"));
}
std::string hexColor(COLORREF color) {
    char text[8];
    std::snprintf(text, sizeof text, "#%02x%02x%02x", GetRValue(color), GetGValue(color), GetBValue(color));
    return text;
}
std::wstring explanation(const std::string& action) {
    if (action == "region") return L"Drag to select an area of your screen. Press Escape to cancel.";
    if (action == "edit") return L"Select a region, then redact, blur, crop or annotate it. Nothing uploads until you choose Upload.\nRight-click to edit the clipboard image or an image file instead, or drop a PNG or JPEG on this button.";
    if (action == "window") return L"Capture the focused window of another app.";
    if (action == "desktop") return L"Capture all monitors.";
    return L"Upload copied files, an image, or plain text, in that order. Text URLs are uploaded as text.";
}
LRESULT CALLBACK editButtonProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR data) {
    if (message == WM_DROPFILES) {
        auto* main = reinterpret_cast<MainWindow*>(data);
        HDROP drop = reinterpret_cast<HDROP>(wParam);
        std::string path;
        if (DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0) == 1) {
            const UINT length = DragQueryFileW(drop, 0, nullptr, 0);
            std::wstring name(length + 1, L'\0');
            DragQueryFileW(drop, 0, name.data(), length + 1);
            name.resize(length);
            path = narrow(name);
        }
        DragFinish(drop);
        const auto lower = [](std::string s) { for (auto& c : s) c = char(tolower(static_cast<unsigned char>(c))); return s; }(path);
        const bool image = lower.size() > 4 && (lower.ends_with(".png") || lower.ends_with(".jpg") || lower.ends_with(".jpeg"));
        if (image) main->openEditor("file", path);
        else main->status(L"Only one PNG or JPEG image can be edited.", true);
        return 0;
    }
    return DefSubclassProc(hwnd, message, wParam, lParam);
}
// About put.gd: a small owned window in the app's theme (TaskDialog cannot follow dark mode).
struct About {
    HWND hwnd = nullptr, owner = nullptr, ok = nullptr, link = nullptr;
    std::wstring version, commit;
    static constexpr int idLink = 10;
    static LRESULT CALLBACK proc(HWND h, UINT message, WPARAM wParam, LPARAM lParam) {
        auto* self = reinterpret_cast<About*>(GetWindowLongPtrW(h, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            self = static_cast<About*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
            SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            self->hwnd = h;
        }
        if (!self) return DefWindowProcW(h, message, wParam, lParam);
        switch (message) {
        case WM_COMMAND:
            if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL) { SendMessageW(h, WM_CLOSE, 0, 0); return 0; }
            if (LOWORD(wParam) == idLink) { openUrl("https://put.gd"); return 0; }
            break;
        case WM_NOTIFY: {
            LRESULT result = 0;
            if (theme::customDraw(reinterpret_cast<NMHDR*>(lParam), result)) return result;
            break;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
            self->paint();
            return 0;
        case WM_CLOSE:
            if (self->owner) EnableWindow(self->owner, TRUE);
            DestroyWindow(h);
            return 0;
        }
        return DefWindowProcW(h, message, wParam, lParam);
    }
    int footer() const { return scale(68, windowDpi(hwnd)); }
    void paint() {
        PAINTSTRUCT ps;
        HDC target = BeginPaint(hwnd, &ps);
        RECT client{};
        GetClientRect(hwnd, &client);
        HDC dc = CreateCompatibleDC(target);
        HBITMAP bitmap = CreateCompatibleBitmap(target, client.right, client.bottom);
        HGDIOBJ old = SelectObject(dc, bitmap);
        const auto& c = theme::colors();
        const UINT d = windowDpi(hwnd);
        theme::fill(dc, client, c.content);
        theme::fill(dc, {0, client.bottom - footer(), client.right, client.bottom}, c.rail);
        theme::fill(dc, {0, client.bottom - footer(), client.right, client.bottom - footer() + 1}, c.divider);
        const int icon = scale(56, d), left = scale(28, d), text = left + icon + scale(18, d);
        theme::drawGlyph(dc, "brand", left, scale(28, d), icon, brandArrow, brandTray);
        const int titleHeight = fontHeight(font(FontRole::Title, d));
        theme::drawText(dc, L"put.gd", {text, scale(22, d), client.right - left, scale(22, d) + titleHeight}, c.text, font(FontRole::Title, d), DT_SINGLELINE | DT_LEFT | DT_NOPREFIX);
        int y = scale(22, d) + titleHeight + scale(2, d);
        theme::drawText(dc, version, {text, y, client.right - left, y + lineHeight(d)}, c.textSecondary, font(FontRole::Normal, d), DT_SINGLELINE | DT_LEFT | DT_NOPREFIX);
        y += lineHeight(d) + scale(2, d);
        if (!commit.empty()) theme::drawText(dc, commit, {text, y, client.right - left, y + lineHeight(d)}, c.textMuted, font(FontRole::Normal, d), DT_SINGLELINE | DT_LEFT | DT_NOPREFIX);
        const int body = scale(28, d) + icon + scale(24, d);
        theme::drawText(dc, L"Capture, upload and share screenshots and files.", {left, body, client.right - left, body + lineHeight(d) * 2}, c.text,
                        font(FontRole::Normal, d), DT_WORDBREAK | DT_LEFT | DT_NOPREFIX);
        BitBlt(target, 0, 0, client.right, client.bottom, dc, 0, 0, SRCCOPY);
        SelectObject(dc, old);
        DeleteObject(bitmap);
        DeleteDC(dc);
        EndPaint(hwnd, &ps);
    }
    void run(HWND parent, const std::string& themeSetting) {
        owner = parent;
        static bool registered = false;
        if (!registered) {
            WNDCLASSEXW cls{};
            cls.cbSize = sizeof cls;
            cls.lpfnWndProc = proc;
            cls.hInstance = GetModuleHandleW(nullptr);
            cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            cls.lpszClassName = L"putgd.About";
            RegisterClassExW(&cls);
            registered = true;
        }
        const UINT d = windowDpi(parent);
        const DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN;
        RECT frame{0, 0, scale(440, d), scale(292, d)};
        adjustWindowRectForDpi(frame, style, WS_EX_DLGMODALFRAME, d);
        RECT ownerRect{};
        GetWindowRect(parent, &ownerRect);
        const int width = frame.right - frame.left, height = frame.bottom - frame.top;
        CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT, L"putgd.About", L"About put.gd", style, (ownerRect.left + ownerRect.right - width) / 2,
                        (ownerRect.top + ownerRect.bottom - height) / 2, width, height, parent, nullptr, GetModuleHandleW(nullptr), this);
        if (!hwnd) return;
        theme::setSurface(hwnd, theme::Surface::Content);
        RECT client{};
        GetClientRect(hwnd, &client);
        link = button(hwnd, idLink, L"https://put.gd");
        theme::setRole(link, theme::Role::Link);
        const int left = scale(28, d), body = scale(28, d) + scale(56, d) + scale(24, d) + lineHeight(d) + scale(10, d);
        place(link, left - scale(4, d), body, buttonWidth(link, 60) - scale(24, d), scale(28, d));
        ok = button(hwnd, IDOK, L"OK", BS_DEFPUSHBUTTON);
        theme::setRole(ok, theme::Role::Primary);
        const int okWidth = scale(120, d), okHeight = scale(32, d);
        place(ok, client.right - left - okWidth, client.bottom - footer() + (footer() - okHeight) / 2, okWidth, okHeight);
        applyTitleBarTheme(hwnd, themeSetting);
        theme::applyTo(hwnd);
        EnableWindow(parent, FALSE);
        ShowWindow(hwnd, SW_SHOW);
        SetFocus(ok);
        MSG msg{};
        while (IsWindow(hwnd)) {
            const BOOL got = GetMessageW(&msg, nullptr, 0, 0);
            if (got == 0) { PostQuitMessage(int(msg.wParam)); break; }
            if (got == -1) break;
            if (IsDialogMessageW(hwnd, &msg)) continue;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (IsWindow(parent)) { EnableWindow(parent, TRUE); SetActiveWindow(parent); }
    }
};
}

MainWindow::MainWindow(UiSession& session, frames::Channel& channel) : session_(session), channel_(channel) {
    editImage_.post = [this](std::function<void()> work) { post(std::move(work)); };
    pages_[HistoryPage] = createHistoryPage(*this);
    pages_[DestinationsPage] = createDestinationsPage(*this);
    pages_[SettingsPage] = createSettingsPage(*this);
}
MainWindow::~MainWindow() { session_.detach(); }

std::wstring MainWindow::actionLabel(const std::string& action) {
    if (action == "region") return L"Capture region";
    if (action == "edit") return L"Capture region & edit";
    if (action == "window") return L"Capture window";
    if (action == "desktop") return L"Capture desktop";
    if (action == "clipboard") return L"Upload clipboard";
    if (action == "record") return L"Record screen";
    return wide(action);
}

std::wstring MainWindow::routeText(const json::Object& document, bool upload, const std::string& profileId, const std::string& publicationMode) {
    const bool local = document.value("archive")["keepUploads"].toBool();
    if (!upload || profileId.empty())
        return local ? L"Saves a local copy using your folders in Settings > General. Uploads are off."
                     : L"This action will fail: no destination and local copies are off. Choose a destination in Settings > Capture or turn on Keep a local copy in Settings > General.";
    std::wstring label;
    for (const auto& entry : document.value("profiles").arrayRef())
        if (entry.objectRef().value("id") == profileId.c_str()) label = wide(entry.objectRef().value("label").toString());
    if (label.empty()) return L"Choose an upload destination in Settings > Capture before using this action.";
    std::wstring text = publicationMode == "private" ? L"Private uploads are not supported by this destination. Choose sharing in Settings > Capture."
                                                     : L"Uploads to " + label + L" and copies its link to the clipboard.";
    if (local) text += L" Also saves a local copy.";
    return text;
}

bool MainWindow::create(const std::string& initialView, const json::Object& state) {
    WNDCLASSEXW cls{};
    cls.cbSize = sizeof cls;
    cls.lpfnWndProc = proc;
    cls.hInstance = GetModuleHandleW(nullptr);
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    cls.hbrBackground = nullptr;
    cls.lpszClassName = windowClass;
    cls.hIcon = static_cast<HICON>(LoadImageW(cls.hInstance, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR));
    cls.hIconSm = static_cast<HICON>(LoadImageW(cls.hInstance, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
    RegisterClassExW(&cls);

    const UINT dpi = systemDpi();
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const int width = std::min(scale(1240, dpi), int(work.right - work.left)), height = std::min(scale(780, dpi), int(work.bottom - work.top));
    hwnd_ = CreateWindowExW(WS_EX_CONTROLPARENT, windowClass, L"put.gd", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                            work.left + (work.right - work.left - width) / 2, work.top + (work.bottom - work.top - height) / 2, width, height,
                            nullptr, nullptr, cls.hInstance, this);
    if (!hwnd_) return false;
    session_.attach(hwnd_);
    tooltip_ = createTooltip(hwnd_);

    theme::setSurface(hwnd_, theme::Surface::Rail);
    const UINT d = this->dpi();
    for (int i = 0; i < 3; ++i) {
        nav_[size_t(i)] = button(hwnd_, idNavFirst + i, pageTitles[i]);
        theme::setRole(nav_[size_t(i)], theme::Role::Nav, pageGlyphs[i]);
    }
    railHeading_ = ui::label(hwnd_, L"Capture & upload");
    setFont(railHeading_, FontRole::SmallBold, d);
    for (size_t i = 0; i < railActions.size(); ++i) {
        auto caption = actionLabel(railActions[i]);
        if (railActions[i] == "edit") caption = L"Capture region && edit";
        HWND b = button(hwnd_, idRailFirst + int(i), caption);
        theme::setRole(b, theme::Role::Action, railActions[i] == "edit" ? "edit" : railActions[i] == "region" ? "region" : railActions[i] == "window" ? "window"
                                                 : railActions[i] == "desktop" ? "desktop" : "clipboard");
        rail_[railActions[i]] = b;
        if (railActions[i] == "edit") {
            DragAcceptFiles(b, TRUE);
            SetWindowSubclass(b, editButtonProc, 1, reinterpret_cast<DWORD_PTR>(this));
        }
    }
    version_ = ui::label(hwnd_, L"");
    setFont(version_, FontRole::Small, d);
    about_ = button(hwnd_, idAbout, L"About put.gd");
    theme::setRole(about_, theme::Role::Action, "info");
    quit_ = button(hwnd_, idQuit, L"Quit put.gd");
    theme::setRole(quit_, theme::Role::Action, "power");
    setTooltip(tooltip_, quit_, L"Exit put.gd, its tray icon and shortcuts. Pending work is cancelled.");
    notice_ = ui::label(hwnd_, L"", 0, SS_CENTERIMAGE | SS_ENDELLIPSIS);
    theme::setSurface(notice_, theme::Surface::Notice);
    noticeButton_ = button(hwnd_, idNoticeButton, L"Reload");
    theme::setSurface(noticeButton_, theme::Surface::Notice);
    show(notice_, false);
    show(noticeButton_, false);
    status_ = ui::label(hwnd_, L"", idStatus, SS_ENDELLIPSIS | SS_NOTIFY);
    theme::setSurface(status_, theme::Surface::Content);
    saveStatus_ = ui::label(hwnd_, L"", idSaveStatus, SS_RIGHT | SS_ENDELLIPSIS);
    theme::setSurface(saveStatus_, theme::Surface::Content);
    for (size_t i = 0; i < pages_.size(); ++i) {
        pageWindows_[i] = pages_[i]->create(hwnd_);
        show(pageWindows_[i], false);
    }
    const auto version = session_.build().value("version").toString();
    setText(version_, version.empty() ? std::string() : "Version " + version);
    updateRail();
    updateSaveStatus();
    applyTheme();
    restore(state);
    view_ = initialView;
    setPage(page_);
    layout();
    return true;
}

void MainWindow::restore(const json::Object& state) {
    const auto main = state.value("main").toObject();
    const auto geometry = main.value("geometry").toArray();
    if (geometry.size() == 4) {
        RECT rect{geometry[0].toInt(), geometry[1].toInt(), geometry[0].toInt() + geometry[2].toInt(), geometry[1].toInt() + geometry[3].toInt()};
        if (rect.right - rect.left >= 400 && rect.bottom - rect.top >= 300 && MonitorFromRect(&rect, MONITOR_DEFAULTTONULL))
            SetWindowPos(hwnd_, nullptr, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    if (main.value("page").isNumber()) page_ = std::clamp(main.value("page").toInt(), 0, 2);
    pages_[HistoryPage]->restoreState(state.value("history").toObject());
    pages_[SettingsPage]->restoreState(state.value("settings").toObject());
    pages_[DestinationsPage]->restoreState(state.value("settings").toObject());
}

json::Object MainWindow::checkpoint() const {
    WINDOWPLACEMENT placement{};
    placement.length = sizeof placement;
    GetWindowPlacement(hwnd_, &placement);
    RECT rect = placement.rcNormalPosition;
    if (!IsIconic(hwnd_) && !IsZoomed(hwnd_)) GetWindowRect(hwnd_, &rect);
    json::Object settings = pages_[SettingsPage]->saveState();
    for (const auto& [key, value] : pages_[DestinationsPage]->saveState()) settings[key] = value;
    json::Object state{{"schemaVersion", 1},
                       {"main", json::Object{{"page", page_}, {"geometry", json::Array{int(rect.left), int(rect.top), int(rect.right - rect.left), int(rect.bottom - rect.top)}}}},
                       {"appearance", appearance()}};
    const auto history = pages_[HistoryPage]->saveState();
    if (!history.empty()) state["history"] = history;
    if (!settings.empty()) state["settings"] = settings;
    return state;
}

namespace {
struct Metrics {
    int rail, item, itemGap, margin, titleTop, titleHeight, top, footer, noticeHeight;
};
Metrics metrics(UINT d) {
    Metrics m{};
    m.rail = scale(248, d);
    m.item = scale(36, d);
    m.itemGap = scale(3, d);
    m.margin = scale(24, d);
    m.titleTop = scale(18, d);
    m.titleHeight = fontHeight(font(FontRole::Title, d));
    m.top = m.titleTop + m.titleHeight + scale(10, d);
    m.footer = scale(34, d);
    m.noticeHeight = scale(44, d);
    return m;
}
}

void MainWindow::layout() {
    if (!hwnd_ || !nav_[0]) return;
    RECT client{};
    GetClientRect(hwnd_, &client);
    const UINT d = dpi();
    const auto m = metrics(d);
    const int itemX = scale(6, d), itemWidth = m.rail - 2 * itemX;
    int y = scale(64, d);
    for (HWND item : nav_) {
        place(item, itemX, y, itemWidth, m.item);
        y += m.item + m.itemGap;
    }
    y += scale(18, d);
    const int small = fontHeight(font(FontRole::SmallBold, d));
    place(railHeading_, scale(18, d), y, itemWidth - scale(12, d), small + scale(2, d));
    y += small + scale(8, d);
    for (const auto& action : railActions) {
        place(rail_[action], itemX, y, itemWidth, m.item);
        y += m.item + m.itemGap;
    }
    const int versionHeight = fontHeight(font(FontRole::Small, d)) + scale(2, d);
    int bottom = client.bottom - scale(14, d) - versionHeight;
    place(version_, scale(18, d), bottom, itemWidth - scale(12, d), versionHeight);
    bottom -= scale(8, d) + m.item;
    place(quit_, itemX, bottom, itemWidth, m.item);
    bottom -= m.itemGap + m.item;
    place(about_, itemX, bottom, itemWidth, m.item);

    const int left = m.rail + m.margin, right = client.right - m.margin;
    int top = m.top;
    if (visible(notice_)) {
        const int buttonHeight = scale(30, d), buttonW = buttonWidth(noticeButton_, 96);
        place(noticeButton_, right - scale(8, d) - buttonW, top + (m.noticeHeight - buttonHeight) / 2, buttonW, buttonHeight);
        place(notice_, left + scale(42, d), top + scale(4, d), right - left - scale(42, d) - buttonW - scale(20, d), m.noticeHeight - scale(8, d));
        top += m.noticeHeight + scale(12, d);
    }
    const int footerTop = client.bottom - m.footer;
    const int line = lineHeight(d) + scale(2, d), saveWidth = scale(240, d);
    place(saveStatus_, right - saveWidth, footerTop + (m.footer - line) / 2, saveWidth, line);
    place(status_, left, footerTop + (m.footer - line) / 2, std::max(0, right - saveWidth - scale(16, d) - left), line);
    for (HWND page : pageWindows_)
        SetWindowPos(page, HWND_TOP, m.rail + 1, top, std::max(0, int(client.right) - m.rail - 1), std::max(0, footerTop - top - scale(4, d)), SWP_NOACTIVATE);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::paint() {
    PAINTSTRUCT ps;
    HDC target = BeginPaint(hwnd_, &ps);
    RECT client{};
    GetClientRect(hwnd_, &client);
    if (client.right > 0 && client.bottom > 0) {
        HDC dc = CreateCompatibleDC(target);
        HBITMAP bitmap = CreateCompatibleBitmap(target, client.right, client.bottom);
        HGDIOBJ old = SelectObject(dc, bitmap);
        const auto& c = theme::colors();
        const UINT d = dpi();
        const auto m = metrics(d);
        theme::fill(dc, client, c.rail);
        // The content layer: a rounded sheet beside the rail, as in Windows 11.
        const int radius = scale(6, d);
        theme::roundRect(dc, {m.rail, 0, client.right + 2 * radius, client.bottom + 2 * radius}, radius, c.content, c.cardBorder);
        // Brand.
        const int icon = scale(24, d);
        theme::drawGlyph(dc, "brand", scale(18, d), scale(20, d), icon, brandArrow, brandTray);
        theme::drawText(dc, L"put.gd", {scale(18, d) + icon + scale(12, d), scale(16, d), m.rail, scale(16, d) + icon + scale(8, d)}, c.text, font(FontRole::Heading, d),
                        DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX);
        // Page title.
        const int left = m.rail + m.margin, right = client.right - m.margin;
        theme::drawText(dc, pageTitles[std::clamp(page_, 0, 2)], {left, m.titleTop, right, m.titleTop + m.titleHeight}, c.text, font(FontRole::Title, d),
                        DT_SINGLELINE | DT_LEFT | DT_NOPREFIX);
        // Notice (Windows 11 InfoBar).
        if (visible(notice_)) {
            RECT bar{left, m.top, right, m.top + m.noticeHeight};
            theme::roundRect(dc, bar, scale(6, d), c.notice, c.noticeBorder);
            const int size = scale(18, d);
            theme::drawGlyph(dc, "warning", left + scale(14, d), m.top + (m.noticeHeight - size) / 2, size, c.warning, c.notice);
        }
        // Footer divider.
        const int footerTop = client.bottom - m.footer;
        theme::fill(dc, {m.rail + 1, footerTop, client.right, footerTop + 1}, c.divider);
        BitBlt(target, ps.rcPaint.left, ps.rcPaint.top, ps.rcPaint.right - ps.rcPaint.left, ps.rcPaint.bottom - ps.rcPaint.top, dc, ps.rcPaint.left, ps.rcPaint.top, SRCCOPY);
        SelectObject(dc, old);
        DeleteObject(bitmap);
        DeleteDC(dc);
    }
    EndPaint(hwnd_, &ps);
}

// The appearance in use (docs/ui-process-state.md): the setting, the Windows app
// mode, the resolved scheme and its colour tokens. Derived; restoring ignores it.
json::Object MainWindow::appearance() const {
    const auto& c = theme::colors();
    const std::pair<const char*, COLORREF> tokens[] = {
        {"rail", c.rail}, {"content", c.content}, {"card", c.card}, {"cardBorder", c.cardBorder}, {"input", c.input}, {"placeholder", c.placeholder},
        {"notice", c.notice}, {"hover", c.hover}, {"selected", c.selected}, {"divider", c.divider}, {"button", c.button}, {"buttonBorder", c.buttonBorder},
        {"text", c.text}, {"textSecondary", c.textSecondary}, {"textMuted", c.textMuted}, {"textDisabled", c.textDisabled}, {"accent", c.accent},
        {"onAccent", c.onAccent}, {"link", c.link}, {"danger", c.danger}, {"warning", c.warning}, {"success", c.success}, {"icon", c.icon}};
    json::Object colours;
    for (const auto& [name, colour] : tokens) colours[name] = hexColor(colour);
    return {{"theme", session_.document().value("preferences")["appearance"]["theme"].toString("system")}, {"desktop", systemPrefersDark() ? "dark" : "light"},
            {"desktopSource", "AppsUseLightTheme"}, {"scheme", theme::dark() ? "dark" : "light"}, {"tokens", colours}};
}

void MainWindow::viewStateChanged() {
    if (hwnd_ && activated_ && !closing_ && !exiting_) SetTimer(hwnd_, timerCheckpoint, checkpointDelayMs, nullptr);
}

void MainWindow::sendCheckpoint() {
    KillTimer(hwnd_, timerCheckpoint);
    if (closing_ || exiting_ || !activated_) return;
    const auto state = checkpoint();
    auto bytes = json::serialize(state);
    if (bytes == lastCheckpoint_ || bytes.size() > protocol::maximumUiStateBytes) return;
    lastCheckpoint_ = std::move(bytes);
    send("checkpoint", {{"state", state}});
}

void MainWindow::handleCommand(const json::Object& payload) {
    const auto requestId = payload.value("requestId");
    const auto name = payload.value("command").toString();
    const auto params = payload.value("params").toObject();
    // Answers arrive at most once; a window that is exiting stays silent and the resident times out.
    auto answer = [this, requestId, answered = std::make_shared<bool>(false)](Error error, json::Object result) {
        if (*answered || exiting_) return;
        *answered = true;
        send("commandResult", {{"requestId", requestId}, {"error", error.toJson()}, {"result", result}});
    };
    if (payload.value("version") != json::Value(1)) { answer({"ui_command_unsupported", "This window does not support that command version.", {{"version", payload.value("version")}}}, {}); return; }
    if (name != "moveDestination" && name != "diagnostics" && name != "historySaveDiagnostics" && name != "fileDialog") { answer({"ui_command_unknown", "This window does not support that command.", {}}, {}); return; }
    if (!IsWindowVisible(hwnd_) || closing_ || hiddenForCapture_) {
        answer({"ui_view_closed", name == "historySaveDiagnostics" ? "Open History before driving it." : name == "fileDialog" ? "Open put.gd's window (History or Settings) before driving it." : "Open Settings before driving it.", {}}, {});
        return;
    }
    if (name == "fileDialog" && params.value("dialog") == "editImage") {
        // #259: Edit an image file…, as the rail's Edit menu opens it.
        const auto action = params.value("action").toString();
        auto withDialog = [answer](Error error, json::Object result) { if (!error) result["dialog"] = "editImage"; answer(error, result); };
        if (action == "open") {
            if (editImage_.isOpen()) { withDialog({"file_dialog_open", "Edit an image file is already open.", {}}, {}); return; }
            if (FileChooser::anyOpen()) { withDialog({"file_dialog_open", "Another file chooser is open in this window.", {}}, {}); return; }
            chooseImageToEdit();
        } else if (action == "cancel") {
            editImage_.cancel();
        }
        withDialog({}, editImage_.state());
        return;
    }
    for (auto& page : pages_) if (page->viewCommand(name, params, answer)) return;
    answer({"ui_command_unknown", "This window does not support that command.", {}}, {});
}

void MainWindow::setPage(int index) {
    index = std::clamp(index, 0, 2);
    const int previous = page_;
    page_ = index;
    for (size_t i = 0; i < nav_.size(); ++i) theme::setSelected(nav_[i], int(i) == index);
    for (size_t i = 0; i < pageWindows_.size(); ++i) ShowWindow(pageWindows_[i], int(i) == index ? SW_SHOW : SW_HIDE);
    InvalidateRect(hwnd_, nullptr, FALSE);
    if (previous != index) pages_[size_t(previous)]->deactivated();
    pages_[size_t(index)]->activated();
    viewStateChanged();
}

void MainWindow::showView(const std::string& view) {
    view_ = view;
    if (view == "settings" || view == "updates") {
        setPage(SettingsPage);
        pages_[SettingsPage]->showSection(view == "updates" ? "updates" : "");
    } else if (view == "destinations") {
        setPage(DestinationsPage);
    } else if (view == "history" || view == "activity") {
        setPage(HistoryPage);
        pages_[HistoryPage]->showSection(view);
    }
    if (!hiddenForCapture_) {
        ShowWindow(hwnd_, IsIconic(hwnd_) ? SW_RESTORE : SW_SHOW);
        SetForegroundWindow(hwnd_);
    }
    if (!activated_) {
        activated_ = true;
        send("heartbeat");
        SetTimer(hwnd_, timerHeartbeat, protocol::heartbeatMs, nullptr);
    }
    viewStateChanged();
    send("viewShown", {{"view", view}});
    if (view == "about") post([this] { showAbout(); });
    if (view == "editFile") post([this] { chooseImageToEdit(); });
}

bool MainWindow::send(const std::string& type, const json::Object& payload) {
    if (channel_.send(type, payload)) return true;
    PostMessageW(hwnd_, controlClosedMessage, 0, 0);
    return false;
}

void MainWindow::post(std::function<void()> work) {
    auto* task = new std::function<void()>(std::move(work));
    if (!PostMessageW(hwnd_, deferredMessage, 0, reinterpret_cast<LPARAM>(task))) delete task;
}

void MainWindow::onFrame(const protocol::Frame& frame) {
    const auto& payload = frame.payload;
    if (frame.type == "activate") {
        if (closing_) return; // the resident re-queues views that cross a close
        showView(payload.value("view").toString(view_.empty() ? std::string("history") : view_));
    } else if (frame.type == "invalidate") {
        const auto kind = payload.value("kind").toString();
        if (kind == "settings") fetchSettings();
        else for (auto& page : pages_) page->invalidated(kind);
    } else if (frame.type == "command") {
        handleCommand(payload);
    } else if (frame.type == "shortcutRecordingChanged") {
        if (recordingReplies_.empty()) return;
        const auto& replyTo = payload.value("replyTo");
        unsigned long long sequence = replyTo.isString() ? std::strtoull(replyTo.stringRef().c_str(), nullptr, 10) : static_cast<unsigned long long>(replyTo.toInteger());
        auto match = std::find_if(recordingReplies_.begin(), recordingReplies_.end(), [sequence](const RecordingReply& r) { return r.sequence == sequence; });
        if (match == recordingReplies_.end()) match = recordingReplies_.begin();
        auto callback = std::move(match->callback);
        recordingReplies_.erase(match);
        const auto error = payload.value("error").toObject().value("code").toString();
        if (callback) callback(error.empty(), error);
    } else if (frame.type == "closeAccepted") {
        if (closing_ && !cancelling_ && static_cast<unsigned long long>(payload.value("sequence").toInteger()) == closeSequence_) finishClose();
    } else if (frame.type == "closeCancelled") {
        if (static_cast<unsigned long long>(payload.value("sequence").toInteger()) != closeSequence_) return;
        KillTimer(hwnd_, timerClose);
        KillTimer(hwnd_, timerCloseSafety);
        closing_ = cancelling_ = false;
        EnableWindow(hwnd_, TRUE);
        if (hiddenForCapture_) { hiddenForCapture_ = false; ShowWindow(hwnd_, SW_SHOW); }
    } else {
        log::debug("ui", "Ignoring control frame", {{"type", frame.type}});
    }
}

// ---- autosave ----------------------------------------------------------------
void MainWindow::queuePatch(const json::Object& patch) {
    pending_ = settings::applyPatch(pending_, patch);
    hasPending_ = true;
    if (!blocked_) SetTimer(hwnd_, timerAutosave, 400, nullptr);
    updateSaveStatus();
}

void MainWindow::flushSettings(std::function<void(bool)> then) {
    if (blocked_) { if (then) then(false); return; }
    if (!hasPending_ && !inFlight_) { if (then) then(true); return; }
    if (then) idle_.push_back(std::move(then));
    KillTimer(hwnd_, timerAutosave);
    sendPending();
}

void MainWindow::sendPending() {
    KillTimer(hwnd_, timerAutosave);
    if (inFlight_ || blocked_ || !hasPending_) return;
    inFlightPatch_ = std::move(pending_);
    pending_ = {};
    hasPending_ = false;
    inFlight_ = true;
    updateSaveStatus();
    session_.call("settings.patch", {{"patch", inFlightPatch_}, {"expectedRevision", session_.revision()}}, [this](const Reply& reply) {
        if (reply.ok) {
            session_.storeSettings(settings::applyPatch(session_.document(), inFlightPatch_), session_.metadata());
            const bool fresh = reply.result.value("observedRevision").isString();
            if (fresh) session_.storeMetadata(reply.result);
            saveText_ = L"Saved";
            if (fresh) { finishSave(); return; }
            // No revision in the reply: read it before the next patch.
            session_.call("settings.get", {}, [this](const Reply& current) {
                if (current.ok) session_.storeSettings(current.result.value("document").toObject(), current.result.value("metadata").toObject());
                finishSave();
            });
            return;
        }
        inFlight_ = false;
        const auto& error = reply.error;
        const auto path = error.details.value("path").toString();
        if (error.code == "config_conflict") {
            failed_ = settings::applyPatch(inFlightPatch_, pending_);
            blocked_ = true;
            saveText_ = L"Not saved";
            showNotice(L"Settings changed outside this window. Your last change was not saved.", noticeReload);
        } else if (!path.empty() || error.code == "config_invalid" || error.code == "s3_settings_invalid" || error.code == "invalid_request") {
            bool shown = false;
            for (auto& page : pages_) shown = page->validationFailed(path.empty() ? std::string("/") : path, UiSession::describe(reply)) || shown;
            saveText_ = L"Not saved: a setting is not valid";
            status(UiSession::describe(reply), true);
            if (hasPending_) { sendPending(); return; }
        } else {
            failed_ = settings::applyPatch(inFlightPatch_, pending_);
            blocked_ = true;
            saveText_ = L"Not saved";
            showNotice(L"Settings were not saved. " + UiSession::guidance(error), noticeRetry);
        }
        inFlightPatch_ = {};
        updateSaveStatus();
        if (fetchAfterSave_) { fetchAfterSave_ = false; fetchSettings(); }
        auto callbacks = std::move(idle_);
        idle_.clear();
        for (auto& callback : callbacks) callback(false);
    });
}

void MainWindow::finishSave() {
    inFlight_ = false;
    inFlightPatch_ = {};
    if (hasPending_) { sendPending(); return; }
    if (fetchAfterSave_) { fetchAfterSave_ = false; fetchSettings(); }
    // Pages see this window's own change once nothing else is queued, so a
    // refresh never overwrites an edit that is still waiting to be saved.
    broadcastSettings(false);
    updateSaveStatus();
    auto callbacks = std::move(idle_);
    idle_.clear();
    for (auto& callback : callbacks) callback(true);
}

void MainWindow::fetchSettings() {
    if (fetching_) { fetchAgain_ = true; return; }
    fetching_ = true;
    session_.call("settings.get", {}, [this](const Reply& reply) {
        fetching_ = false;
        if (!reply.ok) { status(UiSession::describe(reply), true); }
        else {
            const auto document = reply.result.value("document").toObject();
            const auto metadata = reply.result.value("metadata").toObject();
            const bool same = metadata.value("observedRevision") == session_.metadata().value("observedRevision") &&
                              metadata.value("acceptedRevision") == session_.metadata().value("acceptedRevision");
            if (same) {
                if (!settingsBusy()) session_.storeSettings(document, metadata);
            } else if (inFlight_) {
                // Possibly this window's own write: decide once its reply is in.
                fetchAfterSave_ = true;
            } else if (settingsBusy() || anyDirty()) {
                showNotice(L"Settings changed outside this window. Reload to see them; your unsaved edits are kept until then.", noticeReload);
            } else {
                session_.storeSettings(document, metadata);
                broadcastSettings(true);
            }
            const auto validation = metadata.value("validation").toObject();
            if (validation.value("ok").isBool() && !validation.value("ok").toBool()) {
                const auto path = validation.value("error")["details"]["path"].toString(validation.value("path").toString());
                status(L"The settings file is not valid" + (path.empty() ? std::wstring() : L" at " + wide(path)) + L". The last accepted settings stay active.", true);
            }
        }
        if (fetchAgain_) { fetchAgain_ = false; fetchSettings(); }
    });
}

void MainWindow::reloadSettings() {
    for (auto& page : pages_) page->discardDraft();
    pending_ = failed_ = {};
    hasPending_ = blocked_ = false;
    KillTimer(hwnd_, timerAutosave);
    hideNotice();
    session_.call("settings.get", {}, [this](const Reply& reply) {
        if (!reply.ok) { status(UiSession::describe(reply), true); return; }
        session_.storeSettings(reply.result.value("document").toObject(), reply.result.value("metadata").toObject());
        saveText_.clear();
        updateSaveStatus();
        broadcastSettings(true);
        status(L"Settings reloaded.");
    });
}

void MainWindow::handleSettingsError(const Reply& reply) {
    if (reply.ok) return;
    if (reply.error.code == "config_conflict") showNotice(L"Settings changed outside this window. Reload to continue.", noticeReload);
    else status(UiSession::describe(reply), true);
}

void MainWindow::adoptSettings(const json::Object& document, const json::Object& metadata) {
    session_.storeSettings(document, metadata);
    broadcastSettings(false);
}

void MainWindow::broadcastSettings(bool external) {
    for (auto& page : pages_) page->settingsChanged(external);
    updateRail();
    applyTheme();
}

bool MainWindow::anyDirty() const {
    for (const auto& page : pages_) if (page->dirty()) return true;
    return false;
}

void MainWindow::showNotice(const std::wstring& text, int action) {
    noticeAction_ = action;
    setTextW(notice_, text);
    setTextW(noticeButton_, action == noticeRetry ? L"Retry saving" : L"Reload");
    show(notice_, true);
    show(noticeButton_, action != noticeNone);
    layout();
}
void MainWindow::hideNotice() {
    noticeAction_ = noticeNone;
    show(notice_, false);
    show(noticeButton_, false);
    layout();
}

void MainWindow::updateSaveStatus() {
    std::wstring text = L"Settings save automatically";
    if (inFlight_ || hasPending_) text = L"Saving\x2026";
    else if (!saveText_.empty()) text = saveText_;
    setTextW(saveStatus_, text);
}

void MainWindow::status(const std::wstring& text, bool error) {
    const std::wstring shown = error && !text.empty() ? L"\x26A0 " + text : text;
    statusError_ = error;
    setTextW(status_, shown);
    setTooltip(tooltip_, status_, shown);
    InvalidateRect(status_, nullptr, TRUE);
    if (error) log::info("ui", "Status message shown", {{"error", true}});
}

void MainWindow::updateRail() {
    const auto& document = session_.document();
    for (const auto& action : railActions) {
        const auto shortcut = Settings::effectiveAction(document, action).value("shortcut").toString();
        std::wstring tip = explanation(action) + L"\n\n" + outputText(document, action) + L"\n\n" +
                           (shortcut.empty() ? std::wstring(L"No shortcut assigned.") : L"Shortcut: " + wide(shortcut));
        setTooltip(tooltip_, rail_[action], tip);
        theme::setHint(rail_[action], wide(shortcut));
    }
}

void MainWindow::applyTheme(const std::string& themeSetting) {
    const auto theme = themeSetting.empty() ? session_.document().value("preferences")["appearance"]["theme"].toString("system") : themeSetting;
    applyTitleBarTheme(hwnd_, theme);
    // Re-theme the controls only when the scheme changes: settings broadcasts follow every save.
    if (theme::setDark(theme::wantsDark(theme)) || !themeApplied_) {
        themeApplied_ = true;
        theme::applyTo(hwnd_);
        if (tooltip_) theme::applyTo(tooltip_);
        viewStateChanged();
    }
}

// ---- actions -------------------------------------------------------------------
void MainWindow::invokeAction(const std::string& action) {
    if (closing_) return;
    const bool screen = screenAction(action);
    if (screen && pages_[DestinationsPage]->pendingSecrets()) {
        message(hwnd_, L"put.gd", L"Store or clear the credentials you typed first.",
                L"Screen captures close this window. Store the credentials in Destinations or clear them, then start the capture again.");
        return;
    }
    flushSettings([this, action, screen](bool saved) {
        if (!saved) { status(L"Settings were not saved. Retry saving, then start the action again.", true); return; }
        if (screen) { hiddenForCapture_ = true; ShowWindow(hwnd_, SW_HIDE); }
        session_.call("action.invoke", {{"action", action}}, [this, action, screen](const Reply& reply) {
            if (!reply.ok) {
                if (screen) { hiddenForCapture_ = false; ShowWindow(hwnd_, SW_SHOW); SetForegroundWindow(hwnd_); }
                status(UiSession::describe(reply), true);
                if (reply.error.code == "output_required") message(hwnd_, L"put.gd", L"No output is enabled.", UiSession::guidance(reply.error), true);
                return;
            }
            if (screen) { requestClose(); return; }
            status(L"Clipboard upload accepted. Results appear in History.");
            pages_[HistoryPage]->invalidated("operations");
        });
    });
}

void MainWindow::openEditor(const std::string& source, const std::string& path) {
    json::Object params{{"source", source}};
    if (!path.empty()) params["path"] = path;
    flushSettings([this, params](bool) {
        session_.call("editor.open", params, [this](const Reply& reply) {
            if (!reply.ok) status(UiSession::describe(reply), true);
            else status(L"The editor is open. Nothing uploads until you choose Upload there.");
        });
    });
}

FileChooser::Options MainWindow::imageFileOptions() const {
    FileChooser::Options options;
    options.mode = FileChooser::Mode::Open;
    options.title = L"Edit an image file";
    options.folder = FileChooser::knownFolder(FOLDERID_Pictures);
    options.filters = {{L"Images (PNG, JPEG)", L"*.png;*.jpg;*.jpeg"}};
    return options;
}

void MainWindow::chooseImageToEdit() {
    if (!editImage_.open(hwnd_, imageFileOptions(), [this](const std::string& path) { if (!path.empty()) openEditor("file", path); }))
        status(L"Another file chooser is open. Finish it first.", true);
}

void MainWindow::showEditMenu(POINT screen) {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, idEditClipboard, L"Edit clipboard image");
    AppendMenuW(menu, MF_STRING, idEditFile, L"Edit an image file\x2026");
    const int chosen = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, screen.x, screen.y, 0, hwnd_, nullptr);
    DestroyMenu(menu);
    if (chosen == idEditClipboard) openEditor("clipboard");
    else if (chosen == idEditFile) chooseImageToEdit();
}

void MainWindow::showAbout() {
    const auto& build = session_.build();
    auto commit = build.value("commit").toString();
    if (commit.size() > 12) commit.resize(12);
    About about;
    about.version = L"Version " + wide(build.value("version").toString("unknown"));
    if (!commit.empty()) about.commit = L"Commit " + wide(commit);
    about.run(hwnd_, session_.document().value("preferences")["appearance"]["theme"].toString("system"));
}

void MainWindow::quit() {
    if (!confirm(hwnd_, L"Quit put.gd", L"Quit put.gd?", L"The tray icon and shortcuts stop. Pending uploads are cancelled.", L"Quit")) return;
    flushSettings([this](bool) {
        session_.call("ui.quit", {}, [this](const Reply& reply) {
            if (!reply.ok) { status(UiSession::describe(reply), true); return; }
            for (auto& page : pages_) page->clearSecrets();
            status(L"put.gd is quitting.");
        });
    });
}

void MainWindow::shortcutRecording(bool active, std::function<void(bool, const std::string&)> reply) {
    if (!send("shortcutRecording", {{"active", active}})) { if (reply) reply(false, "control_channel_closed"); return; }
    recordingReplies_.push_back({channel_.sent(), std::move(reply)});
}

// ---- close -------------------------------------------------------------------------
void MainWindow::requestClose() {
    if (closing_) return;
    closing_ = true;
    cancelling_ = false;
    EnableWindow(hwnd_, FALSE);
    flushSettings([this](bool saved) {
        if (!saved && !hiddenForCapture_) {
            EnableWindow(hwnd_, TRUE);
            if (!confirm(hwnd_, L"put.gd", L"Some settings were not saved.", L"Close anyway and lose them, or keep the window open to retry saving.", L"Close anyway")) {
                closing_ = false;
                return;
            }
            EnableWindow(hwnd_, FALSE);
        }
        sendCloseRequest();
    });
}

void MainWindow::sendCloseRequest() {
    auto state = checkpoint();
    if (json::serialize(state).size() > protocol::maximumUiStateBytes) state = {{"schemaVersion", 1}};
    ++closeSequence_;
    if (!send("closeRequested", {{"sequence", closeSequence_}, {"state", state}})) return;
    SetTimer(hwnd_, timerClose, 5000, nullptr);
}

void MainWindow::finishClose() {
    exiting_ = true;
    KillTimer(hwnd_, timerCheckpoint);
    KillTimer(hwnd_, timerClose);
    KillTimer(hwnd_, timerCloseSafety);
    send("closeConfirmed", {{"sequence", closeSequence_}});
    for (auto& page : pages_) page->clearSecrets();
    session_.detach();
    DestroyWindow(hwnd_);
    PostQuitMessage(0);
}

bool MainWindow::preTranslate(MSG& msg) {
    if (keyCapture_ && msg.hwnd == keyCapture_ && msg.message >= WM_KEYFIRST && msg.message <= WM_KEYLAST) {
        DispatchMessageW(&msg);
        return true;
    }
    if (msg.message == WM_KEYDOWN && (msg.wParam == VK_TAB || msg.wParam == VK_NEXT || msg.wParam == VK_PRIOR) && (GetKeyState(VK_CONTROL) & 0x8000) &&
        (msg.hwnd == hwnd_ || IsChild(hwnd_, msg.hwnd))) {
        const bool back = msg.wParam == VK_PRIOR || (msg.wParam == VK_TAB && (GetKeyState(VK_SHIFT) & 0x8000));
        setPage((page_ + (back ? 2 : 1)) % 3);
        return true;
    }
    return IsDialogMessageW(hwnd_, &msg);
}

LRESULT CALLBACK MainWindow::proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        self = static_cast<MainWindow*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
    }
    if (!self) return DefWindowProcW(hwnd, message, wParam, lParam);
    return self->handle(message, wParam, lParam);
}

LRESULT MainWindow::handle(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case sessionReplyMessage:
        session_.deliver(lParam);
        return 0;
    case controlFrameMessage: {
        std::unique_ptr<protocol::Frame> frame(reinterpret_cast<protocol::Frame*>(lParam));
        if (frame) onFrame(*frame);
        return 0;
    }
    case controlClosedMessage:
        // The resident is gone: never outlive it, never start another one.
        for (auto& page : pages_) page->clearSecrets();
        session_.detach();
        ExitProcess(0);
    case deferredMessage: {
        std::unique_ptr<std::function<void()>> task(reinterpret_cast<std::function<void()>*>(lParam));
        if (task && *task) (*task)();
        return 0;
    }
    case WM_TIMER:
        if (wParam == timerHeartbeat) send("heartbeat");
        else if (wParam == timerCheckpoint) sendCheckpoint();
        else if (wParam == timerAutosave) sendPending();
        else if (wParam == timerClose) {
            KillTimer(hwnd_, timerClose);
            if (closing_ && !cancelling_) {
                cancelling_ = true;
                send("closeCancel", {{"sequence", closeSequence_}});
                SetTimer(hwnd_, timerCloseSafety, 15000, nullptr);
            }
        } else if (wParam == timerCloseSafety) {
            KillTimer(hwnd_, timerCloseSafety);
            closing_ = cancelling_ = false;
            EnableWindow(hwnd_, TRUE);
            if (hiddenForCapture_) { hiddenForCapture_ = false; ShowWindow(hwnd_, SW_SHOW); }
        }
        return 0;
    case WM_COMMAND: {
        const int id = LOWORD(wParam);
        if (id >= idRailFirst && id < idRailFirst + int(railActions.size()) && HIWORD(wParam) == BN_CLICKED) invokeAction(railActions[size_t(id - idRailFirst)]);
        else if (id >= idNavFirst && id < idNavFirst + 3 && HIWORD(wParam) == BN_CLICKED) setPage(id - idNavFirst);
        else if (id == idAbout) showAbout();
        else if (id == idQuit) quit();
        else if (id == idNoticeButton) {
            if (noticeAction_ == noticeRetry && blocked_) {
                blocked_ = false;
                pending_ = settings::applyPatch(failed_, pending_);
                failed_ = {};
                hasPending_ = true;
                hideNotice();
                sendPending();
            } else {
                reloadSettings();
            }
        }
        return 0;
    }
    case WM_CONTEXTMENU:
        if (reinterpret_cast<HWND>(wParam) == rail_["edit"]) {
            POINT at{static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam))};
            if (at.x == -1 && at.y == -1) {
                RECT r{};
                GetWindowRect(rail_["edit"], &r);
                at = {r.left, r.bottom};
            }
            showEditMenu(at);
            return 0;
        }
        break;
    case WM_NOTIFY: {
        LRESULT result = 0;
        if (theme::customDraw(reinterpret_cast<NMHDR*>(lParam), result)) return result;
        return 0;
    }
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN: {
        const HWND control = reinterpret_cast<HWND>(lParam);
        const int role = control == status_ ? (statusError_ ? 2 : 0) : control == saveStatus_ || control == railHeading_ ? 1 : control == version_ ? 3 : 0;
        return theme::controlColor(message, reinterpret_cast<HDC>(wParam), control, role);
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        paint();
        return 0;
    case WM_ACTIVATE:
        // Dialog-style focus memory: restore the last focused control on activation.
        if (LOWORD(wParam) == WA_INACTIVE) {
            HWND focus = GetFocus();
            if (focus && IsChild(hwnd_, focus)) lastFocus_ = focus;
        } else {
            SetFocus(lastFocus_ && IsWindow(lastFocus_) && IsWindowVisible(lastFocus_) ? lastFocus_ : nav_[size_t(std::clamp(page_, 0, 2))]);
        }
        return 0;
    case WM_SIZE:
        layout();
        viewStateChanged();
        return 0;
    case WM_EXITSIZEMOVE:
        viewStateChanged();
        return 0;
    case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
        info->ptMinTrackSize = {scale(860, dpi()), scale(600, dpi())};
        return 0;
    }
    case WM_DPICHANGED: {
        const UINT newDpi = HIWORD(wParam);
        refreshFonts(hwnd_, newDpi);
        theme::applyTo(hwnd_);
        const auto* suggested = reinterpret_cast<RECT*>(lParam);
        SetWindowPos(hwnd_, nullptr, suggested->left, suggested->top, suggested->right - suggested->left, suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
        for (auto& page : pages_) page->dpiChanged();
        layout();
        return 0;
    }
    case WM_SETTINGCHANGE:
        if (lParam && lstrcmpW(reinterpret_cast<LPCWSTR>(lParam), L"ImmersiveColorSet") == 0) applyTheme();
        break;
    case WM_CLOSE:
        requestClose();
        return 0;
    case WM_DESTROY:
        session_.detach();
        return 0;
    }
    return DefWindowProcW(hwnd_, message, wParam, lParam);
}
}
