#include "editor/Export.h"
#include "core/SanitizedPng.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <new>

// ---------------------------------------------------------------------------
// Portable part (also compiled into the Linux unit test).
// ---------------------------------------------------------------------------
namespace putgd::editor::exporter {
namespace {
Error exportFailed(const char* message) { return {"edit_export_failed", message, {}}; }
unsigned long readBE(std::string_view b, size_t at) {
    return (unsigned long)(unsigned char)b[at] << 24 | (unsigned long)(unsigned char)b[at + 1] << 16 | (unsigned long)(unsigned char)b[at + 2] << 8 | (unsigned char)b[at + 3];
}
}

TextSize fitWithin(int width, int height, int maxEdge) {
    if (width <= maxEdge && height <= maxEdge) return {width, height};
    const double scale = double(maxEdge) / std::max(width, height);
    return {std::clamp(int(std::lround(width * scale)), 1, maxEdge), std::clamp(int(std::lround(height * scale)), 1, maxEdge)};
}

Error finishPng(std::string_view encoded, size_t limit, int width, int height, std::string& out) {
    out.clear();
    auto stripped = png::stripDisallowedChunks(encoded);
    if (stripped.empty()) return exportFailed("The encoder did not produce a PNG");
    int w = 0, h = 0;
    if (png::inspect(stripped, &w, &h)) return exportFailed("The encoded image is not a sanitized PNG");
    if (w != width || h != height) return exportFailed("The encoded image has an unexpected size");
    if (stripped.size() > limit) return exportFailed("The edited image is larger than the upload limit");
    out = std::move(stripped);
    return {};
}

bool animatedPng(std::string_view b) {
    if (!png::hasSignature(b)) return false;
    size_t at = 8;
    while (at + 8 <= b.size()) {
        const auto length = readBE(b, at);
        const auto type = b.substr(at + 4, 4);
        if (type == "acTL") return true;
        if (type == "IDAT" || type == "IEND") return false;
        if (length > b.size() - at - 8) return false;
        at += 12 + size_t(length);
    }
    return false;
}

Raster oriented(Raster source, int orientation) {
    if (orientation < 2 || orientation > 8 || source.isNull()) return source;
    const int w = source.width, h = source.height;
    if (orientation == 2) { for (int y = 0; y < h; ++y) std::reverse(source.row(y), source.row(y) + w); return source; }
    if (orientation == 3) { std::reverse(source.pixels.begin(), source.pixels.end()); return source; }
    if (orientation == 4) {
        for (int y = 0; y < h / 2; ++y) std::swap_ranges(source.row(y), source.row(y) + w, source.row(h - 1 - y));
        return source;
    }
    // 5 transpose, 6 rotate 90 clockwise, 7 transverse, 8 rotate 270 clockwise.
    Raster out;
    out.width = h; out.height = w; out.alpha = source.alpha;
    out.pixels.resize(source.pixels.size());
    for (int oy = 0; oy < out.height; ++oy) {
        uint32_t* dst = out.row(oy);
        for (int ox = 0; ox < out.width; ++ox) {
            int sx = 0, sy = 0;
            switch (orientation) {
            case 5: sx = oy; sy = ox; break;
            case 6: sx = oy; sy = h - 1 - ox; break;
            case 7: sx = w - 1 - oy; sy = h - 1 - ox; break;
            default: sx = w - 1 - oy; sy = ox; break;
            }
            dst[ox] = source.row(sy)[sx];
        }
    }
    return out;
}
}

// ---------------------------------------------------------------------------
// Windows part: GDI+ annotations and text, WIC codecs.
// ---------------------------------------------------------------------------
#ifdef _WIN32
#include "common/Paths.h"
#include <memory>
#include <windows.h>
#include <objidl.h>
#include <propidl.h>
#include <wincodec.h>
namespace Gdiplus { using std::max; using std::min; }
#include <gdiplus.h>

