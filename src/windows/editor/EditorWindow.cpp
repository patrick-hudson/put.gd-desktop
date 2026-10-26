#include "editor/EditorWindow.h"
#include "common/Paths.h"
#include "common/Text.h"
#include "editor/Export.h"
#include <algorithm>
#include <cmath>
#include <ctime>
#include <map>
#include <windows.h>
#include <windowsx.h>
#include <objidl.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shobjidl.h>
#include <shlobj.h>
#include <shellscalingapi.h>
namespace Gdiplus { using std::max; using std::min; }
#include <gdiplus.h>

namespace putgd::editor {
namespace {
enum : int {
    idToolFirst = 100, // + int(Tool)
    idUndo = 110, idRedo, idFit, idActual, idStatus,
    idColorLabel = 119, idSwatchFirst = 120, idRecentFirst = 130, idCustomColor = 140,
    idWidthLabel = 149, idWidth = 150, idStrengthLabel = 159, idStrength = 160,
    idFontLabel = 169, idFont = 170, idSizeLabel = 171, idSize = 172, idSizeSpin = 173,
    idCropHint = 179, idApplyCrop = 180, idCancelCrop = 181, idHint = 185,
    idCanvas = 190, idMessage = 200, idSave = 201, idCancel = 202, idUpload = 203,
};
constexpr int paletteCount = 7, recentCount = 6, discardButton = 1001;
// The Linux editor's palette (src/ui/EditorWindow.cpp).
constexpr Rgb palette[paletteCount] = {0xdc2626, 0xf59e0b, 0xfacc15, 0x22c55e, 0x3b82f6, 0xffffff, 0x111317};
constexpr int widthValues[3] = {2, 4, 8};
constexpr wchar_t windowClass[] = L"PutgdEditImageWindow";
constexpr wchar_t canvasClass[] = L"PutgdEditImageCanvas";
constexpr UINT textDoneMessage = WM_APP + 1;
// Shell file dialog identifiers, spelled out so no uuid import library is needed.
constexpr CLSID clsidFileSaveDialog = {0xC0B4E2F3, 0xBA21, 0x4773, {0x8D, 0xBA, 0x33, 0x5E, 0xC9, 0x46, 0xEB, 0x8B}};
constexpr IID iidFileSaveDialog = {0x84BCCD23, 0x5FDE, 0x4CDB, {0xAE, 0xA4, 0xAF, 0x64, 0xB8, 0x3D, 0x78, 0xAB}};
constexpr KNOWNFOLDERID folderPictures = {0x33E28130, 0x4E1E, 0x4676, {0x83, 0x5A, 0x98, 0x39, 0x5C, 0x3B, 0xC3, 0xBB}};

struct PointD { double x = 0, y = 0; };
struct RectD {
    double x = 0, y = 0, w = 0, h = 0;
    double right() const { return x + w; }
    double bottom() const { return y + h; }
    bool contains(PointD p) const { return p.x >= x && p.y >= y && p.x <= x + w && p.y <= y + h; }
    RectD adjusted(double l, double t, double r, double b) const { return {x + l, y + t, w - l + r, h - t + b}; }
    RectD intersected(const RectD& o) const {
        const double l = std::max(x, o.x), t = std::max(y, o.y), r = std::min(right(), o.right()), b = std::min(bottom(), o.bottom());
        return r > l && b > t ? RectD{l, t, r - l, b - t} : RectD{};
    }
    bool empty() const { return w <= 0 || h <= 0; }
    Gdiplus::RectF f() const { return Gdiplus::RectF(Gdiplus::REAL(x), Gdiplus::REAL(y), Gdiplus::REAL(w), Gdiplus::REAL(h)); }
};
double distance(PointD a, PointD b) { return std::hypot(a.x - b.x, a.y - b.y); }
double distanceToSegment(PointD p, PointD a, PointD b) {
    const double abx = b.x - a.x, aby = b.y - a.y, length = abx * abx + aby * aby;
    if (length <= 0) return distance(p, a);
    const double t = std::clamp(((p.x - a.x) * abx + (p.y - a.y) * aby) / length, 0.0, 1.0);
    return distance(p, {a.x + t * abx, a.y + t * aby});
}
Gdiplus::Color gdi(COLORREF c, int alpha = 255) { return Gdiplus::Color(BYTE(alpha), GetRValue(c), GetGValue(c), GetBValue(c)); }
Gdiplus::Color gdiRgb(Rgb c, int alpha = 255) { return Gdiplus::Color(BYTE(alpha), BYTE(c >> 16 & 255), BYTE(c >> 8 & 255), BYTE(c & 255)); }
COLORREF colorRef(Rgb c) { return RGB(c >> 16 & 255, c >> 8 & 255, c & 255); }
Rgb fromColorRef(COLORREF c) { return Rgb(GetRValue(c)) << 16 | Rgb(GetGValue(c)) << 8 | Rgb(GetBValue(c)); }
std::wstring w(const std::string& s) { return paths::wide(s); }
LPCWSTR resizeCursor(int handle) {
    return handle == 0 || handle == 4 ? IDC_SIZENWSE : handle == 2 || handle == 6 ? IDC_SIZENESW : handle == 1 || handle == 5 ? IDC_SIZENS : IDC_SIZEWE;
}
std::unique_ptr<Gdiplus::Bitmap> view(const Raster& raster) {
    if (raster.isNull()) return {};
    return std::make_unique<Gdiplus::Bitmap>(raster.width, raster.height, raster.width * 4, raster.alpha ? PixelFormat32bppARGB : PixelFormat32bppRGB,
                                             reinterpret_cast<BYTE*>(const_cast<uint32_t*>(raster.pixels.data())));
}
bool isClass(HWND hwnd, const wchar_t* name) {
    wchar_t buffer[64] = {};
    return hwnd && GetClassNameW(hwnd, buffer, 64) && _wcsicmp(buffer, name) == 0;
}
int CALLBACK collectFamily(const LOGFONTW* font, const TEXTMETRICW*, DWORD, LPARAM data) {
    if (font->lfFaceName[0] != L'@') reinterpret_cast<std::vector<std::string>*>(data)->push_back(paths::narrow(font->lfFaceName));
    return 1;
}
struct DialogSweep { HWND owner, skip; };
BOOL CALLBACK cancelOwnedDialog(HWND window, LPARAM data) {
    const auto* sweep = reinterpret_cast<const DialogSweep*>(data);
    if (window != sweep->owner && window != sweep->skip && GetWindow(window, GW_OWNER) == sweep->owner && isClass(window, L"#32770") && IsWindowVisible(window))
        PostMessageW(window, WM_COMMAND, IDCANCEL, 0);
    return TRUE;
}
}

// ---------------------------------------------------------------------------
// Canvas: an interactive view of one EditDocument (port of EditorCanvas).
// The base raster is painted from the document; only bounded display caches
// (a fitted downscale and display-resolution blur tiles) are kept.
// ---------------------------------------------------------------------------
class Canvas {
public:
    static constexpr size_t blurCacheBytes = 64u * 1024 * 1024;
    static constexpr double displayCachePixels = 8e6;
    static constexpr double minimumZoom = 0.02, maximumZoom = 32.0;

    Canvas(EditorWindow& owner, EditDocument& doc) : owner_(owner), doc_(doc), base_(view(doc.base())) {}
    ~Canvas() { if (hwnd_) DestroyWindow(hwnd_); }

    bool create(HWND parent) {
        static const bool registered = [] {
            WNDCLASSEXW wc{};
            wc.cbSize = sizeof wc;
            wc.style = CS_DBLCLKS;
            wc.lpfnWndProc = &Canvas::proc;
            wc.hInstance = GetModuleHandleW(nullptr);
            wc.hCursor = LoadCursorW(nullptr, IDC_CROSS);
            wc.lpszClassName = canvasClass;
            return RegisterClassExW(&wc) != 0;
        }();
        if (!registered) return false;
        hwnd_ = CreateWindowExW(0, canvasClass, L"Image being edited", WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_CLIPCHILDREN, 0, 0, 10, 10, parent,
                                reinterpret_cast<HMENU>(INT_PTR(idCanvas)), GetModuleHandleW(nullptr), this);
        return hwnd_ != nullptr;
    }
    HWND hwnd() const { return hwnd_; }
    void setDpi(UINT dpi) { dpi_ = dpi; invalidate(); }
    void setEnabled(bool enabled) {
        enabled_ = enabled;
        if (!enabled) { if (textEditor_) finishText(false); if (tool_ == Tool::Crop) cancelCrop(); if (GetCapture() == hwnd_) ReleaseCapture(); drag_ = Drag::None; }
        invalidate();
    }

    void setTool(Tool tool) {
        if (textEditor_) finishText(true);
        // A newly chosen drawing tool starts unselected, so its style controls
        // configure the tool rather than restyling an earlier shape.
        const bool deselect = tool != tool_ && tool != Tool::Select && selected_;
        if (tool_ == Tool::Crop && tool != Tool::Crop) cancelCrop();
        tool_ = tool;
        if (tool_ == Tool::Crop) { cropDraft_ = doc_.crop(); selected_ = 0; owner_.canvasSelectionChanged(); }
        else if (deselect) { selected_ = 0; owner_.canvasSelectionChanged(); }
        drag_ = Drag::None;
        setCursorShape(tool_ == Tool::Select ? IDC_ARROW : IDC_CROSS);
        if (fitted_) fit(); else invalidate();
    }
    Tool tool() const { return tool_; }
    void setStyle(const ToolStyle& style) { style_ = style; invalidate(); }
    void applyStyleToSelection() {
        if (const auto* current = doc_.find(selected_)) {
            auto shape = *current;
            using K = Shape::Kind;
            const auto k = shape.kind;
            if (k == K::Arrow || k == K::Rectangle || k == K::Text || k == K::Highlight) shape.color = style_.color;
            if (k == K::Arrow || k == K::Rectangle) shape.width = style_.width;
            if (k == K::Blur) shape.strength = style_.strength;
            if (k == K::Text) { shape.fontPixels = style_.fontPixels; shape.fontFamily = style_.fontFamily; }
            if (k == K::Text && !shape.text.empty()) { const auto size = owner_.measure(shape.text, shape.fontFamily, shape.fontPixels); shape.rect.w = size.width + 1; shape.rect.h = std::max(1, size.height); }
            if (doc_.replace(shape)) owner_.canvasChanged();
        }
        invalidate();
    }
    double zoom() const { return scale_; }
    void fit() {
        const auto bounds = visibleBounds();
        const auto size = clientSize();
        const double roomW = size.x - 32, roomH = size.y - 32;
        scale_ = std::clamp(std::min({1.0, roomW / bounds.w, roomH / bounds.h}), minimumZoom, maximumZoom);
        offset_ = {(size.x - bounds.w * scale_) / 2, (size.y - bounds.h * scale_) / 2};
        fitted_ = true;
        invalidate();
        owner_.canvasViewChanged();
    }
    void actualSize() { const auto size = clientSize(); zoomBy(1.0 / scale_, {size.x / 2.0, size.y / 2.0}); }
    void zoomBy(double factor, PointD anchor = {-1, -1}) {
        const auto size = clientSize();
        if (anchor.x < 0) anchor = {size.x / 2.0, size.y / 2.0};
        const auto image = toImage(anchor);
        const auto bounds = visibleBounds();
        scale_ = std::clamp(scale_ * factor, minimumZoom, maximumZoom);
        offset_ = {anchor.x - (image.x - bounds.x) * scale_, anchor.y - (image.y - bounds.y) * scale_};
        fitted_ = false;
        clampOffset();
        invalidate();
        owner_.canvasViewChanged();
    }
    bool applyCrop() {
        if (tool_ != Tool::Crop) return false;
        const bool changed = doc_.setCrop(cropDraft_);
        tool_ = Tool::Select;
        setCursorShape(IDC_ARROW);
        owner_.canvasToolRequested(Tool::Select);
        documentChanged();
        if (changed) owner_.canvasChanged();
        fit();
        return changed;
    }
    void cancelCrop() {
        if (tool_ != Tool::Crop) return;
        cropDraft_ = doc_.crop();
        tool_ = Tool::Select;
        setCursorShape(IDC_ARROW);
        owner_.canvasToolRequested(Tool::Select);
        fit();
    }
    bool cropPending() const { return tool_ == Tool::Crop; }
    bool editingText() const { return textEditor_ != nullptr; }
    uint64_t selected() const { return selected_; }
    // Ends a pointer gesture without committing it (an agent edit is about to land).
    void abortGesture() {
        if (drag_ == Drag::None) return;
        const bool shape = drag_ == Drag::Create || drag_ == Drag::Move || drag_ == Drag::Resize;
        drag_ = Drag::None;
        editOnClick_ = 0;
        if (GetCapture() == hwnd_) ReleaseCapture();
        if (shape && doc_.inGesture()) doc_.cancelGesture();
        invalidate();
    }

