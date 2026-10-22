#include "record/ReviewWindow.h"
#include "common/Log.h"
#include "core/ReviewTimeline.h"
#include "ui/Controls.h"
#include "ui/Theme.h"
#include <algorithm>
#include <cmath>
#include <shobjidl.h>
#include <windowsx.h>

namespace putgd::record {
namespace theme = ui::theme;
namespace {
// In front of other windows. Activation needs the foreground right, which a stop
// from the shortcut or the bar brings and one from the CLI does not; then the
// window still comes to the top of the z-order without taking the focus.
void bringToFront(HWND hwnd) {
    if (SetForegroundWindow(hwnd)) return;
    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}
constexpr wchar_t windowClass[] = L"putgd.record.review";
constexpr wchar_t viewClass[] = L"putgd.record.view";
constexpr wchar_t timelineClass[] = L"putgd.record.timeline";
constexpr UINT frameArrived = WM_APP + 10, openDialog = WM_APP + 11;
enum : int {
    idPlay = 201, idTime, idSpeed, idTrimIn, idTrimOut, idReset, idKept, idBlur, idSolid, idMasks, idMaskIn, idMaskOut, idRemove,
    idStrength, idOutputLabel, idFormat, idStatus, idSave, idDiscard, idUpload, idNotice
};
const double speeds[] = {0.5, 1.0, 2.0, 4.0};
std::wstring w(const std::string& s) { return ui::wide(s); }
std::string formatLabel(const std::string& format) {
    if (format == "mp4") return "MP4 (H.264)";
    if (format == "gif") return "GIF";
    if (format == "webp") return "WebP";
    return "WebM (VP9)";
}
LRESULT CALLBACK childProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* owner = reinterpret_cast<ReviewWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!owner) return DefWindowProcW(hwnd, message, wParam, lParam);
    wchar_t name[64]{};
    GetClassNameW(hwnd, name, 64);
    return std::wstring(name) == viewClass ? owner->frameMessage(hwnd, message, wParam, lParam) : owner->timelineMessage(hwnd, message, wParam, lParam);
}
bool registerClasses() {
    static const bool done = [] {
        WNDCLASSEXW child{sizeof child};
        child.lpfnWndProc = childProc; child.hInstance = GetModuleHandleW(nullptr); child.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        child.lpszClassName = viewClass;
        if (!RegisterClassExW(&child)) return false;
        child.lpszClassName = timelineClass;
        return RegisterClassExW(&child) != 0;
    }();
    return done;
}
// Double-buffered painting of a child window.
template <class F> void buffered(HWND hwnd, F paint) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    RECT client; GetClientRect(hwnd, &client);
    HDC memory = CreateCompatibleDC(dc);
    HBITMAP bitmap = CreateCompatibleBitmap(dc, std::max(1L, client.right), std::max(1L, client.bottom));
    HGDIOBJ old = SelectObject(memory, bitmap);
    paint(memory, client);
    BitBlt(dc, 0, 0, client.right, client.bottom, memory, 0, 0, SRCCOPY);
    SelectObject(memory, old); DeleteObject(bitmap); DeleteDC(memory);
    EndPaint(hwnd, &ps);
}
}

ReviewWindow::ReviewWindow(std::string clip, RecordingSize canvas, long long duration, RecordingConfig config, RecordingEdit edit, std::vector<std::string> formats, Callbacks callbacks)
    : clip_(std::move(clip)), canvas_(canvas), duration_(duration), config_(std::move(config)), edit_(std::move(edit)), formats_(std::move(formats)), callbacks_(std::move(callbacks)) {}