namespace putgd::editor::exporter {
namespace {
constexpr double pi = 3.14159265358979323846;

template <class T> class Com {
public:
    Com() = default;
    Com(const Com&) = delete;
    Com& operator=(const Com&) = delete;
    ~Com() { reset(); }
    T** put() { reset(); return &p_; }
    T* get() const { return p_; }
    T* operator->() const { return p_; }
    explicit operator bool() const { return p_ != nullptr; }
    void reset() { if (p_) { p_->Release(); p_ = nullptr; } }
private:
    T* p_ = nullptr;
};

IWICImagingFactory* factory() {
    static IWICImagingFactory* instance = [] {
        IWICImagingFactory* created = nullptr;
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&created)))) created = nullptr;
        return created;
    }();
    return instance;
}

Error unsupported(const char* message) { return {"source_unsupported", message, {}}; }

// The family for a stored name: the installed family, else the default.
std::unique_ptr<Gdiplus::FontFamily> familyFor(const std::string& name) {
    if (!name.empty()) {
        auto family = std::make_unique<Gdiplus::FontFamily>(paths::wide(name).c_str());
        if (family->GetLastStatus() == Gdiplus::Ok) return family;
    }
    auto fallback = std::make_unique<Gdiplus::FontFamily>(L"Segoe UI");
    if (fallback->GetLastStatus() == Gdiplus::Ok) return fallback;
    return std::unique_ptr<Gdiplus::FontFamily>(Gdiplus::FontFamily::GenericSansSerif()->Clone());
}
int boldStyle(const Gdiplus::FontFamily& family) {
    if (family.IsStyleAvailable(Gdiplus::FontStyleBold)) return Gdiplus::FontStyleBold;
    return family.IsStyleAvailable(Gdiplus::FontStyleRegular) ? Gdiplus::FontStyleRegular : Gdiplus::FontStyleBoldItalic;
}
Gdiplus::Color opaque(Rgb c, int alpha = 255) { return Gdiplus::Color(BYTE(alpha), BYTE(c >> 16 & 255), BYTE(c >> 8 & 255), BYTE(c & 255)); }
// The outline colour of text: dark on light colours, white otherwise (HSL lightness > 0.6).
Rgb contrast(Rgb c) {
    const int r = int(c >> 16 & 255), g = int(c >> 8 & 255), b = int(c & 255);
    const double lightness = (std::max({r, g, b}) + std::min({r, g, b})) / 510.0;
    return lightness > 0.6 ? 0x141414 : 0xffffff;
}
bool opaquePixels(const Raster& r) {
    if (!r.alpha) return true;
    for (const auto p : r.pixels) if (alphaOf(p) != 255) return false;
    return true;
}
}

std::string defaultFontFamily() {
    Gdiplus::FontFamily family(L"Segoe UI");
    if (family.GetLastStatus() == Gdiplus::Ok) return "Segoe UI";
    WCHAR name[LF_FACESIZE] = {};
    Gdiplus::FontFamily::GenericSansSerif()->GetFamilyName(name);
    return paths::narrow(name);
}

TextSize measureText(const std::string& text, const std::string& familyName, int pixels) {
    static Gdiplus::Bitmap* scratch = new Gdiplus::Bitmap(1, 1, PixelFormat32bppARGB);
    const auto family = familyFor(familyName);
    const int style = boldStyle(*family);
    const double em = std::max(4, pixels);
    const double unitsPerEm = std::max<UINT16>(1, family->GetEmHeight(style));
    TextSize size;
    size.height = int(std::lround((family->GetCellAscent(style) + family->GetCellDescent(style)) * em / unitsPerEm));
    if (text.empty()) return size;
    const auto wide = paths::wide(text);
    Gdiplus::Graphics graphics(scratch);
    graphics.SetPageUnit(Gdiplus::UnitPixel);
    graphics.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAlias);
    Gdiplus::Font font(family.get(), Gdiplus::REAL(em), style, Gdiplus::UnitPixel);
    Gdiplus::StringFormat format(Gdiplus::StringFormat::GenericTypographic());
    format.SetFormatFlags(format.GetFormatFlags() | Gdiplus::StringFormatFlagsMeasureTrailingSpaces | Gdiplus::StringFormatFlagsNoWrap);
    Gdiplus::RectF box;
    if (graphics.MeasureString(wide.c_str(), INT(wide.size()), &font, Gdiplus::PointF(0, 0), &format, &box) == Gdiplus::Ok)
        size.width = int(std::lround(box.Width));
    return size;
}