    // Documents can change outside the canvas (undo/redo, agents); drop stale UI state.
    void documentChanged() {
        if (drag_ != Drag::None && drag_ != Drag::Pan) {
            // An agent edit landed mid-gesture: the gesture was already cancelled by
            // the swap of the document; stop mapping the pointer through it.
            drag_ = Drag::None;
            if (GetCapture() == hwnd_) ReleaseCapture();
        }
        if (!doc_.find(selected_)) { selected_ = 0; owner_.canvasSelectionChanged(); }
        if (textShape_ && !doc_.find(textShape_)) finishText(false);
        for (auto it = tiles_.begin(); it != tiles_.end();) {
            if (!doc_.find(it->first)) { cacheBytes_ -= it->second.image.bytes(); it = tiles_.erase(it); }
            else ++it;
        }
        if (fitted_) fit(); else { clampOffset(); invalidate(); }
    }

    void finishText(bool commit) {
        if (!textEditor_) return;
        const int length = GetWindowTextLengthW(textEditor_);
        std::wstring buffer(size_t(length) + 1, L'\0');
        GetWindowTextW(textEditor_, buffer.data(), length + 1);
        buffer.resize(size_t(length));
        const auto entered = text::trim(paths::narrow(buffer));
        HWND editor = textEditor_;
        textEditor_ = nullptr;
        ++textGeneration_;
        ShowWindow(editor, SW_HIDE);
        DestroyWindow(editor);
        if (textFont_) { DeleteObject(textFont_); textFont_ = nullptr; }
        const auto* existing = doc_.find(textShape_);
        if (commit) {
            Shape shape = existing ? *existing : Shape{};
            if (!existing) { shape.kind = Shape::Kind::Text; shape.color = style_.color; shape.fontPixels = style_.fontPixels; shape.fontFamily = style_.fontFamily; shape.width = style_.width; }
            shape.text = truncateUtf16(entered, EditDocument::maximumTextCharacters);
            const auto size = owner_.measure(shape.text, shape.fontFamily, shape.fontPixels);
            shape.rect = {int(textAnchor_.x), int(textAnchor_.y), size.width + 1, std::max(1, size.height)};
            if (entered.empty()) { if (existing && doc_.remove(existing->id)) owner_.canvasChanged(); selected_ = 0; }
            else if (existing) { if (doc_.replace(shape)) owner_.canvasChanged(); }
            else { selected_ = doc_.add(shape); owner_.canvasChanged(); }
        }
        textShape_ = 0;
        owner_.canvasSelectionChanged();
        if (IsWindowEnabled(hwnd_)) SetFocus(hwnd_);
        invalidate();
    }

private:
    enum class Drag { None, Create, Move, Resize, Pan, CropNew, CropMove, CropResize };
    struct Tile { std::string key; Raster image; uint64_t used = 0; };

