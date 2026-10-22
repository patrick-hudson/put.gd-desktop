// History and Activity: history.list pages as a list or a gallery of thumbnails,
// with filters (type, states, destination, dates and content type) and a details
// pane, and operation.recent with live upload progress. Remote deletion follows
// delete.prepare -> TaskDialog -> delete.confirm -> delete.inspect. The view
// commands ui.historySaveDiagnostics and ui.fileDialog historySaveAs act here.
#include "ui/MainWindow.h"
#include "ui/Theme.h"
#include "common/Files.h"
#include "common/Paths.h"
#include "common/Text.h"
#include "core/HistoryFilters.h"
#include "core/Operation.h"
#include <algorithm>
#include <thread>
#include <cwctype>
#include <map>
#include <optional>
#include <wincodec.h>

namespace putgd::ui {
namespace {
enum : int {
    idSearch = 100, idCategory, idUpload, idLocal, idRemote, idDestination, idActivity, idEditImage, idList,
    idPrevious, idNext, idPageLabel, idPreview, idPreviewNote, idTitle, idDetails, idLinkLabel, idLink,
    idCopyLink, idOpenLink, idOpenFolder, idEditAgain, idDelete, idClearHistory, idClearCache,
    idRetry, idCancel, idCopyResult, idRetryLocal, idSaveDiagnostics, idShare, idRevoke, idInspectLink,
    idViewMode, idMoreFilters, idFrom, idTo, idMime, idSaveAs
};
constexpr UINT_PTR timerSearch = 1, timerActivity = 2, timerDeletion = 3, timerRefresh = 4, timerShare = 5;
// Gallery tiles (logical pixels): the tile, and the thumbnail inside it.
constexpr int tileWidth = 172, tileHeight = 160, thumbWidth = 156, thumbHeight = 100;
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
// onto the preview placeholder's colour as a 24-bit DIB.
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
    const COLORREF background = theme::colors().placeholder;
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
// Share-link states, worded as on Linux.
const std::vector<std::pair<const char*, const wchar_t*>> sharingLabels{
    {"shared", L"Shared link"}, {"not_requested", L"No share link requested"}, {"private", L"Private upload"}, {"pending", L"Creating share link"},
    {"revoked", L"Share link revoked"}, {"failed", L"Sharing failed"}, {"unknown", L"Sharing outcome unknown"}, {"changed", L"Share link changed"}};
// "Link expires on …" / "Link expired on …" for a confirmed shared or private link.
std::wstring linkExpiry(const json::Object& record) {
    const auto expires = record.value("shareExpiresAt").toString();
    const auto status = record.value("shareStatus").toString();
    const long long at = files::parseIsoUtcMs(expires);
    if (at < 0 || (status != "shared" && status != "private")) return {};
    const auto when = localTime(expires);
    if (when.empty()) return {};
    if (at <= files::parseIsoUtcMs(files::nowIsoUtc())) return L"Link expired on " + when;
    return record.value("linkAvailable").toBool() ? L"Link expires on " + when : std::wstring();
}
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
// The preview: a rounded placeholder with the image centred, or a glyph and the
// note text (previewNote_ holds the text; this control draws it).
LRESULT CALLBACK previewProc(HWND h, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR data) {
    if (message == WM_ERASEBKGND) return 1;
    if (message == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC target = BeginPaint(h, &ps);
        RECT rc{};
        GetClientRect(h, &rc);
        if (rc.right > 0 && rc.bottom > 0) {
            HDC dc = CreateCompatibleDC(target);
            HBITMAP canvas = CreateCompatibleBitmap(target, rc.right, rc.bottom);
            HGDIOBJ old = SelectObject(dc, canvas);
            const auto& c = theme::colors();
            const UINT d = windowDpi(h);
            theme::fill(dc, rc, theme::backgroundOf(GetParent(h)));
            theme::roundRect(dc, rc, scale(6, d), c.placeholder);
            HBITMAP image = reinterpret_cast<HBITMAP>(SendMessageW(h, STM_GETIMAGE, IMAGE_BITMAP, 0));
            BITMAP info{};
            if (image && GetObjectW(image, sizeof info, &info)) {
                HDC source = CreateCompatibleDC(dc);
                HGDIOBJ previous = SelectObject(source, image);
                BitBlt(dc, (rc.right - info.bmWidth) / 2, (rc.bottom - info.bmHeight) / 2, info.bmWidth, info.bmHeight, source, 0, 0, SRCCOPY);
                SelectObject(source, previous);
                DeleteDC(source);
            } else {
                const int size = scale(36, d);
                const int top = rc.bottom / 2 - size - scale(2, d);
                theme::drawGlyph(dc, "images", (rc.right - size) / 2, top, size, c.textMuted, c.divider);
                const std::wstring note = textW(reinterpret_cast<HWND>(data));
                theme::drawText(dc, note, {scale(8, d), rc.bottom / 2 + scale(8, d), rc.right - scale(8, d), rc.bottom / 2 + scale(8, d) + lineHeight(d) * 2}, c.textSecondary,
                                font(FontRole::Normal, d), DT_CENTER | DT_WORDBREAK | DT_NOPREFIX);
            }
            BitBlt(target, 0, 0, rc.right, rc.bottom, dc, 0, 0, SRCCOPY);
            SelectObject(dc, old);
            DeleteObject(canvas);
            DeleteDC(dc);
        }
        EndPaint(h, &ps);
        return 0;
    }
    return DefSubclassProc(h, message, wParam, lParam);
}
#ifndef LVN_GETEMPTYMARKUP
#define LVN_GETEMPTYMARKUP (LVN_FIRST - 87)
#define EMF_CENTERED 0x00000001
typedef struct tagNMLVEMPTYMARKUP { NMHDR hdr; DWORD dwFlags; WCHAR szMarkup[2084]; } NMLVEMPTYMARKUP;
#endif

// A filter's natural width: its first (default) choice plus room for the chevron.
int comboWidth(HWND combo) {
    const int length = int(SendMessageW(combo, CB_GETLBTEXTLEN, 0, 0));
    std::wstring text(size_t(std::max(0, length)) + 1, L'\0');
    if (length > 0) SendMessageW(combo, CB_GETLBTEXT, 0, reinterpret_cast<LPARAM>(text.data()));
    text.resize(size_t(std::max(0, length)));
    HDC dc = GetDC(combo);
    HGDIOBJ old = SelectObject(dc, reinterpret_cast<HFONT>(SendMessageW(combo, WM_GETFONT, 0, 0)));
    SIZE size{};
    GetTextExtentPoint32W(dc, text.c_str(), int(text.size()), &size);
    SelectObject(dc, old);
    ReleaseDC(combo, dc);
    return size.cx + scale(52, windowDpi(combo));
}

// A static label's single-line text width in its own font.
int textWidth(HWND label) {
    const auto text = textW(label);
    HDC dc = GetDC(label);
    HGDIOBJ old = SelectObject(dc, reinterpret_cast<HFONT>(SendMessageW(label, WM_GETFONT, 0, 0)));
    SIZE size{};
    GetTextExtentPoint32W(dc, text.c_str(), int(text.size()), &size);
    SelectObject(dc, old);
    ReleaseDC(label, dc);
    return size.cx + 2;
}

// The details text shows a scroll bar only when it overflows.
void fitScrollBar(HWND edit) {
    RECT r{};
    GetClientRect(edit, &r);
    const int lines = int(SendMessageW(edit, EM_GETLINECOUNT, 0, 0));
    const int line = std::max(1, lineHeight(windowDpi(edit)));
    ShowScrollBar(edit, SB_VERT, lines * line > r.bottom + line / 2);
}

// A calendar day typed in the user's time zone, as the UTC instant that starts or ends it.
std::optional<std::string> dayBound(const std::string& raw, bool end) {
    const auto day = historyfilter::parseDay(raw);
    if (!day) return std::nullopt;
    SYSTEMTIME local{WORD(day->year), WORD(day->month), 0, WORD(day->day), WORD(end ? 23 : 0), WORD(end ? 59 : 0), WORD(end ? 59 : 0), WORD(end ? 999 : 0)};
    SYSTEMTIME utc{};
    if (!TzSpecificLocalTimeToSystemTime(nullptr, &local, &utc)) return std::nullopt;
    return historyfilter::isoUtc(utc.wYear, utc.wMonth, utc.wDay, utc.wHour, utc.wMinute, utc.wSecond, utc.wMilliseconds);
}
bool failedUpload(const json::Object& record) {
    const auto status = record.value("uploadStatus").toString();
    return status == "failed" || status == "unknown";
}

bool editableImage(const std::string& path) {
    const auto lower = text::lower(path);
    return text::endsWith(lower, ".png") || text::endsWith(lower, ".jpg") || text::endsWith(lower, ".jpeg");
}

class HistoryPageImpl final : public Page {
public:
    using Page::Page;
    ~HistoryPageImpl() override { if (bitmap_) DeleteObject(bitmap_); } // the list view owns the image list it shows
    HWND create(HWND parent) override;
    void activated() override { refresh(); }
    void settingsChanged(bool) override { fillDestinations(); }
    void invalidated(const std::string& kind) override {
        if (kind == "history" || kind == "operations") SetTimer(panel_.hwnd(), timerRefresh, 250, nullptr);
    }
    json::Object saveState() const override;
    void restoreState(const json::Object&) override;
    void showSection(const std::string& view) override { setActivity(view == "activity"); }
    void dpiChanged() override { setColumns(); resetGallery(); setGridMode(grid_); panel_.layout(); }
    bool viewCommand(const std::string& name, const json::Object& params, const CommandReply& answer) override;
private:
    int layout(int width, int height);
    void command(int id, int code, HWND control);
    LRESULT notify(NMHDR*);
    void timer(UINT_PTR);
    void setActivity(bool on);
    void setColumns();
    void fitColumns();
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
    void loadDetail(const std::string& id);
    void changeSharing(const std::string& action);
    void pollSharing();
    void showSharing(bool on);
    void updateSharingButtons(const json::Object& record);
    // Gallery and the extra filters.
    void setGridMode(bool grid);
    void setFiltersExpanded(bool expanded);
    bool readFilters(json::Object& params);
    int galleryIndex(const json::Object&);
    void resetGallery();
    // Save as…, Save diagnostics for this upload, and the view commands that click them.
    void saveAs();
    void saveDiagnostics(const std::string& operationId, CommandReply answer);
    int rowOf(const std::string& id) const;
    void selectRow(int row);
    void saveAsCommand(const std::string& action, const std::string& id, const CommandReply& answer);
    void saveDiagnosticsCommand(const std::string& id, const CommandReply& answer);

