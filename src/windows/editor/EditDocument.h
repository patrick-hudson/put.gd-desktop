#pragma once
// The editable model of the Windows image editor (docs/image-editor.md): one
// immutable base raster plus geometry. Portable C++20, no Windows headers, so
// the model, the command contract and the sanitized raster pipeline are tested
// on Linux (tests/test_editor.cpp). GDI+ drawing lives in Export.cpp and
// EditorWindow.cpp.
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace putgd::editor {
struct Point {
    int x = 0, y = 0;
    bool operator==(const Point&) const = default;
};

// Integer rectangle in source-image pixels: [x, x+w) x [y, y+h).
struct Rect {
    int x = 0, y = 0, w = 0, h = 0;
    bool empty() const { return w <= 0 || h <= 0; }
    int right() const { return x + w; }   // exclusive
    int bottom() const { return y + h; }  // exclusive
    Rect intersected(const Rect& o) const;
    bool intersects(const Rect& o) const { return !intersected(o).empty(); }
    Rect translated(int dx, int dy) const { return {x + dx, y + dy, w, h}; }
    // Moves each edge outward by the given amount (negative moves inward).
    Rect grown(int left, int top, int rightGrow, int bottomGrow) const { return {x - left, y - top, w + left + rightGrow, h + top + bottomGrow}; }
    bool contains(double px, double py) const { return px >= x && py >= y && px < x + w && py < y + h; }
    bool contains(const Rect& o) const { return o.x >= x && o.y >= y && o.right() <= right() && o.bottom() <= bottom(); }
    bool operator==(const Rect&) const = default;
    // Normalized box covering both corners (inclusive integer points).
    static Rect spanning(Point a, Point b);
};

// 32-bit pixels as 0xAARRGGBB words, which is BGRA in memory on little-endian
// machines (GDI+ PixelFormat32bppARGB, WIC 32bppBGRA). Not premultiplied.
struct Raster {
    int width = 0, height = 0;
    bool alpha = false;  // false: every pixel is opaque (alpha 0xFF)
    std::vector<uint32_t> pixels;
    Raster() = default;
    Raster(int w, int h, bool hasAlpha);
    bool isNull() const { return width <= 0 || height <= 0 || pixels.size() != size_t(width) * size_t(height); }
    uint32_t* row(int y) { return pixels.data() + size_t(y) * size_t(width); }
    const uint32_t* row(int y) const { return pixels.data() + size_t(y) * size_t(width); }
    Rect rect() const { return {0, 0, width, height}; }
    size_t bytes() const { return pixels.size() * sizeof(uint32_t); }
};
inline constexpr uint32_t argb(int a, int r, int g, int b) { return uint32_t(a & 255) << 24 | uint32_t(r & 255) << 16 | uint32_t(g & 255) << 8 | uint32_t(b & 255); }
inline constexpr int alphaOf(uint32_t p) { return int(p >> 24); }
inline constexpr int redOf(uint32_t p) { return int(p >> 16 & 255); }
inline constexpr int greenOf(uint32_t p) { return int(p >> 8 & 255); }
inline constexpr int blueOf(uint32_t p) { return int(p & 255); }

// Colours are 0xRRGGBB.
using Rgb = uint32_t;
std::string colorName(Rgb);                       // "#rrggbb"
bool parseColor(const std::string& text, Rgb& out); // "#RRGGBB" (either case)

// Text box measurement in image pixels; the Windows build measures with GDI+
// (Export.cpp), tests inject a deterministic stand-in.
struct TextSize { int width = 0, height = 0; };
using TextMeasurer = std::function<TextSize(const std::string& text, const std::string& family, int pixels)>;

// One editable mark. All geometry is in source-image pixels, independent of
// zoom, so redaction maps exactly through crop and display scaling.
struct Shape {
    enum class Kind { Redact, Blur, Arrow, Rectangle, Text, Highlight };
    Kind kind = Kind::Rectangle;
    uint64_t id = 0;
    Rect rect;           // Redact, Blur, Rectangle, Highlight, Text (box follows the text)
    Point from, to;      // Arrow tail and head
    Rgb color = 0xdc2626;
    int width = 4;       // outline/arrow pen width in image pixels
    int strength = 12;   // Blur radius in image pixels
    int fontPixels = 28; // Text height in image pixels
    std::string fontFamily; // empty: the default sans-serif family
    std::string text;       // UTF-8, one line
    Rect bounds() const;    // area the mark may paint, for invalidation
    bool operator==(const Shape&) const = default;
};
inline constexpr int minimumFontPixels = 8, maximumFontPixels = 200, maximumFontFamilyCharacters = 100;
inline constexpr Rgb defaultAnnotationColor = 0xdc2626, defaultHighlightColor = 0xfacc15;

// UTF-16 code units, the unit the Qt reference and Win32 edit controls count.
size_t utf16Length(const std::string& utf8);
std::string truncateUtf16(const std::string& utf8, size_t units);

// Undo history stores geometry snapshots, never pixel copies. The base raster
// is immutable and shared between copies of the document (atomic drafts).
class EditDocument {
public:
    static constexpr int maximumHistory = 200;
    static constexpr int64_t maximumHistoryBytes = 4 * 1024 * 1024;
    static constexpr int maximumTextCharacters = 500;
    static constexpr int minimumBlur = 2, maximumBlur = 64;
    static constexpr double blurPreviewPixels = 1.5e6;

    // Takes ownership of an already admitted, normalized raster.
    explicit EditDocument(Raster base);
    const Raster& base() const { return *base_; }
    int width() const { return base_->width; }
    int height() const { return base_->height; }
    Rect imageRect() const { return base_->rect(); }
    Rect crop() const { return state_.crop; }
    const std::vector<Shape>& shapes() const { return state_.shapes; }
    const Shape* find(uint64_t id) const;
    bool modified() const { return !state_.shapes.empty() || state_.crop != imageRect(); }

    uint64_t add(Shape);
    bool replace(const Shape&); // same id; false when unknown or unchanged
    bool remove(uint64_t id);
    bool setCrop(const Rect&);
    // Live drags update without a history entry; commit once on release.
    void preview(const Shape&);
    void beginGesture();
    void commitGesture();
    void cancelGesture();
    bool inGesture() const { return gesture_; }

    bool canUndo() const { return !undo_.empty(); }
    bool canRedo() const { return !redo_.empty(); }
    bool undo();
    bool redo();
    int64_t historyBytes() const { return historyBytes_; }
    int historyEntries() const { return int(undo_.size() + redo_.size()); }
    bool historyTruncated() const { return truncated_; }

    // Sanitized export steps 1-3 on a fresh crop-sized raster: copy the crop of
    // the base, overwrite every redaction with opaque black, then blur reading
    // only already-redacted pixels inside each blur rectangle. The caller draws
    // annotations (step 4, GDI+) and then calls clearHiddenColor (step 5).
    Raster renderBase() const;
    // Display-resolution blur for one shape, computed from the redacted base;
    // at most blurPreviewPixels pixels. Its extent is the shape clamped to the crop.
    Raster blurPreview(const Shape&, double scale) const;

    static Rect clampRect(const Rect&, const Rect& bounds);
    static void blurInPlace(Raster&, const Rect&, int radius);
    static void clearHiddenColor(Raster&);
    // Area-averaging downscale of `area` of `source` to target size (each no
    // larger than the area), premultiplied so transparent pixels do not bleed.
    static Raster scaled(const Raster& source, const Rect& area, int targetWidth, int targetHeight);

private:
    struct State {
        std::vector<Shape> shapes;
        Rect crop;
        bool operator==(const State&) const = default;
    };
    static int64_t bytesOf(const State&);
    void push(State before);
    std::shared_ptr<const Raster> base_;
    State state_, gestureStart_;
    bool gesture_ = false, truncated_ = false;
    std::vector<State> undo_, redo_;
    int64_t historyBytes_ = 0;
    uint64_t nextId_ = 1;
};
}