    static LRESULT CALLBACK proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
        Canvas* self = nullptr;
        if (message == WM_NCCREATE) {
            self = static_cast<Canvas*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, LONG_PTR(self));
            self->hwnd_ = hwnd;
        } else {
            self = reinterpret_cast<Canvas*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        }
        if (!self) return DefWindowProcW(hwnd, message, wParam, lParam);
        if (message == WM_NCDESTROY) { SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0); self->hwnd_ = nullptr; return DefWindowProcW(hwnd, message, wParam, lParam); }
        return self->handle(message, wParam, lParam);
    }
    static LRESULT CALLBACK textProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR data) {
        auto* self = reinterpret_cast<Canvas*>(data);
        switch (message) {
        case WM_GETDLGCODE: return DLGC_WANTALLKEYS | DefSubclassProc(hwnd, message, wParam, lParam);
        case WM_KEYDOWN:
            // Plain Enter finishes the label (never uploads); Escape discards the change.
            if (wParam == VK_RETURN && !(GetKeyState(VK_CONTROL) & 0x8000)) { PostMessageW(self->hwnd_, textDoneMessage, 1, self->textGeneration_); return 0; }
            if (wParam == VK_ESCAPE) { PostMessageW(self->hwnd_, textDoneMessage, 0, self->textGeneration_); return 0; }
            break;
        case WM_CHAR:
            if (wParam == '\r' || wParam == '\n' || wParam == 27) return 0;
            break;
        case WM_KILLFOCUS: {
            // Focus moving elsewhere in the editor commits; switching apps keeps editing.
            const HWND next = reinterpret_cast<HWND>(wParam);
            const HWND top = GetAncestor(self->hwnd_, GA_ROOT);
            if (next && (next == top || IsChild(top, next))) PostMessageW(self->hwnd_, textDoneMessage, 1, self->textGeneration_);
            break;
        }
        case WM_NCDESTROY: RemoveWindowSubclass(hwnd, &Canvas::textProc, 1); break;
        default: break;
        }
        return DefSubclassProc(hwnd, message, wParam, lParam);
    }

    LRESULT handle(UINT message, WPARAM wParam, LPARAM lParam) {
        switch (message) {
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd_, &ps);
            paint(dc);
            EndPaint(hwnd_, &ps);
            return 0;
        }
        case WM_SIZE:
            back_.reset();
            // Never move the image while a gesture maps pointer positions through it.
            if (drag_ != Drag::None) { invalidate(); return 0; }
            if (fitted_) fit(); else { clampOffset(); invalidate(); }
            return 0;
        case WM_GETDLGCODE: {
            LRESULT code = DLGC_WANTARROWS | DLGC_WANTCHARS;
            if (const auto* msg = reinterpret_cast<const MSG*>(lParam); msg && (msg->message == WM_KEYDOWN || msg->message == WM_CHAR) &&
                (msg->wParam == VK_RETURN || msg->wParam == VK_ESCAPE || msg->wParam == VK_SPACE || msg->wParam == VK_DELETE || msg->wParam == VK_BACK))
                code |= DLGC_WANTMESSAGE;
            return code;
        }
        case WM_SETFOCUS: case WM_KILLFOCUS: invalidate(); return 0;
        case WM_SETCURSOR:
            if (LOWORD(lParam) == HTCLIENT) { SetCursor(LoadCursorW(nullptr, enabled_ ? cursor_ : IDC_WAIT)); return TRUE; }
            break;
        case WM_LBUTTONDOWN: case WM_MBUTTONDOWN: press(message == WM_MBUTTONDOWN, point(lParam), wParam); return 0;
        case WM_LBUTTONDBLCLK: doubleClick(point(lParam)); return 0;
        case WM_MOUSEMOVE: move(point(lParam), wParam); return 0;
        case WM_LBUTTONUP: case WM_MBUTTONUP: release(message == WM_MBUTTONUP, point(lParam), wParam); return 0;
        case WM_CAPTURECHANGED:
            // Losing the capture (a dialog, Alt+Tab) ends the gesture where it is.
            if (drag_ != Drag::None && reinterpret_cast<HWND>(lParam) != hwnd_) finishDrag(last_, 0);
            return 0;
        case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL: {
            if (!enabled_) return 0;
            POINT screen{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            ScreenToClient(hwnd_, &screen);
            const int delta = GET_WHEEL_DELTA_WPARAM(wParam);
            const auto keys = GET_KEYSTATE_WPARAM(wParam);
            if (message == WM_MOUSEWHEEL && (keys & MK_CONTROL)) zoomBy(std::pow(1.0015, delta), {double(screen.x), double(screen.y)});
            else {
                if (message == WM_MOUSEHWHEEL) offset_.x -= delta / 2.0;
                else if (keys & MK_SHIFT) offset_.x += delta / 2.0;
                else offset_.y += delta / 2.0;
                fitted_ = false;
                clampOffset();
                invalidate();
                owner_.canvasViewChanged();
            }
            return 0;
        }
        case WM_KEYDOWN: if (keyDown(wParam, lParam)) return 0; break;
        case WM_KEYUP:
            if (wParam == VK_SPACE) { space_ = false; hoverCursor(cursorPoint(), modifiers()); return 0; }
            if (wParam == VK_CONTROL) { hoverCursor(cursorPoint(), modifiers()); return 0; }
            break;
        case WM_CHAR: if (wParam == ' ' || wParam == '\r' || wParam == 27 || wParam == 8) return 0; break;
        case WM_COMMAND:
            if (HIWORD(wParam) == EN_CHANGE && reinterpret_cast<HWND>(lParam) == textEditor_) resizeTextEditor();
            return 0;
        case textDoneMessage:
            if (textEditor_ && LPARAM(textGeneration_) == lParam) finishText(wParam != 0);
            return 0;
        default: break;
        }
        return DefWindowProcW(hwnd_, message, wParam, lParam);
    }

    static PointD point(LPARAM lParam) { return {double(GET_X_LPARAM(lParam)), double(GET_Y_LPARAM(lParam))}; }
    PointD cursorPoint() const { POINT p; GetCursorPos(&p); ScreenToClient(hwnd_, &p); return {double(p.x), double(p.y)}; }
    static WPARAM modifiers() { return (GetKeyState(VK_CONTROL) & 0x8000 ? MK_CONTROL : 0) | (GetKeyState(VK_SHIFT) & 0x8000 ? MK_SHIFT : 0); }
    PointD clientSize() const { RECT r{}; if (hwnd_) GetClientRect(hwnd_, &r); return {double(std::max<LONG>(1, r.right)), double(std::max<LONG>(1, r.bottom))}; }
    int handlePixels() const { return MulDiv(9, int(dpi_), 96); }
    void invalidate() { if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE); }
    void setCursorShape(LPCWSTR shape) {
        cursor_ = shape;
        POINT p; GetCursorPos(&p);
        if (hwnd_ && WindowFromPoint(p) == hwnd_) SetCursor(LoadCursorW(nullptr, enabled_ ? shape : IDC_WAIT));
    }

    Rect visibleBounds() const { return tool_ == Tool::Crop ? doc_.imageRect() : doc_.crop(); }
    PointD toImage(PointD p) const { const auto b = visibleBounds(); return {(p.x - offset_.x) / scale_ + b.x, (p.y - offset_.y) / scale_ + b.y}; }
    PointD toWidget(PointD p) const { const auto b = visibleBounds(); return {(p.x - b.x) * scale_ + offset_.x, (p.y - b.y) * scale_ + offset_.y}; }
    PointD toWidget(Point p) const { return toWidget(PointD{double(p.x), double(p.y)}); }
    RectD toWidget(const Rect& r) const { const auto tl = toWidget(PointD{double(r.x), double(r.y)}); return {tl.x, tl.y, r.w * scale_, r.h * scale_}; }

    void clampOffset() {
        const auto bounds = visibleBounds();
        const auto size = clientSize();
        const double cw = bounds.w * scale_, ch = bounds.h * scale_;
        auto axis = [](double offset, double content, double room) { return content <= room ? (room - content) / 2 : std::clamp(offset, room - content - 48, 48.0); };
        offset_ = {axis(offset_.x, cw, size.x), axis(offset_.y, ch, size.y)};
    }
    Shape::Kind kindFor(Tool tool) const {
        switch (tool) {
        case Tool::Redact: return Shape::Kind::Redact;
        case Tool::Blur: return Shape::Kind::Blur;
        case Tool::Arrow: return Shape::Kind::Arrow;
        case Tool::Highlight: return Shape::Kind::Highlight;
        case Tool::Text: return Shape::Kind::Text;
        default: return Shape::Kind::Rectangle;
        }
    }
    bool drawingTool() const { return tool_ == Tool::Redact || tool_ == Tool::Blur || tool_ == Tool::Arrow || tool_ == Tool::Rectangle || tool_ == Tool::Highlight; }
    // Outward rounding: a partly covered source pixel is inside the box.
    static Rect boxFrom(PointD a, PointD b) {
        const int left = int(std::floor(std::min(a.x, b.x))), top = int(std::floor(std::min(a.y, b.y)));
        const int right = int(std::ceil(std::max(a.x, b.x))), bottom = int(std::ceil(std::max(a.y, b.y)));
        return {left, top, std::max(1, right - left), std::max(1, bottom - top)};
    }
    static void handlePoints(const RectD& r, PointD out[8]) {
        const double cx = r.x + r.w / 2, cy = r.y + r.h / 2;
        out[0] = {r.x, r.y}; out[1] = {cx, r.y}; out[2] = {r.right(), r.y}; out[3] = {r.right(), cy};
        out[4] = {r.right(), r.bottom()}; out[5] = {cx, r.bottom()}; out[6] = {r.x, r.bottom()}; out[7] = {r.x, cy};
    }
    int handleAt(const Rect& rect, PointD p) const {
        PointD points[8];
        handlePoints(toWidget(rect), points);
        for (int i = 0; i < 8; ++i) if (distance(points[i], p) <= handlePixels()) return i;
        return -1;
    }
    int arrowHandleAt(const Shape& shape, PointD p) const {
        if (distance(toWidget(shape.from), p) <= handlePixels()) return 0;
        if (distance(toWidget(shape.to), p) <= handlePixels()) return 1;
        return -1;
    }
    int selectionHandleAt(const Shape& shape, PointD p) const {
        if (shape.kind == Shape::Kind::Arrow) return arrowHandleAt(shape, p);
        const int handle = handleAt(shape.rect, p);
        return shape.kind == Shape::Kind::Text && handle % 2 != 0 ? -1 : handle;
    }
    static RectD box(const Rect& r) { return {double(r.x), double(r.y), double(r.w), double(r.h)}; }
    static PointD pt(Point p) { return {double(p.x), double(p.y)}; }
    // A drawing tool picks up what the user sees: an outlined rectangle by its
    // outline, an arrow by its line, filled shapes and text by their area.
    uint64_t grabAt(PointD p) const {
        const auto& shapes = doc_.shapes();
        const double tolerance = 6 / scale_;
        for (auto it = shapes.rbegin(); it != shapes.rend(); ++it) {
            if (it->kind == Shape::Kind::Arrow) { if (distanceToSegment(p, pt(it->from), pt(it->to)) <= std::max(tolerance, double(it->width))) return it->id; continue; }
            const auto area = box(it->rect);
            if (!area.adjusted(-tolerance, -tolerance, tolerance, tolerance).contains(p)) continue;
            if (it->kind == Shape::Kind::Rectangle) { const double edge = tolerance + it->width / 2.0; if (area.adjusted(edge, edge, -edge, -edge).contains(p)) continue; }
            return it->id;
        }
        return 0;
    }
    uint64_t shapeAt(PointD p) const {
        const auto& shapes = doc_.shapes();
        const double tolerance = 6 / scale_;
        for (auto it = shapes.rbegin(); it != shapes.rend(); ++it) {
            if (it->kind == Shape::Kind::Arrow) { if (distanceToSegment(p, pt(it->from), pt(it->to)) <= std::max(tolerance, double(it->width))) return it->id; continue; }
            if (box(it->rect).adjusted(-tolerance, -tolerance, tolerance, tolerance).contains(p)) return it->id;
        }
        return 0;
    }
    void hoverCursor(PointD position, WPARAM keys) {
        if (drag_ != Drag::None) return;
        if (space_) { setCursorShape(IDC_SIZEALL); return; }
        if (tool_ == Tool::Crop) {
            const int h = handleAt(cropDraft_, position);
            setCursorShape(h >= 0 ? resizeCursor(h) : box(cropDraft_).contains(toImage(position)) ? IDC_SIZEALL : IDC_CROSS);
            return;
        }
        const LPCWSTR fallback = tool_ == Tool::Select ? IDC_ARROW : IDC_CROSS;
        // Ctrl draws a new shape even over existing ones.
        if (drawingTool() && (keys & MK_CONTROL)) { setCursorShape(fallback); return; }
        const auto image = toImage(position);
        if (const auto* shape = doc_.find(selected_); shape && (tool_ != Tool::Text || shape->kind == Shape::Kind::Text)) {
            const int h = selectionHandleAt(*shape, position);
            if (h >= 0) { setCursorShape(shape->kind == Shape::Kind::Arrow ? IDC_SIZEALL : resizeCursor(h)); return; }
        }
        uint64_t hit = 0;
        if (tool_ == Tool::Select) hit = shapeAt(image);
        else if (tool_ == Tool::Text) { const auto* shape = doc_.find(shapeAt(image)); hit = shape && shape->kind == Shape::Kind::Text ? shape->id : 0; }
        else if (drawingTool()) hit = grabAt(image);
        setCursorShape(hit ? IDC_SIZEALL : fallback);
    }
    // Dragging a text corner scales the font around the opposite corner.
    Shape scaledText(const Shape& original, int handle, PointD now) const {
        const auto r = box(original.rect);
        const PointD corners[4] = {{r.x, r.y}, {r.right(), r.y}, {r.right(), r.bottom()}, {r.x, r.bottom()}};
        const PointD dragged = corners[handle / 2], anchor = corners[(handle / 2 + 2) % 4];
        const double dx = dragged.x - anchor.x, dy = dragged.y - anchor.y, length = dx * dx + dy * dy;
        const double scale = length > 0 ? std::max(0.0, ((now.x - anchor.x) * dx + (now.y - anchor.y) * dy) / length) : 1.0;
        auto shape = original;
        shape.fontPixels = std::clamp(int(std::lround(original.fontPixels * scale)), minimumFontPixels, maximumFontPixels);
        const auto size = owner_.measure(shape.text, shape.fontFamily, shape.fontPixels);
        const int width = size.width + 1, height = std::max(1, size.height);
        const auto& b = original.rect;
        if (handle == 0) shape.rect = {b.right() - width, b.bottom() - height, width, height};
        else if (handle == 2) shape.rect = {b.x, b.bottom() - height, width, height};
        else if (handle == 4) shape.rect = {b.x, b.y, width, height};
        else shape.rect = {b.right() - width, b.y, width, height};
        return shape;
    }
    static Rect resized(const Rect& r, int handle, Point d) {
        int l = r.x, t = r.y, rr = r.right(), bb = r.bottom();
        if (handle == 0 || handle == 6 || handle == 7) l += d.x;
        if (handle == 2 || handle == 3 || handle == 4) rr += d.x;
        if (handle == 0 || handle == 1 || handle == 2) t += d.y;
        if (handle == 4 || handle == 5 || handle == 6) bb += d.y;
        if (rr < l) std::swap(l, rr);
        if (bb < t) std::swap(t, bb);
        return {l, t, std::max(1, rr - l), std::max(1, bb - t)};
    }

    const Raster& blurTile(const Shape& shape) {
        const double bucket = std::pow(2.0, std::round(std::log2(std::max(scale_, 1e-3)) * 2) / 2);
        const auto crop = doc_.crop();
        std::string key = std::to_string(shape.rect.x) + "," + std::to_string(shape.rect.y) + "," + std::to_string(shape.rect.w) + "," + std::to_string(shape.rect.h) + "/" +
                          std::to_string(shape.strength) + "/" + std::to_string(bucket) + "/" + std::to_string(crop.x) + "," + std::to_string(crop.y) + "," +
                          std::to_string(crop.w) + "," + std::to_string(crop.h);
        for (const auto& other : doc_.shapes())
            if (other.kind == Shape::Kind::Redact && other.rect.intersects(shape.rect))
                key += ";" + std::to_string(other.rect.x) + "," + std::to_string(other.rect.y) + "," + std::to_string(other.rect.w) + "," + std::to_string(other.rect.h);
        auto& tile = tiles_[shape.id];
        tile.used = ++tick_;
        if (tile.key == key) return tile.image;
        cacheBytes_ -= tile.image.bytes();
        tile.image = doc_.blurPreview(shape, bucket);
        tile.key = key;
        cacheBytes_ += tile.image.bytes();
        // Least-recently painted tiles are recomputed later rather than retained.
        while (cacheBytes_ > blurCacheBytes && tiles_.size() > 1) {
            auto oldest = tiles_.end();
            for (auto it = tiles_.begin(); it != tiles_.end(); ++it)
                if (it->first != shape.id && (oldest == tiles_.end() || it->second.used < oldest->second.used)) oldest = it;
            if (oldest == tiles_.end()) break;
            cacheBytes_ -= oldest->second.image.bytes();
            tiles_.erase(oldest);
        }
        return tiles_[shape.id].image;
    }

    void paint(HDC dc) {
        const auto size = clientSize();
        const int cw = int(size.x), ch = int(size.y);
        if (!back_ || int(back_->GetWidth()) != cw || int(back_->GetHeight()) != ch) back_ = std::make_unique<Gdiplus::Bitmap>(cw, ch, PixelFormat32bppPARGB);
        const Gdiplus::Color background(255, 58, 58, 60), accent = gdi(GetSysColor(COLOR_HIGHLIGHT));
        {
            Gdiplus::Graphics g(back_.get());
            g.Clear(background);
            g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
            const auto bounds = visibleBounds();
            const auto target = toWidget(bounds);
            // Paint only the visible source rectangle; never a scaled full frame at 100%+.
            const auto visible = target.intersected({0, 0, size.x, size.y});
            if (!visible.empty() && base_) {
                const double pixels = bounds.w * scale_ * bounds.h * scale_;
                if (scale_ < 1 && pixels <= displayCachePixels) {
                    // An area-averaged downscale of the visible bounds, rebuilt on zoom.
                    const int dw = std::max(1, int(std::lround(bounds.w * scale_))), dh = std::max(1, int(std::lround(bounds.h * scale_)));
                    if (displayBounds_ != bounds || displayScale_ != scale_ || display_.isNull()) {
                        display_ = {};
                        display_ = EditDocument::scaled(doc_.base(), bounds, dw, dh);
                        displayBounds_ = bounds;
                        displayScale_ = scale_;
                    }
                    if (const auto bitmap = view(display_)) {
                        g.SetInterpolationMode(Gdiplus::InterpolationModeBilinear);
                        g.DrawImage(bitmap.get(), target.f(), 0, 0, Gdiplus::REAL(display_.width), Gdiplus::REAL(display_.height), Gdiplus::UnitPixel);
                    }
                } else {
                    display_ = {};
                    const auto from = toImage({visible.x, visible.y}), to = toImage({visible.right(), visible.bottom()});
                    g.SetInterpolationMode(scale_ >= 1 ? Gdiplus::InterpolationModeNearestNeighbor : Gdiplus::InterpolationModeBilinear);
                    g.DrawImage(base_.get(), visible.f(), Gdiplus::REAL(from.x), Gdiplus::REAL(from.y), Gdiplus::REAL(to.x - from.x), Gdiplus::REAL(to.y - from.y), Gdiplus::UnitPixel);
                }
            }
            g.SetClip(target.f());
            g.TranslateTransform(Gdiplus::REAL(offset_.x), Gdiplus::REAL(offset_.y));
            g.ScaleTransform(Gdiplus::REAL(scale_), Gdiplus::REAL(scale_));
            g.TranslateTransform(Gdiplus::REAL(-bounds.x), Gdiplus::REAL(-bounds.y));
            Gdiplus::SolidBrush black(Gdiplus::Color(255, 0, 0, 0));
            for (const auto& shape : doc_.shapes()) {
                if (shape.kind == Shape::Kind::Redact) {
                    g.SetSmoothingMode(Gdiplus::SmoothingModeNone);
                    g.FillRectangle(&black, shape.rect.x, shape.rect.y, shape.rect.w, shape.rect.h);
                } else if (shape.kind == Shape::Kind::Blur) {
                    const auto& tile = blurTile(shape);
                    const auto area = shape.rect.intersected(doc_.crop());
                    if (const auto bitmap = view(tile); bitmap && !area.empty()) {
                        g.SetInterpolationMode(Gdiplus::InterpolationModeBilinear);
                        g.DrawImage(bitmap.get(), Gdiplus::RectF(Gdiplus::REAL(area.x), Gdiplus::REAL(area.y), Gdiplus::REAL(area.w), Gdiplus::REAL(area.h)), 0, 0,
                                    Gdiplus::REAL(tile.width), Gdiplus::REAL(tile.height), Gdiplus::UnitPixel);
                    }
                } else if (!(textEditor_ && shape.id == textShape_)) {
                    exporter::paintAnnotation(g, shape);
                }
            }
            g.ResetTransform();
            g.ResetClip();
            g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            auto handles = [&](const PointD* points, int count) {
                Gdiplus::Pen pen(accent, Gdiplus::REAL(1.5));
                Gdiplus::SolidBrush fill(background);
                const double half = MulDiv(4, int(dpi_), 96);
                for (int i = 0; i < count; ++i) {
                    const Gdiplus::RectF r(Gdiplus::REAL(points[i].x - half), Gdiplus::REAL(points[i].y - half), Gdiplus::REAL(2 * half), Gdiplus::REAL(2 * half));
                    g.FillRectangle(&fill, r);
                    g.DrawRectangle(&pen, r);
                }
            };
            if (tool_ == Tool::Crop) {
                const auto crop = toWidget(cropDraft_);
                {
                    Gdiplus::SolidBrush dim(Gdiplus::Color(170, 58, 58, 60));
                    g.SetClip(target.f());
                    g.ExcludeClip(crop.f());
                    g.FillRectangle(&dim, target.f());
                    g.ResetClip();
                }
                Gdiplus::Pen frame(accent, 2);
                g.DrawRectangle(&frame, crop.f());
                Gdiplus::Pen thirds(Gdiplus::Color(90, 255, 255, 255), 1);
                thirds.SetDashStyle(Gdiplus::DashStyleDash);
                for (int i = 1; i < 3; ++i) {
                    g.DrawLine(&thirds, Gdiplus::REAL(crop.x + crop.w * i / 3), Gdiplus::REAL(crop.y), Gdiplus::REAL(crop.x + crop.w * i / 3), Gdiplus::REAL(crop.bottom()));
                    g.DrawLine(&thirds, Gdiplus::REAL(crop.x), Gdiplus::REAL(crop.y + crop.h * i / 3), Gdiplus::REAL(crop.right()), Gdiplus::REAL(crop.y + crop.h * i / 3));
                }
                PointD points[8];
                handlePoints(crop, points);
                handles(points, 8);
            } else if (const auto* shape = doc_.find(selected_)) {
                if (shape->kind == Shape::Kind::Arrow) {
                    const PointD points[2] = {toWidget(shape->from), toWidget(shape->to)};
                    handles(points, 2);
                } else {
                    const auto r = toWidget(shape->rect);
                    Gdiplus::Pen dashed(accent, 1);
                    dashed.SetDashStyle(Gdiplus::DashStyleDash);
                    g.DrawRectangle(&dashed, r.adjusted(-2, -2, 2, 2).f());
                    PointD points[8];
                    handlePoints(r, points);
                    if (shape->kind == Shape::Kind::Text) { const PointD corners[4] = {points[0], points[2], points[4], points[6]}; handles(corners, 4); }
                    else handles(points, 8);
                }
            }
            if (GetFocus() == hwnd_) {
                Gdiplus::Pen focus(accent, 2);
                g.SetSmoothingMode(Gdiplus::SmoothingModeNone);
                g.DrawRectangle(&focus, Gdiplus::RectF(1, 1, Gdiplus::REAL(cw - 2), Gdiplus::REAL(ch - 2)));
            }
        }
        Gdiplus::Graphics screen(dc);
        screen.SetCompositingMode(Gdiplus::CompositingModeSourceCopy);
        screen.SetInterpolationMode(Gdiplus::InterpolationModeNearestNeighbor);
        screen.DrawImage(back_.get(), 0, 0, cw, ch);
    }

    void press(bool middle, PointD position, WPARAM keys) {
        if (!enabled_) return;
        SetFocus(hwnd_);
        press_ = last_ = position;
        const auto image = toImage(press_);
        if (middle || space_) { drag_ = Drag::Pan; SetCapture(hwnd_); setCursorShape(IDC_SIZEALL); return; }
        if (textEditor_) { finishText(true); return; }
        if (tool_ == Tool::Crop) {
            cropOriginal_ = cropDraft_;
            handle_ = handleAt(cropDraft_, press_);
            // An uncropped image has nothing to move: dragging anywhere draws a new crop.
            drag_ = handle_ >= 0 ? Drag::CropResize : (box(cropDraft_).contains(image) && cropDraft_ != doc_.imageRect()) ? Drag::CropMove : Drag::CropNew;
            SetCapture(hwnd_);
            return;
        }
        moved_ = false;
        editOnClick_ = 0;
        auto begin = [&](Drag kind, const Shape& shape, int handle = -1) { original_ = shape; handle_ = handle; drag_ = kind; doc_.beginGesture(); SetCapture(hwnd_); };
        if (tool_ == Tool::Text) {
            if (const auto* current = doc_.find(selected_); current && current->kind == Shape::Kind::Text) {
                const int handle = selectionHandleAt(*current, press_);
                if (handle >= 0) { begin(Drag::Resize, *current, handle); return; }
            }
            const auto* shape = doc_.find(shapeAt(image));
            // Dragging a text label moves it; a click edits it.
            if (shape && shape->kind == Shape::Kind::Text) {
                if (selected_ != shape->id) { selected_ = shape->id; owner_.canvasSelectionChanged(); }
                editOnClick_ = shape->id;
                begin(Drag::Move, *doc_.find(editOnClick_));
                invalidate();
                return;
            }
            beginText(image, nullptr);
            return;
        }
        const bool force = drawingTool() && (keys & MK_CONTROL);
        // Handles and the visible body of the current selection edit it in every tool.
        if (!force) if (const auto* shape = doc_.find(selected_)) {
            const int handle = selectionHandleAt(*shape, press_);
            if (handle >= 0) { begin(Drag::Resize, *shape, handle); return; }
            if ((tool_ == Tool::Select ? shapeAt(image) : grabAt(image)) == selected_) { begin(Drag::Move, *shape); return; }
        }
        if (tool_ == Tool::Select) {
            selected_ = shapeAt(image);
            owner_.canvasSelectionChanged();
            if (const auto* shape = doc_.find(selected_)) begin(Drag::Move, *shape);
            invalidate();
            return;
        }
        if (!drawingTool()) return;
        // A drawing tool picks up a visible shape to move it; Ctrl draws over it.
        if (!force) if (const auto hit = grabAt(image)) {
            selected_ = hit;
            owner_.canvasSelectionChanged();
            begin(Drag::Move, *doc_.find(hit));
            invalidate();
            return;
        }
        Shape shape;
        shape.kind = kindFor(tool_);
        shape.color = shape.kind == Shape::Kind::Redact ? 0x000000 : style_.color;
        shape.width = style_.width; shape.strength = style_.strength; shape.fontPixels = style_.fontPixels; shape.fontFamily = style_.fontFamily;
        const Point at{int(std::floor(image.x)), int(std::floor(image.y))};
        shape.from = shape.to = at;
        shape.rect = boxFrom(image, image);
        doc_.beginGesture();
        selected_ = doc_.add(shape);
        original_ = *doc_.find(selected_);
        drag_ = Drag::Create;
        SetCapture(hwnd_);
        owner_.canvasSelectionChanged();
        invalidate();
    }

    void move(PointD position, WPARAM keys) {
        if (drag_ == Drag::None) { if (enabled_) hoverCursor(position, keys); return; }
        // A press that has not travelled the drag distance is a click.
        if (drag_ == Drag::Move && !moved_) {
            if (std::abs(position.x - press_.x) < GetSystemMetrics(SM_CXDRAG) && std::abs(position.y - press_.y) < GetSystemMetrics(SM_CYDRAG)) return;
            moved_ = true;
        }
        if (drag_ == Drag::Pan) {
            offset_.x += position.x - last_.x; offset_.y += position.y - last_.y;
            last_ = position;
            fitted_ = false;
            clampOffset();
            invalidate();
            owner_.canvasViewChanged();
            return;
        }
        const auto start = toImage(press_), now = toImage(position);
        const Point delta{int(std::lround(now.x - start.x)), int(std::lround(now.y - start.y))};
        last_ = position;
        const auto image = doc_.imageRect();
        if (drag_ == Drag::CropNew) { cropDraft_ = EditDocument::clampRect(boxFrom(start, now), image); if (cropDraft_.empty()) cropDraft_ = cropOriginal_; invalidate(); return; }
        if (drag_ == Drag::CropMove) {
            auto moved = cropOriginal_.translated(delta.x, delta.y);
            moved.x = std::clamp(moved.x, 0, image.w - moved.w);
            moved.y = std::clamp(moved.y, 0, image.h - moved.h);
            cropDraft_ = moved;
            invalidate();
            return;
        }
        if (drag_ == Drag::CropResize) { const auto r = EditDocument::clampRect(resized(cropOriginal_, handle_, delta), image); if (!r.empty()) cropDraft_ = r; invalidate(); return; }
        auto shape = original_;
        if (drag_ == Drag::Create) {
            if (shape.kind == Shape::Kind::Arrow) shape.to = {int(std::lround(now.x)), int(std::lround(now.y))};
            else {
                shape.rect = boxFrom(start, now);
                if (keys & MK_SHIFT) { const int side = std::max(shape.rect.w, shape.rect.h); shape.rect.w = shape.rect.h = side; }
            }
        } else if (drag_ == Drag::Move) {
            shape.rect = shape.rect.translated(delta.x, delta.y);
            shape.from = {shape.from.x + delta.x, shape.from.y + delta.y};
            shape.to = {shape.to.x + delta.x, shape.to.y + delta.y};
        } else if (drag_ == Drag::Resize) {
            if (shape.kind == Shape::Kind::Arrow) { if (handle_ == 0) shape.from = {shape.from.x + delta.x, shape.from.y + delta.y}; else shape.to = {shape.to.x + delta.x, shape.to.y + delta.y}; }
            else if (shape.kind == Shape::Kind::Text) shape = scaledText(original_, handle_, now);
            else shape.rect = resized(shape.rect, handle_, delta);
        }
        doc_.preview(shape);
        invalidate();
        // The size control follows a text label while its corner is dragged.
        if (drag_ == Drag::Resize && shape.kind == Shape::Kind::Text) owner_.canvasSelectionChanged();
    }

    void release(bool middle, PointD position, WPARAM keys) {
        if (drag_ == Drag::Pan) { drag_ = Drag::None; if (GetCapture() == hwnd_) ReleaseCapture(); hoverCursor(position, keys); return; }
        if (middle) return;
        finishDrag(position, keys);
    }
    void finishDrag(PointD position, WPARAM keys) {
        const auto kind = drag_;
        drag_ = Drag::None;
        if (GetCapture() == hwnd_) ReleaseCapture();
        const auto edit = std::exchange(editOnClick_, 0);
        if (kind == Drag::Pan || kind == Drag::CropNew || kind == Drag::CropMove || kind == Drag::CropResize || kind == Drag::None) return;
        if (!doc_.inGesture()) { invalidate(); return; } // an agent edit replaced the document mid-drag
        if (kind == Drag::Create) {
            const auto* shape = doc_.find(selected_);
            const bool tiny = !shape || (shape->kind == Shape::Kind::Arrow ? distance(toWidget(shape->from), toWidget(shape->to)) < 6
                                                                             : (shape->rect.w * scale_ < 4 || shape->rect.h * scale_ < 4));
            if (tiny) { doc_.cancelGesture(); selected_ = 0; owner_.canvasSelectionChanged(); invalidate(); return; }
        }
        doc_.commitGesture();
        if (kind == Drag::Move && edit && !moved_) if (const auto* shape = doc_.find(edit)) { beginText({double(shape->rect.x), double(shape->rect.y)}, shape); return; }
        invalidate();
        owner_.canvasChanged();
        hoverCursor(position, keys);
    }
    void doubleClick(PointD position) {
        if (!enabled_) return;
        const auto* shape = doc_.find(shapeAt(toImage(position)));
        if (shape && shape->kind == Shape::Kind::Text) beginText({double(shape->rect.x), double(shape->rect.y)}, shape);
    }

    bool keyDown(WPARAM key, LPARAM lParam) {
        if (!enabled_) return true;
        const bool repeat = (lParam & (1 << 30)) != 0;
        const bool control = GetKeyState(VK_CONTROL) & 0x8000, shift = GetKeyState(VK_SHIFT) & 0x8000;
        if (key == VK_SPACE) { if (!repeat) { space_ = true; setCursorShape(IDC_SIZEALL); } return true; }
        if (key == VK_CONTROL) { hoverCursor(cursorPoint(), MK_CONTROL); return true; }
        if (tool_ == Tool::Crop) {
            // Plain Enter applies only the pending crop; upload requires Ctrl+Enter.
            if (key == VK_RETURN && !control) { applyCrop(); return true; }
            if (key == VK_ESCAPE) { cancelCrop(); return true; }
        }
        const int step = shift ? 10 : 1;
        Point nudge;
        if (key == VK_LEFT) nudge = {-step, 0};
        else if (key == VK_RIGHT) nudge = {step, 0};
        else if (key == VK_UP) nudge = {0, -step};
        else if (key == VK_DOWN) nudge = {0, step};
        if (nudge.x || nudge.y) {
            if (tool_ == Tool::Crop) { const auto r = cropDraft_.translated(nudge.x, nudge.y); if (doc_.imageRect().contains(r)) cropDraft_ = r; invalidate(); return true; }
            if (const auto* current = doc_.find(selected_)) {
                auto shape = *current;
                shape.rect = shape.rect.translated(nudge.x, nudge.y);
                shape.from = {shape.from.x + nudge.x, shape.from.y + nudge.y};
                shape.to = {shape.to.x + nudge.x, shape.to.y + nudge.y};
                if (doc_.replace(shape)) { invalidate(); owner_.canvasChanged(); }
            }
            return true;
        }
        if ((key == VK_DELETE || key == VK_BACK) && selected_) {
            if (doc_.remove(selected_)) { selected_ = 0; owner_.canvasSelectionChanged(); invalidate(); owner_.canvasChanged(); }
            return true;
        }
        if (key == VK_ESCAPE && selected_) { selected_ = 0; owner_.canvasSelectionChanged(); invalidate(); return true; }
        return false;
    }

    void beginText(PointD image, const Shape* existing) {
        textAnchor_ = {std::floor(image.x), std::floor(image.y)};
        textShape_ = existing ? existing->id : 0;
        const int pixels = existing ? existing->fontPixels : style_.fontPixels;
        auto family = existing ? existing->fontFamily : style_.fontFamily;
        if (family.empty()) family = exporter::defaultFontFamily();
        const int screenPixels = std::clamp(int(std::lround(pixels * scale_)), MulDiv(12, int(dpi_), 96), MulDiv(96, int(dpi_), 96));
        textFont_ = CreateFontW(-screenPixels, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                DEFAULT_PITCH | FF_SWISS, w(family).substr(0, LF_FACESIZE - 1).c_str());
        const auto at = toWidget(textAnchor_);
        textEditor_ = CreateWindowExW(0, WC_EDITW, existing ? w(existing->text).c_str() : L"", WS_CHILD | WS_BORDER | ES_AUTOHSCROLL, int(at.x) - 4, int(at.y) - 4, 220, screenPixels + 12,
                                      hwnd_, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!textEditor_) { textShape_ = 0; return; }
        ++textGeneration_;
        SendMessageW(textEditor_, WM_SETFONT, WPARAM(textFont_), FALSE);
        SendMessageW(textEditor_, EM_SETLIMITTEXT, EditDocument::maximumTextCharacters, 0);
        SendMessageW(textEditor_, EM_SETCUEBANNER, TRUE, LPARAM(L"Type, then press Enter"));
        SetWindowSubclass(textEditor_, &Canvas::textProc, 1, DWORD_PTR(this));
        resizeTextEditor();
        ShowWindow(textEditor_, SW_SHOW);
        SetFocus(textEditor_);
        SendMessageW(textEditor_, EM_SETSEL, 0, -1);
        selected_ = textShape_;
        owner_.canvasSelectionChanged();
        invalidate();
    }
    void resizeTextEditor() {
        if (!textEditor_) return;
        const int length = GetWindowTextLengthW(textEditor_);
        std::wstring buffer(size_t(length) + 1, L'\0');
        GetWindowTextW(textEditor_, buffer.data(), length + 1);
        HDC dc = GetDC(textEditor_);
        const auto old = SelectObject(dc, textFont_);
        SIZE extent{}, line{};
        GetTextExtentPoint32W(dc, buffer.c_str(), length, &extent);
        GetTextExtentPoint32W(dc, L"Ay", 2, &line);
        SelectObject(dc, old);
        ReleaseDC(textEditor_, dc);
        SetWindowPos(textEditor_, nullptr, 0, 0, std::max(MulDiv(220, int(dpi_), 96), int(extent.cx) + MulDiv(48, int(dpi_), 96)), line.cy + MulDiv(12, int(dpi_), 96),
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }

    EditorWindow& owner_;
    EditDocument& doc_;
    std::unique_ptr<Gdiplus::Bitmap> base_, back_;
    HWND hwnd_ = nullptr;
    UINT dpi_ = 96;
    bool enabled_ = true;
    Tool tool_ = Tool::Redact;
    ToolStyle style_;
    double scale_ = 1;
    PointD offset_;
    bool fitted_ = true, space_ = false;
    Drag drag_ = Drag::None;
    int handle_ = -1;
    bool moved_ = false;
    uint64_t editOnClick_ = 0;
    PointD press_, last_;
    Shape original_;
    Rect cropDraft_, cropOriginal_;
    uint64_t selected_ = 0;
    std::map<uint64_t, Tile> tiles_;
    size_t cacheBytes_ = 0;
    uint64_t tick_ = 0;
    Raster display_;
    Rect displayBounds_;
    double displayScale_ = 0;
    LPCWSTR cursor_ = IDC_CROSS;
    HWND textEditor_ = nullptr;
    HFONT textFont_ = nullptr;
    PointD textAnchor_;
    uint64_t textShape_ = 0;
    unsigned textGeneration_ = 0;
};

