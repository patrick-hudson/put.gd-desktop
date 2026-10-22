#include "capture/Png.h"
#include "common/Paths.h"
#include "core/SanitizedPng.h"
#include <algorithm>
#include <cstring>
#include <objbase.h>
#include <propidl.h>
#include <vector>

namespace putgd::wic {
namespace {
// {E094B0E2-67F2-45B3-B0EA-115337CA7CBF}: not declared by every SDK's wincodec.h.
constexpr GUID containerWebp{0xe094b0e2, 0x67f2, 0x45b3, {0xb0, 0xea, 0x11, 0x53, 0x37, 0xca, 0x7c, 0xbf}};
// {DC2BB46D-3F07-481E-8625-220C4AEDBB33}: GUID_VendorMicrosoftBuiltIn.
constexpr GUID vendorBuiltIn{0xdc2bb46d, 0x3f07, 0x481e, {0x86, 0x25, 0x22, 0x0c, 0x4a, 0xed, 0xbb, 0x33}};
constexpr int rowsPerWrite = 256;

Error encodingFailed(const char* message) { return {"encoding_failed", message}; }
Error unsupported(const char* message) { return {"source_unsupported", message}; }
Error io(const char* message) { return {"artifact_io", message}; }

unsigned long readBE(std::string_view b, size_t at) {
    return (unsigned long)(unsigned char)b[at] << 24 | (unsigned long)(unsigned char)b[at + 1] << 16 |
           (unsigned long)(unsigned char)b[at + 2] << 8 | (unsigned char)b[at + 3];
}

// A regular file on disk: no directory, no reparse point, exactly one link.
bool plainFile(HANDLE h) {
    if (GetFileType(h) != FILE_TYPE_DISK) return false;
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(h, &info)) return false;
    if (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DEVICE)) return false;
    return info.nNumberOfLinks == 1;
}

void adopt(IWICBitmap* source, Com<IWICBitmap>& out) {
    source->AddRef();
    *out.put() = source;
}
}

Apartment::Apartment() { ok_ = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE)); }
Apartment::~Apartment() { if (ok_) CoUninitialize(); }

IWICImagingFactory* factory() {
    static IWICImagingFactory* instance = [] {
        IWICImagingFactory* f = nullptr;
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)))) return (IWICImagingFactory*)nullptr;
        return f;
    }();
    return instance;
}

