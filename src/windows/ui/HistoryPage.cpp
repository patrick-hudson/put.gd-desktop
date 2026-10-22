// History and Activity: history.list pages with filters and a details pane, and
// operation.recent with live upload progress. Remote deletion follows
// delete.prepare -> TaskDialog -> delete.confirm -> delete.inspect.
#include "ui/MainWindow.h"
#include "common/Files.h"
#include "common/Text.h"
#include "core/Operation.h"
#include <algorithm>
#include <cwctype>
#include <map>
#include <wincodec.h>

namespace putgd::ui {
namespace {
enum : int {
    idSearch = 100, idCategory, idUpload, idLocal, idRemote, idDestination, idActivity, idEditImage, idList,
    idPrevious, idNext, idPageLabel, idPreview, idPreviewNote, idTitle, idDetails, idLinkLabel, idLink,
    idCopyLink, idOpenLink, idOpenFolder, idEditAgain, idDelete, idClearHistory, idClearCache,
    idRetry, idCancel, idCopyResult, idRetryLocal, idSaveDiagnostics
};
constexpr UINT_PTR timerSearch = 1, timerActivity = 2, timerDeletion = 3, timerRefresh = 4;
constexpr int pageSize = 50;

const GUID clsidWicFactory{0xCACAF262, 0x9370, 0x4615, {0xA1, 0x3B, 0x9F, 0x55, 0x39, 0xDA, 0x4C, 0x0A}};
const GUID iidWicFactory{0xEC5EC8A9, 0xC395, 0x4314, {0x9C, 0x77, 0x54, 0xD7, 0xA9, 0x35, 0xFF, 0x70}};
const GUID pixelFormatPbgra{0x6FDDC324, 0x4E03, 0x4BFE, {0xB1, 0x85, 0x3D, 0x77, 0x76, 0x8D, 0xC9, 0x10}};

template <class T> struct Com {
    T* p = nullptr;
    Com() = default;
    Com(const Com&) = delete;
    ~Com() { if (p) p->Release(); }
    T** out() { return &p; }
    T* operator->() const { return p; }
    explicit operator bool() const { return p != nullptr; }
};

// Decodes a bounded preview PNG/JPEG with WIC, scales it to fit and flattens it
// onto the window colour as a 24-bit DIB (a static control keeps it as is).
HBITMAP decodePreview(const std::string& path, int maxWidth, int maxHeight) {
    const long long bytes = files::size(path);
    if (bytes <= 0 || bytes > 8 * 1024 * 1024 || maxWidth < 8 || maxHeight < 8) return nullptr;
    Com<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(clsidWicFactory, nullptr, CLSCTX_INPROC_SERVER, iidWicFactory, reinterpret_cast<void**>(factory.out())))) return nullptr;
    Com<IWICBitmapDecoder> decoder;
    if (FAILED(factory->CreateDecoderFromFilename(wide(path).c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, decoder.out()))) return nullptr;
    Com<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, frame.out()))) return nullptr;
    UINT width = 0, height = 0;
    if (FAILED(frame->GetSize(&width, &height)) || !width || !height || static_cast<unsigned long long>(width) * height > 16000000ULL) return nullptr;
    const double ratio = std::min({1.0, double(maxWidth) / width, double(maxHeight) / height});
    const UINT targetWidth = std::max(1U, UINT(width * ratio)), targetHeight = std::max(1U, UINT(height * ratio));
    Com<IWICBitmapScaler> scaler;
    if (FAILED(factory->CreateBitmapScaler(scaler.out())) || FAILED(scaler->Initialize(frame.p, targetWidth, targetHeight, WICBitmapInterpolationModeFant))) return nullptr;
    Com<IWICFormatConverter> converter;
    if (FAILED(factory->CreateFormatConverter(converter.out())) ||
        FAILED(converter->Initialize(scaler.p, pixelFormatPbgra, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom))) return nullptr;
    std::vector<BYTE> pixels(size_t(targetWidth) * targetHeight * 4);
    if (FAILED(converter->CopyPixels(nullptr, targetWidth * 4, UINT(pixels.size()), pixels.data()))) return nullptr;
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof info.bmiHeader;
    info.bmiHeader.biWidth = LONG(targetWidth);
    info.bmiHeader.biHeight = -LONG(targetHeight);
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 24;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bitmap || !bits) return nullptr;
    const COLORREF background = GetSysColor(COLOR_WINDOW);
    const int back[3] = {GetBValue(background), GetGValue(background), GetRValue(background)};
    const size_t stride = (size_t(targetWidth) * 3 + 3) & ~size_t(3);
    auto* out = static_cast<BYTE*>(bits);
    for (UINT y = 0; y < targetHeight; ++y)
        for (UINT x = 0; x < targetWidth; ++x) {
            const BYTE* source = &pixels[(size_t(y) * targetWidth + x) * 4];
            BYTE* target = out + y * stride + size_t(x) * 3;
            for (int c = 0; c < 3; ++c) target[c] = BYTE(std::min(255, source[c] + back[c] * (255 - source[3]) / 255));
        }
    return bitmap;
}

std::wstring statusLabel(const std::vector<std::pair<const char*, const wchar_t*>>& table, const std::string& value) {
    for (const auto& [key, text] : table) if (value == key) return text;
    if (value.empty()) return L"\x2014";
    auto readable = wide(text::replace(value, "_", " "));
    if (!readable.empty()) readable[0] = towupper(readable[0]);
    return readable;
}
const std::vector<std::pair<const char*, const wchar_t*>> uploadLabels{
    {"uploaded", L"Uploaded"}, {"failed", L"Upload failed"}, {"local_only", L"Local capture"}, {"pending", L"Waiting to upload"},
    {"uploading", L"Uploading"}, {"unknown", L"Upload outcome unknown"}, {"not_requested", L"No upload requested"}, {"cancelled", L"Cancelled"},
    {"stored", L"Waiting to upload"}, {"private", L"Private upload"}};