// ---------------------------------------------------------------------------
// EditorWindow
// ---------------------------------------------------------------------------
EditorWindow::EditorWindow(EditDocument& doc, std::string destination, std::vector<std::string> recentColors, Callbacks callbacks)
    : doc_(doc), destination_(std::move(destination)), callbacks_(std::move(callbacks)) {
    for (const auto& color : recentColors)
        if (text::matchesColor(color) && std::find(recentColors_.begin(), recentColors_.end(), color) == recentColors_.end() && recentColors_.size() < size_t(recentCount))
            recentColors_.push_back(color);
}
EditorWindow::~EditorWindow() {
    canvas_.reset();
    if (hwnd_) { closing_ = true; DestroyWindow(hwnd_); }
    if (font_) DeleteObject(font_);
    if (boldFont_) DeleteObject(boldFont_);
}

TextSize EditorWindow::measure(const std::string& text, const std::string& family, int pixels) const { return exporter::measureText(text, family, pixels); }

bool EditorWindow::create() {
    static const bool registered = [] {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof wc;
        wc.lpfnWndProc = &EditorWindow::windowProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
        wc.hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1));
        if (!wc.hIcon) wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
        wc.lpszClassName = windowClass;
        return RegisterClassExW(&wc) != 0;
    }();
    if (!registered) return false;
    defaultFamily_ = exporter::defaultFontFamily();
    // Open on the monitor under the pointer, sized for the image in physical pixels.
    POINT cursor;
    GetCursorPos(&cursor);
    HMONITOR monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{};
    info.cbSize = sizeof info;
    GetMonitorInfoW(monitor, &info);
    UINT dpiX = 96, dpiY = 96;
    if (FAILED(GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY))) dpiX = 96;
    dpi_ = dpiX;
    const RECT work = info.rcWork;
    const int availableW = work.right - work.left, availableH = work.bottom - work.top;
    const int maximumW = std::max(320, int(availableW * 0.9)), maximumH = std::max(240, int(availableH * 0.9));
    const int width = std::min(std::max(doc_.width() + scaled(64), scaled(900)), maximumW);
    const int height = std::min(std::max(doc_.height() + scaled(150), scaled(620)), maximumH);
    const auto title = L"put.gd – Edit & upload to " + w(destination_);
    hwnd_ = CreateWindowExW(WS_EX_CONTROLPARENT, windowClass, title.c_str(), WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, work.left + (availableW - width) / 2,
                            work.top + (availableH - height) / 2, width, height, nullptr, nullptr, GetModuleHandleW(nullptr), this);
    if (!hwnd_) return false;
    dpi_ = GetDpiForWindow(hwnd_);
    createControls();
    applyFonts();
    selectTool(Tool::Redact);
    layout();
    ShowWindow(hwnd_, SW_SHOWNORMAL);
    UpdateWindow(hwnd_);
    SetForegroundWindow(hwnd_);
    canvas_->fit();
    SetFocus(canvas_->hwnd());
    return true;
}