void paintAnnotation(Gdiplus::Graphics& g, const Shape& shape) {
    using K = Shape::Kind;
    if (shape.kind == K::Redact || shape.kind == K::Blur) return;
    const auto state = g.Save();
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    g.SetCompositingMode(Gdiplus::CompositingModeSourceOver);
    g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAlias);
    switch (shape.kind) {
    case K::Rectangle: {
        Gdiplus::Pen pen(opaque(shape.color), Gdiplus::REAL(shape.width));
        pen.SetLineJoin(Gdiplus::LineJoinMiter);
        const double half = shape.width / 2.0;
        g.DrawRectangle(&pen, Gdiplus::RectF(Gdiplus::REAL(shape.rect.x + half), Gdiplus::REAL(shape.rect.y + half),
                                            Gdiplus::REAL(std::max(0.0, shape.rect.w - 2 * half)), Gdiplus::REAL(std::max(0.0, shape.rect.h - 2 * half))));
        break;
    }
    case K::Highlight: {
        // A translucent marker stays visible on dark and light captures and never hides text.
        Gdiplus::SolidBrush brush(opaque(shape.color, 105));
        g.FillRectangle(&brush, shape.rect.x, shape.rect.y, shape.rect.w, shape.rect.h);
        break;
    }
    case K::Arrow: {
        const double dx = shape.to.x - shape.from.x, dy = shape.to.y - shape.from.y, length = std::hypot(dx, dy);
        if (length < 1) break;
        const double head = std::max(12.0, shape.width * 4.0), angle = std::atan2(-dy, dx);
        const double tipX = shape.to.x, tipY = shape.to.y;
        const double leftX = tipX - std::sin(angle + pi / 3) * head, leftY = tipY - std::cos(angle + pi / 3) * head;
        const double rightX = tipX - std::sin(angle + pi - pi / 3) * head, rightY = tipY - std::cos(angle + pi - pi / 3) * head;
        const double baseX = (leftX + rightX) / 2, baseY = (leftY + rightY) / 2;
        if (length > head) {
            Gdiplus::Pen pen(opaque(shape.color), Gdiplus::REAL(shape.width));
            pen.SetStartCap(Gdiplus::LineCapRound); pen.SetEndCap(Gdiplus::LineCapRound); pen.SetLineJoin(Gdiplus::LineJoinRound);
            g.DrawLine(&pen, Gdiplus::REAL(shape.from.x), Gdiplus::REAL(shape.from.y), Gdiplus::REAL(baseX), Gdiplus::REAL(baseY));
        }
        const Gdiplus::PointF points[3] = {{Gdiplus::REAL(tipX), Gdiplus::REAL(tipY)}, {Gdiplus::REAL(leftX), Gdiplus::REAL(leftY)}, {Gdiplus::REAL(rightX), Gdiplus::REAL(rightY)}};
        Gdiplus::SolidBrush brush(opaque(shape.color));
        g.FillPolygon(&brush, points, 3);
        break;
    }
    case K::Text: {
        if (shape.text.empty()) break;
        const auto family = familyFor(shape.fontFamily);
        const auto wide = paths::wide(shape.text);
        Gdiplus::GraphicsPath path;
        Gdiplus::StringFormat format(Gdiplus::StringFormat::GenericTypographic());
        format.SetFormatFlags(format.GetFormatFlags() | Gdiplus::StringFormatFlagsMeasureTrailingSpaces | Gdiplus::StringFormatFlagsNoWrap);
        // The layout origin is the top of the line; the baseline sits one cell ascent below it.
        path.AddString(wide.c_str(), INT(wide.size()), family.get(), boldStyle(*family), Gdiplus::REAL(std::max(4, shape.fontPixels)),
                       Gdiplus::PointF(Gdiplus::REAL(shape.rect.x), Gdiplus::REAL(shape.rect.y)), &format);
        Gdiplus::Pen outline(opaque(contrast(shape.color)), Gdiplus::REAL(std::max(2.0, shape.fontPixels / 7.0)));
        outline.SetLineJoin(Gdiplus::LineJoinRound); outline.SetStartCap(Gdiplus::LineCapRound); outline.SetEndCap(Gdiplus::LineCapRound);
        g.DrawPath(&outline, &path);
        Gdiplus::SolidBrush fill(opaque(shape.color));
        g.FillPath(&fill, &path);
        break;
    }
    default: break;
    }
    g.Restore(state);
}