ReviewWindow::~ReviewWindow() {
    frames_.reset();
    if (hwnd_) { closing_ = true; DestroyWindow(hwnd_); }
}
bool ReviewWindow::create() {
    static const bool registered = [] {
        WNDCLASSEXW wc{sizeof wc};
        wc.lpfnWndProc = &ReviewWindow::proc; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = windowClass; wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1));
        return RegisterClassExW(&wc) != 0;
    }();
    if (!registered || !registerClasses()) return false;
    // 960x600 fits a 1366x768 desktop; opened on the monitor under the pointer.
    POINT cursor; GetCursorPos(&cursor);
    MONITORINFO info{sizeof info};
    GetMonitorInfoW(MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST), &info);
    hwnd_ = CreateWindowExW(WS_EX_CONTROLPARENT, windowClass, L"Review recording", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, 0, 0, 960, 600, nullptr, nullptr, GetModuleHandleW(nullptr), this);
    if (!hwnd_) return false;
    dpi_ = GetDpiForWindow(hwnd_);
    const RECT work = info.rcWork;
    const int width = std::min(scaled(960), int(work.right - work.left)), height = std::min(scaled(600), int(work.bottom - work.top));
    SetWindowPos(hwnd_, nullptr, work.left + (work.right - work.left - width) / 2, work.top + (work.bottom - work.top - height) / 2, width, height, SWP_NOZORDER);
    theme::setSurface(hwnd_, theme::Surface::Content);
    createControls();
    frames_ = std::make_unique<FrameSource>(hwnd_, frameArrived, clip_, canvas_);
    theme::applyTo(hwnd_);
    layout();
    refresh();
    setPlayhead(0);
    ShowWindow(hwnd_, SW_SHOWNORMAL);
    bringToFront(hwnd_);
    return true;
}
void ReviewWindow::createControls() {
    // Icon buttons carry their name as text (screen readers, recording.review) and show only the glyph.
    auto button = [&](int id, const wchar_t* text, const char* glyph, const std::wstring& tip, theme::Role role = theme::Role::Standard, bool iconOnly = false) {
        HWND b = ui::button(hwnd_, id, text);
        theme::setRole(b, role, glyph);
        theme::setIconOnly(b, iconOnly);
        ui::setTooltip(tooltip_, b, tip);
        return b;
    };
    tooltip_ = ui::createTooltip(hwnd_);
    view_ = CreateWindowExW(0, viewClass, L"Recording frame", WS_CHILD | WS_VISIBLE, 0, 0, 10, 10, hwnd_, nullptr, GetModuleHandleW(nullptr), nullptr);
    SetWindowLongPtrW(view_, GWLP_USERDATA, LONG_PTR(this));
    timeline_ = CreateWindowExW(0, timelineClass, L"Timeline", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 10, 10, hwnd_, nullptr, GetModuleHandleW(nullptr), nullptr);
    SetWindowLongPtrW(timeline_, GWLP_USERDATA, LONG_PTR(this));
    ui::label(hwnd_, L"", idNotice);
    button(idPlay, L"Play", "play", L"Play the kept part silently (Space)", theme::Role::Standard, true);
    ui::label(hwnd_, L"", idTime, SS_CENTERIMAGE | SS_NOPREFIX);
    HWND speed = ui::combo(hwnd_, idSpeed);
    ui::setChoices(speed, {{L"0.5×", "0.5"}, {L"1×", "1"}, {L"2×", "2"}, {L"4×", "4"}});
    ui::select(speed, "1");
    theme::styleCombo(speed);
    ui::setTooltip(tooltip_, speed, L"Playback speed");
    button(idTrimIn, L"Keep from here", "trimIn", L"Keep from the playhead (I)", theme::Role::Standard, true);
    button(idTrimOut, L"Keep until here", "trimOut", L"Keep until the playhead (O)", theme::Role::Standard, true);
    button(idReset, L"Keep everything", "reset", L"Keep the whole recording", theme::Role::Standard, true);
    ui::label(hwnd_, L"", idKept, SS_CENTERIMAGE | SS_NOPREFIX);
    button(idBlur, L"Add blur", "blur", L"Add a blur, then drag on the frame", theme::Role::Standard, true);
    button(idSolid, L"Add cover", "solid", L"Add a solid cover, then drag on the frame", theme::Role::Standard, true);
    HWND masks = ui::combo(hwnd_, idMasks);
    theme::styleCombo(masks);
    ui::setTooltip(tooltip_, masks, L"Masks");
    button(idMaskIn, L"Mask starts here", "maskIn", L"Selected mask starts at the playhead (Shift+I)", theme::Role::Standard, true);
    button(idMaskOut, L"Mask ends here", "maskOut", L"Selected mask ends at the playhead (Shift+O)", theme::Role::Standard, true);
    button(idRemove, L"Remove mask", "remove", L"Remove the selected mask (Delete)", theme::Role::Standard, true);
    HWND strength = CreateWindowExW(0, TRACKBAR_CLASSW, L"Blur strength", WS_CHILD | WS_TABSTOP | TBS_HORZ | TBS_NOTICKS, 0, 0, 10, 10, hwnd_, reinterpret_cast<HMENU>(INT_PTR(idStrength)), GetModuleHandleW(nullptr), nullptr);
    SendMessageW(strength, TBM_SETRANGE, TRUE, MAKELPARAM(2, 64));
    ui::setTooltip(tooltip_, strength, L"Blur strength");
    ui::label(hwnd_, L"Output", idOutputLabel, SS_CENTERIMAGE);
    HWND format = ui::combo(hwnd_, idFormat);
    std::vector<ui::Choice> choices;
    for (const auto& f : formats_) choices.push_back({w(formatLabel(f)), f});
    ui::setChoices(format, choices);
    ui::select(format, config_.format);
    theme::styleCombo(format);
    ui::setTooltip(tooltip_, format, L"Output format of Save copy and Upload");
    ui::label(hwnd_, L"", idStatus, SS_ENDELLIPSIS | SS_NOPREFIX | SS_CENTERIMAGE);
    button(idSave, L"Save copy…", nullptr, L"Save the edited recording to a file; nothing is uploaded");
    button(idDiscard, L"Discard", nullptr, L"Discard this recording");
    button(idUpload, L"Upload", nullptr, L"Upload the edited recording", theme::Role::Primary);
    for (HWND child = GetWindow(hwnd_, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) ui::setFont(child, ui::FontRole::Normal, dpi_);
    ui::setFont(GetDlgItem(hwnd_, idUpload), ui::FontRole::Bold, dpi_);
}
void ReviewWindow::layout() {
    RECT client; GetClientRect(hwnd_, &client);
    const int pad = scaled(10), gap = scaled(6), row = scaled(32), button = scaled(32);
    const auto maskLanes = recording::maskLanes(edit_.masks);
    const int lanes = std::max(1, maskLanes.empty() ? 0 : *std::max_element(maskLanes.begin(), maskLanes.end()) + 1);
    const int timelineHeight = scaled(30) + lanes * scaled(10);
    int bottom = client.bottom - pad;
    // The action row.
    const int actionY = bottom - row;
    int x = pad;
    auto place = [&](int id, int width, int y) { HWND h = GetDlgItem(hwnd_, id); if (!ui::visible(h)) return; ui::place(h, x, y, width, row); x += width + gap; };
    place(idOutputLabel, scaled(48), actionY);
    place(idFormat, scaled(130), actionY);
    const int right = client.right - pad;
    const int uploadW = ui::buttonWidth(GetDlgItem(hwnd_, idUpload), 96), discardW = ui::buttonWidth(GetDlgItem(hwnd_, idDiscard), 88), saveW = ui::buttonWidth(GetDlgItem(hwnd_, idSave), 104);
    ui::place(GetDlgItem(hwnd_, idUpload), right - uploadW, actionY, uploadW, row);
    ui::place(GetDlgItem(hwnd_, idDiscard), right - uploadW - gap - discardW, actionY, discardW, row);
    ui::place(GetDlgItem(hwnd_, idSave), right - uploadW - discardW - saveW - 2 * gap, actionY, saveW, row);
    ui::place(GetDlgItem(hwnd_, idStatus), x, actionY, std::max(0, right - uploadW - discardW - saveW - 3 * gap - x), row);
    // The tool row.
    const int toolY = actionY - gap - row;
    x = pad;
    place(idPlay, button, toolY);
    place(idTime, scaled(104), toolY);
    place(idSpeed, scaled(64), toolY);
    x += gap;
    place(idTrimIn, button, toolY);
    place(idTrimOut, button, toolY);
    place(idReset, button, toolY);
    place(idKept, scaled(104), toolY);
    x += gap;
    place(idBlur, button, toolY);
    place(idSolid, button, toolY);
    place(idMasks, scaled(150), toolY);
    place(idMaskIn, button, toolY);
    place(idMaskOut, button, toolY);
    place(idRemove, button, toolY);
    place(idStrength, scaled(110), toolY);
    // The timeline, the notice and the frame.
    const int timelineY = toolY - gap - timelineHeight;
    MoveWindow(timeline_, pad, timelineY, client.right - 2 * pad, timelineHeight, TRUE);
    int top = pad;
    HWND notice = GetDlgItem(hwnd_, idNotice);
    if (ui::visible(notice)) { ui::place(notice, pad, top, client.right - 2 * pad, ui::textHeight(notice, client.right - 2 * pad) + scaled(8)); RECT r; GetWindowRect(notice, &r); top += (r.bottom - r.top) + gap; }
    MoveWindow(view_, pad, top, client.right - 2 * pad, std::max(0, timelineY - gap - top), TRUE);
    InvalidateRect(hwnd_, nullptr, TRUE);
}
const RecordingMask* ReviewWindow::selected() const {
    for (const auto& m : edit_.masks) if (m.id == selected_) return &m;
    return nullptr;
}
void ReviewWindow::refresh() {
    if (!hwnd_) return;
    const auto* mask = selected();
    if (!selected_.empty() && !mask) selected_.clear();
    const bool idle = !exporting_ && !dialog_;
    ui::setTextW(GetDlgItem(hwnd_, idTime), w(recording::clockText(playhead_) + " / " + recording::clockText(duration_)));
    ui::setTextW(GetDlgItem(hwnd_, idKept), w(recording::clockText(edit_.trimStart) + " – " + recording::clockText(edit_.exportEnd(duration_))));
    theme::setRole(GetDlgItem(hwnd_, idPlay), theme::Role::Standard, playing_ ? "pause" : "play");
    ui::setTooltip(tooltip_, GetDlgItem(hwnd_, idPlay), playing_ ? L"Pause the preview (Space)" : L"Play the kept part silently (Space)");
    SetWindowTextW(GetDlgItem(hwnd_, idPlay), playing_ ? L"Pause" : L"Play"); // the accessible name; the face shows the glyph
    // Contextual mask controls: the list once masks exist; times and Remove for a
    // selection; the strength only for a selected blur.
    HWND list = GetDlgItem(hwnd_, idMasks);
    std::vector<ui::Choice> choices{{L"No mask selected", ""}};
    int blur = 0, solid = 0;
    for (const auto& m : edit_.masks) {
        const auto label = (m.kind == "blur" ? "Blur " + std::to_string(++blur) : "Cover " + std::to_string(++solid)) + "  " + recording::clockText(m.start) + "–" + recording::clockText(m.end);
        choices.push_back({w(label), m.id});
    }
    ui::setChoices(list, choices);
    ui::select(list, selected_);
    ui::show(list, !edit_.masks.empty());
    for (int id : {idMaskIn, idMaskOut, idRemove}) ui::show(GetDlgItem(hwnd_, id), mask != nullptr);
    const bool strength = mask && mask->kind == "blur";
    ui::show(GetDlgItem(hwnd_, idStrength), strength);
    if (strength) SendMessageW(GetDlgItem(hwnd_, idStrength), TBM_SETPOS, TRUE, mask->strength);
    theme::setSelected(GetDlgItem(hwnd_, idBlur), armed_ == "blur");
    theme::setSelected(GetDlgItem(hwnd_, idSolid), armed_ == "solid");
    for (int id : {idPlay, idSpeed, idTrimIn, idTrimOut, idReset, idBlur, idSolid, idMasks, idMaskIn, idMaskOut, idRemove, idStrength, idFormat, idSave, idDiscard, idUpload}) ui::enable(GetDlgItem(hwnd_, id), idle);
    ui::enable(GetDlgItem(hwnd_, idFormat), idle && formats_.size() > 1);
    // The status line: an error, the export's progress, or what the output will be.
    std::string status;
    const long long kept = edit_.exportDuration(duration_);
    if (!error_.empty()) status = error_;
    else if (exporting_) status = status_ + (exportFraction_ >= 0 ? " " + std::to_string(int(exportFraction_ * 100)) + "%" : "");
    else if (config_.animation() && kept > animationMaxUs_) status = formatLabel(config_.format) + " is limited to " + std::to_string(animationMaxUs_ / 1000000) + " s: trim the clip";
    else if (!status_.empty()) status = status_;
    else status = "Keeps " + recording::clockText(kept) + " · " + formatLabel(config_.format) + " · no audio";
    ui::setTextW(GetDlgItem(hwnd_, idStatus), w(status));
    HWND notice = GetDlgItem(hwnd_, idNotice);
    ui::setTextW(notice, w(notice_));
    ui::show(notice, !notice_.empty());
    layout(); // contextual controls and the notice change the rows
    InvalidateRect(view_, nullptr, FALSE);
    InvalidateRect(timeline_, nullptr, FALSE);
}
void ReviewWindow::setPlayhead(long long us, bool decode) {
    playhead_ = std::clamp<long long>(us, 0, std::max<long long>(0, duration_ - 1));
    if (decode && frames_) request_ = frames_->seek(playhead_, edit_);
    refresh();
}
void ReviewWindow::play() {
    if (playing_ || exporting_ || !frames_) return;
    const long long end = edit_.exportEnd(duration_);
    long long start = playhead_;
    // At or past the kept end, play the kept part again from its start.
    if (start >= end - 1 || start < edit_.trimStart) start = edit_.trimStart;
    armed_.clear();
    playing_ = true;
    request_ = frames_->play(start, end, speed_, edit_);
    refresh();
}
void ReviewWindow::pause() {
    if (!playing_) return;
    playing_ = false;
    if (frames_) frames_->stop();
    setPlayhead(playhead_);
}
void ReviewWindow::propose(json::Array commands) {
    // The resident decides; the same rules here only refuse what it would refuse.
    auto copy = edit_;
    if (auto e = recording::applyEditCommands(copy, commands, duration_, canvas_, config_.fps, [] { return std::string("pending"); })) { error_ = e.message; refresh(); return; }
    error_.clear();
    if (callbacks_.edit) callbacks_.edit(edit_.revision, commands);
}
void ReviewWindow::setTrimIn() {
    pause();
    json::Object c{{"op", "setTrim"}, {"start", double(playhead_)}, {"end", edit_.trimEnd ? json::Value(double(*edit_.trimEnd)) : json::Value()}};
    propose({c});
}
void ReviewWindow::setTrimOut() {
    pause();
    propose({json::Object{{"op", "setTrim"}, {"start", double(edit_.trimStart)}, {"end", double(std::max(playhead_, edit_.trimStart + 1))}}});
}
void ReviewWindow::resetTrim() { pause(); propose({json::Object{{"op", "resetTrim"}}}); }
void ReviewWindow::arm(const std::string& kind) { pause(); armed_ = armed_ == kind ? std::string() : kind; refresh(); }
json::Object ReviewWindow::maskJson(const RecordingMask& m) const {
    json::Object o{{"kind", m.kind}, {"rect", m.rect.toJson()}, {"start", double(m.start)}, {"end", double(m.end)}};
    if (m.kind == "blur") o["strength"] = m.strength;
    return o;
}
void ReviewWindow::addMask(const std::string& kind, const RecordingRect& rect) {
    // From the playhead to the end of the kept part, as Add blur/cover and a drag do.
    const long long end = edit_.exportEnd(duration_);
    if (playhead_ >= end) { error_ = "Move the playhead into the kept part before adding a mask"; refresh(); return; }
    RecordingMask m; m.kind = kind; m.rect = rect; m.start = playhead_; m.end = end;
    armed_.clear();
    selectNewest_ = true;
    propose({json::Object{{"op", "addMask"}, {"mask", maskJson(m)}}});
}
void ReviewWindow::selectMask(const std::string& id) { selected_ = id; refresh(); }
void ReviewWindow::setMaskTime(bool in) {
    const auto* mask = selected();
    if (!mask) return;
    pause();
    auto m = *mask;
    if (in) m.start = playhead_; else m.end = std::max(playhead_, m.start + 1);
    propose({json::Object{{"op", "updateMask"}, {"maskId", m.id}, {"mask", maskJson(m)}}});
}
void ReviewWindow::setStrength(int strength) {
    const auto* mask = selected();
    if (!mask || mask->kind != "blur" || mask->strength == strength) return;
    auto m = *mask; m.strength = strength;
    propose({json::Object{{"op", "updateMask"}, {"maskId", m.id}, {"mask", maskJson(m)}}});
}
void ReviewWindow::removeMask() {
    if (!selected()) return;
    propose({json::Object{{"op", "removeMask"}, {"maskId", selected_}}});
}
void ReviewWindow::chooseFormat(const std::string& format) {
    if (format == config_.format) return;
    json::Object config{{"format", format}};
    const auto codecs = recording::codecs(format);
    if (!codecs.empty()) config["codec"] = codecs.front();
    if (callbacks_.configChanged) callbacks_.configChanged(config);
}
void ReviewWindow::saveCopyDialog() {
    if (exporting_ || dialog_) return;
    pause();
    IFileSaveDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) { error_ = "The save dialog is not available"; refresh(); return; }
    const auto extension = w(config_.extension());
    const auto pattern = L"*." + extension, name = L"recording." + extension, type = w(formatLabel(config_.format));
    COMDLG_FILTERSPEC filter{type.c_str(), pattern.c_str()};
    dialog->SetFileTypes(1, &filter);
    dialog->SetDefaultExtension(extension.c_str());
    dialog->SetFileName(name.c_str());
    dialog->SetTitle(L"Save a copy");
    DWORD options = 0; dialog->GetOptions(&options); dialog->SetOptions(options | FOS_OVERWRITEPROMPT | FOS_FORCEFILESYSTEM);
    dialog_ = dialog;
    refresh();
    const HRESULT shown = dialog->Show(hwnd_);
    std::string path;
    IShellItem* item = nullptr;
    if (SUCCEEDED(shown) && SUCCEEDED(dialog->GetResult(&item))) {
        PWSTR chosen = nullptr;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &chosen))) { path = ui::narrow(chosen); CoTaskMemFree(chosen); }
        item->Release();
    }
    dialog->Release();
    dialog_ = nullptr;
    dialogLast_ = path.empty() ? "cancelled" : path;
    refresh();
    if (!path.empty() && callbacks_.saveCopy) callbacks_.saveCopy(path, edit_.revision);
}
// IFileDialog::Close works only from the dialog's own callbacks; outside them the
// dialog's window gets Cancel, as a click on its Cancel button would.
void ReviewWindow::closeFileDialog() {
    if (!dialog_) return;
    IOleWindow* window = nullptr;
    HWND dialog = nullptr;
    if (SUCCEEDED(static_cast<IFileDialog*>(dialog_)->QueryInterface(IID_PPV_ARGS(&window)))) { window->GetWindow(&dialog); window->Release(); }
    if (dialog) PostMessageW(dialog, WM_COMMAND, MAKEWPARAM(IDCANCEL, BN_CLICKED), 0);
    else static_cast<IFileDialog*>(dialog_)->Close(HRESULT_FROM_WIN32(ERROR_CANCELLED));
}
void ReviewWindow::upload() {
    if (exporting_ || dialog_) return;
    pause();
    // Upload closes the window at once; the export and upload go on in the background.
    closing_ = true;
    ShowWindow(hwnd_, SW_HIDE);
    closing_ = false;
    if (callbacks_.upload) callbacks_.upload(edit_.revision);
}
void ReviewWindow::discard() {
    pause();
    ShowWindow(hwnd_, SW_HIDE);
    if (callbacks_.discard) callbacks_.discard();
}
void ReviewWindow::reopen(const std::string& error) {
    error_ = error;
    exporting_ = false;
    ShowWindow(hwnd_, SW_SHOWNORMAL);
    bringToFront(hwnd_);
    refresh();
}
void ReviewWindow::hide() { pause(); if (hwnd_) ShowWindow(hwnd_, SW_HIDE); }
void ReviewWindow::setEdit(const RecordingEdit& edit, long long animationMaxUs, int animationEdge) {
    const size_t before = edit_.masks.size();
    edit_ = edit;
    if (animationMaxUs > 0) animationMaxUs_ = animationMaxUs;
    if (animationEdge > 0) animationEdge_ = animationEdge;
    // A mask just added becomes the selection.
    if (selectNewest_ && edit_.masks.size() > before) selected_ = edit_.masks.back().id;
    selectNewest_ = false;
    setPlayhead(playhead_);
}
void ReviewWindow::setConfig(const RecordingConfig& config) { config_ = config; if (hwnd_) ui::select(GetDlgItem(hwnd_, idFormat), config_.format); refresh(); }
void ReviewWindow::setNotice(const std::string& text) { notice_ = text; refresh(); }
void ReviewWindow::setExporting(bool exporting, const std::string& status) {
    exporting_ = exporting; status_ = status; exportFraction_ = exporting ? 0 : -1;
    if (exporting) { pause(); error_.clear(); }
    refresh();
}
void ReviewWindow::setExportProgress(double fraction) { exportFraction_ = fraction; if (exporting_) refresh(); }