void EditorWindow::createControls() {
    const auto instance = GetModuleHandleW(nullptr);
    auto make = [&](const wchar_t* cls, const wchar_t* text, DWORD style, int id, DWORD ex = 0) {
        return CreateWindowExW(ex, cls, text, WS_CHILD | style, 0, 0, 10, 10, hwnd_, reinterpret_cast<HMENU>(INT_PTR(id)), instance, nullptr);
    };
    tooltip_ = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, hwnd_, nullptr, instance, nullptr);
    auto tip = [&](int id, const std::wstring& text) {
        TTTOOLINFOW tool{};
        tool.cbSize = sizeof tool;
        tool.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
        tool.hwnd = hwnd_;
        tool.uId = UINT_PTR(control(id));
        tool.lpszText = const_cast<LPWSTR>(text.c_str());
        SendMessageW(tooltip_, TTM_ADDTOOLW, 0, LPARAM(&tool));
    };
    struct Entry { Tool tool; const wchar_t* label; const wchar_t* tip; };
    const Entry entries[] = {{Tool::Select, L"Select", L"Select and move (V)"}, {Tool::Redact, L"Redact", L"Redact — opaque black box (X)"},
                             {Tool::Blur, L"Blur", L"Blur — visual effect only (B)"}, {Tool::Crop, L"Crop", L"Crop (C)"},
                             {Tool::Arrow, L"Arrow", L"Arrow (A)"}, {Tool::Rectangle, L"Rectangle", L"Rectangle (R)"},
                             {Tool::Highlight, L"Highlight", L"Highlight (H)"}, {Tool::Text, L"Text", L"Text (T)"}};
    for (const auto& entry : entries) {
        const int id = idToolFirst + int(entry.tool);
        make(WC_BUTTONW, entry.label, WS_VISIBLE | WS_TABSTOP | BS_PUSHLIKE | BS_AUTORADIOBUTTON | (entry.tool == Tool::Select ? WS_GROUP : 0), id);
        tip(id, entry.tip);
    }
    make(WC_BUTTONW, L"Undo", WS_VISIBLE | WS_TABSTOP | WS_GROUP | BS_PUSHBUTTON, idUndo); tip(idUndo, L"Undo (Ctrl+Z)");
    make(WC_BUTTONW, L"Redo", WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, idRedo); tip(idRedo, L"Redo (Ctrl+Shift+Z)");
    make(WC_BUTTONW, L"Fit", WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, idFit); tip(idFit, L"Fit to window (Ctrl+0)");
    make(WC_BUTTONW, L"100%", WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, idActual); tip(idActual, L"Actual size, 100% (Ctrl+1)");
    make(WC_STATICW, L"", WS_VISIBLE | SS_RIGHT | SS_CENTERIMAGE | SS_NOPREFIX, idStatus);

    make(WC_STATICW, L"Color", SS_CENTERIMAGE, idColorLabel);
    for (int i = 0; i < paletteCount; ++i) {
        const auto name = L"Color " + w(colorName(palette[i]));
        make(WC_BUTTONW, name.c_str(), WS_TABSTOP | BS_OWNERDRAW | (i == 0 ? WS_GROUP : 0), idSwatchFirst + i);
        tip(idSwatchFirst + i, w(colorName(palette[i])));
    }
    for (int i = 0; i < recentCount; ++i) make(WC_BUTTONW, L"Recent color", WS_TABSTOP | BS_OWNERDRAW, idRecentFirst + i);
    make(WC_BUTTONW, L"Custom color…", WS_TABSTOP | BS_OWNERDRAW, idCustomColor);
    tip(idCustomColor, L"Custom color…");
    make(WC_STATICW, L"Line", SS_CENTERIMAGE, idWidthLabel);
    HWND widthBox = make(WC_COMBOBOXW, L"Line width", WS_TABSTOP | WS_GROUP | CBS_DROPDOWNLIST | WS_VSCROLL, idWidth);
    for (const auto* label : {L"Thin", L"Medium", L"Thick"}) SendMessageW(widthBox, CB_ADDSTRING, 0, LPARAM(label));
    SendMessageW(widthBox, CB_SETCURSEL, 1, 0);
    make(WC_STATICW, L"Strength", SS_CENTERIMAGE, idStrengthLabel);
    HWND strength = make(TRACKBAR_CLASSW, L"Blur strength", WS_TABSTOP | WS_GROUP | TBS_HORZ | TBS_NOTICKS, idStrength);
    SendMessageW(strength, TBM_SETRANGE, TRUE, MAKELPARAM(EditDocument::minimumBlur, EditDocument::maximumBlur));
    SendMessageW(strength, TBM_SETPOS, TRUE, 12);
    make(WC_STATICW, L"Font", SS_CENTERIMAGE, idFontLabel);
    make(WC_COMBOBOXW, L"Font", WS_TABSTOP | WS_GROUP | CBS_DROPDOWNLIST | WS_VSCROLL, idFont);
    make(WC_STATICW, L"Size", SS_CENTERIMAGE, idSizeLabel);
    HWND size = make(WC_EDITW, L"28", WS_TABSTOP | WS_BORDER | ES_NUMBER | ES_RIGHT, idSize, 0);
    SendMessageW(size, EM_SETLIMITTEXT, 3, 0);
    HWND spin = make(UPDOWN_CLASSW, L"", UDS_SETBUDDYINT | UDS_ALIGNRIGHT | UDS_ARROWKEYS | UDS_NOTHOUSANDS | UDS_HOTTRACK, idSizeSpin);
    SendMessageW(spin, UDM_SETBUDDY, WPARAM(size), 0);
    SendMessageW(spin, UDM_SETRANGE32, minimumFontPixels, maximumFontPixels);
    SendMessageW(spin, UDM_SETPOS32, 0, 28);
    tip(idSize, L"Text size in image pixels (8–200)");
    make(WC_STATICW, L"Drag to choose the area to keep.", SS_CENTERIMAGE, idCropHint);
    make(WC_BUTTONW, L"Apply crop", WS_TABSTOP | WS_GROUP | BS_PUSHBUTTON, idApplyCrop); tip(idApplyCrop, L"Apply crop (Enter)");
    make(WC_BUTTONW, L"Cancel", WS_TABSTOP | BS_PUSHBUTTON, idCancelCrop); tip(idCancelCrop, L"Cancel crop (Esc)");
    make(WC_STATICW, L"", SS_CENTERIMAGE | SS_NOPREFIX | SS_ENDELLIPSIS, idHint);

    canvas_ = std::make_unique<Canvas>(*this, doc_);
    canvas_->create(hwnd_);
    canvas_->setDpi(dpi_);

    make(WC_STATICW, L"", WS_VISIBLE | SS_CENTERIMAGE | SS_NOPREFIX | SS_ENDELLIPSIS, idMessage);
    make(WC_BUTTONW, L"Save a copy…", WS_VISIBLE | WS_TABSTOP | WS_GROUP | BS_PUSHBUTTON, idSave); tip(idSave, L"Save the edited image to a file (Ctrl+S)");
    make(WC_BUTTONW, L"Cancel", WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, idCancel); tip(idCancel, L"Discard this edit without uploading");
    make(WC_BUTTONW, L"Upload", WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, idUpload); tip(idUpload, L"Upload the edited image (Ctrl+Enter)");
    populateFonts();
    rebuildRecentColors();
}