Raster render(const EditDocument& doc) {
    Raster out;
    try { out = doc.renderBase(); } catch (const std::bad_alloc&) { return {}; }
    if (out.isNull()) return {};
    bool annotations = false;
    for (const auto& shape : doc.shapes()) annotations = annotations || (shape.kind != Shape::Kind::Redact && shape.kind != Shape::Kind::Blur);
    if (annotations) {
        // 4. GDI+ draws straight into the fresh raster's pixels.
        Gdiplus::Bitmap bitmap(out.width, out.height, out.width * 4, PixelFormat32bppARGB, reinterpret_cast<BYTE*>(out.pixels.data()));
        if (bitmap.GetLastStatus() != Gdiplus::Ok) return {};
        {
            Gdiplus::Graphics graphics(&bitmap);
            if (graphics.GetLastStatus() != Gdiplus::Ok) return {};
            graphics.TranslateTransform(Gdiplus::REAL(-doc.crop().x), Gdiplus::REAL(-doc.crop().y));
            for (const auto& shape : doc.shapes()) paintAnnotation(graphics, shape);
            graphics.Flush(Gdiplus::FlushIntentionSync);
        }
    }
    // 5. Fully transparent pixels keep no hidden colour.
    EditDocument::clearHiddenColor(out);
    if (!out.alpha) for (auto& p : out.pixels) p |= 0xFF000000u;
    return out;
}

bool encodePng(const Raster& raster, std::string& out) {
    out.clear();
    auto* wic = factory();
    if (!wic || raster.isNull()) return false;
    Com<IStream> stream;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, stream.put()))) return false;
    Com<IWICBitmapEncoder> encoder;
    if (FAILED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.put())) || FAILED(encoder->Initialize(stream.get(), WICBitmapEncoderNoCache))) return false;
    Com<IWICBitmapFrameEncode> frame;
    Com<IPropertyBag2> options;
    if (FAILED(encoder->CreateNewFrame(frame.put(), options.put())) || FAILED(frame->Initialize(options.get()))) return false;
    if (FAILED(frame->SetSize(UINT(raster.width), UINT(raster.height)))) return false;
    const bool rgb = opaquePixels(raster);
    const WICPixelFormatGUID wanted = rgb ? GUID_WICPixelFormat24bppBGR : GUID_WICPixelFormat32bppBGRA;
    WICPixelFormatGUID format = wanted;
    if (FAILED(frame->SetPixelFormat(&format)) || !IsEqualGUID(format, wanted)) return false;
    constexpr int strip = 128;
    const UINT stride = UINT(raster.width) * (rgb ? 3u : 4u);
    std::vector<BYTE> buffer;
    for (int y = 0; y < raster.height; y += strip) {
        const int rows = std::min(strip, raster.height - y);
        if (rgb) {
            buffer.resize(size_t(stride) * size_t(rows));
            for (int r = 0; r < rows; ++r) {
                const uint32_t* src = raster.row(y + r);
                BYTE* dst = buffer.data() + size_t(r) * stride;
                for (int x = 0; x < raster.width; ++x) { dst[3 * x] = BYTE(blueOf(src[x])); dst[3 * x + 1] = BYTE(greenOf(src[x])); dst[3 * x + 2] = BYTE(redOf(src[x])); }
            }
            if (FAILED(frame->WritePixels(UINT(rows), stride, UINT(buffer.size()), buffer.data()))) return false;
        } else {
            auto* first = reinterpret_cast<BYTE*>(const_cast<uint32_t*>(raster.row(y)));
            if (FAILED(frame->WritePixels(UINT(rows), stride, stride * UINT(rows), first))) return false;
        }
    }
    if (FAILED(frame->Commit()) || FAILED(encoder->Commit())) return false;
    STATSTG stat = {};
    if (FAILED(stream->Stat(&stat, STATFLAG_NONAME)) || stat.cbSize.QuadPart == 0 || stat.cbSize.QuadPart > 0x7FFFFFFFULL) return false;
    HGLOBAL memory = nullptr;
    if (FAILED(GetHGlobalFromStream(stream.get(), &memory))) return false;
    const void* data = GlobalLock(memory);
    if (!data) return false;
    try { out.assign(static_cast<const char*>(data), size_t(stat.cbSize.QuadPart)); } catch (const std::bad_alloc&) { GlobalUnlock(memory); return false; }
    GlobalUnlock(memory);
    return true;
}