const std::vector<std::pair<const char*, const wchar_t*>> localLabels{
    {"saved", L"Saved locally"}, {"failed", L"Local save failed"}, {"disabled", L"No local copy"}, {"pending", L"Waiting for local save"}};
const std::vector<std::pair<const char*, const wchar_t*>> remoteLabels{
    {"present", L"Remote file available"}, {"removed", L"Deleted from destination"}, {"already_missing", L"Remote file already missing"},
    {"missing", L"Remote file already missing"}, {"failed", L"Remote deletion failed"}, {"unknown", L"Remote outcome unknown"},
    {"not_requested", L"Not deleted"}, {"changed", L"Remote file changed"}, {"deleting", L"Deleting from destination"},
    {"pending", L"Waiting for remote deletion"}, {"hidden_version_retained", L"Hidden; original version remains"},
    {"version_retained", L"Original version remains"}, {"superseded", L"Replaced by a newer upload"},
    {"unsupported_safe_removal", L"Safe removal not supported"}};
const std::vector<std::pair<const char*, const wchar_t*>> deletionLabels{
    {"removed", L"removed"}, {"missing", L"already missing"}, {"changed", L"changed since upload, not deleted"}, {"failed", L"failed"},
    {"unknown", L"outcome unknown"}, {"pending", L"waiting"}, {"hidden_version_retained", L"hidden; original version remains"},
    {"version_retained", L"original version remains"}, {"unsupported_safe_removal", L"safe removal not supported"}, {"cancelled", L"cancelled"}};

std::wstring progressText(const json::Object& item) {
    const auto progress = item.value("uploadProgress").toObject();
    if (progress.empty()) return statusLabel(uploadLabels, item.value("uploadStatus").toString());
    const auto transferred = progress.value("bytesTransferred").toInteger(-1), total = progress.value("bytesTotal").toInteger(-1);
    std::wstring text = statusLabel({{"preparing", L"Preparing"}, {"uploading", L"Uploading"}, {"finalizing", L"Finalizing"}}, progress.value("phase").toString());
    if (transferred >= 0 && total > 0)
        text += L" " + wide(text::byteSize(transferred)) + L" of " + wide(text::byteSize(total)) + L" (" + std::to_wstring(std::min(100LL, transferred * 100 / total)) + L"%)";
    else if (transferred >= 0) text += L" " + wide(text::byteSize(transferred));
    return text;
}
bool editableImage(const std::string& path) {
    const auto lower = text::lower(path);
    return text::endsWith(lower, ".png") || text::endsWith(lower, ".jpg") || text::endsWith(lower, ".jpeg");
}

class HistoryPageImpl final : public Page {
public:
    using Page::Page;
    ~HistoryPageImpl() override { if (bitmap_) DeleteObject(bitmap_); }
    HWND create(HWND parent) override;
    void activated() override { refresh(); }
    void settingsChanged(bool) override { fillDestinations(); }
    void invalidated(const std::string& kind) override {
        if (kind == "history" || kind == "operations") SetTimer(panel_.hwnd(), timerRefresh, 250, nullptr);
    }
    json::Object saveState() const override;
    void restoreState(const json::Object&) override;
    void showSection(const std::string& view) override { setActivity(view == "activity"); }
    void dpiChanged() override { setColumns(); panel_.layout(); }
private:
    int layout(int width, int height);
    void command(int id, int code, HWND control);
    LRESULT notify(NMHDR*);
    void timer(UINT_PTR);
    void setActivity(bool on);
    void setColumns();
    void refresh();
    void fill();
    void fillDestinations();
    void selectionChanged();
    void showRecord(const json::Object&);
    void showOperation(const json::Object&);
    void setPreview(HBITMAP, const std::wstring& note);
    void loadPreview(const json::Object&);
    json::Object selectedRecord() const;
    json::Object selectedOperation() const;
    std::vector<std::string> selectedIds() const;
    void prepareDelete();
    void pollDeletion();
    void operationAction(const std::string& method);
    void maintenance(bool cache);
    std::wstring nameOf(const std::string& historyId) const;

    Panel panel_;
    HWND search_{}, category_{}, upload_{}, local_{}, remote_{}, destination_{}, activity_{}, editImage_{}, list_{};
    HWND previous_{}, next_{}, pageLabel_{}, preview_{}, previewNote_{}, title_{}, details_{}, linkLabel_{}, link_{};
    HWND copyLink_{}, openLink_{}, openFolder_{}, editAgain_{}, delete_{}, clearHistory_{}, clearCache_{};
    HWND retry_{}, cancel_{}, copyResult_{}, retryLocal_{}, saveDiagnostics_{};
    json::Array records_, operations_;
    long long total_ = 0;
    int offset_ = 0;
    bool activityMode_ = false, loading_ = false, again_ = false, populating_ = false;
    unsigned long long generation_ = 0, previewGeneration_ = 0;
    std::string selectedId_, deletionId_, previewKey_;
    HBITMAP bitmap_ = nullptr;
};