void EditorWindow::applyFonts() {
    NONCLIENTMETRICSW metrics{};
    metrics.cbSize = sizeof metrics;
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof metrics, &metrics, 0, dpi_);
    if (font_) DeleteObject(font_);
    if (boldFont_) DeleteObject(boldFont_);
    font_ = CreateFontIndirectW(&metrics.lfMessageFont);
    auto bold = metrics.lfMessageFont;
    bold.lfWeight = FW_SEMIBOLD;
    boldFont_ = CreateFontIndirectW(&bold);
    for (HWND child = GetWindow(hwnd_, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) SendMessageW(child, WM_SETFONT, WPARAM(child == control(idUpload) ? boldFont_ : font_), TRUE);
    SendMessageW(tooltip_, WM_SETFONT, WPARAM(font_), TRUE);
    if (canvas_) canvas_->setDpi(dpi_);
}

void EditorWindow::layout() {
    if (!hwnd_ || !canvas_) return;
    RECT client;
    GetClientRect(hwnd_, &client);
    const int width = client.right, height = client.bottom;
    const int margin = scaled(12), gap = scaled(4), barHeight = scaled(28), toolbarHeight = scaled(44), optionsHeight = scaled(40), footerHeight = scaled(50);
    HDWP batch = BeginDeferWindowPos(48);
    auto place = [&](int id, int x, int y, int cx, int cy) { batch = DeferWindowPos(batch, control(id), nullptr, x, y, cx, cy, SWP_NOZORDER | SWP_NOACTIVATE); };
    int x = margin;
    const int top = (toolbarHeight - barHeight) / 2;
    for (int i = 0; i < 8; ++i) {
        place(idToolFirst + i, x, top, scaled(i == int(Tool::Rectangle) || i == int(Tool::Highlight) ? 78 : 64), barHeight);
        x += scaled(i == int(Tool::Rectangle) || i == int(Tool::Highlight) ? 78 : 64) + gap;
        if (i == int(Tool::Crop)) x += scaled(10);
    }
    x += scaled(10);
    for (int id : {idUndo, idRedo}) { place(id, x, top, scaled(58), barHeight); x += scaled(58) + gap; }
    x += scaled(10);
    for (int id : {idFit, idActual}) { place(id, x, top, scaled(52), barHeight); x += scaled(52) + gap; }
    place(idStatus, std::max(x, width - margin - scaled(200)), top, scaled(200), barHeight);
    EndDeferWindowPos(batch);
    layoutOptions(toolbarHeight, optionsHeight);
    batch = BeginDeferWindowPos(8);
    const int canvasTop = toolbarHeight + optionsHeight;
    place(idCanvas, 0, canvasTop, width, std::max(1, height - canvasTop - footerHeight));
    const int footerTop = height - footerHeight + (footerHeight - barHeight) / 2;
    int right = width - margin;
    place(idUpload, right - scaled(96), footerTop, scaled(96), barHeight); right -= scaled(96) + scaled(8);
    place(idCancel, right - scaled(84), footerTop, scaled(84), barHeight); right -= scaled(84) + scaled(8);
    place(idSave, right - scaled(118), footerTop, scaled(118), barHeight); right -= scaled(118) + scaled(8);
    place(idMessage, margin, footerTop, std::max(1, right - margin), barHeight);
    EndDeferWindowPos(batch);
    InvalidateRect(hwnd_, nullptr, TRUE);
}

int EditorWindow::layoutOptions(int top, int height) {
    RECT client;
    GetClientRect(hwnd_, &client);
    const int margin = scaled(12), rowHeight = scaled(26), swatch = scaled(22), y = top + (height - rowHeight) / 2;
    int x = margin;
    auto visible = [&](int id) { return (GetWindowLongW(control(id), GWL_STYLE) & WS_VISIBLE) != 0; };
    auto put = [&](int id, int cx, int cy = 0) {
        if (!visible(id)) return;
        const int h = cy ? cy : rowHeight;
        SetWindowPos(control(id), nullptr, x, y + (rowHeight - h) / 2, cx, h, SWP_NOZORDER | SWP_NOACTIVATE);
        x += cx + scaled(6);
    };
    // A drop-down list's height is its open list; the edit part follows the font.
    auto drop = [&](int id, int cx, int listHeight) {
        if (!visible(id)) return;
        SetWindowPos(control(id), nullptr, x, y, cx, listHeight, SWP_NOZORDER | SWP_NOACTIVATE);
        x += cx + scaled(6);
    };
    put(idColorLabel, scaled(38));
    for (int i = 0; i < paletteCount; ++i) put(idSwatchFirst + i, swatch, swatch);
    if (visible(idColorLabel)) x += scaled(6);
    for (int i = 0; i < recentCount; ++i) put(idRecentFirst + i, swatch, swatch);
    put(idCustomColor, swatch, swatch);
    if (visible(idColorLabel)) x += scaled(8);
    put(idWidthLabel, scaled(30));
    drop(idWidth, scaled(96), scaled(200));
    if (visible(idWidthLabel)) x += scaled(8);
    put(idStrengthLabel, scaled(56));
    put(idStrength, scaled(150));
    if (visible(idStrengthLabel)) x += scaled(8);
    put(idFontLabel, scaled(32));
    drop(idFont, scaled(220), scaled(320));
    put(idSizeLabel, scaled(30));
    if (visible(idSize)) {
        SetWindowPos(control(idSize), nullptr, x, y, scaled(60), rowHeight, SWP_NOZORDER | SWP_NOACTIVATE);
        SendMessageW(control(idSizeSpin), UDM_SETBUDDY, WPARAM(control(idSize)), 0);
        x += scaled(66);
    }
    if (visible(idSizeLabel)) x += scaled(8);
    put(idCropHint, scaled(230));
    put(idApplyCrop, scaled(96));
    put(idCancelCrop, scaled(80));
    if (visible(idHint)) SetWindowPos(control(idHint), nullptr, x, y, std::max(1, int(client.right) - x - margin), rowHeight, SWP_NOZORDER | SWP_NOACTIVATE);
    InvalidateRect(hwnd_, nullptr, TRUE);
    return x;
}

void EditorWindow::populateFonts() {
    HWND box = control(idFont);
    LOGFONTW query = {};
    query.lfCharSet = DEFAULT_CHARSET;
    std::vector<std::string> names;
    HDC dc = GetDC(hwnd_);
    EnumFontFamiliesExW(dc, &query, &collectFamily, LPARAM(&names), 0);
    ReleaseDC(hwnd_, dc);
    std::sort(names.begin(), names.end(), [](const std::string& a, const std::string& b) { return text::lower(a) < text::lower(b); });
    names.erase(std::unique(names.begin(), names.end()), names.end());
    fontFamilies_.clear();
    fontFamilies_.push_back(""); // index 0: the default family
    const auto label = L"Default (" + w(defaultFamily_) + L")";
    SendMessageW(box, CB_ADDSTRING, 0, LPARAM(label.c_str()));
    for (const auto& name : names) {
        if (name.size() > 300) continue;
        fontFamilies_.push_back(name);
        SendMessageW(box, CB_ADDSTRING, 0, LPARAM(w(name).c_str()));
    }
    SendMessageW(box, CB_SETCURSEL, 0, 0);
}
void EditorWindow::selectFontFamily(const std::string& family) {
    HWND box = control(idFont);
    auto found = std::find(fontFamilies_.begin(), fontFamilies_.end(), family);
    if (found == fontFamilies_.end()) {
        // A family named by an agent that is not installed still shows (rendered with the default).
        fontFamilies_.push_back(family);
        SendMessageW(box, CB_ADDSTRING, 0, LPARAM(w(family).c_str()));
        found = fontFamilies_.end() - 1;
    }
    SendMessageW(box, CB_SETCURSEL, WPARAM(found - fontFamilies_.begin()), 0);
}
std::string EditorWindow::currentFontFamily() const {
    const auto index = SendMessageW(control(idFont), CB_GETCURSEL, 0, 0);
    return index > 0 && size_t(index) < fontFamilies_.size() ? fontFamilies_[size_t(index)] : std::string();
}
int EditorWindow::currentWidth() const {
    const auto index = SendMessageW(control(idWidth), CB_GETCURSEL, 0, 0);
    return widthValues[index >= 0 && index < 3 ? index : 1];
}
int EditorWindow::currentFontPixels() const {
    BOOL ok = FALSE;
    const int value = int(GetDlgItemInt(hwnd_, idSize, &ok, FALSE));
    return std::clamp(ok ? value : 28, minimumFontPixels, maximumFontPixels);
}
bool EditorWindow::highlightContext() const {
    const auto* shape = doc_.find(canvas_->selected());
    return canvas_->tool() == Tool::Highlight || (shape && shape->kind == Shape::Kind::Highlight);
}

void EditorWindow::selectTool(Tool tool) {
    for (int i = 0; i < 8; ++i) SendMessageW(control(idToolFirst + i), BM_SETCHECK, i == int(tool) ? BST_CHECKED : BST_UNCHECKED, 0);
    canvas_->setTool(tool);
    syncStyle(false);
    refresh();
    SetFocus(canvas_->hwnd());
}

void EditorWindow::syncStyle(bool applyToSelection) {
    ToolStyle style;
    const auto* shape = doc_.find(canvas_->selected());
    style.color = highlightContext() ? highlightColor_ : annotationColor_;
    style.width = currentWidth();
    style.fontPixels = currentFontPixels();
    style.fontFamily = currentFontFamily();
    style.strength = int(SendMessageW(control(idStrength), TBM_GETPOS, 0, 0));
    if (shape && shape->kind == Shape::Kind::Redact) style.color = 0x000000;
    canvas_->setStyle(style);
    if (applyToSelection) canvas_->applyStyleToSelection();
    shownColor_ = style.color;
    for (int i = 0; i < paletteCount; ++i) InvalidateRect(control(idSwatchFirst + i), nullptr, FALSE);
    for (int i = 0; i < recentCount; ++i) InvalidateRect(control(idRecentFirst + i), nullptr, FALSE);
    refresh();
}