    Panel panel_;
    HWND search_{}, category_{}, upload_{}, local_{}, remote_{}, destination_{}, activity_{}, editImage_{}, list_{};
    HWND previous_{}, next_{}, pageLabel_{}, preview_{}, previewNote_{}, title_{}, details_{}, linkLabel_{}, link_{};
    HWND copyLink_{}, openLink_{}, openFolder_{}, editAgain_{}, delete_{}, clearHistory_{}, clearCache_{};
    HWND retry_{}, cancel_{}, copyResult_{}, retryLocal_{}, saveDiagnostics_{};
    HWND viewMode_{}, moreFilters_{}, fromLabel_{}, from_{}, toLabel_{}, to_{}, mimeLabel_{}, mime_{}, filterError_{}, saveAs_{};
    bool grid_ = false, filtersExpanded_ = false, savingDiagnostics_ = false;
    // Gallery thumbnails, kept across refreshes (keyed by record and preview) and
    // made again for another scheme or DPI.
    HIMAGELIST gallery_ = nullptr;
    std::map<std::string, int> galleryImages_;
    bool galleryDark_ = false;
    UINT galleryDpi_ = 0;
    FileChooser saveAsChooser_;
    HIMAGELIST thumbnails_ = nullptr;
    int thumbnailIndex(const json::Object&);
    void setRowImage(int row, int image);
    void resetThumbnails();
    json::Array records_, operations_;
    long long total_ = 0;
    int offset_ = 0;
    bool activityMode_ = false, loading_ = false, again_ = false, populating_ = false;
    unsigned long long generation_ = 0, previewGeneration_ = 0;
    std::string selectedId_, deletionId_, previewKey_;
    HBITMAP bitmap_ = nullptr;
    // history.detail of the selected record (sharing state and provider capabilities).
    json::Object detail_;
    unsigned long long detailGeneration_ = 0;
    // Share-link actions: history.share / revokeLink / inspectLink, then share.inspect polls.
    HWND share_{}, revoke_{}, inspectLink_{};
    std::string sharingId_;
    bool sharingVisible_ = false, sharingAccepting_ = false, sharingActive_ = false, sharingPolling_ = false, sharingStopped_ = false;
};

HWND HistoryPageImpl::create(HWND parent) {
    panel_.onLayout = [this](int w, int h) { return layout(w, h); };
    panel_.onCommand = [this](int id, int code, HWND control) { command(id, code, control); };
    panel_.onNotify = [this](NMHDR* header) { return notify(header); };
    panel_.onTimer = [this](UINT_PTR id) { timer(id); };
    HWND p = panel_.create(parent, false);
    search_ = edit(p, idSearch);
    setCue(search_, L"Search history by file name");
    activity_ = theme::pivot(p, idActivity, {L"Files", L"Activity"});
    theme::setSegmented(activity_);
    setTooltip(main_.tooltip(), activity_, L"Files: everything put.gd captured or uploaded. Activity: current and recent operations with live progress.");
    editImage_ = button(p, idEditImage, L"Edit image\x2026");
    setTooltip(main_.tooltip(), editImage_, L"Edit the clipboard image or an image file, then upload it.");
    viewMode_ = theme::pivot(p, idViewMode, {L"List", L"Gallery"});
    theme::setSegmented(viewMode_);
    setTooltip(main_.tooltip(), viewMode_, L"List: one row per file with its details. Gallery: thumbnails of your captures.");
    moreFilters_ = button(p, idMoreFilters, L"More filters");
    theme::setRole(moreFilters_, theme::Role::Subtle);
    setTooltip(main_.tooltip(), moreFilters_, L"Filter by the day a capture was made and by its content type.");
    fromLabel_ = ui::label(p, L"From");
    from_ = edit(p, idFrom);
    setCue(from_, L"YYYY-MM-DD");
    toLabel_ = ui::label(p, L"To");
    to_ = edit(p, idTo);
    setCue(to_, L"YYYY-MM-DD");
    mimeLabel_ = ui::label(p, L"Content type");
    mime_ = edit(p, idMime);
    setCue(mime_, L"such as image/png");
    filterError_ = ui::label(p, L"");
    panel_.markError(filterError_, true);
    for (HWND h : {fromLabel_, toLabel_, mimeLabel_}) panel_.markMuted(h);
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
    SendMessageW(list_, LVM_SETEXTENDEDLISTVIEWSTYLE, LVS_EX_SUBITEMIMAGES, 0);
    setColumns();
    previous_ = button(p, idPrevious, L"Previous");
    next_ = button(p, idNext, L"Next");
    pageLabel_ = ui::label(p, L"", idPageLabel);
    panel_.markMuted(pageLabel_);
    clearHistory_ = button(p, idClearHistory, L"Clear history\x2026");
    theme::setRole(clearHistory_, theme::Role::Subtle);
    setTooltip(main_.tooltip(), clearHistory_, L"Remove local history records. Remote objects and your local files stay.");
    clearCache_ = button(p, idClearCache, L"Clean artifacts\x2026");
    theme::setRole(clearCache_, theme::Role::Subtle);
    setTooltip(main_.tooltip(), clearCache_, L"Remove retry artifacts and previews. Remote objects and your local files stay.");
    preview_ = imageBox(p, idPreview);
    previewNote_ = ui::label(p, L"Select a capture to preview", idPreviewNote, SS_CENTER);
    panel_.markMuted(previewNote_);
    show(previewNote_, false);
    SetWindowSubclass(preview_, previewProc, 1, reinterpret_cast<DWORD_PTR>(previewNote_));
    title_ = ui::label(p, L"", idTitle, SS_ENDELLIPSIS);
    setFont(title_, FontRole::Bold, panel_.dpi());
    details_ = multilineReadOnly(p, idDetails);
    linkLabel_ = ui::label(p, L"Link");
    setFont(linkLabel_, FontRole::SmallBold, panel_.dpi());
    panel_.markMuted(linkLabel_);
    link_ = edit(p, idLink, ES_READONLY);
    copyLink_ = button(p, idCopyLink, L"Copy link");
    openLink_ = button(p, idOpenLink, L"Open link");
    openFolder_ = button(p, idOpenFolder, L"Open folder");
    saveAs_ = button(p, idSaveAs, L"Save as\x2026");
    setTooltip(main_.tooltip(), saveAs_, L"Save a copy of the retained local file somewhere else.");
    saveAsChooser_.post = [this](std::function<void()> work) { main_.post(std::move(work)); };
    editAgain_ = button(p, idEditAgain, L"Edit again");
    setTooltip(main_.tooltip(), editAgain_, L"Open the retained local copy in the editor. The result uploads as a new item.");
    delete_ = button(p, idDelete, L"Delete from destination\x2026");
    theme::setRole(delete_, theme::Role::Danger);
    share_ = button(p, idShare, L"Create share link");
    setTooltip(main_.tooltip(), share_, L"Create a share link for this file with the destination's current link settings.");
    revoke_ = button(p, idRevoke, L"Revoke link\x2026");
    setTooltip(main_.tooltip(), revoke_, L"Revoke this file's public share link. The uploaded file remains.");
    inspectLink_ = button(p, idInspectLink, L"Inspect link");
    setTooltip(main_.tooltip(), inspectLink_, L"Ask the destination for this file's current share-link state. Nothing is created or changed.");
    for (HWND h : {share_, revoke_, inspectLink_}) show(h, false);
    retry_ = button(p, idRetry, L"Retry upload");
    cancel_ = button(p, idCancel, L"Cancel");
    copyResult_ = button(p, idCopyResult, L"Copy result");
    retryLocal_ = button(p, idRetryLocal, L"Retry local save");
    saveDiagnostics_ = button(p, idSaveDiagnostics, L"Save diagnostics for this upload");
    setTooltip(main_.tooltip(), saveDiagnostics_, L"Saves a redacted diagnostics package about this upload, without credentials or file contents.");
    setFiltersExpanded(false);
    setActivity(false);
    return p;
}

void HistoryPageImpl::setColumns() {
    while (SendMessageW(list_, LVM_DELETECOLUMN, 0, 0)) {}
    const UINT d = panel_.dpi();
    if (activityMode_) {
        addColumn(list_, 0, L"Action", scale(170, d));
        addColumn(list_, 1, L"Started", scale(132, d));
        addColumn(list_, 2, L"State", scale(130, d));
        addColumn(list_, 3, L"Progress", scale(200, d));
        addColumn(list_, 4, L"Files", scale(52, d));
        theme::setStateColumn(list_, 2);
    } else {
        addColumn(list_, 0, L"Name", scale(220, d));
        addColumn(list_, 1, L"When", scale(132, d));
        addColumn(list_, 2, L"Size", scale(68, d));
        addColumn(list_, 3, L"Destination", scale(120, d));
        addColumn(list_, 4, L"State", scale(130, d));
        theme::setStateColumn(list_, 4);
    }
    fitColumns();
}

// One column takes the width the others leave, so the table never scrolls sideways;
// the fixed columns give way proportionally in a narrow window.
void HistoryPageImpl::fitColumns() {
    RECT client{};
    GetClientRect(list_, &client);
    if (client.right <= 0) return;
    const UINT d = panel_.dpi();
    if (grid_ && !activityMode_) {
        // Gallery tiles share the width: as many columns as fit, none left over.
        const int available = int(client.right) - (GetWindowLongW(list_, GWL_STYLE) & WS_VSCROLL ? 0 : GetSystemMetrics(SM_CXVSCROLL)) - 1;
        const int columns = std::max(1, available / scale(tileWidth, d));
        LVTILEVIEWINFO info{};
        info.cbSize = sizeof info;
        info.dwMask = LVTVIM_TILESIZE;
        info.dwFlags = LVTVIF_FIXEDSIZE;
        info.sizeTile = {std::max(scale(tileWidth, d), available / columns), scale(tileHeight, d)};
        SendMessageW(list_, LVM_SETTILEVIEWINFO, 0, reinterpret_cast<LPARAM>(&info));
    }
    std::vector<int> widths = activityMode_ ? std::vector<int>{170, 132, 130, 0, 52} : std::vector<int>{0, 104, 64, 104, 116};
    const size_t flex = activityMode_ ? 3 : 0;
    const int minimum = scale(activityMode_ ? 140 : 160, d);
    const int available = int(client.right) - (GetWindowLongW(list_, GWL_STYLE) & WS_VSCROLL ? 0 : GetSystemMetrics(SM_CXVSCROLL)) - 1;
    int fixed = 0;
    for (auto& w : widths) { w = scale(w, d); fixed += w; }
    double ratio = 1.0;
    if (available - fixed < minimum && fixed > 0) ratio = std::max(0.6, double(available - minimum) / fixed);
    int used = 0;
    for (size_t i = 0; i < widths.size(); ++i) if (i != flex) { widths[i] = int(widths[i] * ratio); used += widths[i]; }
    widths[flex] = std::max(scale(80, d), available - used);
    for (size_t i = 0; i < widths.size(); ++i) SendMessageW(list_, LVM_SETCOLUMNWIDTH, WPARAM(i), widths[i]);
}

int HistoryPageImpl::layout(int width, int height) {
    const UINT d = panel_.dpi();
    Rows rows(d, 0, 0, width, 0);
    const int gap = scale(8, d), fieldHeight = rows.fieldHeight(), buttonHeight = rows.buttonHeight();
    const int margin = scale(24, d), detailsWidth = scale(320, d), column = scale(24, d);
    // Row 1: the Files | Activity segmented control; More filters, List | Gallery and Edit image at the right.
    place(activity_, margin, 0, theme::pivotWidth(activity_), buttonHeight);
    const int editWidth = buttonWidth(editImage_, 110);
    place(editImage_, width - margin - editWidth, 0, editWidth, buttonHeight);
    const int viewWidth = theme::pivotWidth(viewMode_);
    place(viewMode_, width - margin - editWidth - gap - viewWidth, 0, viewWidth, buttonHeight);
    const int moreWidth = buttonWidth(moreFilters_, 80);
    place(moreFilters_, width - margin - editWidth - 2 * gap - viewWidth - scale(4, d) - moreWidth, 0, moreWidth, buttonHeight);
    int y = buttonHeight + scale(12, d);
    // Row 2: one toolbar, search at the left and the filters at their natural widths at the right.
    const HWND filters[] = {category_, upload_, local_, remote_, destination_};
    int natural[5]{}, total = 0;
    for (int i = 0; i < 5; ++i) { natural[i] = comboWidth(filters[i]); total += natural[i]; }
    const int toolbar = width - 2 * margin;
    int searchWidth = toolbar - total - 5 * gap;
    if (searchWidth < scale(180, d)) {
        searchWidth = std::max(scale(140, d), toolbar / 5);
        const double shrink = double(toolbar - searchWidth - 5 * gap) / std::max(1, total);
        total = 0;
        for (auto& w : natural) { w = int(w * shrink); total += w; }
    } else {
        searchWidth = std::min(searchWidth, scale(360, d));
    }
    place(search_, margin, y, searchWidth, fieldHeight);
    int fx = width - margin - total - 4 * gap;
    for (int i = 0; i < 5; ++i) { place(filters[i], fx, y, natural[i], fieldHeight); fx += natural[i] + gap; }
    y += fieldHeight + scale(16, d);
    // Row 3 (More filters): the days and the content type, each caption beside its field.
    if (filtersExpanded_ && !activityMode_) {
        const int caption = lineHeight(d) + 2, captionY = y + (fieldHeight - lineHeight(d)) / 2;
        int x = margin;
        for (auto [label, field, fieldWidth] : {std::tuple{fromLabel_, from_, 132}, std::tuple{toLabel_, to_, 132}, std::tuple{mimeLabel_, mime_, 200}}) {
            const int labelWidth = textWidth(label);
            place(label, x, captionY, labelWidth, caption);
            x += labelWidth + scale(8, d);
            place(field, x, y, scale(fieldWidth, d), fieldHeight);
            x += scale(fieldWidth, d) + scale(20, d);
        }
        place(filterError_, x, captionY, std::max(scale(80, d), width - margin - x), caption);
        y += fieldHeight + scale(16, d);
    }
    // Main: the table, and the details pane at a fixed width on the right.
    const int bottomY = height - scale(12, d) - buttonHeight;
    const int listWidth = width - 2 * margin - detailsWidth - column;
    place(list_, margin, y, listWidth, bottomY - scale(12, d) - y);
    SetPropW(list_, L"putgd.dividerRight", reinterpret_cast<HANDLE>(static_cast<INT_PTR>(column / 2)));
    fitColumns();
    const int pageButton = scale(96, d);
    place(previous_, margin, bottomY, pageButton, buttonHeight);
    place(next_, margin + pageButton + gap, bottomY, pageButton, buttonHeight);
    place(pageLabel_, margin + 2 * (pageButton + gap) + scale(4, d), bottomY + (buttonHeight - lineHeight(d)) / 2, scale(220, d), lineHeight(d) + 2);
    const int x = width - margin - detailsWidth;
    const int half = (detailsWidth - gap) / 2;
    // Maintenance is secondary: quiet text buttons under the pane.
    const int clearWidth = buttonWidth(clearHistory_, 80), cleanWidth = buttonWidth(clearCache_, 80);
    place(clearCache_, width - margin - cleanWidth, bottomY, cleanWidth, buttonHeight);
    place(clearHistory_, width - margin - cleanWidth - gap - clearWidth, bottomY, clearWidth, buttonHeight);
    // Details pane: the preview gives up height first, down to a strip, so the actions always fit.
    int dy = y;
    const int titleHeight = fontHeight(font(FontRole::Bold, d));
    const int small = fontHeight(font(FontRole::SmallBold, d));
    const bool sharing = !activityMode_ && sharingVisible_;
    const int buttonsHeight = (buttonHeight + gap) * 3 - gap + (activityMode_ ? 0 : small + scale(4, d) + fieldHeight + scale(12, d)) + (sharing ? buttonHeight + gap : 0);
    const int below = scale(14, d) + titleHeight + scale(8, d) + scale(64, d) + gap + buttonsHeight;
    const int previewHeight = std::clamp(bottomY - scale(12, d) - y - below, scale(72, d), scale(180, d));
    place(preview_, x, dy, detailsWidth, previewHeight);
    dy += previewHeight + scale(14, d);
    place(title_, x, dy, detailsWidth, titleHeight + scale(2, d));
    dy += titleHeight + scale(8, d);
    const int detailsHeight = std::max(scale(40, d), bottomY - scale(12, d) - dy - gap - buttonsHeight);
    place(details_, x, dy, detailsWidth, detailsHeight);
    dy += detailsHeight + gap;
    if (!activityMode_) {
        place(linkLabel_, x, dy, detailsWidth, small + 2);
        dy += small + scale(4, d);
        place(link_, x, dy, detailsWidth, fieldHeight);
        dy += fieldHeight + scale(12, d);
        place(copyLink_, x, dy, half, buttonHeight);
        place(openLink_, x + half + gap, dy, half, buttonHeight);
        dy += buttonHeight + gap;
        if (sharing) {
            // Create | Revoke | Inspect: the outer two at their natural widths, Inspect takes the rest.
            const int third = (detailsWidth - 2 * gap) / 3;
            const int shareWidth = std::max(third, std::min(buttonWidth(share_, 80), detailsWidth / 2 - gap));
            const int revokeWidth = std::max(scale(60, d), std::min(buttonWidth(revoke_, 80), detailsWidth - shareWidth - 2 * gap - scale(80, d)));
            place(share_, x, dy, shareWidth, buttonHeight);
            place(revoke_, x + shareWidth + gap, dy, revokeWidth, buttonHeight);
            place(inspectLink_, x + shareWidth + revokeWidth + 2 * gap, dy, detailsWidth - shareWidth - revokeWidth - 2 * gap, buttonHeight);
            dy += buttonHeight + gap;
        }
        const int third = (detailsWidth - 2 * gap) / 3;
        place(openFolder_, x, dy, third, buttonHeight);
        place(saveAs_, x + third + gap, dy, third, buttonHeight);
        place(editAgain_, x + 2 * (third + gap), dy, detailsWidth - 2 * (third + gap), buttonHeight);
        dy += buttonHeight + gap;
        // A failed upload has nothing to delete remotely: its diagnostics take that place.
        place(delete_, x, dy, detailsWidth, buttonHeight);
        place(saveDiagnostics_, x, dy, detailsWidth, buttonHeight);
    } else {
        place(retry_, x, dy, half, buttonHeight);
        place(cancel_, x + half + gap, dy, half, buttonHeight);
        dy += buttonHeight + gap;
        place(copyResult_, x, dy, half, buttonHeight);
        place(retryLocal_, x + half + gap, dy, half, buttonHeight);
        dy += buttonHeight + gap;
        place(saveDiagnostics_, x, dy, detailsWidth, buttonHeight);
    }
    fitScrollBar(details_);
    return height;
}

void HistoryPageImpl::setActivity(bool on) {
    const bool changed = on != activityMode_;
    activityMode_ = on;
    SendMessageW(activity_, LB_SETCURSEL, on ? 1 : 0, 0);
    for (HWND h : {category_, upload_, local_, remote_, destination_, search_, previous_, next_}) enable(h, !on);
    for (HWND h : {linkLabel_, link_, copyLink_, openLink_, openFolder_, saveAs_, editAgain_, delete_, viewMode_, moreFilters_}) show(h, !on);
    for (HWND h : {share_, revoke_, inspectLink_}) show(h, !on && sharingVisible_);
    for (HWND h : {retry_, cancel_, copyResult_, retryLocal_}) show(h, on);
    show(saveDiagnostics_, on); // in Files, showRecord shows it for a failed upload
    for (HWND h : {fromLabel_, from_, toLabel_, to_, mimeLabel_, mime_, filterError_}) show(h, !on && filtersExpanded_);
    show(pageLabel_, !on);
    if (changed) {
        SendMessageW(list_, LVM_DELETEALLITEMS, 0, 0);
        // Activity is always a list; Files keeps its own list or gallery choice.
        SendMessageW(list_, LVM_SETVIEW, !on && grid_ ? LV_VIEW_TILE : LV_VIEW_DETAILS, 0);
        SendMessageW(list_, LVM_SETEXTENDEDLISTVIEWSTYLE, LVS_EX_LABELTIP, !on && grid_ ? 0 : LVS_EX_LABELTIP);
        setColumns();
        selectedId_.clear();
        selectionChanged();
        main_.viewStateChanged();
    }
    panel_.layout();
    if (changed && IsWindowVisible(panel_.hwnd())) refresh();
    if (!on) KillTimer(panel_.hwnd(), timerActivity);
}

// The gallery: a tile view of fixed-size tiles that the theme draws (thumbnail,
// name, state and time); the list: the report view with small thumbnails.
void HistoryPageImpl::setGridMode(bool grid) {
    grid_ = grid;
    SendMessageW(viewMode_, LB_SETCURSEL, grid ? 1 : 0, 0);
    if (grid) {
        const UINT d = panel_.dpi();
        LVTILEVIEWINFO info{};
        info.cbSize = sizeof info;
        info.dwMask = LVTVIM_TILESIZE | LVTVIM_COLUMNS | LVTVIM_LABELMARGIN;
        info.dwFlags = LVTVIF_FIXEDSIZE;
        info.sizeTile = {scale(tileWidth, d), scale(tileHeight, d)};
        info.cLines = 0;
        info.rcLabelMargin = {0, 0, 0, 0};
        SendMessageW(list_, LVM_SETTILEVIEWINFO, 0, reinterpret_cast<LPARAM>(&info));
        if (!gallery_) resetGallery();
    }
    if (!activityMode_) {
        SendMessageW(list_, LVM_SETVIEW, grid ? LV_VIEW_TILE : LV_VIEW_DETAILS, 0);
        // Label tips place themselves for report rows; tiles show the whole name on hover instead.
        SendMessageW(list_, LVM_SETEXTENDEDLISTVIEWSTYLE, LVS_EX_LABELTIP, grid ? 0 : LVS_EX_LABELTIP);
        fitColumns();
        fill(); // each item's image indexes the list its view draws
    }
    main_.viewStateChanged();
}

void HistoryPageImpl::resetGallery() {
    galleryImages_.clear();
    galleryDark_ = theme::dark();
    galleryDpi_ = panel_.dpi();
    HIMAGELIST fresh = ImageList_Create(scale(thumbWidth, galleryDpi_), scale(thumbHeight, galleryDpi_), ILC_COLOR32, 32, 16);
    HIMAGELIST old = reinterpret_cast<HIMAGELIST>(SendMessageW(list_, LVM_SETIMAGELIST, LVSIL_NORMAL, reinterpret_cast<LPARAM>(fresh)));
    if (old && old != fresh) ImageList_Destroy(old);
    gallery_ = fresh;
}

// A gallery thumbnail: the preview fitted into the tile's picture area on the
// placeholder colour, or the file type's glyph. Kept across refreshes.
int HistoryPageImpl::galleryIndex(const json::Object& r) {
    if (!gallery_ || galleryDark_ != theme::dark() || galleryDpi_ != panel_.dpi() || galleryImages_.size() > 400) resetGallery();
    const auto path = r.value("previewPath").toString();
    const auto category = r.value("category").toString();
    const auto key = path.empty() ? "glyph:" + category : r.value("id").toString() + "\n" + path;
    if (const auto it = galleryImages_.find(key); it != galleryImages_.end()) return it->second;
    const UINT d = panel_.dpi();
    const int width = scale(thumbWidth, d), height = scale(thumbHeight, d);
    BITMAPINFO tile{};
    tile.bmiHeader.biSize = sizeof tile.bmiHeader; tile.bmiHeader.biWidth = width; tile.bmiHeader.biHeight = -height; tile.bmiHeader.biPlanes = 1; tile.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HBITMAP canvas = CreateDIBSection(nullptr, &tile, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!canvas || !bits) { if (canvas) DeleteObject(canvas); return -1; }
    const auto& c = theme::colors();
    HDC screen = GetDC(nullptr);
    HDC dst = CreateCompatibleDC(screen);
    HGDIOBJ oldDst = SelectObject(dst, canvas);
    theme::fill(dst, {0, 0, width, height}, c.placeholder);
    HBITMAP image = path.empty() ? nullptr : decodePreview(path, width, height);
    if (image) {
        BITMAP info{};
        GetObjectW(image, sizeof info, &info);
        HDC src = CreateCompatibleDC(screen);
        HGDIOBJ oldSrc = SelectObject(src, image);
        BitBlt(dst, (width - info.bmWidth) / 2, (height - info.bmHeight) / 2, info.bmWidth, info.bmHeight, src, 0, 0, SRCCOPY);
        SelectObject(src, oldSrc);
        DeleteDC(src);
        DeleteObject(image);
    } else {
        const int size = scale(40, d);
        const char* name = category == "images" ? "images" : category == "videos" ? "window" : category == "text" ? "clipboard" : "destinations";
        theme::drawGlyph(dst, name, (width - size) / 2, (height - size) / 2, size, c.iconSecondary, c.divider);
    }
    SelectObject(dst, oldDst);
    DeleteDC(dst);
    ReleaseDC(nullptr, screen);
    // GDI leaves alpha at 0; make the tile opaque for the image list.
    auto* px = static_cast<unsigned*>(bits);
    for (int i = 0; i < width * height; ++i) px[i] |= 0xFF000000u;
    const int index = ImageList_Add(gallery_, canvas, nullptr);
    DeleteObject(canvas);
    if (index >= 0) galleryImages_[key] = index;
    return index;
}

void HistoryPageImpl::setFiltersExpanded(bool expanded) {
    filtersExpanded_ = expanded;
    int active = 0;
    for (HWND h : {from_, to_, mime_}) if (!text::trim(text(h)).empty()) ++active;
    setTextW(moreFilters_, expanded ? std::wstring(L"Fewer filters") : active ? L"More filters (" + std::to_wstring(active) + L")" : std::wstring(L"More filters"));
    for (HWND h : {fromLabel_, from_, toLabel_, to_, mimeLabel_, mime_, filterError_}) show(h, expanded && !activityMode_);
    panel_.layout();
    main_.viewStateChanged();
}

// The date and content-type filters; false (with the reason shown) when one is not valid yet.
bool HistoryPageImpl::readFilters(json::Object& params) {
    std::wstring problem;
    const auto from = text::trim(text(from_)), to = text::trim(text(to_)), mime = text::trim(text(mime_));
    if (!from.empty()) { if (auto bound = dayBound(from, false)) params["from"] = *bound; else problem = L"Enter From as YYYY-MM-DD."; }
    if (!to.empty()) { if (auto bound = dayBound(to, true)) params["to"] = *bound; else if (problem.empty()) problem = L"Enter To as YYYY-MM-DD."; }
    if (problem.empty() && params.contains("from") && params.contains("to") && params.value("from").stringRef() > params.value("to").stringRef()) problem = L"From is after To.";
    if (!mime.empty()) { if (auto type = historyfilter::normalizeMime(mime)) params["mime"] = *type; else if (problem.empty()) problem = L"Enter a content type such as image/png."; }
    setTextW(filterError_, problem);
    return problem.empty();
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
    if (!readFilters(params)) { loading_ = false; return; }
    const auto searchText = text::trim(text(search_));
    if (!searchText.empty()) params["search"] = searchText;
    const std::pair<const char*, HWND> filters[] = {{"category", category_}, {"uploadStatus", upload_}, {"localStatus", local_}, {"remoteStatus", remote_}, {"profileId", destination_}};
    for (const auto& [key, control] : filters) if (const auto value = selected(control); !value.empty()) params[key] = value;
    main_.session().call("history.list", params, [done](const Reply& reply) { done(reply, false); });
}

// Row thumbnails: the bounded history preview scaled into a square tile, or a category
// glyph; one image list per fill so nothing accumulates across refreshes.
int HistoryPageImpl::thumbnailIndex(const json::Object& r) {
    if (!thumbnails_) return -1;
    const UINT d = panel_.dpi();
    const int size = scale(28, d);
    const auto& c = theme::colors();
    const auto path = r.value("previewPath").toString();
    HBITMAP thumb = path.empty() ? nullptr : decodePreview(path, size, size);
    if (thumb) {
        BITMAP info{};
        GetObjectW(thumb, sizeof info, &info);
        BITMAPINFO tile{};
        tile.bmiHeader.biSize = sizeof tile.bmiHeader; tile.bmiHeader.biWidth = size; tile.bmiHeader.biHeight = -size; tile.bmiHeader.biPlanes = 1; tile.bmiHeader.biBitCount = 32;
        void* bits = nullptr;
        HBITMAP square = CreateDIBSection(nullptr, &tile, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (square && bits) {
            HDC screen = GetDC(nullptr);
            HDC dst = CreateCompatibleDC(screen), src = CreateCompatibleDC(screen);
            HGDIOBJ oldDst = SelectObject(dst, square), oldSrc = SelectObject(src, thumb);
            theme::roundRect(dst, {0, 0, size, size}, scale(4, d), c.placeholder);
            BitBlt(dst, (size - info.bmWidth) / 2, (size - info.bmHeight) / 2, info.bmWidth, info.bmHeight, src, 0, 0, SRCCOPY);
            SelectObject(dst, oldDst); SelectObject(src, oldSrc); DeleteDC(dst); DeleteDC(src); ReleaseDC(nullptr, screen);
            // GDI leaves alpha at 0; make the tile opaque for the image list.
            auto* px = static_cast<unsigned*>(bits);
            for (int i = 0; i < size * size; ++i) px[i] |= 0xFF000000u;
            const int index = ImageList_Add(thumbnails_, square, nullptr);
            DeleteObject(square);
            DeleteObject(thumb);
            return index;
        }
        DeleteObject(thumb);
    }
    const auto category = r.value("category").toString();
    const char* name = category == "images" ? "images" : category == "videos" ? "window" : category == "text" ? "clipboard" : "destinations";
    HICON icon = theme::glyph(name, size, c.iconSecondary, c.divider);
    return icon ? ImageList_AddIcon(thumbnails_, icon) : -1;
}
void HistoryPageImpl::setRowImage(int row, int image) {
    if (row < 0 || image < 0) return;
    LVITEMW item{};
    item.mask = LVIF_IMAGE; item.iItem = row; item.iImage = image;
    SendMessageW(list_, LVM_SETITEMW, 0, reinterpret_cast<LPARAM>(&item));
}
void HistoryPageImpl::resetThumbnails() {
    const int size = scale(28, panel_.dpi());
    HIMAGELIST fresh = activityMode_ || grid_ ? nullptr : ImageList_Create(size, size, ILC_COLOR32 | ILC_MASK, 64, 16);
    HIMAGELIST old = reinterpret_cast<HIMAGELIST>(SendMessageW(list_, LVM_SETIMAGELIST, LVSIL_SMALL, reinterpret_cast<LPARAM>(fresh)));
    if (old && old != fresh) ImageList_Destroy(old);
    if (thumbnails_ && thumbnails_ != old && thumbnails_ != fresh) ImageList_Destroy(thumbnails_);
    thumbnails_ = fresh;
}
void HistoryPageImpl::fill() {
    populating_ = true;
    SendMessageW(list_, WM_SETREDRAW, FALSE, 0);
    resetThumbnails();
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
            const auto profileId = r.value("profileId").toString(r.value("receipt")["destination"]["profileId"].toString());
            const auto destination = profileId.empty() ? std::wstring(L"\x2014") : wide(main_.session().profileLabel(profileId));
            const int row = addRow(list_, {wide(r.value("name").toString()), relativeTime(r.value("created").toString()), wide(text::byteSize(r.value("bytes").toInteger())),
                                           destination, statusLabel(uploadLabels, r.value("uploadStatus").toString())},
                                   LPARAM(i));
            (void)link;
            setRowImage(row, grid_ ? galleryIndex(r) : thumbnailIndex(r));
            if (r.value("id").toString() == selectedId_) selectedRow = row;
        }
        const long long first = total_ ? offset_ + 1 : 0, last = std::min<long long>(total_, offset_ + static_cast<long long>(records_.size()));
        setTextW(pageLabel_, std::to_wstring(first) + L"\x2013" + std::to_wstring(last) + L" of " + std::to_wstring(total_));
        enable(previous_, offset_ > 0);
        enable(next_, offset_ + pageSize < total_);
    }
    // Keep the scroll position across refreshes (Activity refreshes every second).
    if (top > 0 && !(grid_ && !activityMode_) && SendMessageW(list_, LVM_GETITEMCOUNT, 0, 0) > top) {
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
        if (record.empty() || detail_.value("id").toString() != selectedId_) detail_ = {};
        showRecord(record);
        enable(delete_, !selectedIds().empty());
        if (!record.empty()) loadDetail(selectedId_);
    }
    main_.viewStateChanged();
}

// The detail adds what the list does not carry: sharing state and the provider's capabilities.
void HistoryPageImpl::loadDetail(const std::string& id) {
    const auto generation = ++detailGeneration_;
    main_.session().call("history.detail", {{"id", id}}, [this, generation, id](const Reply& reply) {
        if (generation != detailGeneration_ || selectedId_ != id || activityMode_ || !reply.ok) return;
        detail_ = reply.result;
        const auto record = selectedRecord();
        if (record.value("id").toString() == id) showRecord(record);
    });
}

void HistoryPageImpl::showSharing(bool on) {
    if (on == sharingVisible_) return;
    sharingVisible_ = on;
    for (HWND h : {share_, revoke_, inspectLink_}) show(h, on && !activityMode_);
    panel_.layout();
}

// As on Linux: busy while a request is being accepted, polled, or a pending result is not yet stopped.
void HistoryPageImpl::updateSharingButtons(const json::Object& record) {
    const auto status = record.value("shareStatus").toString();
    const bool stopped = sharingStopped_ && sharingId_ == record.value("shareOperationId").toString();
    const bool busy = sharingAccepting_ || sharingActive_ || (status == "pending" && !stopped);
    enable(share_, !busy && status != "pending" && status != "unknown" && status != "changed");
    enable(revoke_, !busy && record.value("linkAvailable").toBool());
    enable(inspectLink_, !busy);
}

void HistoryPageImpl::changeSharing(const std::string& action) {
    const auto record = selectedRecord();
    if (record.empty() || sharingAccepting_ || sharingActive_) return;
    const auto historyId = record.value("id").toString();
    if (action == "revoke" &&
        !confirm(main_.hwnd(), L"Revoke recorded share link", L"Revoke this file\x2019s public share link?",
                 L"The uploaded file remains. Existing member access and downloaded copies remain.", L"Revoke link"))
        return;
    sharingAccepting_ = true;
    for (HWND h : {share_, revoke_, inspectLink_}) enable(h, false);
    const char* method = action == "create" ? "history.share" : action == "revoke" ? "history.revokeLink" : "history.inspectLink";
    main_.status(action == "create" ? L"Creating a share link\x2026" : action == "revoke" ? L"Revoking the share link\x2026" : L"Inspecting the share link\x2026");
    main_.session().call(method, {{"id", historyId}}, [this](const Reply& reply) {
        sharingAccepting_ = false;
        if (!reply.ok) {
            main_.status(UiSession::describe(reply), true);
            selectionChanged();
            return;
        }
        sharingId_ = reply.result.value("shareOperationId").toString();
        sharingStopped_ = false;
        if (sharingId_.empty()) { selectionChanged(); return; }
        sharingActive_ = true;
        SetTimer(panel_.hwnd(), timerShare, 1000, nullptr);
        pollSharing();
    });
}

void HistoryPageImpl::pollSharing() {
    if (sharingPolling_) return;
    if (sharingId_.empty()) { sharingActive_ = false; KillTimer(panel_.hwnd(), timerShare); return; }
    sharingPolling_ = true;
    const auto id = sharingId_;
    main_.session().call("share.inspect", {{"id", id}}, [this, id](const Reply& reply) {
        sharingPolling_ = false;
        if (id != sharingId_) return;
        if (!reply.ok) {
            if (reply.outcomeUnknown) return; // keep the accepted ID and ask again on the next tick
            sharingActive_ = false;
            KillTimer(panel_.hwnd(), timerShare);
            main_.status(UiSession::describe(reply), true);
            refresh();
            return;
        }
        const auto doc = reply.result.contains("state") ? reply.result : reply.result.value("sharing").toObject();
        const auto state = doc.value("state").toString();
        sharingStopped_ = state == "interrupted";
        if (state != "completed" && state != "interrupted") return;
        sharingActive_ = false;
        KillTimer(panel_.hwnd(), timerShare);
        const auto error = doc.value("error").toObject();
        std::wstring text = L"Link: " + statusLabel(sharingLabels, doc.value("status").toString());
        const auto message = error.value("message").toString();
        if (!message.empty()) text += L" \x2014 " + wide(message);
        main_.status(text, !error.value("code").toString().empty());
        refresh(); // the list, then the selected record's detail
    });
}

void HistoryPageImpl::setPreview(HBITMAP bitmap, const std::wstring& note) {
    HBITMAP previous = reinterpret_cast<HBITMAP>(SendMessageW(preview_, STM_SETIMAGE, IMAGE_BITMAP, reinterpret_cast<LPARAM>(bitmap)));
    if (previous && previous != bitmap && previous != bitmap_) DeleteObject(previous);
    if (bitmap_ && bitmap_ != bitmap) DeleteObject(bitmap_);
    bitmap_ = bitmap;
    setTextW(previewNote_, note); // drawn by the preview itself (previewProc)
    InvalidateRect(preview_, nullptr, FALSE);
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
    InflateRect(&box, -scale(10, panel_.dpi()), -scale(10, panel_.dpi()));
    OffsetRect(&box, -box.left, -box.top);
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
        InflateRect(&box, -scale(10, panel_.dpi()), -scale(10, panel_.dpi()));
        OffsetRect(&box, -box.left, -box.top);
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
        fitScrollBar(details_);
        setText(link_, "");
        for (HWND h : {copyLink_, openLink_, openFolder_, saveAs_, editAgain_}) enable(h, false);
        show(delete_, true);
        show(saveDiagnostics_, false);
        showSharing(false);
        return;
    }
    // The detail of this record, once loaded, is the authoritative source for its link and sharing state.
    const bool haveDetail = !detail_.empty() && detail_.value("id") == r.value("id");
    const json::Object& source = haveDetail ? detail_ : r;
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
    const bool sharingCapable = source.value("capabilities")["sharing"].toBool();
    const auto shareStatus = source.value("shareStatus").toString();
    if (sharingCapable || (!shareStatus.empty() && shareStatus != "not_requested")) {
        text += L"\r\nSharing: " + (shareStatus.empty() ? std::wstring(L"Sharing not recorded") : statusLabel(sharingLabels, shareStatus));
        if (const auto expiry = linkExpiry(source); !expiry.empty()) text += L"\r\n" + expiry;
    }
    const auto error = r.value("error").toObject();
    if (!error.value("code").toString().empty())
        text += L"\r\nError: " + wide(error.value("message").toString()) + L" (" + wide(error.value("code").toString()) + L")";
    if (!local.value("error")["code"].toString().empty())
        text += L"\r\nLocal save error: " + wide(local.value("error")["message"].toString());
    setTextW(details_, text);
    fitScrollBar(details_);
    const bool linkAvailable = source.value("linkAvailable").toBool() && !source.value("receipt")["directUrl"].toString().empty();
    setText(link_, linkAvailable ? source.value("receipt")["directUrl"].toString() : std::string());
    setCue(link_, linkAvailable ? L"" : L"No link");
    const bool localSaved = local.value("status") == "saved" && !local.value("path").toString().empty();
    enable(copyLink_, linkAvailable || r.value("uploadStatus") == "local_only");
    setTextW(copyLink_, r.value("uploadStatus") == "local_only" ? L"Copy result" : L"Copy link");
    enable(openLink_, linkAvailable);
    enable(openFolder_, localSaved);
    enable(saveAs_, localSaved && !saveAsChooser_.isOpen());
    enable(editAgain_, localSaved && editableImage(local.value("path").toString()));
    const bool failed = failedUpload(r);
    show(delete_, !failed);
    show(saveDiagnostics_, failed);
    enable(saveDiagnostics_, failed && !savingDiagnostics_);
    // Share-link actions for providers that manage links; until the detail arrives they keep
    // their previous visibility (no layout jump) but stay off.
    if (haveDetail) showSharing(sharingCapable);
    if (haveDetail) updateSharingButtons(source);
    else for (HWND h : {share_, revoke_, inspectLink_}) enable(h, false);
}

