#pragma once
// WIC image codec helpers shared by the capture helper and the image job. Only
// these helpers decode or encode pixels; the resident never links them into its
// own code paths. COM must be initialized (apartment threaded) on the calling
// thread before any function here is used; see Apartment.
#include "common/Error.h"
#include <cstdint>
#include <string>
#include <string_view>
#include <windows.h>
#include <wincodec.h>

namespace putgd::wic {
// Minimal owning COM pointer (no WRL/ATL dependency).
template <class T> class Com {
public:
    Com() = default;
    Com(const Com&) = delete;
    Com& operator=(const Com&) = delete;
    Com(Com&& o) noexcept : p_(o.p_) { o.p_ = nullptr; }
    Com& operator=(Com&& o) noexcept { if (this != &o) { reset(); p_ = o.p_; o.p_ = nullptr; } return *this; }
    ~Com() { reset(); }
    void reset() { if (p_) { p_->Release(); p_ = nullptr; } }
    T* get() const { return p_; }
    T* operator->() const { return p_; }
    T** put() { reset(); return &p_; }
    explicit operator bool() const { return p_ != nullptr; }
private:
    T* p_ = nullptr;
};

// CoInitializeEx(COINIT_APARTMENTTHREADED) for the helper's main thread.
class Apartment {
public:
    Apartment();
    ~Apartment();
    bool ok() const { return ok_; }
private:
    bool ok_ = false;
};

// Process-wide WIC factory created on first use; nullptr when WIC is unavailable.
IWICImagingFactory* factory();

// Encodes top-down 32-bpp rows (B, G, R, A/X in memory order) into a PNG with no
// metadata, interlacing off. alpha=false writes 24-bpp BGR and ignores the fourth
// byte; alpha=true writes 32-bpp BGRA (straight alpha). The result is passed
// through the core PNG sanitizer and must satisfy png::inspect.
// Errors: encoding_failed.
Error encodePixels(const uint8_t* bits, int width, int height, size_t stride, bool alpha, std::string& png);
// Locks a 32-bpp BGRA IWICBitmap and encodes it as above.
Error encodeBitmap(IWICBitmap* bitmap, bool alpha, std::string& png);

// Opens an existing, owner-created regular file (no reparse point, one link),
// truncates it, writes `bytes`, FlushFileBuffers, closes. Never creates the file
// and never changes its security descriptor. Errors: artifact_io.
Error writeExisting(const std::string& path, std::string_view bytes);
// Reads a whole regular file; input_excessive above maximumBytes, artifact_io otherwise.
Error readFile(const std::string& path, size_t maximumBytes, std::string& bytes);

enum class Container { Unknown, Png, Jpeg, Bmp, Gif, Tiff, Webp };
// Content sniffing by signature (never by file name).
Container sniff(std::string_view bytes);
const char* containerName(Container);
// True when a PNG stream has an acTL chunk before its first IDAT (animated PNG).
bool animatedPng(std::string_view bytes);

// Creates the decoder for `container` explicitly (built-in Microsoft codecs for
// PNG/JPEG/BMP/GIF/TIFF, any installed WebP codec) over an in-memory stream and
// returns frame 0. `bytes` must outlive the frame. Errors: source_unsupported.
Error openFrame(std::string_view bytes, Container container, Com<IWICBitmapFrameDecode>& frame);
// EXIF orientation (1-8) of a JPEG frame; 1 when absent or invalid.
int jpegOrientation(IWICBitmapFrameDecode* frame);
// True when the frame's pixel format can carry transparency.
bool formatHasAlpha(IWICBitmapSource* source);
// Converts any source to a cached 32-bpp BGRA bitmap. Errors: source_unsupported.
Error toBgraBitmap(IWICBitmapSource* source, Com<IWICBitmap>& bitmap);
// Applies an EXIF orientation to a BGRA bitmap (returns a new bitmap; unchanged for 1).
Error orient(IWICBitmap* source, int orientation, Com<IWICBitmap>& out);
// Fant-scales so the longer edge is at most `edge` (never upscales).
Error scaleToEdge(IWICBitmap* source, int edge, Com<IWICBitmap>& out);
}
