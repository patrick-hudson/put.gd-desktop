#include "ui/MainWindow.h"
#include "common/Log.h"
#include "common/Paths.h"
#include "core/Settings.h"
#include "core/SettingsPatch.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <shellapi.h>

namespace putgd::ui {
namespace {
constexpr wchar_t windowClass[] = L"putgd.Main";
constexpr int idRailFirst = 200, idAbout = 220, idQuit = 221, idNoticeButton = 222, idTab = 230, idStatus = 231;
constexpr int idEditClipboard = 240, idEditFile = 241;
constexpr UINT_PTR timerHeartbeat = 1, timerAutosave = 2, timerClose = 3, timerCloseSafety = 4;
constexpr int noticeNone = 0, noticeReload = 1, noticeRetry = 2;
const std::vector<std::string> railActions{"region", "edit", "window", "desktop", "clipboard"};

bool screenAction(const std::string& action) { return action == "region" || action == "edit" || action == "window" || action == "desktop"; }

std::wstring outputText(const json::Object& document, const std::string& action) {
    const auto saved = Settings::effectiveAction(document, action);
    const bool local = document.value("archive")["keepUploads"].toBool();
    const auto profileId = saved.value("profileId").toString();
    if (saved.value("destination") == "local" || profileId.empty())
        return local ? L"Saves a local copy using your folders in Settings > General. Uploads are off."
                     : L"This action will fail: no destination and local copies are off. Choose a destination in Settings > Capture or turn on Keep a local copy in Settings > General.";
    std::wstring label;
    for (const auto& entry : document.value("profiles").arrayRef())
        if (entry.objectRef().value("id") == profileId.c_str()) label = wide(entry.objectRef().value("label").toString());
    if (label.empty()) return L"Choose an upload destination in Settings > Capture before using this action.";
    std::wstring text = saved.value("publicationMode") == "private" ? L"Private uploads are not supported by this destination. Choose sharing in Settings > Capture."
                                                                    : L"Uploads to " + label + L" and copies its link to the clipboard.";
    if (local) text += L" Also saves a local copy.";
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
HRESULT CALLBACK aboutCallback(HWND, UINT notification, WPARAM, LPARAM lParam, LONG_PTR) {
    if (notification == TDN_HYPERLINK_CLICKED) openUrl(narrow(reinterpret_cast<PCWSTR>(lParam)));
    return S_OK;
}
}

MainWindow::MainWindow(UiSession& session, frames::Channel& channel) : session_(session), channel_(channel) {
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

bool MainWindow::create(const std::string& initialView, const json::Object& state) {
    WNDCLASSEXW cls{};
    cls.cbSize = sizeof cls;
    cls.lpfnWndProc = proc;
    cls.hInstance = GetModuleHandleW(nullptr);
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    cls.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
    cls.lpszClassName = windowClass;
    cls.hIcon = static_cast<HICON>(LoadImageW(cls.hInstance, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR));
    cls.hIconSm = static_cast<HICON>(LoadImageW(cls.hInstance, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
    RegisterClassExW(&cls);

    const UINT dpi = systemDpi();
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const int width = std::min(scale(1120, dpi), int(work.right - work.left)), height = std::min(scale(780, dpi), int(work.bottom - work.top));
    hwnd_ = CreateWindowExW(WS_EX_CONTROLPARENT, windowClass, L"put.gd", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                            work.left + (work.right - work.left - width) / 2, work.top + (work.bottom - work.top - height) / 2, width, height,
                            nullptr, nullptr, cls.hInstance, this);
    if (!hwnd_) return false;
    session_.attach(hwnd_);
    tooltip_ = createTooltip(hwnd_);

    railHeading_ = ui::label(hwnd_, L"Capture & upload");
    setFont(railHeading_, FontRole::Bold, this->dpi());
    for (size_t i = 0; i < railActions.size(); ++i) {
        auto caption = actionLabel(railActions[i]);
        if (railActions[i] == "edit") caption = L"Capture region && edit";
        HWND b = button(hwnd_, idRailFirst + int(i), caption);
        rail_[railActions[i]] = b;
        if (railActions[i] == "edit") {
            DragAcceptFiles(b, TRUE);
            SetWindowSubclass(b, editButtonProc, 1, reinterpret_cast<DWORD_PTR>(this));
        }
    }
    version_ = ui::label(hwnd_, L"");
    about_ = button(hwnd_, idAbout, L"About put.gd");
    quit_ = button(hwnd_, idQuit, L"Quit put.gd");
    setTooltip(tooltip_, quit_, L"Exit put.gd, its tray icon and shortcuts. Pending work is cancelled.");
    notice_ = ui::label(hwnd_, L"", 0, SS_CENTERIMAGE);
    noticeButton_ = button(hwnd_, idNoticeButton, L"Reload");
    show(notice_, false);
    show(noticeButton_, false);

    tab_ = CreateWindowExW(0, WC_TABCONTROLW, L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_TABSTOP, 0, 0, 10, 10, hwnd_,
                           reinterpret_cast<HMENU>(static_cast<INT_PTR>(idTab)), cls.hInstance, nullptr);
    setFont(tab_, FontRole::Normal, this->dpi());
    const wchar_t* names[] = {L"History", L"Destinations", L"Settings"};
    for (int i = 0; i < 3; ++i) {
        TCITEMW item{};
        item.mask = TCIF_TEXT;
        item.pszText = const_cast<wchar_t*>(names[i]);
        SendMessageW(tab_, TCM_INSERTITEMW, WPARAM(i), reinterpret_cast<LPARAM>(&item));
    }
    statusBar_ = CreateWindowExW(0, STATUSCLASSNAMEW, L"", WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP, 0, 0, 0, 0, hwnd_,
                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(idStatus)), cls.hInstance, nullptr);
    setFont(statusBar_, FontRole::Normal, this->dpi());
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
                       {"main", json::Object{{"page", page_}, {"geometry", json::Array{int(rect.left), int(rect.top), int(rect.right - rect.left), int(rect.bottom - rect.top)}}}}};
    const auto history = pages_[HistoryPage]->saveState();
    if (!history.empty()) state["history"] = history;
    if (!settings.empty()) state["settings"] = settings;
    return state;
}

void MainWindow::layout() {
    if (!hwnd_ || !tab_) return;
    RECT client{};
    GetClientRect(hwnd_, &client);
    const UINT d = dpi();
    SendMessageW(statusBar_, WM_SIZE, 0, 0);
    RECT bar{};
    GetWindowRect(statusBar_, &bar);
    const int statusHeight = bar.bottom - bar.top;
    int parts[2] = {std::max(0, int(client.right) - scale(300, d)), -1};
    SendMessageW(statusBar_, SB_SETPARTS, 2, reinterpret_cast<LPARAM>(parts));

    const int pad = scale(12, d), railWidth = scale(210, d), line = lineHeight(d), buttonHeight = std::max(scale(32, d), line + scale(14, d));
    int y = pad;
    place(railHeading_, pad, y, railWidth - 2 * pad, line + scale(2, d));
    y += line + scale(10, d);
    for (const auto& action : railActions) {
        place(rail_[action], pad, y, railWidth - 2 * pad, buttonHeight);
        y += buttonHeight + scale(6, d);
    }
    int bottom = client.bottom - statusHeight - pad - buttonHeight;
    place(quit_, pad, bottom, railWidth - 2 * pad, buttonHeight);
    bottom -= buttonHeight + scale(6, d);
    place(about_, pad, bottom, railWidth - 2 * pad, buttonHeight);
    bottom -= line * 2 + scale(8, d);
    place(version_, pad, bottom, railWidth - 2 * pad, line * 2 + scale(2, d));

    const int left = railWidth, right = client.right - pad;
    int top = scale(10, d);
    if (visible(notice_)) {
        const int buttonWidth = scale(120, d);
        place(notice_, left, top, right - left - buttonWidth - scale(8, d), buttonHeight);
        place(noticeButton_, right - buttonWidth, top, buttonWidth, buttonHeight);
        top += buttonHeight + scale(8, d);
    }
    const int tabBottom = client.bottom - statusHeight - scale(8, d);
    place(tab_, left, top, right - left, tabBottom - top);
    RECT display{left, top, right, tabBottom};
    SendMessageW(tab_, TCM_ADJUSTRECT, FALSE, reinterpret_cast<LPARAM>(&display));
    for (HWND page : pageWindows_)
        SetWindowPos(page, HWND_TOP, display.left, display.top, display.right - display.left, display.bottom - display.top, SWP_NOACTIVATE);
}

void MainWindow::setPage(int index) {
    index = std::clamp(index, 0, 2);
    const int previous = page_;
    page_ = index;
    SendMessageW(tab_, TCM_SETCURSEL, WPARAM(index), 0);
    for (size_t i = 0; i < pageWindows_.size(); ++i) ShowWindow(pageWindows_[i], int(i) == index ? SW_SHOW : SW_HIDE);
    if (previous != index) pages_[size_t(previous)]->deactivated();
    pages_[size_t(index)]->activated();
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
    SendMessageW(statusBar_, SB_SETTEXTW, 1, reinterpret_cast<LPARAM>(text.c_str()));
}

void MainWindow::status(const std::wstring& text, bool error) {
    const std::wstring shown = error && !text.empty() ? L"\x26A0 " + text : text;
    SendMessageW(statusBar_, SB_SETTEXTW, 0, reinterpret_cast<LPARAM>(shown.c_str()));
    SendMessageW(statusBar_, SB_SETTIPTEXTW, 0, reinterpret_cast<LPARAM>(shown.c_str()));
    if (error) log::info("ui", "Status message shown", {{"error", true}});
}

void MainWindow::updateRail() {
    const auto& document = session_.document();
    for (const auto& action : railActions) {
        const auto shortcut = Settings::effectiveAction(document, action).value("shortcut").toString();
        std::wstring tip = explanation(action) + L"\n\n" + outputText(document, action) + L"\n\n" +
                           (shortcut.empty() ? std::wstring(L"No shortcut assigned.") : L"Shortcut: " + wide(shortcut));
        setTooltip(tooltip_, rail_[action], tip);
    }
}

void MainWindow::applyTheme(const std::string& themeSetting) {
    const auto theme = themeSetting.empty() ? session_.document().value("preferences")["appearance"]["theme"].toString("system") : themeSetting;
    applyTitleBarTheme(hwnd_, theme);
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

void MainWindow::chooseImageToEdit() {
    std::string path;
    if (pickImageFile(hwnd_, path)) openEditor("file", path);
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
    std::wstring content = L"Version " + wide(build.value("version").toString("unknown"));
    if (!commit.empty()) content += L"\nCommit " + wide(commit);
    content += L"\n\nCapture, upload and share screenshots and files.\n<a href=\"https://put.gd\">https://put.gd</a>";
    TASKDIALOGCONFIG config{};
    config.cbSize = sizeof config;
    config.hwndParent = hwnd_;
    config.hInstance = GetModuleHandleW(nullptr);
    config.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_ENABLE_HYPERLINKS | TDF_POSITION_RELATIVE_TO_WINDOW;
    config.dwCommonButtons = TDCBF_OK_BUTTON;
    config.pszWindowTitle = L"About put.gd";
    config.pszMainIcon = MAKEINTRESOURCEW(1);
    config.pszMainInstruction = L"put.gd";
    config.pszContent = content.c_str();
    config.pfCallback = aboutCallback;
    TaskDialogIndirect(&config, nullptr, nullptr, nullptr);
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
        auto* header = reinterpret_cast<NMHDR*>(lParam);
        if (header->hwndFrom == tab_ && header->code == TCN_SELCHANGE) setPage(int(SendMessageW(tab_, TCM_GETCURSEL, 0, 0)));
        return 0;
    }
    case WM_CTLCOLORSTATIC: {
        HDC dc = reinterpret_cast<HDC>(wParam);
        if (reinterpret_cast<HWND>(lParam) == notice_) {
            static HBRUSH noticeBrush = CreateSolidBrush(RGB(255, 244, 206));
            SetBkColor(dc, RGB(255, 244, 206));
            SetTextColor(dc, RGB(32, 32, 32));
            return reinterpret_cast<LRESULT>(noticeBrush);
        }
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, GetSysColor(COLOR_BTNTEXT));
        return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_BTNFACE));
    }
    case WM_ACTIVATE:
        // Dialog-style focus memory: restore the last focused control on activation.
        if (LOWORD(wParam) == WA_INACTIVE) {
            HWND focus = GetFocus();
            if (focus && IsChild(hwnd_, focus)) lastFocus_ = focus;
        } else {
            SetFocus(lastFocus_ && IsWindow(lastFocus_) && IsWindowVisible(lastFocus_) ? lastFocus_ : tab_);
        }
        return 0;
    case WM_SIZE:
        layout();
        return 0;
    case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
        info->ptMinTrackSize = {scale(860, dpi()), scale(600, dpi())};
        return 0;
    }
    case WM_DPICHANGED: {
        const UINT newDpi = HIWORD(wParam);
        refreshFonts(hwnd_, newDpi);
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