void EditorWindow::refresh() {
    if (!canvas_) return;
    const auto tool = canvas_->tool();
    const auto* shape = doc_.find(canvas_->selected());
    auto kind = [&](Shape::Kind k) { return shape && shape->kind == k; };
    using K = Shape::Kind;
    const bool annotation = tool == Tool::Arrow || tool == Tool::Rectangle || tool == Tool::Text || tool == Tool::Highlight || kind(K::Arrow) || kind(K::Rectangle) || kind(K::Text) || kind(K::Highlight);
    const bool colorRow = annotation && tool != Tool::Crop;
    const bool widthRow = tool != Tool::Crop && (tool == Tool::Arrow || tool == Tool::Rectangle || kind(K::Arrow) || kind(K::Rectangle));
    const bool blurRow = tool != Tool::Crop && (tool == Tool::Blur || kind(K::Blur));
    const bool textRow = tool != Tool::Crop && (tool == Tool::Text || kind(K::Text));
    const bool cropRow = tool == Tool::Crop;
    std::wstring hint;
    failure_ = failure_ && !message_.empty();
    bool warning = false;
    if (tool == Tool::Blur || kind(K::Blur)) { hint = L"Blur is a visual effect. Use Redact for passwords and private information."; warning = true; }
    else if (tool == Tool::Redact && !shape) hint = L"Redact removes the covered pixels from the uploaded image.";
    bool relayout = false;
    auto show = [&](int id, bool on) {
        HWND h = control(id);
        const bool now = (GetWindowLongW(h, GWL_STYLE) & WS_VISIBLE) != 0;
        if (now != on) { ShowWindow(h, on ? SW_SHOWNA : SW_HIDE); relayout = true; }
    };
    show(idColorLabel, colorRow);
    for (int i = 0; i < paletteCount; ++i) show(idSwatchFirst + i, colorRow);
    for (int i = 0; i < recentCount; ++i) show(idRecentFirst + i, colorRow && size_t(i) < recentColors_.size());
    show(idCustomColor, colorRow);
    show(idWidthLabel, widthRow); show(idWidth, widthRow);
    show(idStrengthLabel, blurRow); show(idStrength, blurRow);
    show(idFontLabel, textRow); show(idFont, textRow); show(idSizeLabel, textRow); show(idSize, textRow); show(idSizeSpin, textRow);
    show(idCropHint, cropRow); show(idApplyCrop, cropRow); show(idCancelCrop, cropRow);
    SetWindowLongPtrW(control(idHint), GWLP_USERDATA, warning ? 1 : 0);
    {
        wchar_t current[160] = {};
        GetWindowTextW(control(idHint), current, 160);
        if (hint != current) { SetWindowTextW(control(idHint), hint.c_str()); InvalidateRect(control(idHint), nullptr, TRUE); }
    }
    show(idHint, !hint.empty());
    EnableWindow(control(idUndo), !busy_ && doc_.canUndo());
    EnableWindow(control(idRedo), !busy_ && doc_.canRedo());
    const auto crop = doc_.crop();
    const auto status = std::to_wstring(crop.w) + L" × " + std::to_wstring(crop.h) + L" px · " + std::to_wstring(int(std::lround(canvas_->zoom() * 100))) + L"%";
    SetWindowTextW(control(idStatus), status.c_str());
    if (relayout) layoutOptions(scaled(44), scaled(40));
}

void EditorWindow::useColor(Rgb color) {
    if (highlightContext()) highlightColor_ = color; else annotationColor_ = color;
    syncStyle(true);
}

void EditorWindow::pickCustomColor() {
    if (busy_ || modal_) return;
    const bool highlight = highlightContext();
    // The dialog's custom-colour grid shows the same recent picks.
    static COLORREF custom[16];
    for (int i = 0; i < 16; ++i) {
        Rgb value = 0xffffff;
        if (size_t(i) < recentColors_.size()) parseColor(recentColors_[size_t(i)], value);
        custom[i] = colorRef(value);
    }
    CHOOSECOLORW chooser{};
    chooser.lStructSize = sizeof chooser;
    chooser.hwndOwner = hwnd_;
    chooser.rgbResult = colorRef(highlight ? highlightColor_ : annotationColor_);
    chooser.lpCustColors = custom;
    chooser.Flags = CC_FULLOPEN | CC_RGBINIT | CC_ANYCOLOR;
    ++modal_;
    const BOOL picked = ChooseColorW(&chooser);
    --modal_;
    if (!picked || busy_) return;
    const Rgb color = fromColorRef(chooser.rgbResult);
    // Palette colours are already one click away; only custom colours are remembered.
    if (std::find(std::begin(palette), std::end(palette), color) == std::end(palette)) {
        const auto name = colorName(color);
        recentColors_.erase(std::remove(recentColors_.begin(), recentColors_.end(), name), recentColors_.end());
        recentColors_.insert(recentColors_.begin(), name);
        if (recentColors_.size() > size_t(recentCount)) recentColors_.resize(size_t(recentCount));
        rebuildRecentColors();
    }
    useColor(color);
}

void EditorWindow::rebuildRecentColors() {
    for (int i = 0; i < recentCount; ++i) {
        HWND button = control(idRecentFirst + i);
        const bool used = size_t(i) < recentColors_.size();
        const auto name = used ? L"Recent color " + w(recentColors_[size_t(i)]) : std::wstring(L"Recent color");
        SetWindowTextW(button, name.c_str());
        TTTOOLINFOW tool{};
        tool.cbSize = sizeof tool;
        tool.hwnd = hwnd_;
        tool.uId = UINT_PTR(button);
        SendMessageW(tooltip_, TTM_DELTOOLW, 0, LPARAM(&tool));
        if (used) {
            const auto text = L"Recent: " + w(recentColors_[size_t(i)]);
            tool.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
            tool.lpszText = const_cast<LPWSTR>(text.c_str());
            SendMessageW(tooltip_, TTM_ADDTOOLW, 0, LPARAM(&tool));
        }
        InvalidateRect(button, nullptr, FALSE);
    }
    if (canvas_) refresh();
}

void EditorWindow::drawSwatch(const DRAWITEMSTRUCT& item) {
    const int id = int(item.CtlID);
    const RECT r = item.rcItem;
    Gdiplus::Graphics g(item.hDC);
    g.Clear(gdi(GetSysColor(COLOR_BTNFACE)));
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    const Gdiplus::REAL inset = Gdiplus::REAL(scaled(2));
    const Gdiplus::RectF circle(Gdiplus::REAL(r.left) + inset, Gdiplus::REAL(r.top) + inset, Gdiplus::REAL(r.right - r.left) - 2 * inset, Gdiplus::REAL(r.bottom - r.top) - 2 * inset);
    bool checked = false;
    if (id == idCustomColor) {
        // The rainbow shows colours to pick from, so it is the same in every scheme.
        const Rgb wheel[6] = {0xef4444, 0xf59e0b, 0xfacc15, 0x22c55e, 0x3b82f6, 0xa855f7};
        for (int i = 0; i < 6; ++i) {
            Gdiplus::SolidBrush brush(gdiRgb(wheel[i]));
            g.FillPie(&brush, circle, Gdiplus::REAL(-90 + 60 * i), 60);
        }
    } else {
        Rgb color = 0;
        if (id >= idSwatchFirst && id < idSwatchFirst + paletteCount) color = palette[id - idSwatchFirst];
        else if (id >= idRecentFirst && id < idRecentFirst + recentCount && size_t(id - idRecentFirst) < recentColors_.size()) parseColor(recentColors_[size_t(id - idRecentFirst)], color);
        Gdiplus::SolidBrush brush(gdiRgb(color));
        g.FillEllipse(&brush, circle);
        checked = color == shownColor_;
    }
    Gdiplus::Pen border(checked ? gdi(GetSysColor(COLOR_WINDOWTEXT)) : gdi(GetSysColor(COLOR_GRAYTEXT)), Gdiplus::REAL(checked ? scaled(2) : 1));
    g.DrawEllipse(&border, circle);
    if (item.itemState & ODS_FOCUS) {
        Gdiplus::Pen focus(gdi(GetSysColor(COLOR_HIGHLIGHT)), Gdiplus::REAL(scaled(2)));
        g.DrawEllipse(&focus, Gdiplus::RectF(circle.X - inset / 2, circle.Y - inset / 2, circle.Width + inset, circle.Height + inset));
    }
}

void EditorWindow::undo() {
    if (busy_ || canvas_->editingText()) return;
    if (doc_.undo()) { canvas_->documentChanged(); refresh(); }
}
void EditorWindow::redo() {
    if (busy_ || canvas_->editingText()) return;
    if (doc_.redo()) { canvas_->documentChanged(); refresh(); }
}

void EditorWindow::abortGesture() { if (canvas_) canvas_->abortGesture(); }
void EditorWindow::documentEdited() { if (canvas_) { canvas_->documentChanged(); refresh(); } }
void EditorWindow::commitPendingInput() {
    if (!canvas_) return;
    if (canvas_->editingText()) canvas_->finishText(true);
    if (canvas_->cropPending()) canvas_->applyCrop();
}
void EditorWindow::canvasChanged() { refresh(); }
void EditorWindow::canvasViewChanged() { refresh(); }
void EditorWindow::canvasToolRequested(Tool tool) {
    for (int i = 0; i < 8; ++i) SendMessageW(control(idToolFirst + i), BM_SETCHECK, i == int(tool) ? BST_CHECKED : BST_UNCHECKED, 0);
    refresh();
}
void EditorWindow::canvasSelectionChanged() {
    // A selected annotation shows, and is edited by, its own style controls.
    if (const auto* shape = doc_.find(canvas_->selected())) {
        if (shape->kind == Shape::Kind::Highlight) highlightColor_ = shape->color;
        else if (shape->kind != Shape::Kind::Redact && shape->kind != Shape::Kind::Blur) annotationColor_ = shape->color;
        syncing_ = true;
        int widthIndex = 0;
        for (int i = 0; i < 3; ++i) if (widthValues[i] == shape->width) widthIndex = i;
        SendMessageW(control(idWidth), CB_SETCURSEL, WPARAM(widthIndex), 0);
        SendMessageW(control(idSizeSpin), UDM_SETPOS32, 0, shape->fontPixels);
        SetDlgItemInt(hwnd_, idSize, UINT(shape->fontPixels), FALSE);
        SendMessageW(control(idStrength), TBM_SETPOS, TRUE, shape->strength);
        if (shape->kind == Shape::Kind::Text) selectFontFamily(shape->fontFamily);
        syncing_ = false;
        syncStyle(false);
        return;
    }
    refresh();
}

void EditorWindow::saveCopy() {
    if (busy_ || modal_) return;
    if (canvas_->editingText()) canvas_->finishText(true);
    IFileSaveDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(clsidFileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, iidFileSaveDialog, reinterpret_cast<void**>(&dialog))) || !dialog) {
        setFailure("The save dialog could not be opened.");
        return;
    }
    const COMDLG_FILTERSPEC types[] = {{L"PNG image", L"*.png"}};
    dialog->SetFileTypes(1, types);
    dialog->SetFileTypeIndex(1);
    dialog->SetDefaultExtension(L"png");
    dialog->SetTitle(L"Save a copy");
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_OVERWRITEPROMPT | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_NOREADONLYRETURN | FOS_STRICTFILETYPES);
    IShellItem* pictures = nullptr;
    if (SUCCEEDED(SHGetKnownFolderItem(folderPictures, KF_FLAG_DEFAULT, nullptr, IID_PPV_ARGS(&pictures))) && pictures) {
        dialog->SetDefaultFolder(pictures);
        pictures->Release();
    }
    const auto now = std::time(nullptr);
    std::tm local{};
    localtime_s(&local, &now);
    wchar_t name[64];
    wcsftime(name, 64, L"put.gd-edit-%Y-%m-%d-%H-%M-%S.png", &local);
    dialog->SetFileName(name);
    fileDialog_ = dialog;
    ++modal_;
    const HRESULT shown = dialog->Show(hwnd_);
    --modal_;
    fileDialog_ = nullptr;
    std::string path;
    if (SUCCEEDED(shown)) {
        IShellItem* result = nullptr;
        if (SUCCEEDED(dialog->GetResult(&result)) && result) {
            PWSTR chosen = nullptr;
            if (SUCCEEDED(result->GetDisplayName(SIGDN_FILESYSPATH, &chosen)) && chosen) { path = paths::narrow(chosen); CoTaskMemFree(chosen); }
            result->Release();
        }
    }
    dialog->Release();
    if (path.empty() || busy_) return;
    if (!text::endsWith(text::lower(path), ".png")) path += ".png";
    // The same sanitized export as Upload; written atomically beside the target.
    if (const auto error = callbacks_.saveCopy ? callbacks_.saveCopy(path) : Error{"save_failed", "Unavailable", {}}) setFailure("The copy could not be saved.");
    else setMessage("Saved a copy.");
}