HWND HistoryPageImpl::create(HWND parent) {
    panel_.onLayout = [this](int w, int h) { return layout(w, h); };
    panel_.onCommand = [this](int id, int code, HWND control) { command(id, code, control); };
    panel_.onNotify = [this](NMHDR* header) { return notify(header); };
    panel_.onTimer = [this](UINT_PTR id) { timer(id); };
    HWND p = panel_.create(parent, false);
    search_ = edit(p, idSearch);
    setCue(search_, L"Search history by file name");
    activity_ = check(p, idActivity, L"Show current activity");
    editImage_ = button(p, idEditImage, L"Edit image\x2026");
    setTooltip(main_.tooltip(), editImage_, L"Edit the clipboard image or an image file, then upload it.");
    category_ = combo(p, idCategory);
    setChoices(category_, {{L"All types", ""}, {L"Images", "images"}, {L"Videos", "videos"}, {L"Text", "text"}, {L"Other files", "other"}});
    upload_ = combo(p, idUpload);
    setChoices(upload_, {{L"Any upload state", ""}, {L"Uploaded", "uploaded"}, {L"Upload failed", "failed"}, {L"Local capture", "local_only"},
                         {L"Waiting to upload", "pending"}, {L"Uploading", "uploading"}, {L"Upload outcome unknown", "unknown"}, {L"Cancelled", "cancelled"}});
    local_ = combo(p, idLocal);
    setChoices(local_, {{L"Any local copy", ""}, {L"Saved locally", "saved"}, {L"Local save failed", "failed"}, {L"No local copy", "disabled"}, {L"Waiting for local save", "pending"}});
    remote_ = combo(p, idRemote);
    setChoices(remote_, {{L"Any remote state", ""}, {L"Remote file available", "present"}, {L"Deleted from destination", "removed"},
                         {L"Remote file already missing", "already_missing"}, {L"Remote deletion failed", "failed"}, {L"Remote outcome unknown", "unknown"},
                         {L"Not deleted", "not_requested"}});
    destination_ = combo(p, idDestination);
    fillDestinations();
    list_ = listView(p, idList);
    setColumns();
    previous_ = button(p, idPrevious, L"Previous");
    next_ = button(p, idNext, L"Next");
    pageLabel_ = ui::label(p, L"", idPageLabel);
    clearHistory_ = button(p, idClearHistory, L"Clear history\x2026");
    setTooltip(main_.tooltip(), clearHistory_, L"Remove local history records. Remote objects and your local files stay.");
    clearCache_ = button(p, idClearCache, L"Clean artifacts\x2026");
    setTooltip(main_.tooltip(), clearCache_, L"Remove retry artifacts and previews. Remote objects and your local files stay.");
    preview_ = imageBox(p, idPreview);
    previewNote_ = ui::label(p, L"Select a capture to preview", idPreviewNote, SS_CENTER);
    panel_.markMuted(previewNote_);
    title_ = ui::label(p, L"", idTitle, SS_ENDELLIPSIS);
    setFont(title_, FontRole::Bold, panel_.dpi());
    details_ = multilineReadOnly(p, idDetails);
    linkLabel_ = ui::label(p, L"Link");
    link_ = edit(p, idLink, ES_READONLY);
    copyLink_ = button(p, idCopyLink, L"Copy link");
    openLink_ = button(p, idOpenLink, L"Open link");
    openFolder_ = button(p, idOpenFolder, L"Open folder");
    editAgain_ = button(p, idEditAgain, L"Edit again");
    setTooltip(main_.tooltip(), editAgain_, L"Open the retained local copy in the editor. The result uploads as a new item.");
    delete_ = button(p, idDelete, L"Delete from destination\x2026");
    retry_ = button(p, idRetry, L"Retry upload");
    cancel_ = button(p, idCancel, L"Cancel");
    copyResult_ = button(p, idCopyResult, L"Copy result");
    retryLocal_ = button(p, idRetryLocal, L"Retry local save");
    saveDiagnostics_ = button(p, idSaveDiagnostics, L"Save diagnostics for this upload");
    setActivity(false);
    return p;
}

void HistoryPageImpl::setColumns() {
    while (SendMessageW(list_, LVM_DELETECOLUMN, 0, 0)) {}
    const UINT d = panel_.dpi();
    if (activityMode_) {
        addColumn(list_, 0, L"Action", scale(170, d));
        addColumn(list_, 1, L"Started", scale(140, d));
        addColumn(list_, 2, L"State", scale(150, d));
        addColumn(list_, 3, L"Progress", scale(240, d));
        addColumn(list_, 4, L"Files", scale(60, d));
    } else {
        addColumn(list_, 0, L"Name", scale(230, d));
        addColumn(list_, 1, L"Created", scale(135, d));
        addColumn(list_, 2, L"Size", scale(80, d));
        addColumn(list_, 3, L"Upload", scale(130, d));
        addColumn(list_, 4, L"Local copy", scale(120, d));
        addColumn(list_, 5, L"Link", scale(240, d));
    }
}