Error exportPng(const EditDocument& doc, size_t limit, std::string& png, int& width, int& height) {
    png.clear();
    std::string encoded;
    {
        const auto raster = render(doc);
        if (raster.isNull()) return exportFailed("The edited image could not be rendered");
        width = raster.width; height = raster.height;
        if (!encodePng(raster, encoded)) return exportFailed("The edited image could not be encoded");
    }
    return finishPng(encoded, limit, width, height, png);
}

Error exportPreview(const EditDocument& doc, int maxEdge, std::string& png, int& width, int& height) {
    png.clear();
    std::string encoded;
    {
        auto raster = render(doc);
        if (raster.isNull()) return exportFailed("The preview could not be rendered");
        const auto size = fitWithin(raster.width, raster.height, maxEdge);
        if (size.width != raster.width || size.height != raster.height) {
            try { raster = EditDocument::scaled(raster, raster.rect(), size.width, size.height); } catch (const std::bad_alloc&) { return exportFailed("The preview could not be rendered"); }
        }
        width = raster.width; height = raster.height;
        if (!encodePng(raster, encoded)) return exportFailed("The preview could not be encoded");
    }
    // The caller compares the size with maximumPreviewBytes (preview_too_large).
    return finishPng(encoded, SIZE_MAX, width, height, png);
}