RECT ReviewWindow::videoRect() const {
    RECT client; GetClientRect(view_, &client);
    const int cw = client.right, ch = client.bottom;
    if (canvas_.empty() || cw <= 0 || ch <= 0) return client;
    const double scale = std::min(double(cw) / canvas_.w, double(ch) / canvas_.h);
    const int vw = int(canvas_.w * scale), vh = int(canvas_.h * scale);
    return {(cw - vw) / 2, (ch - vh) / 2, (cw - vw) / 2 + vw, (ch - vh) / 2 + vh};
}
RecordingRect ReviewWindow::toCanvas(const RECT& r) const {
    const RECT video = videoRect();
    const double sx = double(canvas_.w) / std::max(1L, video.right - video.left), sy = double(canvas_.h) / std::max(1L, video.bottom - video.top);
    long long x0 = std::clamp<long long>(std::lround((r.left - video.left) * sx), 0, canvas_.w), y0 = std::clamp<long long>(std::lround((r.top - video.top) * sy), 0, canvas_.h);
    long long x1 = std::clamp<long long>(std::lround((r.right - video.left) * sx), 0, canvas_.w), y1 = std::clamp<long long>(std::lround((r.bottom - video.top) * sy), 0, canvas_.h);
    return {std::min(x0, x1), std::min(y0, y1), std::llabs(x1 - x0), std::llabs(y1 - y0)};
}
RECT ReviewWindow::toView(const RecordingRect& c) const {
    const RECT video = videoRect();
    const double sx = double(video.right - video.left) / std::max(1, canvas_.w), sy = double(video.bottom - video.top) / std::max(1, canvas_.h);
    return {video.left + int(c.x * sx), video.top + int(c.y * sy), video.left + int((c.x + c.w) * sx), video.top + int((c.y + c.h) * sy)};
}
RECT ReviewWindow::trackRect() const {
    RECT client; GetClientRect(timeline_, &client);
    return {scaled(8), scaled(8), client.right - scaled(8), scaled(20)};
}
void ReviewWindow::paintFrame(HDC dc, const RECT& client) {
    const auto& c = theme::colors();
    theme::fill(dc, client, theme::dark() ? RGB(0x14, 0x14, 0x14) : RGB(0x20, 0x20, 0x20));
    const RECT video = videoRect();
    if (!frame_.empty()) {
        BITMAPINFO info{};
        info.bmiHeader = {sizeof(BITMAPINFOHEADER), canvas_.w, -canvas_.h, 1, 32, BI_RGB};
        SetStretchBltMode(dc, HALFTONE);
        SetBrushOrgEx(dc, 0, 0, nullptr);
        StretchDIBits(dc, video.left, video.top, video.right - video.left, video.bottom - video.top, 0, 0, canvas_.w, canvas_.h, frame_.data(), &info, DIB_RGB_COLORS, SRCCOPY);
    }
    // Outlines: masks active at the playhead, the selection in the accent colour.
    for (const auto& m : edit_.masks) {
        const bool active = playhead_ >= m.start && playhead_ < m.end, chosen = m.id == selected_;
        if (!active && !chosen) continue;
        const RECT r = toView(m.rect);
        HPEN pen = CreatePen(chosen ? PS_SOLID : PS_DOT, chosen ? scaled(2) : 1, chosen ? c.accent : (m.kind == "blur" ? RGB(0x9B, 0xBC, 0xFF) : RGB(0xC8, 0xC8, 0xC8)));
        HGDIOBJ oldPen = SelectObject(dc, pen), oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
        Rectangle(dc, r.left, r.top, r.right, r.bottom);
        SelectObject(dc, oldPen); SelectObject(dc, oldBrush); DeleteObject(pen);
    }
    if (drag_ == Drag::Mask) {
        // The pending area as a dashed rubber band.
        HPEN pen = CreatePen(PS_DASH, 1, RGB(0xFF, 0xFF, 0xFF));
        HGDIOBJ oldPen = SelectObject(dc, pen), oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
        SetBkMode(dc, OPAQUE); SetBkColor(dc, RGB(0, 0, 0));
        Rectangle(dc, std::min(dragFrom_.x, dragTo_.x), std::min(dragFrom_.y, dragTo_.y), std::max(dragFrom_.x, dragTo_.x), std::max(dragFrom_.y, dragTo_.y));
        SelectObject(dc, oldPen); SelectObject(dc, oldBrush); DeleteObject(pen);
    }
    if (!armed_.empty() && drag_ != Drag::Mask) {
        const std::wstring hint = armed_ == "blur" ? L"Drag on the frame to place the blur. Esc cancels." : L"Drag on the frame to place the cover. Esc cancels.";
        RECT band{client.left, client.top, client.right, client.top + scaled(28)};
        theme::fill(dc, band, RGB(0, 0, 0));
        theme::drawText(dc, hint, band, RGB(0xFF, 0xFF, 0xFF), ui::font(ui::FontRole::Normal, dpi_), DT_SINGLELINE | DT_CENTER | DT_VCENTER);
    }
}
void ReviewWindow::paintTimeline(HDC dc, const RECT& client) {
    const auto& c = theme::colors();
    theme::fill(dc, client, c.content);
    const RECT track = trackRect();
    const int width = track.right - track.left;
    auto x = [&](long long us) { return recording::xAt(us, track.left, width, duration_); };
    theme::roundRect(dc, track, scaled(4), c.input, c.inputBorder);
    // The kept part between the trim handles; a handle being dragged shows where it goes.
    long long in = edit_.trimStart, out = edit_.exportEnd(duration_);
    if (drag_ == Drag::TrimIn) in = std::min(dragTime_, out - 1);
    if (drag_ == Drag::TrimOut) out = std::max(dragTime_, in + 1);
    theme::roundRect(dc, {x(in), track.top, x(out), track.bottom}, scaled(4), theme::blend(c.input, c.accent, 90));
    for (long long t : {in, out}) theme::fill(dc, {x(t) - scaled(2), track.top - scaled(4), x(t) + scaled(2), track.bottom + scaled(4)}, c.accent);
    // Mask bars, a lane per overlapping mask.
    const auto lanes = recording::maskLanes(edit_.masks);
    for (size_t i = 0; i < edit_.masks.size(); ++i) {
        const auto& m = edit_.masks[i];
        long long start = m.start, end = m.end;
        if (m.id == selected_ && drag_ == Drag::MaskIn) start = std::min(dragTime_, end - 1);
        if (m.id == selected_ && drag_ == Drag::MaskOut) end = std::max(dragTime_, start + 1);
        const int top = track.bottom + scaled(6) + lanes[i] * scaled(10);
        const COLORREF fill = m.kind == "blur" ? theme::blend(c.content, c.accent, 140) : c.textSecondary;
        theme::roundRect(dc, {x(start), top, std::max(x(end), x(start) + scaled(3)), top + scaled(7)}, scaled(3), m.id == selected_ ? c.accent : fill);
    }
    // The playhead.
    const int px = x(drag_ == Drag::Scrub ? dragTime_ : playhead_);
    theme::fill(dc, {px - 1, track.top - scaled(6), px + 1, client.bottom - scaled(2)}, c.text);
}
LRESULT ReviewWindow::frameMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: buffered(hwnd, [this](HDC dc, const RECT& r) { paintFrame(dc, r); }); return 0;
    case WM_LBUTTONDOWN: {
        const POINT p{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        SetFocus(hwnd_);
        if (exporting_) return 0;
        if (!armed_.empty()) { drag_ = Drag::Mask; dragFrom_ = dragTo_ = p; SetCapture(hwnd); return 0; }
        // A click selects the topmost active mask under it, or clears the selection.
        std::string hit;
        for (const auto& m : edit_.masks) {
            if (playhead_ < m.start || playhead_ >= m.end) continue;
            const RECT r = toView(m.rect);
            if (PtInRect(&r, p)) hit = m.id;
        }
        selectMask(hit);
        return 0;
    }
    case WM_MOUSEMOVE: if (drag_ == Drag::Mask) { dragTo_ = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)}; InvalidateRect(hwnd, nullptr, FALSE); } return 0;
    case WM_LBUTTONUP:
        if (drag_ == Drag::Mask) {
            ReleaseCapture();
            drag_ = Drag::None;
            const RECT r{std::min(dragFrom_.x, dragTo_.x), std::min(dragFrom_.y, dragTo_.y), std::max(dragFrom_.x, dragTo_.x), std::max(dragFrom_.y, dragTo_.y)};
            const auto rect = toCanvas(r);
            if (rect.w >= 4 && rect.h >= 4) addMask(armed_, rect); else InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_CAPTURECHANGED: if (drag_ == Drag::Mask && reinterpret_cast<HWND>(lParam) != hwnd) { drag_ = Drag::None; InvalidateRect(hwnd, nullptr, FALSE); } return 0;
    case WM_SETCURSOR: if (!armed_.empty() && LOWORD(lParam) == HTCLIENT) { SetCursor(LoadCursorW(nullptr, IDC_CROSS)); return TRUE; } break;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}