int HistoryPageImpl::layout(int width, int height) {
    const UINT d = panel_.dpi();
    Rows rows(d, scale(12, d), scale(10, d), width - scale(24, d), 0);
    const int gap = scale(8, d), fieldHeight = rows.fieldHeight(), buttonHeight = rows.buttonHeight();
    const int pad = scale(12, d), detailsWidth = scale(330, d);
    int y = rows.y();
    const int checkWidth = scale(170, d), editWidth = scale(130, d);
    place(search_, pad, y, width - 2 * pad - checkWidth - editWidth - 2 * gap, fieldHeight);
    place(activity_, width - pad - editWidth - gap - checkWidth, y, checkWidth, fieldHeight);
    place(editImage_, width - pad - editWidth, y - (buttonHeight - fieldHeight) / 2, editWidth, buttonHeight);
    y += buttonHeight + gap;
    const HWND filters[] = {category_, upload_, local_, remote_, destination_};
    const int filterWidth = (width - 2 * pad - 4 * gap) / 5;
    for (int i = 0; i < 5; ++i) place(filters[i], pad + i * (filterWidth + gap), y, filterWidth, fieldHeight);
    y += fieldHeight + gap;
    const int bottomY = height - pad - buttonHeight;
    const int listWidth = width - 2 * pad - detailsWidth - gap * 2;
    place(list_, pad, y, listWidth, bottomY - gap - y);
    // Paging and maintenance.
    place(previous_, pad, bottomY, scale(90, d), buttonHeight);
    place(next_, pad + scale(90, d) + gap, bottomY, scale(90, d), buttonHeight);
    place(pageLabel_, pad + 2 * (scale(90, d) + gap), bottomY + (buttonHeight - lineHeight(d)) / 2, scale(220, d), lineHeight(d) + 2);
    place(clearCache_, width - pad - scale(140, d), bottomY, scale(140, d), buttonHeight);
    place(clearHistory_, width - pad - 2 * scale(140, d) - gap, bottomY, scale(140, d), buttonHeight);
    // Details pane.
    const int x = width - pad - detailsWidth;
    int dy = y;
    const int previewHeight = scale(200, d);
    place(preview_, x, dy, detailsWidth, previewHeight);
    place(previewNote_, x, dy + previewHeight / 2 - lineHeight(d) / 2, detailsWidth, lineHeight(d) + 2);
    dy += previewHeight + gap;
    place(title_, x, dy, detailsWidth, lineHeight(d) + scale(4, d));
    dy += lineHeight(d) + scale(8, d);
    const int half = (detailsWidth - gap) / 2;
    const int buttonsHeight = (buttonHeight + gap) * (activityMode_ ? 3 : 3) + (activityMode_ ? 0 : fieldHeight + lineHeight(d) + gap);
    const int detailsHeight = std::max(scale(60, d), bottomY - gap - dy - buttonsHeight);
    place(details_, x, dy, detailsWidth, detailsHeight);
    dy += detailsHeight + gap;
    if (!activityMode_) {
        place(linkLabel_, x, dy, detailsWidth, lineHeight(d) + 2);
        dy += lineHeight(d) + scale(2, d);
        place(link_, x, dy, detailsWidth, fieldHeight);
        dy += fieldHeight + gap;
        place(copyLink_, x, dy, half, buttonHeight);
        place(openLink_, x + half + gap, dy, half, buttonHeight);
        dy += buttonHeight + gap;
        place(openFolder_, x, dy, half, buttonHeight);
        place(editAgain_, x + half + gap, dy, half, buttonHeight);
        dy += buttonHeight + gap;
        place(delete_, x, dy, detailsWidth, buttonHeight);
    } else {
        place(retry_, x, dy, half, buttonHeight);
        place(cancel_, x + half + gap, dy, half, buttonHeight);
        dy += buttonHeight + gap;
        place(copyResult_, x, dy, half, buttonHeight);
        place(retryLocal_, x + half + gap, dy, half, buttonHeight);
        dy += buttonHeight + gap;
        place(saveDiagnostics_, x, dy, detailsWidth, buttonHeight);
    }
    return height;
}

void HistoryPageImpl::setActivity(bool on) {
    const bool changed = on != activityMode_;
    activityMode_ = on;
    setChecked(activity_, on);
    for (HWND h : {category_, upload_, local_, remote_, destination_, search_, previous_, next_}) enable(h, !on);
    for (HWND h : {linkLabel_, link_, copyLink_, openLink_, openFolder_, editAgain_, delete_}) show(h, !on);
    for (HWND h : {retry_, cancel_, copyResult_, retryLocal_, saveDiagnostics_}) show(h, on);
    show(pageLabel_, !on);
    if (changed) {
        SendMessageW(list_, LVM_DELETEALLITEMS, 0, 0);
        setColumns();
        selectedId_.clear();
        selectionChanged();
    }
    panel_.layout();
    if (changed && IsWindowVisible(panel_.hwnd())) refresh();
    if (!on) KillTimer(panel_.hwnd(), timerActivity);
}

void HistoryPageImpl::fillDestinations() {
    std::vector<Choice> choices{{L"All destinations", ""}};
    for (const auto& entry : main_.session().document().value("profiles").arrayRef())
        choices.push_back({wide(entry.objectRef().value("label").toString()), entry.objectRef().value("id").toString()});
    populating_ = true;
    setChoices(destination_, choices);
    populating_ = false;
}

void HistoryPageImpl::refresh() {
    if (loading_) { again_ = true; return; }
    loading_ = true;
    const auto generation = ++generation_;
    auto done = [this, generation](const Reply& reply, bool activity) {
        loading_ = false;
        if (generation == generation_ && activity == activityMode_) {
            if (!reply.ok) main_.status(UiSession::describe(reply), true);
            else if (activity) operations_ = reply.result.value("operations").toArray();
            else { records_ = reply.result.value("records").toArray(); total_ = reply.result.value("total").toInteger(); }
            if (reply.ok) fill();
        }
        if (again_) { again_ = false; refresh(); }
    };
    if (activityMode_) {
        main_.session().call("operation.recent", {}, [done](const Reply& reply) { done(reply, true); });
        return;
    }
    json::Object params{{"limit", pageSize}, {"offset", offset_}};
    const auto searchText = text::trim(text(search_));
    if (!searchText.empty()) params["search"] = searchText;
    const std::pair<const char*, HWND> filters[] = {{"category", category_}, {"uploadStatus", upload_}, {"localStatus", local_}, {"remoteStatus", remote_}, {"profileId", destination_}};
    for (const auto& [key, control] : filters) if (const auto value = selected(control); !value.empty()) params[key] = value;
    main_.session().call("history.list", params, [done](const Reply& reply) { done(reply, false); });
}