void EditorWindow::requestCancel() {
    if (busy_ || modal_) return;
    if (doc_.modified()) {
        TASKDIALOG_BUTTON buttons[] = {{discardButton, L"Discard"}, {IDCANCEL, L"Keep editing"}};
        TASKDIALOGCONFIG config{};
        config.cbSize = sizeof config;
        config.hwndParent = hwnd_;
        config.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW;
        config.pszWindowTitle = L"put.gd";
        config.pszMainInstruction = L"Discard edits?";
        config.pszContent = L"Close the editor without uploading? Your edits will be discarded.";
        config.pszMainIcon = TD_WARNING_ICON;
        config.cButtons = 2;
        config.pButtons = buttons;
        config.nDefaultButton = IDCANCEL;
        config.lpCallbackData = LONG_PTR(this);
        config.pfCallback = &EditorWindow::taskDialogCallback;
        int pressed = IDCANCEL;
        // Resolved at run time: without a comctl32 v6 manifest there is no TaskDialog.
        using TaskDialogFunction = HRESULT(WINAPI*)(const TASKDIALOGCONFIG*, int*, int*, BOOL*);
        const auto taskDialog = reinterpret_cast<TaskDialogFunction>(reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"comctl32.dll"), "TaskDialogIndirect")));
        ++modal_;
        HRESULT result = E_NOTIMPL;
        if (taskDialog) result = taskDialog(&config, &pressed, nullptr, nullptr);
        else {
            result = S_OK;
            pressed = MessageBoxW(hwnd_, L"Close the editor without uploading? Your edits will be discarded.", L"Discard edits?", MB_OKCANCEL | MB_ICONWARNING | MB_DEFBUTTON2) == IDOK
                          ? discardButton : IDCANCEL;
        }
        --modal_;
        if (FAILED(result) || pressed != discardButton || busy_) return;
    }
    if (callbacks_.cancel) callbacks_.cancel();
}

HRESULT CALLBACK EditorWindow::taskDialogCallback(HWND dialog, UINT notification, WPARAM, LPARAM, LONG_PTR data) {
    auto* self = reinterpret_cast<EditorWindow*>(data);
    if (notification == TDN_CREATED) self->taskDialog_ = dialog;
    if (notification == TDN_DESTROYED) self->taskDialog_ = nullptr;
    return S_OK;
}

void EditorWindow::raise() {
    if (!hwnd_) return;
    if (IsIconic(hwnd_)) ShowWindow(hwnd_, SW_RESTORE);
    else ShowWindow(hwnd_, SW_SHOW);
    BringWindowToTop(hwnd_);
    if (!SetForegroundWindow(hwnd_)) {
        // Windows may refuse focus theft; flash the taskbar button instead.
        FLASHWINFO flash{};
        flash.cbSize = sizeof flash;
        flash.hwnd = hwnd_;
        flash.dwFlags = FLASHW_ALL | FLASHW_TIMERNOFG;
        flash.uCount = 3;
        FlashWindowEx(&flash);
    }
}

void EditorWindow::setBusy(const std::string& message) {
    closeDialogs();
    busy_ = true;
    if (canvas_) canvas_->setEnabled(false);
    for (HWND child = GetWindow(hwnd_, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT))
        if (child != control(idMessage) && child != control(idStatus)) EnableWindow(child, FALSE);
    failure_ = false;
    message_ = w(message);
    SetWindowTextW(control(idMessage), message_.c_str());
    SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    // Paint the busy state now: a posted export would otherwise run before WM_PAINT.
    RedrawWindow(hwnd_, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
}
void EditorWindow::setFailure(const std::string& message) {
    failure_ = true;
    message_ = w(message);
    SetWindowTextW(control(idMessage), message_.c_str());
    InvalidateRect(control(idMessage), nullptr, TRUE);
}
void EditorWindow::setMessage(const std::string& message) {
    failure_ = false;
    message_ = w(message);
    SetWindowTextW(control(idMessage), message_.c_str());
    InvalidateRect(control(idMessage), nullptr, TRUE);
}

void EditorWindow::closeDialogs() {
    if (fileDialog_) static_cast<IFileDialog*>(fileDialog_)->Close(HRESULT_FROM_WIN32(ERROR_CANCELLED));
    if (taskDialog_) PostMessageW(taskDialog_, TDM_CLICK_BUTTON, IDCANCEL, 0);
    // The colour picker (and any other common dialog) owned by this window.
    DialogSweep sweep{hwnd_, taskDialog_};
    EnumThreadWindows(GetCurrentThreadId(), &cancelOwnedDialog, LPARAM(&sweep));
}

bool EditorWindow::preTranslate(MSG& msg) {
    if (!hwnd_ || busy_ || modal_) return false;
    if (msg.hwnd != hwnd_ && !IsChild(hwnd_, msg.hwnd)) return false;
    if (msg.message != WM_KEYDOWN) return false;
    const bool ctrl = GetKeyState(VK_CONTROL) & 0x8000, shift = GetKeyState(VK_SHIFT) & 0x8000, alt = GetKeyState(VK_MENU) & 0x8000;
    if (alt) return false;
    const HWND focus = GetFocus();
    const bool inEdit = isClass(focus, WC_EDITW);
    const bool inCombo = isClass(focus, WC_COMBOBOXW) || isClass(GetParent(focus), WC_COMBOBOXW);
    const auto key = msg.wParam;
    if (ctrl) {
        if (key == VK_RETURN) {
            // Upload is never plain Enter, which completes text entry and crops.
            commitPendingInput();
            if (callbacks_.upload) callbacks_.upload();
            return true;
        }
        if (inEdit && (key == 'Z' || key == 'Y' || key == 'A' || key == 'C' || key == 'V' || key == 'X')) return false;
        if (key == 'Z') { shift ? redo() : undo(); return true; }
        if (key == 'Y') { redo(); return true; }
        if (key == '0' || key == VK_NUMPAD0) { canvas_->fit(); return true; }
        if (key == '1' || key == VK_NUMPAD1) { canvas_->actualSize(); return true; }
        if (key == VK_OEM_PLUS || key == VK_ADD) { canvas_->zoomBy(1.25); return true; }
        if (key == VK_OEM_MINUS || key == VK_SUBTRACT) { canvas_->zoomBy(0.8); return true; }
        if (key == 'S') { saveCopy(); return true; }
        return false;
    }
    if (focus == control(idSize) && key == VK_RETURN) {
        SetDlgItemInt(hwnd_, idSize, UINT(currentFontPixels()), FALSE);
        syncStyle(true);
        SetFocus(canvas_->hwnd());
        return true;
    }
    if (inEdit || inCombo || canvas_->editingText()) return false;
    static const std::pair<WPARAM, Tool> letters[] = {{'V', Tool::Select}, {'X', Tool::Redact}, {'B', Tool::Blur}, {'C', Tool::Crop},
                                                      {'A', Tool::Arrow}, {'R', Tool::Rectangle}, {'H', Tool::Highlight}, {'T', Tool::Text}};
    if (!shift) for (const auto& [letter, tool] : letters) if (key == letter) { selectTool(tool); return true; }
    if (key == VK_RETURN && canvas_->cropPending() && !isClass(focus, WC_BUTTONW)) { canvas_->applyCrop(); return true; }
    if (key == VK_ESCAPE && canvas_->cropPending()) { canvas_->cancelCrop(); return true; }
    return false;
}

LRESULT CALLBACK EditorWindow::windowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    EditorWindow* self = nullptr;
    if (message == WM_NCCREATE) {
        self = static_cast<EditorWindow*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, LONG_PTR(self));
        self->hwnd_ = hwnd;
    } else {
        self = reinterpret_cast<EditorWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (!self) return DefWindowProcW(hwnd, message, wParam, lParam);
    if (message == WM_NCDESTROY) { SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0); self->hwnd_ = nullptr; return DefWindowProcW(hwnd, message, wParam, lParam); }
    return self->handle(message, wParam, lParam);
}

LRESULT EditorWindow::handle(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_SIZE: layout(); return 0;
    case WM_ACTIVATE:
        // Keyboard focus returns to the control that had it (the canvas by default).
        if (LOWORD(wParam) == WA_INACTIVE) { const HWND focus = GetFocus(); if (focus && IsChild(hwnd_, focus)) lastFocus_ = focus; }
        else if (!busy_) { SetFocus(lastFocus_ && IsWindow(lastFocus_) && IsWindowEnabled(lastFocus_) ? lastFocus_ : canvas_ ? canvas_->hwnd() : hwnd_); }
        return 0;
    case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
        info->ptMinTrackSize = {scaled(640), scaled(420)};
        return 0;
    }
    case WM_DPICHANGED: {
        dpi_ = HIWORD(wParam);
        const auto* suggested = reinterpret_cast<const RECT*>(lParam);
        applyFonts();
        SetWindowPos(hwnd_, nullptr, suggested->left, suggested->top, suggested->right - suggested->left, suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
        layout();
        return 0;
    }
    case WM_SETCURSOR:
        if (busy_) { SetCursor(LoadCursorW(nullptr, IDC_WAIT)); return TRUE; }
        break;
    case WM_DRAWITEM: drawSwatch(*reinterpret_cast<const DRAWITEMSTRUCT*>(lParam)); return TRUE;
    case WM_CTLCOLORSTATIC: {
        HDC dc = reinterpret_cast<HDC>(wParam);
        const HWND from = reinterpret_cast<HWND>(lParam);
        SetBkMode(dc, TRANSPARENT);
        if (from == control(idMessage) && failure_) SetTextColor(dc, RGB(185, 28, 28));
        else if (from == control(idHint) && GetWindowLongPtrW(from, GWLP_USERDATA)) SetTextColor(dc, RGB(180, 83, 9));
        else if (from == control(idStatus) || from == control(idCropHint) || from == control(idHint)) SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
        else SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
        return LRESULT(GetSysColorBrush(COLOR_BTNFACE));
    }
    case WM_HSCROLL:
        if (reinterpret_cast<HWND>(lParam) == control(idStrength) && !syncing_ && LOWORD(wParam) != TB_THUMBTRACK) syncStyle(true);
        return 0;
    case WM_VSCROLL:
        if (reinterpret_cast<HWND>(lParam) == control(idSizeSpin) && !syncing_ && LOWORD(wParam) == SB_THUMBPOSITION) syncStyle(true);
        return 0;
    case WM_COMMAND: {
        const int id = LOWORD(wParam), code = HIWORD(wParam);
        if (busy_) return 0;
        if (id >= idToolFirst && id < idToolFirst + 8 && code == BN_CLICKED) { selectTool(Tool(id - idToolFirst)); return 0; }
        if (id >= idSwatchFirst && id < idSwatchFirst + paletteCount && code == BN_CLICKED) { useColor(palette[id - idSwatchFirst]); return 0; }
        if (id >= idRecentFirst && id < idRecentFirst + recentCount && code == BN_CLICKED) {
            Rgb color = 0;
            if (size_t(id - idRecentFirst) < recentColors_.size() && parseColor(recentColors_[size_t(id - idRecentFirst)], color)) useColor(color);
            return 0;
        }
        switch (id) {
        case idCustomColor: if (code == BN_CLICKED) pickCustomColor(); return 0;
        case idUndo: if (code == BN_CLICKED) undo(); return 0;
        case idRedo: if (code == BN_CLICKED) redo(); return 0;
        case idFit: if (code == BN_CLICKED) canvas_->fit(); return 0;
        case idActual: if (code == BN_CLICKED) canvas_->actualSize(); return 0;
        case idApplyCrop: if (code == BN_CLICKED) canvas_->applyCrop(); return 0;
        case idCancelCrop: if (code == BN_CLICKED) canvas_->cancelCrop(); return 0;
        case idWidth: case idFont: if (code == CBN_SELCHANGE && !syncing_) syncStyle(true); return 0;
        case idSize:
            if (code == EN_KILLFOCUS && !syncing_) { syncing_ = true; SetDlgItemInt(hwnd_, idSize, UINT(currentFontPixels()), FALSE); syncing_ = false; syncStyle(true); }
            return 0;
        case idSave: if (code == BN_CLICKED) saveCopy(); return 0;
        case idCancel: if (code == BN_CLICKED) requestCancel(); return 0;
        case idUpload:
            if (code == BN_CLICKED) { commitPendingInput(); if (callbacks_.upload) callbacks_.upload(); }
            return 0;
        case IDCANCEL: if (canvas_->cropPending()) canvas_->cancelCrop(); return 0;
        case IDOK: return 0;
        default: break;
        }
        break;
    }
    case WM_CLOSE:
        if (closing_) { DestroyWindow(hwnd_); return 0; }
        requestCancel();
        return 0;
    default: break;
    }
    return DefWindowProcW(hwnd_, message, wParam, lParam);
}
}