LRESULT ReviewWindow::timelineMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    const RECT track = trackRect();
    const int width = track.right - track.left;
    auto timeAt = [&](int x) { return recording::timeAt(x, track.left, width, duration_); };
    auto xAt = [&](long long us) { return recording::xAt(us, track.left, width, duration_); };
    switch (message) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: buffered(hwnd, [this](HDC dc, const RECT& r) { paintTimeline(dc, r); }); return 0;
    case WM_GETDLGCODE: return DLGC_WANTARROWS;
    case WM_LBUTTONDOWN: {
        if (exporting_) return 0;
        const int x = GET_X_LPARAM(lParam), y = GET_Y_LPARAM(lParam), grab = scaled(6);
        SetFocus(hwnd_);
        pause();
        if (y <= track.bottom + scaled(4)) {
            if (std::abs(x - xAt(edit_.trimStart)) <= grab) drag_ = Drag::TrimIn;
            else if (std::abs(x - xAt(edit_.exportEnd(duration_))) <= grab) drag_ = Drag::TrimOut;
            else drag_ = Drag::Scrub;
        } else {
            // Mask lanes: a selected mask's ends retime it; a bar selects its mask.
            const auto lanes = recording::maskLanes(edit_.masks);
            drag_ = Drag::Scrub;
            for (size_t i = 0; i < edit_.masks.size(); ++i) {
                const auto& m = edit_.masks[i];
                const int top = track.bottom + scaled(6) + lanes[i] * scaled(10);
                if (y < top - scaled(2) || y > top + scaled(9)) continue;
                if (m.id == selected_ && std::abs(x - xAt(m.start)) <= grab) { drag_ = Drag::MaskIn; break; }
                if (m.id == selected_ && std::abs(x - xAt(m.end)) <= grab) { drag_ = Drag::MaskOut; break; }
                if (x >= xAt(m.start) && x <= xAt(m.end)) { selectMask(m.id); drag_ = Drag::None; return 0; }
            }
        }
        dragTime_ = timeAt(x);
        SetCapture(hwnd);
        if (drag_ == Drag::Scrub) setPlayhead(dragTime_);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_MOUSEMOVE:
        if (drag_ != Drag::None && GetCapture() == hwnd) {
            dragTime_ = timeAt(GET_X_LPARAM(lParam));
            if (drag_ == Drag::Scrub) setPlayhead(dragTime_);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_LBUTTONUP: {
        if (GetCapture() != hwnd) return 0;
        ReleaseCapture();
        const auto finished = drag_;
        drag_ = Drag::None;
        const long long t = timeAt(GET_X_LPARAM(lParam));
        if (finished == Drag::TrimIn) propose({json::Object{{"op", "setTrim"}, {"start", double(std::min(t, edit_.exportEnd(duration_) - 1))}, {"end", edit_.trimEnd ? json::Value(double(*edit_.trimEnd)) : json::Value()}}});
        else if (finished == Drag::TrimOut) propose({json::Object{{"op", "setTrim"}, {"start", double(edit_.trimStart)}, {"end", double(std::max(t, edit_.trimStart + 1))}}});
        else if ((finished == Drag::MaskIn || finished == Drag::MaskOut) && selected()) {
            auto m = *selected();
            if (finished == Drag::MaskIn) m.start = std::min(t, m.end - 1); else m.end = std::max(t, m.start + 1);
            propose({json::Object{{"op", "updateMask"}, {"maskId", m.id}, {"mask", maskJson(m)}}});
        }
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}
bool ReviewWindow::preTranslate(MSG& msg) {
    if (!hwnd_ || msg.message != WM_KEYDOWN || GetForegroundWindow() != hwnd_ || dialog_) return false;
    wchar_t cls[32]{};
    GetClassNameW(GetFocus(), cls, 32);
    if (std::wstring(cls) == WC_COMBOBOXW) return false;
    const bool shift = GetKeyState(VK_SHIFT) < 0;
    switch (msg.wParam) {
    case VK_SPACE: togglePlay(); return true;
    case 'I': if (shift) setMaskTime(true); else setTrimIn(); return true;
    case 'O': if (shift) setMaskTime(false); else setTrimOut(); return true;
    case VK_DELETE: removeMask(); return true;
    case VK_ESCAPE: if (!armed_.empty()) { armed_.clear(); refresh(); } return true;
    case VK_LEFT: pause(); setPlayhead(playhead_ - (shift ? 1000000 : 100000)); return true;
    case VK_RIGHT: pause(); setPlayhead(playhead_ + (shift ? 1000000 : 100000)); return true;
    case VK_HOME: pause(); setPlayhead(edit_.trimStart); return true;
    case VK_END: pause(); setPlayhead(edit_.exportEnd(duration_) - 1); return true;
    }
    return false;
}
LRESULT CALLBACK ReviewWindow::proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    ReviewWindow* self = nullptr;
    if (message == WM_NCCREATE) { self = static_cast<ReviewWindow*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams); SetWindowLongPtrW(hwnd, GWLP_USERDATA, LONG_PTR(self)); self->hwnd_ = hwnd; }
    else self = reinterpret_cast<ReviewWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    return self ? self->handle(message, wParam, lParam) : DefWindowProcW(hwnd, message, wParam, lParam);
}
LRESULT ReviewWindow::handle(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case frameArrived: {
        std::unique_ptr<ReviewFrame> frame(reinterpret_cast<ReviewFrame*>(lParam));
        if (frame->request != request_) return 0;
        if (frame->ended) { playing_ = false; setPlayhead(frame->timeUs); return 0; }
        frame_ = std::move(frame->bits);
        frameTime_ = frame->timeUs;
        if (frame->playing) { playhead_ = frame->timeUs; ui::setTextW(GetDlgItem(hwnd_, idTime), w(recording::clockText(playhead_) + " / " + recording::clockText(duration_))); InvalidateRect(timeline_, nullptr, FALSE); }
        InvalidateRect(view_, nullptr, FALSE);
        return 0;
    }
    case openDialog: saveCopyDialog(); return 0;
    case WM_ERASEBKGND: { RECT r; GetClientRect(hwnd_, &r); theme::fill(reinterpret_cast<HDC>(wParam), r, theme::colors().content); return 1; }
    case WM_SIZE: layout(); return 0;
    case WM_GETMINMAXINFO: { auto* info = reinterpret_cast<MINMAXINFO*>(lParam); info->ptMinTrackSize = {scaled(720), scaled(420)}; return 0; }
    case WM_NOTIFY: { LRESULT result = 0; if (theme::customDraw(reinterpret_cast<NMHDR*>(lParam), result)) return result; break; }
    case WM_CTLCOLORSTATIC: case WM_CTLCOLORBTN: case WM_CTLCOLOREDIT: case WM_CTLCOLORLISTBOX: {
        const HWND control = reinterpret_cast<HWND>(lParam);
        const int id = GetDlgCtrlID(control);
        return theme::controlColor(message, reinterpret_cast<HDC>(wParam), control, id == idStatus && !error_.empty() ? 2 : id == idKept || id == idOutputLabel ? 1 : 0);
    }
    case WM_HSCROLL:
        if (reinterpret_cast<HWND>(lParam) == GetDlgItem(hwnd_, idStrength) && (LOWORD(wParam) == TB_ENDTRACK || LOWORD(wParam) == TB_THUMBPOSITION))
            setStrength(int(SendMessageW(GetDlgItem(hwnd_, idStrength), TBM_GETPOS, 0, 0)));
        return 0;
    case WM_COMMAND: {
        const int id = LOWORD(wParam), code = HIWORD(wParam);
        if (code == CBN_SELCHANGE) {
            if (id == idSpeed) { speed_ = std::stod(ui::selected(GetDlgItem(hwnd_, idSpeed))); if (playing_) { pause(); play(); } }
            else if (id == idMasks) selectMask(ui::selected(GetDlgItem(hwnd_, idMasks)));
            else if (id == idFormat) chooseFormat(ui::selected(GetDlgItem(hwnd_, idFormat)));
            return 0;
        }
        if (code != BN_CLICKED) break;
        switch (id) {
        case idPlay: togglePlay(); break;
        case idTrimIn: setTrimIn(); break;
        case idTrimOut: setTrimOut(); break;
        case idReset: resetTrim(); break;
        case idBlur: arm("blur"); break;
        case idSolid: arm("solid"); break;
        case idMaskIn: setMaskTime(true); break;
        case idMaskOut: setMaskTime(false); break;
        case idRemove: removeMask(); break;
        case idSave: saveCopyDialog(); break;
        case idDiscard: discard(); break;
        case idUpload: upload(); break;
        }
        return 0;
    }
    case WM_CLOSE: if (!closing_) discard(); return 0;
    case WM_DPICHANGED: {
        dpi_ = HIWORD(wParam);
        ui::refreshFonts(hwnd_, dpi_);
        const auto* r = reinterpret_cast<RECT*>(lParam);
        SetWindowPos(hwnd_, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_NCDESTROY: hwnd_ = nullptr; break;
    }
    return DefWindowProcW(hwnd_, message, wParam, lParam);
}

ReviewWindow::Drive ReviewWindow::drive(const json::Object& p) {
    const auto action = p.value("action").toString();
    auto refuse = [&](const std::string& message) { return Drive{"invalid_request", message, {}}; };
    if (action == "inspect") return {{}, {}, state()};
    if (dialog_ && action != "closeFileDialog") return {"recording_state", "The Save copy dialog is open", {}};
    if (exporting_) return {"recording_state", "An export is running", {}};
    error_.clear();
    const auto revision = edit_.revision;
    if (action == "seek") { if (!json::integer(p.value("time"), 0, double(duration_))) return refuse("seek takes a clip time in microseconds"); pause(); setPlayhead(p.value("time").toInteger()); }
    else if (action == "play") play();
    else if (action == "pause") pause();
    else if (action == "speed") {
        const double v = p.value("speed").toDouble(0);
        if (std::find(std::begin(speeds), std::end(speeds), v) == std::end(speeds)) return refuse("speed is 0.5, 1, 2 or 4");
        speed_ = v; ui::select(GetDlgItem(hwnd_, idSpeed), v == 0.5 ? "0.5" : std::to_string(int(v)));
        if (playing_) { pause(); play(); }
    }
    else if (action == "setIn") setTrimIn();
    else if (action == "setOut") setTrimOut();
    else if (action == "resetTrim") resetTrim();
    else if (action == "addMask") {
        const auto kind = p.value("kind").toString();
        const auto& r = p.value("rect");
        if ((kind != "blur" && kind != "solid") || !r.isArray() || r.arrayRef().size() != 4) return refuse("addMask takes kind blur or solid and rect [x, y, width, height]");
        addMask(kind, {r[size_t(0)].toInteger(), r[size_t(1)].toInteger(), r[size_t(2)].toInteger(), r[size_t(3)].toInteger()});
    }
    else if (action == "selectMask") {
        const auto& id = p.value("maskId");
        if (!id.isNull() && !id.isString()) return refuse("selectMask takes a maskId or null");
        if (id.isString() && std::none_of(edit_.masks.begin(), edit_.masks.end(), [&](const RecordingMask& m) { return m.id == id.stringRef(); })) return {"recording_mask_unknown", "No mask has this id", {}};
        selectMask(id.toString());
    }
    else if (action == "setMaskIn" || action == "setMaskOut" || action == "removeMask" || action == "strength") {
        if (!selected()) return refuse("Select a mask first");
        if (action == "setMaskIn") setMaskTime(true);
        else if (action == "setMaskOut") setMaskTime(false);
        else if (action == "removeMask") removeMask();
        else { if (!json::integer(p.value("strength"), 2, 64) || selected()->kind != "blur") return refuse("strength is 2 to 64, for a selected blur"); setStrength(p.value("strength").toInt()); }
    }
    else if (action == "format") {
        const auto format = p.value("format").toString();
        if (std::find(formats_.begin(), formats_.end(), format) == formats_.end()) return {"recording_unsupported", "This format is not available", {{"format", format}}};
        chooseFormat(format);
    }
    else if (action == "saveCopyDialog") { PostMessageW(hwnd_, openDialog, 0, 0); auto s = state(); s["fileDialog"].objectRef()["open"] = true; return {{}, {}, s}; }
    else if (action == "closeFileDialog") { if (!dialog_) return {"recording_state", "No file dialog is open", {}}; closeFileDialog(); }
    else return {"invalid_request", "Unknown review action", {{"action", action}}};
    if (!error_.empty()) return refuse(error_);
    (void)revision;
    return {{}, {}, state()};
}
json::Object ReviewWindow::state() const {
    RECT window{}; if (hwnd_) GetClientRect(hwnd_, &window);
    const RECT video = videoRect();
    RECT viewBox{}; GetWindowRect(view_, &viewBox); MapWindowPoints(nullptr, hwnd_, reinterpret_cast<POINT*>(&viewBox), 2);
    RECT timelineBox{}; GetWindowRect(timeline_, &timelineBox); MapWindowPoints(nullptr, hwnd_, reinterpret_cast<POINT*>(&timelineBox), 2);
    const RECT track = trackRect();
    const auto lanes = recording::maskLanes(edit_.masks);
    json::Array masks;
    for (size_t i = 0; i < edit_.masks.size(); ++i) {
        auto m = edit_.masks[i].toJson();
        m["bar"] = json::Object{{"lane", lanes[i]}, {"x0", recording::xAt(edit_.masks[i].start, int(track.left), int(track.right - track.left), duration_)}, {"x1", recording::xAt(edit_.masks[i].end, int(track.left), int(track.right - track.left), duration_)}};
        masks.push_back(m);
    }
    const auto* mask = selected();
    const double share = window.right > 0 && window.bottom > 0 ? double((video.right - video.left) * (video.bottom - video.top)) / double(window.right * window.bottom) : 0;
    const int laneCount = edit_.masks.empty() ? 0 : *std::max_element(lanes.begin(), lanes.end()) + 1;
    json::Object dialog{{"open", dialog_ != nullptr}, {"mode", dialog_ ? json::Value("save") : json::Value()}, {"title", dialog_ ? json::Value("Save a copy") : json::Value()},
        {"backend", "windows"}, {"parentWindow", dialog_ ? json::Value("review") : json::Value()}, {"fallbackReason", json::Value()}, {"last", dialogLast_.empty() ? json::Value() : json::Value(dialogLast_)}};
    auto n = [](LONG v) { return int(v); };
    POINT origin{0, 0}; if (hwnd_) ClientToScreen(hwnd_, &origin);
    return {{"visible", hwnd_ && IsWindowVisible(hwnd_)}, {"size", json::Array{n(window.right), n(window.bottom)}}, {"origin", json::Array{n(origin.x), n(origin.y)}}, {"playheadUs", double(playhead_)}, {"playing", playing_}, {"speed", speed_},
        {"trim", json::Object{{"start", double(edit_.trimStart)}, {"end", double(edit_.exportEnd(duration_))}}}, {"masks", masks},
        {"selectedMaskId", selected_.empty() ? json::Value() : json::Value(selected_)}, {"armed", armed_.empty() ? json::Value() : json::Value(armed_)},
        {"maskControls", json::Object{{"list", !edit_.masks.empty()}, {"times", mask != nullptr}, {"strength", mask && mask->kind == "blur"}, {"remove", mask != nullptr}}},
        {"status", ui::narrow(ui::textW(GetDlgItem(hwnd_, idStatus)))}, {"notice", notice_.empty() ? json::Value() : json::Value(notice_)},
        {"video", json::Object{{"rect", json::Array{n(viewBox.left + video.left), n(viewBox.top + video.top), n(video.right - video.left), n(video.bottom - video.top)}}, {"share", share}}},
        {"timeline", json::Object{{"rect", json::Array{n(timelineBox.left), n(timelineBox.top), n(timelineBox.right - timelineBox.left), n(timelineBox.bottom - timelineBox.top)}}, {"lanes", laneCount}}},
        {"scheme", theme::dark() ? "dark" : "light"}, {"fileDialog", dialog}, {"edit", edit_.toJson()}};
}
}