Error decodeImage(std::string_view bytes, long long maxPixels, Raster& out) {
    out = {};
    const bool isPng = png::hasSignature(bytes);
    const bool isJpeg = bytes.size() >= 3 && (unsigned char)bytes[0] == 0xFF && (unsigned char)bytes[1] == 0xD8 && (unsigned char)bytes[2] == 0xFF;
    if (!isPng && !isJpeg) return unsupported("Only PNG and JPEG images can be edited");
    if (isPng && animatedPng(bytes)) return unsupported("Animated PNG images cannot be edited");
    if (bytes.size() > 0xFFFFFFFFULL) return {"input_excessive", "The image file is too large to edit", {}};
    auto* wic = factory();
    if (!wic) return unsupported("Windows Imaging Component is unavailable");
    Com<IWICStream> stream;
    if (FAILED(wic->CreateStream(stream.put())) ||
        FAILED(stream->InitializeFromMemory(reinterpret_cast<BYTE*>(const_cast<char*>(bytes.data())), DWORD(bytes.size()))))
        return unsupported("The image could not be read");
    // The decoder follows the signature; nothing else is probed.
    Com<IWICBitmapDecoder> decoder;
    if (FAILED(wic->CreateDecoder(isPng ? GUID_ContainerFormatPng : GUID_ContainerFormatJpeg, nullptr, decoder.put())) ||
        FAILED(decoder->Initialize(stream.get(), WICDecodeMetadataCacheOnDemand)))
        return unsupported("The image could not be decoded");
    UINT frames = 0;
    if (FAILED(decoder->GetFrameCount(&frames)) || frames != 1) return unsupported("Only single-frame images can be edited");
    Com<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, frame.put()))) return unsupported("The image could not be decoded");
    UINT width = 0, height = 0;
    if (FAILED(frame->GetSize(&width, &height)) || width == 0 || height == 0 || width > 0x7FFFFFFF || height > 0x7FFFFFFF) return unsupported("The image has no valid size");
    // Pixel admission before any pixel allocation.
    if ((long long)width * (long long)height > maxPixels) return {"input_excessive", "The image has too many pixels to edit", {}};
    int orientation = 1;
    if (isJpeg) {
        Com<IWICMetadataQueryReader> reader;
        if (SUCCEEDED(frame->GetMetadataQueryReader(reader.put()))) {
            PROPVARIANT value;
            PropVariantInit(&value);
            if (SUCCEEDED(reader->GetMetadataByName(L"/app1/ifd/{ushort=274}", &value)) && value.vt == VT_UI2) orientation = value.uiVal;
            PropVariantClear(&value);
        }
    }
    // Convert an embedded colour profile to sRGB when one is present.
    IWICBitmapSource* source = frame.get();
    Com<IWICColorTransform> transform;
    {
        UINT count = 0;
        if (SUCCEEDED(frame->GetColorContexts(0, nullptr, &count)) && count > 0 && count < 16) {
            std::vector<IWICColorContext*> contexts(count, nullptr);
            bool created = true;
            for (auto& context : contexts) created = created && SUCCEEDED(wic->CreateColorContext(&context));
            UINT actual = 0;
            if (created && SUCCEEDED(frame->GetColorContexts(count, contexts.data(), &actual))) {
                for (UINT i = 0; i < actual && i < count; ++i) {
                    WICColorContextType type = WICColorContextUninitialized;
                    if (FAILED(contexts[i]->GetType(&type)) || type != WICColorContextProfile) continue;
                    Com<IWICColorContext> srgb;
                    if (SUCCEEDED(wic->CreateColorContext(srgb.put())) && SUCCEEDED(srgb->InitializeFromExifColorSpace(1)) &&
                        SUCCEEDED(wic->CreateColorTransformer(transform.put())) &&
                        SUCCEEDED(transform->Initialize(frame.get(), contexts[i], srgb.get(), GUID_WICPixelFormat32bppBGRA)))
                        source = transform.get();
                    else transform.reset();
                    break;
                }
            }
            for (auto* context : contexts) if (context) context->Release();
        }
    }
    Com<IWICFormatConverter> converter;
    if (FAILED(wic->CreateFormatConverter(converter.put())) ||
        FAILED(converter->Initialize(source, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom)))
        return unsupported("The image pixel format is not supported");
    Raster raster;
    try {
        raster.width = int(width); raster.height = int(height); raster.alpha = true;
        raster.pixels.resize(size_t(width) * size_t(height));
    } catch (const std::bad_alloc&) {
        return {"input_excessive", "The image is too large to edit", {}};
    }
    constexpr int strip = 256;
    for (int y = 0; y < raster.height; y += strip) {
        const int rows = std::min(strip, raster.height - y);
        const WICRect rect{0, y, raster.width, rows};
        if (FAILED(converter->CopyPixels(&rect, UINT(raster.width) * 4u, UINT(raster.width) * 4u * UINT(rows), reinterpret_cast<BYTE*>(raster.row(y)))))
            return unsupported("The image could not be decoded");
    }
    raster.alpha = !opaquePixels(raster);
    try { out = oriented(std::move(raster), orientation); } catch (const std::bad_alloc&) { return {"input_excessive", "The image is too large to edit", {}}; }
    return {};
}
}
#endif