void HistoryPageImpl::showOperation(const json::Object& op) {
    loadPreview({});
    if (op.empty()) {
        setTextW(title_, L"");
        setTextW(details_, operations_.empty() ? L"No recent activity." : L"Select an operation to see its files and progress.");
        fitScrollBar(details_);
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
    fitScrollBar(details_);
    enable(retry_, !active && failedUpload);
    enable(cancel_, active);
    enable(copyResult_, !active && copyable);
    enable(retryLocal_, !active && failedLocal);
    enable(saveDiagnostics_, !active && (state == "failed" || failedUpload) && !savingDiagnostics_);
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

// ---- Save as…, Save diagnostics and the view commands ----------------------------
void HistoryPageImpl::saveAs() {
    const auto record = selectedRecord();
    const auto source = record.value("local")["path"].toString();
    if (record.value("local")["status"] != "saved" || source.empty() || !files::isRegularFile(source)) return;
    FileChooser::Options options;
    options.mode = FileChooser::Mode::Save;
    options.title = L"Save original file as";
    options.fileName = wide(record.value("name").toString(paths::fileName(source)));
    const bool opened = saveAsChooser_.open(main_.hwnd(), options, [this, source](const std::string& destination) {
        enable(saveAs_, !selectedRecord().empty());
        if (destination.empty()) return;
        if (text::lower(destination) == text::lower(source)) { main_.status(L"This is already the retained local file."); return; }
        main_.status(L"Saving the original file\x2026");
        // A copy beside the target, then an atomic replace; large files copy off the UI thread.
        std::thread([this, source, destination] {
            const auto temporary = destination + ".putgd-save";
            const bool ok = CopyFileW(paths::wide(source).c_str(), paths::wide(temporary).c_str(), FALSE) &&
                            MoveFileExW(paths::wide(temporary).c_str(), paths::wide(destination).c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
            if (!ok) DeleteFileW(paths::wide(temporary).c_str());
            main_.post([this, ok] { main_.status(ok ? L"Original file saved." : L"The original file could not be saved there.", !ok); });
        }).detach();
    });
    if (opened) enable(saveAs_, false);
}

void HistoryPageImpl::saveDiagnostics(const std::string& operationId, CommandReply answer) {
    if (operationId.empty() || savingDiagnostics_) { if (answer) answer({"diagnostics_busy", "A diagnostics package is already being saved.", {}}, {}); return; }
    savingDiagnostics_ = true;
    enable(saveDiagnostics_, false);
    main_.status(L"Saving diagnostics for this upload\x2026");
    main_.session().call("diagnostics.bundle", {{"operation", operationId}}, [this, answer](const Reply& reply) {
        savingDiagnostics_ = false;
        selectionChanged();
        if (!reply.ok) {
            main_.status(UiSession::describe(reply), true);
            if (answer) { auto error = reply.error; if (reply.outcomeUnknown) error.details["outcomeUnknown"] = true; answer(error, {}); }
            return;
        }
        const auto path = reply.result.value("path").toString();
        main_.status(L"Diagnostics saved to " + wide(path));
        revealInExplorer(path);
        if (answer) answer({}, reply.result);
    });
}

// The row of a file (history ID) or of an operation's file (operation ID) on the page shown.
int HistoryPageImpl::rowOf(const std::string& id) const {
    const int count = int(SendMessageW(list_, LVM_GETITEMCOUNT, 0, 0));
    for (int row = 0; row < count; ++row) {
        const auto index = size_t(rowData(list_, row));
        if (activityMode_) {
            if (index >= operations_.size()) continue;
            const auto& op = operations_[index].objectRef();
            if (op.value("id") == id.c_str()) return row;
            for (const auto& item : op.value("items").arrayRef()) if (item.objectRef().value("artifact")["id"] == id.c_str()) return row;
        } else if (index < records_.size()) {
            const auto& record = records_[index].objectRef();
            if (record.value("id") == id.c_str() || record.value("operationId") == id.c_str()) return row;
        }
    }
    return -1;
}

void HistoryPageImpl::selectRow(int row) {
    LVITEMW clear{};
    clear.stateMask = LVIS_SELECTED;
    SendMessageW(list_, LVM_SETITEMSTATE, WPARAM(-1), reinterpret_cast<LPARAM>(&clear));
    LVITEMW state{};
    state.stateMask = LVIS_SELECTED | LVIS_FOCUSED;
    state.state = LVIS_SELECTED | LVIS_FOCUSED;
    SendMessageW(list_, LVM_SETITEMSTATE, WPARAM(row), reinterpret_cast<LPARAM>(&state));
    SendMessageW(list_, LVM_ENSUREVISIBLE, WPARAM(row), FALSE);
    selectionChanged();
}

void HistoryPageImpl::saveAsCommand(const std::string& action, const std::string& id, const CommandReply& answer) {
    auto withDialog = [answer](Error error, json::Object result) { if (!error) result["dialog"] = "historySaveAs"; answer(error, result); };
    if (action == "inspect") { withDialog({}, saveAsChooser_.state()); return; }
    if (action == "cancel") { saveAsChooser_.cancel(); withDialog({}, saveAsChooser_.state()); return; }
    if (saveAsChooser_.isOpen()) { withDialog({"file_dialog_open", "Save as is already open.", {}}, saveAsChooser_.state()); return; }
    if (FileChooser::anyOpen()) { withDialog({"file_dialog_open", "Another file chooser is open in this window.", {}}, {}); return; }
    if (!id.empty()) {
        const int row = activityMode_ ? -1 : rowOf(id);
        if (row < 0) { withDialog({"history_row_not_found", "That file is not on the History page shown; search or page to it first.", {{"id", id}}}, {}); return; }
        selectRow(row);
    }
    main_.setPage(MainWindow::HistoryPage);
    if (selectedRecord().empty() || !IsWindowEnabled(saveAs_)) { withDialog({"history_save_unavailable", "Save as needs a selected file with a retained local original.", {}}, {}); return; }
    saveAs();
    withDialog({}, saveAsChooser_.state());
}

void HistoryPageImpl::saveDiagnosticsCommand(const std::string& id, const CommandReply& answer) {
    const int row = rowOf(id);
    if (row < 0) { answer({"history_row_not_found", "That upload is not on the History page shown; search or page to it first.", {{"id", id}}}, {}); return; }
    selectRow(row);
    main_.setPage(MainWindow::HistoryPage);
    const auto operationId = activityMode_ ? selectedOperation().value("id").toString() : selectedRecord().value("operationId").toString();
    const bool failed = activityMode_ ? IsWindowEnabled(saveDiagnostics_) || savingDiagnostics_ : failedUpload(selectedRecord());
    if (!failed) { answer({"diagnostics_row_not_failed", "Only a failed upload has diagnostics to save.", {{"id", id}}}, {}); return; }
    saveDiagnostics(operationId, answer);
}

bool HistoryPageImpl::viewCommand(const std::string& name, const json::Object& params, const CommandReply& answer) {
    if (name == "historySaveDiagnostics") { saveDiagnosticsCommand(params.value("id").toString(), answer); return true; }
    if (name == "fileDialog" && params.value("dialog") == "historySaveAs") { saveAsCommand(params.value("action").toString(), params.value("id").toString(), answer); return true; }
    return false;
}

void HistoryPageImpl::command(int id, int code, HWND control) {
    if (populating_) return;
    switch (id) {
    case idSearch: case idFrom: case idTo: case idMime:
        if (code == EN_CHANGE) { SetTimer(panel_.hwnd(), timerSearch, 300, nullptr); main_.viewStateChanged(); }
        break;
    case idViewMode:
        if (code == LBN_SELCHANGE) setGridMode(SendMessageW(viewMode_, LB_GETCURSEL, 0, 0) == 1);
        break;
    case idMoreFilters: setFiltersExpanded(!filtersExpanded_); break;
    case idSaveAs: saveAs(); break;
    case idCategory: case idUpload: case idLocal: case idRemote: case idDestination:
        if (code == CBN_SELCHANGE) { offset_ = 0; refresh(); main_.viewStateChanged(); }
        break;
    case idActivity:
        if (code == LBN_SELCHANGE) setActivity(SendMessageW(activity_, LB_GETCURSEL, 0, 0) == 1);
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
    case idShare: changeSharing("create"); break;
    case idRevoke: changeSharing("revoke"); break;
    case idInspectLink: changeSharing("inspect"); break;
    case idClearHistory: maintenance(false); break;
    case idClearCache: maintenance(true); break;
    case idRetry: operationAction("operation.retry"); break;
    case idCancel: operationAction("operation.cancel"); break;
    case idCopyResult: operationAction("operation.copy"); break;
    case idRetryLocal: operationAction("operation.retryLocalSave"); break;
    case idSaveDiagnostics:
        saveDiagnostics(activityMode_ ? selectedOperation().value("id").toString() : selectedRecord().value("operationId").toString(), {});
        break;
    }
}

LRESULT HistoryPageImpl::notify(NMHDR* header) {
    if (header->hwndFrom != list_) return 0;
    if (header->code == LVN_GETEMPTYMARKUP) {
        auto* markup = reinterpret_cast<NMLVEMPTYMARKUP*>(header);
        markup->dwFlags = EMF_CENTERED;
        bool filtered = !text::trim(text(search_)).empty() || !text::trim(text(from_)).empty() || !text::trim(text(to_)).empty() || !text::trim(text(mime_)).empty();
        for (HWND h : {category_, upload_, local_, remote_, destination_}) filtered = filtered || !selected(h).empty();
        const wchar_t* message = activityMode_ ? L"No recent activity. Captures and uploads in progress appear here."
                                 : filtered    ? L"Nothing matches the search and filters."
                                               : L"No captures yet. Captures and uploads appear here.";
        lstrcpynW(markup->szMarkup, message, int(sizeof markup->szMarkup / sizeof markup->szMarkup[0]));
        return TRUE;
    }
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
    if (id == timerSearch) { KillTimer(panel_.hwnd(), timerSearch); offset_ = 0; refresh(); if (!filtersExpanded_) setFiltersExpanded(false); }
    else if (id == timerRefresh) { KillTimer(panel_.hwnd(), timerRefresh); if (IsWindowVisible(panel_.hwnd())) refresh(); }
    else if (id == timerActivity) { if (activityMode_ && IsWindowVisible(panel_.hwnd())) refresh(); else KillTimer(panel_.hwnd(), timerActivity); }
    else if (id == timerDeletion) pollDeletion();
    else if (id == timerShare) pollSharing();
}

json::Object HistoryPageImpl::saveState() const {
    json::Object filters{{"offset", offset_}, {"limit", pageSize}};
    const std::pair<const char*, HWND> controls[] = {{"category", category_}, {"uploadStatus", upload_}, {"localStatus", local_}, {"remoteStatus", remote_}, {"profileId", destination_}};
    for (const auto& [key, control] : controls) if (const auto value = selected(control); !value.empty()) filters[key] = value;
    // The raw day and content-type text, as typed (docs/ui-process-state.md keeps raw filters).
    for (const auto& [key, control] : {std::pair{"from", from_}, std::pair{"to", to_}, std::pair{"mime", mime_}})
        if (const auto value = text(control); !value.empty()) filters[key] = value;
    json::Object state{{"query", text(search_)}, {"filters", filters}, {"gridMode", grid_}, {"filtersExpanded", filtersExpanded_}};
    if (!selectedId_.empty() && !activityMode_) state["selectedIds"] = json::Array{selectedId_};
    if (!deletionId_.empty()) state["deletionId"] = deletionId_;
    if (sharingActive_ && !sharingId_.empty()) state["sharingId"] = sharingId_;
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
    setText(from_, filters.value("from").toString());
    setText(to_, filters.value("to").toString());
    setText(mime_, filters.value("mime").toString());
    offset_ = std::max(0, int(filters.value("offset").toInteger()));
    const auto ids = state.value("selectedIds").toArray();
    if (!ids.empty()) selectedId_ = ids.front().toString();
    populating_ = false;
    deletionId_ = state.value("deletionId").toString();
    if (!deletionId_.empty()) SetTimer(panel_.hwnd(), timerDeletion, 500, nullptr);
    sharingId_ = state.value("sharingId").toString();
    sharingStopped_ = false;
    sharingActive_ = !sharingId_.empty();
    if (sharingActive_) SetTimer(panel_.hwnd(), timerShare, 500, nullptr);
    setFiltersExpanded(state.value("filtersExpanded").toBool(false));
    if (state.value("gridMode").toBool(false)) setGridMode(true);
}
}

std::unique_ptr<Page> createHistoryPage(MainWindow& main) { return std::make_unique<HistoryPageImpl>(main); }
}