Error encodePixels(const uint8_t* bits, int width, int height, size_t stride, bool alpha, std::string& png) {
    png.clear();
    if (!bits || width <= 0 || height <= 0 || stride < size_t(width) * 4 || stride > 0xFFFFFFFFu) return encodingFailed("Invalid pixel buffer");
    auto* f = factory();
    if (!f) return encodingFailed("Windows Imaging Component is unavailable");
    Com<IStream> stream;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, stream.put()))) return encodingFailed("Could not allocate the PNG stream");
    Com<IWICBitmapEncoder> encoder;
    if (FAILED(f->CreateEncoder(GUID_ContainerFormatPng, &vendorBuiltIn, encoder.put())) &&
        FAILED(f->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.put())))
        return encodingFailed("PNG encoder unavailable");
    if (FAILED(encoder->Initialize(stream.get(), WICBitmapEncoderNoCache))) return encodingFailed("PNG encoder initialization failed");
    Com<IWICBitmapFrameEncode> frame;
    Com<IPropertyBag2> options;
    if (FAILED(encoder->CreateNewFrame(frame.put(), options.put()))) return encodingFailed("PNG frame creation failed");
    if (options) {
        PROPBAG2 name{};
        VARIANT value{};
        wchar_t interlace[] = L"InterlaceOption";
        name.pstrName = interlace;
        value.vt = VT_BOOL;
        value.boolVal = VARIANT_FALSE;
        options->Write(1, &name, &value);
        wchar_t filter[] = L"FilterOption";
        name.pstrName = filter;
        value.vt = VT_UI1;
        value.bVal = WICPngFilterAdaptive;
        options->Write(1, &name, &value);
    }
    if (FAILED(frame->Initialize(options.get()))) return encodingFailed("PNG frame initialization failed");
    if (FAILED(frame->SetSize(UINT(width), UINT(height)))) return encodingFailed("PNG size rejected");
    const WICPixelFormatGUID wanted = alpha ? GUID_WICPixelFormat32bppBGRA : GUID_WICPixelFormat24bppBGR;
    WICPixelFormatGUID format = wanted;
    if (FAILED(frame->SetPixelFormat(&format)) || !IsEqualGUID(format, wanted)) return encodingFailed("PNG pixel format rejected");
    if (alpha) {
        for (int row = 0; row < height; row += rowsPerWrite) {
            const int lines = std::min(rowsPerWrite, height - row);
            const size_t size = stride * size_t(lines);
            if (size > 0xFFFFFFFFu) return encodingFailed("Image rows too large");
            if (FAILED(frame->WritePixels(UINT(lines), UINT(stride), UINT(size), const_cast<BYTE*>(bits + stride * size_t(row)))))
                return encodingFailed("PNG pixel write failed");
        }
    } else {
        const size_t outStride = size_t(width) * 3;
        const int chunk = int(std::max<size_t>(1, std::min<size_t>(rowsPerWrite, (64u << 20) / outStride)));
        std::vector<uint8_t> rows;
        try { rows.resize(outStride * size_t(std::min(chunk, height))); } catch (...) { return encodingFailed("Out of memory"); }
        for (int row = 0; row < height; row += chunk) {
            const int lines = std::min(chunk, height - row);
            for (int line = 0; line < lines; ++line) {
                const uint8_t* in = bits + stride * size_t(row + line);
                uint8_t* out = rows.data() + outStride * size_t(line);
                for (int x = 0; x < width; ++x, in += 4, out += 3) { out[0] = in[0]; out[1] = in[1]; out[2] = in[2]; }
            }
            const size_t size = outStride * size_t(lines);
            if (size > 0xFFFFFFFFu) return encodingFailed("Image rows too large");
            if (FAILED(frame->WritePixels(UINT(lines), UINT(outStride), UINT(size), rows.data()))) return encodingFailed("PNG pixel write failed");
        }
    }
    if (FAILED(frame->Commit()) || FAILED(encoder->Commit())) return encodingFailed("PNG commit failed");
    STATSTG stat{};
    HGLOBAL global = nullptr;
    if (FAILED(stream->Stat(&stat, STATFLAG_NONAME)) || FAILED(GetHGlobalFromStream(stream.get(), &global))) return encodingFailed("PNG stream unreadable");
    const auto size = size_t(stat.cbSize.QuadPart);
    const void* data = GlobalLock(global);
    if (!data && size) return encodingFailed("PNG stream unreadable");
    std::string encoded;
    try { encoded.assign(static_cast<const char*>(data), size); } catch (...) { GlobalUnlock(global); return encodingFailed("Out of memory"); }
    GlobalUnlock(global);
    stream.reset();
    // WIC writes only allow-listed chunks today; the sanitizer keeps that true.
    auto clean = png::stripDisallowedChunks(encoded);
    int w = 0, h = 0;
    if (clean.empty() || png::inspect(clean, &w, &h) || w != width || h != height) return encodingFailed("Encoded PNG failed validation");
    png = std::move(clean);
    return {};
}

Error encodeBitmap(IWICBitmap* bitmap, bool alpha, std::string& png) {
    UINT width = 0, height = 0;
    if (!bitmap || FAILED(bitmap->GetSize(&width, &height)) || !width || !height) return encodingFailed("Empty image");
    WICRect all{0, 0, INT(width), INT(height)};
    Com<IWICBitmapLock> lock;
    if (FAILED(bitmap->Lock(&all, WICBitmapLockRead, lock.put()))) return encodingFailed("Bitmap lock failed");
    UINT stride = 0, size = 0;
    BYTE* data = nullptr;
    if (FAILED(lock->GetStride(&stride)) || FAILED(lock->GetDataPointer(&size, &data)) || !data) return encodingFailed("Bitmap lock failed");
    return encodePixels(data, int(width), int(height), stride, alpha, png);
}