void HistoryPageImpl::fill() {
    populating_ = true;
    SendMessageW(list_, WM_SETREDRAW, FALSE, 0);
    const int top = int(SendMessageW(list_, LVM_GETTOPINDEX, 0, 0));
    SendMessageW(list_, LVM_DELETEALLITEMS, 0, 0);
    int selectedRow = -1;
    bool anyActive = false;
    if (activityMode_) {
        for (size_t i = 0; i < operations_.size(); ++i) {
            const auto& op = operations_[i].objectRef();
            const bool active = Operation::activeState(op.value("state").toString());
            anyActive = anyActive || active;
            std::wstring progress;
            const auto& items = op.value("items").arrayRef();
            for (const auto& item : items) {
                if (item.objectRef().value("uploadProgress").isObject()) { progress = progressText(item.objectRef()); break; }
            }
            if (progress.empty() && !items.empty()) progress = statusLabel(uploadLabels, items.front().objectRef().value("uploadStatus").toString());
            auto state = statusLabel({}, op.value("state").toString());
            const auto phase = op.value("phase").toString();
            if (active && !phase.empty() && phase != op.value("state").toString()) state += L" (" + statusLabel({}, phase) + L")";
            const int row = addRow(list_, {MainWindow::actionLabel(op.value("action").toString()), localTime(op.value("created").toString()), state, progress,
                                           std::to_wstring(items.size())}, LPARAM(i));
            if (op.value("id").toString() == selectedId_) selectedRow = row;
        }
        if (anyActive) SetTimer(panel_.hwnd(), timerActivity, 1000, nullptr);
        else KillTimer(panel_.hwnd(), timerActivity);
    } else {
        for (size_t i = 0; i < records_.size(); ++i) {
            const auto& r = records_[i].objectRef();
            const auto link = r.value("linkAvailable").toBool() ? r.value("receipt")["directUrl"].toString() : std::string();
            const int row = addRow(list_, {wide(r.value("name").toString()), localTime(r.value("created").toString()), wide(text::byteSize(r.value("bytes").toInteger())),
                                           statusLabel(uploadLabels, r.value("uploadStatus").toString()), statusLabel(localLabels, r.value("local")["status"].toString()), wide(link)},
                                   LPARAM(i));
            if (r.value("id").toString() == selectedId_) selectedRow = row;
        }
        const long long first = total_ ? offset_ + 1 : 0, last = std::min<long long>(total_, offset_ + static_cast<long long>(records_.size()));
        setTextW(pageLabel_, std::to_wstring(first) + L"\x2013" + std::to_wstring(last) + L" of " + std::to_wstring(total_));
        enable(previous_, offset_ > 0);
        enable(next_, offset_ + pageSize < total_);
    }
    // Keep the scroll position across refreshes (Activity refreshes every second).
    if (top > 0 && SendMessageW(list_, LVM_GETITEMCOUNT, 0, 0) > top) {
        RECT item{};
        item.left = LVIR_BOUNDS;
        if (SendMessageW(list_, LVM_GETITEMRECT, 0, reinterpret_cast<LPARAM>(&item)))
            SendMessageW(list_, LVM_SCROLL, 0, LPARAM(top * (item.bottom - item.top)));
    }
    if (selectedRow >= 0) {
        LVITEMW state{};
        state.stateMask = LVIS_SELECTED | LVIS_FOCUSED;
        state.state = LVIS_SELECTED | LVIS_FOCUSED;
        SendMessageW(list_, LVM_SETITEMSTATE, WPARAM(selectedRow), reinterpret_cast<LPARAM>(&state));
        SendMessageW(list_, LVM_ENSUREVISIBLE, WPARAM(selectedRow), FALSE);
    }
    SendMessageW(list_, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(list_, nullptr, TRUE);
    populating_ = false;
    if (selectedRow < 0) selectedId_.clear();
    selectionChanged();
}

json::Object HistoryPageImpl::selectedRecord() const {
    if (activityMode_) return {};
    const auto rows = selectedRows(list_);
    if (rows.size() != 1) return {};
    const auto index = size_t(rowData(list_, rows.front()));
    return index < records_.size() ? records_[index].objectRef() : json::Object{};
}
json::Object HistoryPageImpl::selectedOperation() const {
    if (!activityMode_) return {};
    const auto rows = selectedRows(list_);
    if (rows.size() != 1) return {};
    const auto index = size_t(rowData(list_, rows.front()));
    return index < operations_.size() ? operations_[index].objectRef() : json::Object{};
}
std::vector<std::string> HistoryPageImpl::selectedIds() const {
    std::vector<std::string> ids;
    if (activityMode_) return ids;
    for (int row : selectedRows(list_)) {
        const auto index = size_t(rowData(list_, row));
        if (index < records_.size()) ids.push_back(records_[index].objectRef().value("id").toString());
    }
    return ids;
}
std::wstring HistoryPageImpl::nameOf(const std::string& historyId) const {
    for (const auto& r : records_) if (r.objectRef().value("id") == historyId.c_str()) return wide(r.objectRef().value("name").toString());
    return wide(historyId);
}

void HistoryPageImpl::selectionChanged() {
    if (activityMode_) {
        const auto op = selectedOperation();
        selectedId_ = op.value("id").toString();
        showOperation(op);
    } else {
        const auto record = selectedRecord();
        selectedId_ = record.value("id").toString();
        showRecord(record);
        enable(delete_, !selectedIds().empty());
    }
}

void HistoryPageImpl::setPreview(HBITMAP bitmap, const std::wstring& note) {
    HBITMAP previous = reinterpret_cast<HBITMAP>(SendMessageW(preview_, STM_SETIMAGE, IMAGE_BITMAP, reinterpret_cast<LPARAM>(bitmap)));
    if (previous && previous != bitmap && previous != bitmap_) DeleteObject(previous);
    if (bitmap_ && bitmap_ != bitmap) DeleteObject(bitmap_);
    bitmap_ = bitmap;
    setTextW(previewNote_, note);
    show(previewNote_, !note.empty());
}

void HistoryPageImpl::loadPreview(const json::Object& record) {
    // Refreshes keep the preview of an unchanged selection.
    const auto key = record.value("id").toString() + "\n" + record.value("previewPath").toString();
    if (!record.empty() && key == previewKey_) return;
    previewKey_ = record.empty() ? std::string() : key;
    const auto generation = ++previewGeneration_;
    const auto category = record.value("category").toString();
    if (record.empty()) { setPreview(nullptr, L"Select a capture to preview"); return; }
    if (category != "images" && category != "videos") { setPreview(nullptr, L"No preview for this file type"); return; }
    RECT box{};
    GetClientRect(preview_, &box);
    const auto path = record.value("previewPath").toString();
    if (!path.empty() && files::isRegularFile(path)) {
        HBITMAP bitmap = decodePreview(path, box.right, box.bottom);
        setPreview(bitmap, bitmap ? L"" : L"Preview unavailable");
        return;
    }
    setPreview(nullptr, L"Making a preview\x2026");
    const auto id = record.value("id").toString();
    main_.session().call("history.preview", {{"id", id}}, [this, generation, id, category](const Reply& reply) {
        if (generation != previewGeneration_ || selectedId_ != id) return;
        const auto path = reply.result.value("previewPath").toString();
        RECT box{};
        GetClientRect(preview_, &box);
        HBITMAP bitmap = reply.ok && !path.empty() ? decodePreview(path, box.right, box.bottom) : nullptr;
        setPreview(bitmap, bitmap ? L"" : L"Preview unavailable");
        if (!bitmap && reply.outcomeUnknown) previewKey_.clear();
    });
}

void HistoryPageImpl::showRecord(const json::Object& r) {
    loadPreview(r);
    if (r.empty()) {
        const auto count = selectedIds().size();
        setTextW(title_, count > 1 ? std::to_wstring(count) + L" files selected" : L"");
        setTextW(details_, count > 1 ? L"Delete from destination applies to every selected file." : L"Select a capture to inspect its files and link.");
        setText(link_, "");
        for (HWND h : {copyLink_, openLink_, openFolder_, editAgain_}) enable(h, false);
        return;
    }
    setText(title_, r.value("name").toString());
    const auto profileId = r.value("profileId").toString(r.value("receipt")["destination"]["profileId"].toString());
    const auto local = r.value("local").toObject();
    std::wstring text = L"Created: " + localTime(r.value("created").toString()) + L"\r\n";
    text += L"Size: " + wide(text::byteSize(r.value("bytes").toInteger())) + L"\r\n";
    if (!r.value("mime").toString().empty()) text += L"Type: " + wide(r.value("mime").toString()) + L"\r\n";
    text += L"Destination: " + (profileId.empty() ? std::wstring(L"None") : wide(main_.session().profileLabel(profileId))) + L"\r\n";
    text += L"Upload: " + statusLabel(uploadLabels, r.value("uploadStatus").toString()) + L"\r\n";
    text += L"Local copy: " + statusLabel(localLabels, local.value("status").toString());
    if (!local.value("path").toString().empty()) text += L"\r\n    " + wide(local.value("path").toString());
    text += L"\r\nRemote: " + statusLabel(remoteLabels, r.value("remoteStatus").toString());
    const auto error = r.value("error").toObject();
    if (!error.value("code").toString().empty())
        text += L"\r\nError: " + wide(error.value("message").toString()) + L" (" + wide(error.value("code").toString()) + L")";
    if (!local.value("error")["code"].toString().empty())
        text += L"\r\nLocal save error: " + wide(local.value("error")["message"].toString());
    setTextW(details_, text);
    const bool linkAvailable = r.value("linkAvailable").toBool() && !r.value("receipt")["directUrl"].toString().empty();
    setText(link_, linkAvailable ? r.value("receipt")["directUrl"].toString() : std::string());
    setCue(link_, linkAvailable ? L"" : L"No link");
    const bool localSaved = local.value("status") == "saved" && !local.value("path").toString().empty();
    enable(copyLink_, linkAvailable || r.value("uploadStatus") == "local_only");
    setTextW(copyLink_, r.value("uploadStatus") == "local_only" ? L"Copy result" : L"Copy link");
    enable(openLink_, linkAvailable);
    enable(openFolder_, localSaved);
    enable(editAgain_, localSaved && editableImage(local.value("path").toString()));
}

void HistoryPageImpl::showOperation(const json::Object& op) {
    loadPreview({});
    if (op.empty()) {
        setTextW(title_, L"");
        setTextW(details_, operations_.empty() ? L"No recent activity." : L"Select an operation to see its files and progress.");
        for (HWND h : {retry_, cancel_, copyResult_, retryLocal_, saveDiagnostics_}) enable(h, false);
        return;
    }
    const auto state = op.value("state").toString();
    const bool active = Operation::activeState(state);
    setTextW(title_, MainWindow::actionLabel(op.value("action").toString()) + L" \x2014 " + statusLabel({}, state));
    std::wstring text = L"Started: " + localTime(op.value("created").toString()) + L"\r\n";
    if (!op.value("phase").toString().empty()) text += L"Phase: " + statusLabel({}, op.value("phase").toString()) + L"\r\n";
    const auto destination = op.value("destination")["profileId"].toString();
    if (!destination.empty()) text += L"Destination: " + wide(main_.session().profileLabel(destination)) + L"\r\n";
    const auto error = op.value("error").toObject();
    if (!error.value("code").toString().empty()) text += L"Error: " + wide(error.value("message").toString()) + L" (" + wide(error.value("code").toString()) + L")\r\n";
    bool failedUpload = false, failedLocal = false, copyable = false;
    for (const auto& value : op.value("items").arrayRef()) {
        const auto& item = value.objectRef();
        const auto upload = item.value("uploadStatus").toString();
        failedUpload = failedUpload || upload == "failed" || upload == "unknown" || upload == "cancelled";
        failedLocal = failedLocal || item.value("local")["status"] == "failed";
        copyable = copyable || upload == "uploaded" || upload == "local_only" || item.value("local")["status"] == "saved";
        text += L"\r\n" + wide(item.value("artifact")["name"].toString("(acquiring)")) + L"\r\n    " + progressText(item);
        if (item.value("artifact")["bytes"].isNumber()) text += L" \x00B7 " + wide(text::byteSize(item.value("artifact")["bytes"].toInteger()));
        const auto itemError = item.value("error")["code"].toString();
        if (!itemError.empty()) text += L"\r\n    " + wide(item.value("error")["message"].toString()) + L" (" + wide(itemError) + L")";
    }
    setTextW(details_, text);
    enable(retry_, !active && failedUpload);
    enable(cancel_, active);
    enable(copyResult_, !active && copyable);
    enable(retryLocal_, !active && failedLocal);
    enable(saveDiagnostics_, !active && (state == "failed" || failedUpload));
}

void HistoryPageImpl::operationAction(const std::string& method) {
    const auto op = selectedOperation();
    const auto id = op.value("id").toString();
    if (id.empty()) return;
    main_.session().call(method, {{"id", id}}, [this, method](const Reply& reply) {
        if (!reply.ok) { main_.status(UiSession::describe(reply), true); return; }
        if (method == "operation.cancel") main_.status(L"Cancellation requested.");
        else if (method == "operation.copy") main_.status(L"Copied to the clipboard.");
        else main_.status(L"Retry accepted.");
        refresh();
    });
}

void HistoryPageImpl::prepareDelete() {
    const auto ids = selectedIds();
    if (ids.empty()) return;
    main_.session().call("delete.prepare", {{"ids", json::stringArray(ids)}}, [this, ids](const Reply& reply) {
        if (!reply.ok) { message(main_.hwnd(), L"put.gd", L"Remote deletion cannot start.", UiSession::describe(reply), true); return; }
        const auto targets = reply.result.value("targets").toArray();
        if (targets.empty()) { message(main_.hwnd(), L"put.gd", L"Nothing to delete.", L"The selected files have no remote object that can be deleted."); return; }
        std::wstring content;
        for (const auto& target : targets) {
            const auto& t = target.objectRef();
            const auto profile = t.value("receipt")["destination"]["profileId"].toString();
            content += L"\x2022 " + (t.value("name").isString() ? wide(t.value("name").toString()) : nameOf(t.value("historyId").toString()));
            if (!profile.empty()) content += L" (" + wide(main_.session().profileLabel(profile)) + L")";
            content += L"\n    " + wide(t.value("effect").toString()) + L"\n";
        }
        std::wstring footer = L"Local copies and history records remain.";
        if (!reply.result.value("cacheNotice").toString().empty()) footer = wide(reply.result.value("cacheNotice").toString()) + L". " + footer;
        const auto instruction = targets.size() == 1 ? std::wstring(L"Delete this file from its destination?") : L"Delete " + std::to_wstring(targets.size()) + L" files from their destinations?";
        if (!confirm(main_.hwnd(), L"Delete from destination", instruction, content, L"Delete", footer)) return;
        main_.session().call("delete.confirm", {{"ids", json::stringArray(ids)}, {"acknowledgement", reply.result.value("acknowledgement")}}, [this](const Reply& confirmed) {
            if (!confirmed.ok) { message(main_.hwnd(), L"put.gd", L"Remote deletion was not accepted.", UiSession::describe(confirmed), true); return; }
            deletionId_ = confirmed.result.value("deleteOperationId").toString();
            main_.status(L"Remote deletion accepted. Waiting for each file's outcome\x2026");
            SetTimer(panel_.hwnd(), timerDeletion, 1000, nullptr);
        });
    });
}

void HistoryPageImpl::pollDeletion() {
    if (deletionId_.empty()) { KillTimer(panel_.hwnd(), timerDeletion); return; }
    KillTimer(panel_.hwnd(), timerDeletion);
    const auto id = deletionId_;
    main_.session().call("delete.inspect", {{"id", id}}, [this, id](const Reply& reply) {
        if (id != deletionId_) return;
        if (!reply.ok) {
            if (reply.outcomeUnknown) { SetTimer(panel_.hwnd(), timerDeletion, 2000, nullptr); return; }
            main_.status(UiSession::describe(reply), true);
            deletionId_.clear();
            return;
        }
        const auto deletion = reply.result.contains("items") ? reply.result : reply.result.value("deletion").toObject();
        const auto state = deletion.value("state").toString();
        std::map<std::string, int> counts;
        bool waiting = state == "accepted" || state == "running" || state == "deleting" || state == "pending";
        std::wstring outcome;
        for (const auto& value : deletion.value("items").arrayRef()) {
            const auto& item = value.objectRef();
            const auto status = item.value("status").toString();
            ++counts[status];
            waiting = waiting || status == "pending" || status == "deleting";
            outcome += L"\x2022 " + nameOf(item.value("historyId").toString()) + L": " + statusLabel(deletionLabels, status);
            if (!item.value("error")["code"].toString().empty()) outcome += L" (" + wide(item.value("error")["message"].toString()) + L")";
            outcome += L"\n";
        }
        std::wstring summary = L"Remote deletion: ";
        bool first = true;
        for (const auto& [status, count] : counts) { summary += (first ? L"" : L", ") + std::to_wstring(count) + L" " + statusLabel(deletionLabels, status); first = false; }
        main_.status(summary);
        if (waiting) { SetTimer(panel_.hwnd(), timerDeletion, 1000, nullptr); return; }
        deletionId_.clear();
        refresh();
        message(main_.hwnd(), L"Delete from destination", L"Remote deletion finished.", outcome + L"\nDeletion ID: " + wide(id) + L"\nBrowser and CDN caches may still serve older copies.");
    });
}

void HistoryPageImpl::maintenance(bool cache) {
    const bool accepted = cache
        ? confirm(main_.hwnd(), L"Clean artifacts", L"Remove retry artifacts and previews?", L"Failed uploads can no longer be retried. Remote objects and your local files are not touched.", L"Clean")
        : confirm(main_.hwnd(), L"Clear history", L"Clear all history records?", L"This removes the local records only. Remote objects and your local files are not touched.", L"Clear history");
    if (!accepted) return;
    main_.session().call(cache ? "cache.clear" : "history.clear", {}, [this, cache](const Reply& reply) {
        if (!reply.ok) { main_.status(UiSession::describe(reply), true); return; }
        if (cache) main_.status(L"Artifact cleanup finished: " + std::to_wstring(reply.result.value("removedOperations").toInteger()) + L" operations cleaned.");
        else main_.status(L"History cleared.");
        offset_ = 0;
        refresh();
    });
}

void HistoryPageImpl::command(int id, int code, HWND control) {
    if (populating_) return;
    switch (id) {
    case idSearch:
        if (code == EN_CHANGE) SetTimer(panel_.hwnd(), timerSearch, 300, nullptr);
        break;
    case idCategory: case idUpload: case idLocal: case idRemote: case idDestination:
        if (code == CBN_SELCHANGE) { offset_ = 0; refresh(); }
        break;
    case idActivity:
        if (code == BN_CLICKED) setActivity(checked(activity_));
        break;
    case idEditImage: {
        RECT r{};
        GetWindowRect(editImage_, &r);
        main_.showEditMenu({r.left, r.bottom});
        break;
    }
    case idPrevious: offset_ = std::max(0, offset_ - pageSize); refresh(); break;
    case idNext: if (offset_ + pageSize < total_) { offset_ += pageSize; refresh(); } break;
    case idCopyLink: {
        const auto r = selectedRecord();
        if (r.empty()) break;
        if (r.value("uploadStatus") == "local_only") {
            main_.session().call("operation.copy", {{"id", r.value("operationId")}}, [this](const Reply& reply) {
                main_.status(reply.ok ? L"Copied to the clipboard." : UiSession::describe(reply), !reply.ok);
            });
        } else {
            main_.session().call("history.copyLink", {{"id", r.value("id")}}, [this](const Reply& reply) {
                main_.status(reply.ok ? L"Link copied." : UiSession::describe(reply), !reply.ok);
            });
        }
        break;
    }
    case idOpenLink: {
        const auto r = selectedRecord();
        if (r.empty()) break;
        main_.session().call("history.openLink", {{"id", r.value("id")}}, [this](const Reply& reply) {
            if (!reply.ok) main_.status(UiSession::describe(reply), true);
            else if (!reply.result.value("opened").toBool(true)) main_.status(L"No browser could open the link.", true);
        });
        break;
    }
    case idOpenFolder: revealInExplorer(selectedRecord().value("local")["path"].toString()); break;
    case idEditAgain: {
        const auto path = selectedRecord().value("local")["path"].toString();
        if (!path.empty()) main_.openEditor("file", path);
        break;
    }
    case idDelete: prepareDelete(); break;
    case idClearHistory: maintenance(false); break;
    case idClearCache: maintenance(true); break;
    case idRetry: operationAction("operation.retry"); break;
    case idCancel: operationAction("operation.cancel"); break;
    case idCopyResult: operationAction("operation.copy"); break;
    case idRetryLocal: operationAction("operation.retryLocalSave"); break;
    case idSaveDiagnostics: {
        const auto id = selectedOperation().value("id").toString();
        if (id.empty()) break;
        main_.status(L"Saving diagnostics for this upload\x2026");
        main_.session().call("diagnostics.bundle", {{"operation", id}}, [this](const Reply& reply) {
            if (!reply.ok) { main_.status(UiSession::describe(reply), true); return; }
            const auto path = reply.result.value("path").toString();
            main_.status(L"Diagnostics saved to " + wide(path));
            revealInExplorer(path);
        });
        break;
    }
    }
}

LRESULT HistoryPageImpl::notify(NMHDR* header) {
    if (header->hwndFrom != list_) return 0;
    if (header->code == LVN_ITEMCHANGED) {
        const auto* change = reinterpret_cast<NMLISTVIEW*>(header);
        if (!populating_ && (change->uChanged & LVIF_STATE) && ((change->uNewState ^ change->uOldState) & LVIS_SELECTED)) selectionChanged();
    } else if (header->code == NM_DBLCLK && !activityMode_) {
        const auto r = selectedRecord();
        if (r.value("linkAvailable").toBool()) command(idOpenLink, BN_CLICKED, nullptr);
    } else if (header->code == LVN_KEYDOWN) {
        const auto* key = reinterpret_cast<NMLVKEYDOWN*>(header);
        if (key->wVKey == VK_F5) refresh();
        else if (key->wVKey == VK_DELETE && !activityMode_) prepareDelete();
    }
    return 0;
}

void HistoryPageImpl::timer(UINT_PTR id) {
    if (id == timerSearch) { KillTimer(panel_.hwnd(), timerSearch); offset_ = 0; refresh(); }
    else if (id == timerRefresh) { KillTimer(panel_.hwnd(), timerRefresh); if (IsWindowVisible(panel_.hwnd())) refresh(); }
    else if (id == timerActivity) { if (activityMode_ && IsWindowVisible(panel_.hwnd())) refresh(); else KillTimer(panel_.hwnd(), timerActivity); }
    else if (id == timerDeletion) pollDeletion();
}

json::Object HistoryPageImpl::saveState() const {
    json::Object filters{{"offset", offset_}, {"limit", pageSize}};
    const std::pair<const char*, HWND> controls[] = {{"category", category_}, {"uploadStatus", upload_}, {"localStatus", local_}, {"remoteStatus", remote_}, {"profileId", destination_}};
    for (const auto& [key, control] : controls) if (const auto value = selected(control); !value.empty()) filters[key] = value;
    json::Object state{{"query", text(search_)}, {"filters", filters}};
    if (!selectedId_.empty() && !activityMode_) state["selectedIds"] = json::Array{selectedId_};
    if (!deletionId_.empty()) state["deletionId"] = deletionId_;
    return state;
}

void HistoryPageImpl::restoreState(const json::Object& state) {
    if (state.empty()) return;
    populating_ = true;
    setText(search_, state.value("query").toString());
    const auto filters = state.value("filters").toObject();
    select(category_, filters.value("category").toString());
    select(upload_, filters.value("uploadStatus").toString());
    select(local_, filters.value("localStatus").toString());
    select(remote_, filters.value("remoteStatus").toString());
    select(destination_, filters.value("profileId").toString());
    offset_ = std::max(0, int(filters.value("offset").toInteger()));
    const auto ids = state.value("selectedIds").toArray();
    if (!ids.empty()) selectedId_ = ids.front().toString();
    populating_ = false;
    deletionId_ = state.value("deletionId").toString();
    if (!deletionId_.empty()) SetTimer(panel_.hwnd(), timerDeletion, 500, nullptr);
}
}

std::unique_ptr<Page> createHistoryPage(MainWindow& main) { return std::make_unique<HistoryPageImpl>(main); }
}