Error writeExisting(const std::string& path, std::string_view bytes) {
    if (path.empty() || !paths::isAbsolute(path)) return io("Output path is not absolute");
    HANDLE h = CreateFileW(paths::wide(path).c_str(), GENERIC_WRITE | FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (h == INVALID_HANDLE_VALUE) return io("Could not open the output file");
    Error result;
    if (!plainFile(h)) result = io("Output is not a private regular file");
    LARGE_INTEGER zero{};
    if (!result && !SetFilePointerEx(h, zero, nullptr, FILE_BEGIN)) result = io("Could not write the output file");
    size_t offset = 0;
    while (!result && offset < bytes.size()) {
        const DWORD chunk = DWORD(std::min<size_t>(bytes.size() - offset, 1u << 24));
        DWORD written = 0;
        if (!WriteFile(h, bytes.data() + offset, chunk, &written, nullptr) || written == 0) result = io("Could not write the output file");
        offset += written;
    }
    if (!result && !SetEndOfFile(h)) result = io("Could not write the output file");
    if (!result && !FlushFileBuffers(h)) result = io("Could not flush the output file");
    CloseHandle(h);
    return result;
}

Error readFile(const std::string& path, size_t maximumBytes, std::string& bytes) {
    bytes.clear();
    if (path.empty() || !paths::isAbsolute(path)) return io("Input path is not absolute");
    HANDLE h = CreateFileW(paths::wide(path).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) return io("Could not open the input file");
    Error result;
    LARGE_INTEGER size{};
    if (!plainFile(h) || !GetFileSizeEx(h, &size) || size.QuadPart < 0) result = io("Input is not a regular file");
    else if (size.QuadPart == 0) result = unsupported("Input is empty");
    else if (static_cast<unsigned long long>(size.QuadPart) > maximumBytes) result = {"input_excessive", "Input exceeds the accepted byte limit"};
    if (!result) {
        try { bytes.resize(size_t(size.QuadPart)); } catch (...) { result = {"input_excessive", "Input exceeds available memory"}; }
    }
    size_t offset = 0;
    while (!result && offset < bytes.size()) {
        const DWORD chunk = DWORD(std::min<size_t>(bytes.size() - offset, 1u << 24));
        DWORD read = 0;
        if (!ReadFile(h, bytes.data() + offset, chunk, &read, nullptr) || read == 0) result = io("Could not read the input file");
        offset += read;
    }
    // A file that grew while reading is not the admitted input.
    if (!result) {
        char extra;
        DWORD read = 0;
        if (ReadFile(h, &extra, 1, &read, nullptr) && read != 0) result = io("Input changed while reading");
    }
    CloseHandle(h);
    if (result) bytes.clear();
    return result;
}

Container sniff(std::string_view b) {
    auto starts = [&](std::string_view s) { return b.size() >= s.size() && b.substr(0, s.size()) == s; };
    if (png::hasSignature(b)) return Container::Png;
    if (starts(std::string_view("\xFF\xD8\xFF", 3))) return Container::Jpeg;
    if (starts("BM") && b.size() >= 26) return Container::Bmp;
    if (starts("GIF87a") || starts("GIF89a")) return Container::Gif;
    if (starts(std::string_view("II*\0", 4)) || starts(std::string_view("MM\0*", 4))) return Container::Tiff;
    if (b.size() >= 12 && starts("RIFF") && b.substr(8, 4) == "WEBP") return Container::Webp;
    return Container::Unknown;
}

const char* containerName(Container c) {
    switch (c) {
    case Container::Png: return "png";
    case Container::Jpeg: return "jpeg";
    case Container::Bmp: return "bmp";
    case Container::Gif: return "gif";
    case Container::Tiff: return "tiff";
    case Container::Webp: return "webp";
    default: return "unknown";
    }
}

bool animatedPng(std::string_view b) {
    if (!png::hasSignature(b)) return false;
    size_t at = 8;
    while (b.size() - at >= 12) {
        const auto length = readBE(b, at);
        const auto type = b.substr(at + 4, 4);
        if (type == "acTL") return true;
        if (type == "IDAT" || type == "IEND") return false;
        if (length > b.size() - at - 12) return false;
        at += 12 + size_t(length);
    }
    return false;
}

Error openFrame(std::string_view bytes, Container container, Com<IWICBitmapFrameDecode>& frame) {
    auto* f = factory();
    if (!f) return unsupported("Windows Imaging Component is unavailable");
    if (bytes.empty() || bytes.size() > 0xFFFFFFFFu) return unsupported("Unsupported input size");
    GUID format{};
    const GUID* vendor = &vendorBuiltIn;
    switch (container) {
    case Container::Png: format = GUID_ContainerFormatPng; break;
    case Container::Jpeg: format = GUID_ContainerFormatJpeg; break;
    case Container::Bmp: format = GUID_ContainerFormatBmp; break;
    case Container::Gif: format = GUID_ContainerFormatGif; break;
    case Container::Tiff: format = GUID_ContainerFormatTiff; break;
    case Container::Webp: format = containerWebp; vendor = nullptr; break; // Store extension codec, when installed
    default: return unsupported("Unsupported image format");
    }
    Com<IWICBitmapDecoder> decoder;
    if (FAILED(f->CreateDecoder(format, vendor, decoder.put()))) return unsupported("No decoder for this image format");
    Com<IWICStream> stream;
    if (FAILED(f->CreateStream(stream.put())) ||
        FAILED(stream->InitializeFromMemory(reinterpret_cast<BYTE*>(const_cast<char*>(bytes.data())), DWORD(bytes.size()))))
        return unsupported("Could not open the image stream");
    if (FAILED(decoder->Initialize(stream.get(), WICDecodeMetadataCacheOnDemand))) return unsupported("The image could not be decoded");
    UINT frames = 0;
    if (FAILED(decoder->GetFrameCount(&frames)) || frames == 0) return unsupported("The image has no frames");
    if (FAILED(decoder->GetFrame(0, frame.put()))) return unsupported("The image could not be decoded");
    return {};
}

int jpegOrientation(IWICBitmapFrameDecode* frame) {
    Com<IWICMetadataQueryReader> reader;
    if (!frame || FAILED(frame->GetMetadataQueryReader(reader.put())) || !reader) return 1;
    PROPVARIANT value;
    PropVariantInit(&value);
    int orientation = 1;
    if (SUCCEEDED(reader->GetMetadataByName(L"/app1/ifd/{ushort=274}", &value)) && value.vt == VT_UI2 && value.uiVal >= 1 && value.uiVal <= 8)
        orientation = value.uiVal;
    PropVariantClear(&value);
    return orientation;
}

bool formatHasAlpha(IWICBitmapSource* source) {
    auto* f = factory();
    WICPixelFormatGUID format{};
    if (!f || !source || FAILED(source->GetPixelFormat(&format))) return false;
    Com<IWICComponentInfo> info;
    if (FAILED(f->CreateComponentInfo(format, info.put()))) return false;
    Com<IWICPixelFormatInfo2> pixel;
    if (FAILED(info->QueryInterface(IID_PPV_ARGS(pixel.put())))) return false;
    BOOL transparent = FALSE;
    return SUCCEEDED(pixel->SupportsTransparency(&transparent)) && transparent;
}

Error toBgraBitmap(IWICBitmapSource* source, Com<IWICBitmap>& bitmap) {
    auto* f = factory();
    if (!f || !source) return unsupported("Windows Imaging Component is unavailable");
    Com<IWICFormatConverter> converter;
    if (FAILED(f->CreateFormatConverter(converter.put())) ||
        FAILED(converter->Initialize(source, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom)))
        return unsupported("The image pixel format cannot be converted");
    if (FAILED(f->CreateBitmapFromSource(converter.get(), WICBitmapCacheOnLoad, bitmap.put()))) return unsupported("The image could not be decoded");
    return {};
}

Error orient(IWICBitmap* source, int orientation, Com<IWICBitmap>& out) {
    auto* f = factory();
    if (!f || !source) return unsupported("Windows Imaging Component is unavailable");
    // Rotation first, then the mirror, as two separate steps so the order is explicit.
    WICBitmapTransformOptions rotate = WICBitmapTransformRotate0;
    bool mirror = false;
    switch (orientation) {
    case 2: mirror = true; break;
    case 3: rotate = WICBitmapTransformRotate180; break;
    case 4: rotate = WICBitmapTransformRotate180; mirror = true; break;
    case 5: rotate = WICBitmapTransformRotate90; mirror = true; break;
    case 6: rotate = WICBitmapTransformRotate90; break;
    case 7: rotate = WICBitmapTransformRotate270; mirror = true; break;
    case 8: rotate = WICBitmapTransformRotate270; break;
    default: adopt(source, out); return {};
    }
    Com<IWICBitmap> current;
    adopt(source, current);
    auto step = [&](WICBitmapTransformOptions option) -> Error {
        Com<IWICBitmapFlipRotator> rotator;
        Com<IWICBitmap> next;
        if (FAILED(f->CreateBitmapFlipRotator(rotator.put())) || FAILED(rotator->Initialize(current.get(), option)) ||
            FAILED(f->CreateBitmapFromSource(rotator.get(), WICBitmapCacheOnLoad, next.put())))
            return unsupported("The image orientation could not be applied");
        current = std::move(next);
        return {};
    };
    if (rotate != WICBitmapTransformRotate0) if (auto e = step(rotate)) return e;
    if (mirror) if (auto e = step(WICBitmapTransformFlipHorizontal)) return e;
    out = std::move(current);
    return {};
}

Error scaleToEdge(IWICBitmap* source, int edge, Com<IWICBitmap>& out) {
    auto* f = factory();
    UINT width = 0, height = 0;
    if (!f || !source || edge <= 0 || FAILED(source->GetSize(&width, &height)) || !width || !height) return encodingFailed("Invalid preview request");
    if (width <= UINT(edge) && height <= UINT(edge)) { adopt(source, out); return {}; }
    UINT w, h;
    if (width >= height) { w = UINT(edge); h = UINT(std::max<unsigned long long>(1, (unsigned long long)height * UINT(edge) / width)); }
    else { h = UINT(edge); w = UINT(std::max<unsigned long long>(1, (unsigned long long)width * UINT(edge) / height)); }
    Com<IWICBitmapScaler> scaler;
    if (FAILED(f->CreateBitmapScaler(scaler.put())) || FAILED(scaler->Initialize(source, w, h, WICBitmapInterpolationModeFant)) ||
        FAILED(f->CreateBitmapFromSource(scaler.get(), WICBitmapCacheOnLoad, out.put())))
        return encodingFailed("Preview scaling failed");
    return {};
}
}
