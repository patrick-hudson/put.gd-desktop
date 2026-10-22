// putgd.exe --image-job: decodes one image away from the resident (role
// "image-job" in common/Protocol.h). Input is a PNG, a CF_DIB/CF_DIBV5 payload or
// any image file; output is a sanitized PNG (or verbatim PNG bytes) or CF_DIBV5
// bytes, plus an optional preview PNG. Only built-in WIC codecs (and an installed
// WebP codec) are used, chosen by content signature.
//
// A failed frame is a clean answer and the process exits 0; a closed parent pipe
// or a protocol violation exits 1.
#include "capture/Png.h"
#include "common/Frames.h"
#include "common/Log.h"
#include "common/Modes.h"
#include "common/Paths.h"
#include "common/Protocol.h"
#include "common/Sha256.h"
#include "core/SanitizedPng.h"
#include <algorithm>
#include <cstring>
#include <mutex>

namespace putgd {
namespace {
constexpr int maximumPreviewEdge = 4096;
// Headroom over the raw pixel budget for container overhead (DIB headers, palettes).
constexpr size_t inputSlackBytes = 1 << 20;

struct Job {
    std::string nonce;
    std::string inputPath, inputKind;
    std::string outputPath, outputKind = "png";
    std::string previewPath;
    int previewEdge = 0;
    long long pixels = 0, bytes = 0;
    bool reencode = false, previewOnly = false;
    bool preview() const { return !previewPath.empty(); }
};

class Session {
public:
    explicit Session(frames::Channel& channel) : channel_(channel) {}
    bool send(const std::string& type, const json::Object& payload = {}) {
        std::lock_guard lock(mutex_);
        return channel_.send(type, payload);
    }
    void startHeartbeat() { CloseHandle(CreateThread(nullptr, 0, &Session::heartbeat, this, 0, nullptr)); }
private:
    static DWORD WINAPI heartbeat(void* data) {
        auto* self = static_cast<Session*>(data);
        while (true) {
            Sleep(protocol::heartbeatMs);
            if (!self->send("heartbeat")) TerminateProcess(GetCurrentProcess(), 1);
        }
    }
    frames::Channel& channel_;
    std::mutex mutex_;
};

uint32_t le32(std::string_view b, size_t at) {
    return uint32_t(uint8_t(b[at])) | uint32_t(uint8_t(b[at + 1])) << 8 | uint32_t(uint8_t(b[at + 2])) << 16 | uint32_t(uint8_t(b[at + 3])) << 24;
}
uint16_t le16(std::string_view b, size_t at) { return uint16_t(uint8_t(b[at]) | uint8_t(b[at + 1]) << 8); }
void put32(std::string& s, uint32_t v) { for (int i = 0; i < 4; ++i) s.push_back(char((v >> (8 * i)) & 0xFF)); }

Error limitsAllow(long long width, long long height, const Job& job) {
    if (width <= 0 || height <= 0) return {"source_unsupported", "The image has no pixels"};
    const long long pixels = width * height;
    if (pixels > job.pixels || pixels > (1LL << 40) || pixels * 4 > job.bytes) return {"input_excessive", "The image exceeds the accepted limits"};
    return {};
}

// CF_DIB/CF_DIBV5 bytes (header, masks, color table, bits) -> a BMP file image the
// WIC BMP decoder accepts. Validates the header and the declared sizes first.
Error dibToBmp(std::string_view dib, const Job& job, std::string& bmp) {
    auto bad = [] { return Error{"source_unsupported", "The clipboard bitmap is malformed"}; };
    if (dib.size() < 40) return bad();
    const uint32_t headerSize = le32(dib, 0);
    if (headerSize != 40 && headerSize != 52 && headerSize != 56 && headerSize != 108 && headerSize != 124) return bad();
    if (dib.size() < headerSize) return bad();
    const int32_t width = int32_t(le32(dib, 4)), height = int32_t(le32(dib, 8));
    const uint16_t planes = le16(dib, 12), bpp = le16(dib, 14);
    const uint32_t compression = le32(dib, 16), colorsUsed = le32(dib, 32);
    if (width <= 0 || height == 0 || height == INT32_MIN || planes != 1) return bad();
    if (bpp != 1 && bpp != 4 && bpp != 8 && bpp != 16 && bpp != 24 && bpp != 32) return bad();
    const bool rle = (compression == BI_RLE8 && bpp == 8) || (compression == BI_RLE4 && bpp == 4);
    const bool bitfields = compression == BI_BITFIELDS && (bpp == 16 || bpp == 32);
    if (compression != BI_RGB && !rle && !bitfields) return bad();
    if (rle && height < 0) return bad();
    if (auto e = limitsAllow(width, height < 0 ? -(long long)height : height, job)) return e;
    if (bpp <= 8 && colorsUsed > (1u << bpp)) return bad();
    if (colorsUsed > 256) return bad();
    const uint64_t masks = (headerSize == 40 && bitfields) ? 12 : 0;
    const uint64_t colors = colorsUsed ? colorsUsed : (bpp <= 8 ? (1u << bpp) : 0);
    const uint64_t bitsOffset = headerSize + masks + colors * 4;
    if (bitsOffset > dib.size()) return bad();
    if (!rle) {
        const uint64_t stride = ((uint64_t(width) * bpp + 31) / 32) * 4;
        const uint64_t rows = height < 0 ? uint64_t(-(int64_t)height) : uint64_t(height);
        if (dib.size() - bitsOffset < stride * rows) return bad();
    }
    if (dib.size() + 14 > 0xFFFFFFFFull) return {"input_excessive", "The clipboard bitmap is too large"};
    try {
        bmp.clear();
        bmp.reserve(dib.size() + 14);
        bmp += "BM";
        put32(bmp, uint32_t(dib.size() + 14));
        put32(bmp, 0);
        put32(bmp, uint32_t(14 + bitsOffset));
        bmp.append(dib.data(), dib.size());
    } catch (...) { return {"input_excessive", "The clipboard bitmap exceeds available memory"}; }
    return {};
}

// CF_DIBV5: BITMAPV5HEADER with BGRA bit fields, sRGB, bottom-up rows (the most
// widely understood orientation), straight alpha (255 when opaque).
Error bitmapToDibV5(IWICBitmap* bitmap, std::string& out) {
    UINT width = 0, height = 0;
    if (FAILED(bitmap->GetSize(&width, &height)) || !width || !height) return {"encoding_failed", "Empty image"};
    WICRect all{0, 0, INT(width), INT(height)};
    wic::Com<IWICBitmapLock> lock;
    UINT stride = 0, size = 0;
    BYTE* data = nullptr;
    if (FAILED(bitmap->Lock(&all, WICBitmapLockRead, lock.put())) || FAILED(lock->GetStride(&stride)) || FAILED(lock->GetDataPointer(&size, &data)) || !data)
        return {"encoding_failed", "Bitmap lock failed"};
    const size_t rowBytes = size_t(width) * 4;
    BITMAPV5HEADER header{};
    header.bV5Size = sizeof header;
    header.bV5Width = LONG(width);
    header.bV5Height = LONG(height);
    header.bV5Planes = 1;
    header.bV5BitCount = 32;
    header.bV5Compression = BI_BITFIELDS;
    header.bV5SizeImage = DWORD(rowBytes * height);
    header.bV5RedMask = 0x00FF0000;
    header.bV5GreenMask = 0x0000FF00;
    header.bV5BlueMask = 0x000000FF;
    header.bV5AlphaMask = 0xFF000000;
    header.bV5CSType = 0x73524742; // LCS_sRGB ('sRGB'), spelled out to avoid a multi-character constant
    header.bV5Intent = LCS_GM_IMAGES;
    try {
        out.clear();
        out.resize(sizeof header + rowBytes * height);
    } catch (...) { return {"input_excessive", "The image exceeds available memory"}; }
    std::memcpy(out.data(), &header, sizeof header);
    for (UINT y = 0; y < height; ++y)
        std::memcpy(out.data() + sizeof header + rowBytes * (height - 1 - y), data + size_t(stride) * y, rowBytes);
    return {};
}

// Scans alpha; forces it to 255 when the image is opaque (or, for clipboard DIBs,
// when every alpha byte is 0: the classic CF_DIB "no alpha" encoding).
bool settleAlpha(IWICBitmap* bitmap, bool formatAlpha, bool fromDib) {
    UINT width = 0, height = 0;
    if (FAILED(bitmap->GetSize(&width, &height))) return false;
    WICRect all{0, 0, INT(width), INT(height)};
    wic::Com<IWICBitmapLock> lock;
    UINT stride = 0, size = 0;
    BYTE* data = nullptr;
    if (FAILED(bitmap->Lock(&all, WICBitmapLockWrite, lock.put())) || FAILED(lock->GetStride(&stride)) || FAILED(lock->GetDataPointer(&size, &data)) || !data)
        return false;
    bool translucent = false, anyVisible = false;
    if (formatAlpha) {
        for (UINT y = 0; y < height; ++y) {
            const BYTE* p = data + size_t(stride) * y;
            for (UINT x = 0; x < width; ++x, p += 4) {
                if (p[3] != 255) translucent = true;
                if (p[3] != 0) anyVisible = true;
            }
        }
    }
    const bool alpha = translucent && !(fromDib && !anyVisible);
    if (!alpha)
        for (UINT y = 0; y < height; ++y) {
            BYTE* p = data + size_t(stride) * y;
            for (UINT x = 0; x < width; ++x, p += 4) p[3] = 255;
        }
    return alpha;
}

bool parseJob(const json::Object& p, Job& job) {
    if (p.value("role") != "image-job" || !p.value("nonce").isString() || p.value("nonce").stringRef().empty()) return false;
    job.nonce = p.value("nonce").stringRef();
    const auto& input = p.value("input");
    job.inputPath = input["path"].toString();
    job.inputKind = input["kind"].toString();
    if (job.inputPath.empty() || !paths::isAbsolute(job.inputPath)) return false;
    if (job.inputKind != "png" && job.inputKind != "dib" && job.inputKind != "file") return false;
    const auto& previewOnly = p.value("previewOnly");
    if (!previewOnly.isNull() && !previewOnly.isBool()) return false;
    job.previewOnly = previewOnly.toBool();
    const auto& output = p.value("output");
    job.outputPath = output["path"].toString();
    if (output["kind"].isString()) job.outputKind = output["kind"].stringRef();
    else if (!output["kind"].isNull()) return false;
    if (job.outputKind != "png" && job.outputKind != "dib") return false;
    if (!job.previewOnly && (job.outputPath.empty() || !paths::isAbsolute(job.outputPath))) return false;
    const auto& preview = p.value("preview");
    if (!preview.isNull()) {
        job.previewPath = preview["path"].toString();
        if (job.previewPath.empty() || !paths::isAbsolute(job.previewPath)) return false;
        if (!json::integer(preview["edge"], 1, maximumPreviewEdge)) return false;
        job.previewEdge = preview["edge"].toInt();
    }
    if (job.previewOnly && !job.preview()) return false;
    const auto& limits = p.value("limits");
    if (!json::integer(limits["imagePixels"], 1, 9007199254740991.0) || !json::integer(limits["imageBytes"], 1, 9007199254740991.0)) return false;
    job.pixels = limits["imagePixels"].toInteger();
    job.bytes = limits["imageBytes"].toInteger();
    const auto& reencode = p.value("reencode");
    if (!reencode.isNull() && !reencode.isBool()) return false;
    job.reencode = reencode.toBool();
    return true;
}

struct Outcome {
    std::string bytes; // what went to output.path
    int width = 0, height = 0;
    bool previewWritten = false;
};

Error run(const Job& job, Outcome& outcome) {
    std::string source;
    const size_t maximum = size_t(std::min<long long>(job.bytes, (long long)0x7FFFFFFF - (long long)inputSlackBytes)) + inputSlackBytes;
    if (auto e = wic::readFile(job.inputPath, maximum, source)) return e;
    wic::Container container = wic::sniff(source);
    if (job.inputKind == "png" && container != wic::Container::Png) return {"source_unsupported", "The input is not a PNG image"};
    if (job.inputKind == "dib") {
        std::string bmp;
        if (auto e = dibToBmp(source, job, bmp)) return e;
        source = std::move(bmp);
        container = wic::Container::Bmp;
    }
    if (container == wic::Container::Unknown) return {"source_unsupported", "Unsupported image format"};
    if (container == wic::Container::Png && wic::animatedPng(source)) return {"source_unsupported", "Animated PNG images are not supported"};

    // Verbatim PNG: only already-sanitized PNG bytes, never when re-encoding.
    if (container == wic::Container::Png && job.outputKind == "png" && !job.reencode && !job.previewOnly) {
        int width = 0, height = 0;
        if (!png::inspect(source, &width, &height)) {
            if (auto e = limitsAllow(width, height, job)) return e;
            if (auto e = wic::writeExisting(job.outputPath, source)) return e;
            outcome.bytes = std::move(source);
            outcome.width = width;
            outcome.height = height;
            if (!job.preview()) return {};
            // Fall through to decode for the preview only.
            wic::Com<IWICBitmapFrameDecode> frame;
            if (auto e = wic::openFrame(outcome.bytes, container, frame)) return e;
            wic::Com<IWICBitmap> bitmap, scaled;
            if (auto e = wic::toBgraBitmap(frame.get(), bitmap)) return e;
            frame.reset();
            const bool alpha = settleAlpha(bitmap.get(), wic::formatHasAlpha(bitmap.get()), false);
            if (auto e = wic::scaleToEdge(bitmap.get(), job.previewEdge, scaled)) return e;
            bitmap.reset();
            std::string preview;
            if (auto e = wic::encodeBitmap(scaled.get(), alpha, preview)) return e;
            if (auto e = wic::writeExisting(job.previewPath, preview)) return e;
            outcome.previewWritten = true;
            return {};
        }
    }

    wic::Com<IWICBitmapFrameDecode> frame;
    if (auto e = wic::openFrame(source, container, frame)) return e;
    UINT width = 0, height = 0;
    if (FAILED(frame->GetSize(&width, &height))) return {"source_unsupported", "The image could not be decoded"};
    if (auto e = limitsAllow(width, height, job)) return e;
    const bool formatAlpha = wic::formatHasAlpha(frame.get());
    const int orientation = container == wic::Container::Jpeg ? wic::jpegOrientation(frame.get()) : 1;
    wic::Com<IWICBitmap> decoded, bitmap;
    if (auto e = wic::toBgraBitmap(frame.get(), decoded)) return e;
    frame.reset();
    source.clear();
    source.shrink_to_fit();
    if (auto e = wic::orient(decoded.get(), orientation, bitmap)) return e;
    decoded.reset();
    if (FAILED(bitmap->GetSize(&width, &height))) return {"source_unsupported", "The image could not be decoded"};
    outcome.width = int(width);
    outcome.height = int(height);
    const bool alpha = settleAlpha(bitmap.get(), formatAlpha, job.inputKind == "dib");

    if (!job.previewOnly) {
        std::string bytes;
        if (job.outputKind == "dib") {
            if (auto e = bitmapToDibV5(bitmap.get(), bytes)) return e;
        } else if (auto e = wic::encodeBitmap(bitmap.get(), alpha, bytes)) {
            return e;
        }
        if (auto e = wic::writeExisting(job.outputPath, bytes)) return e;
        outcome.bytes = std::move(bytes);
    }
    if (job.preview()) {
        wic::Com<IWICBitmap> scaled;
        if (auto e = wic::scaleToEdge(bitmap.get(), job.previewEdge, scaled)) return e;
        bitmap.reset();
        std::string preview;
        if (auto e = wic::encodeBitmap(scaled.get(), alpha, preview)) return e;
        if (auto e = wic::writeExisting(job.previewPath, preview)) return e;
        outcome.previewWritten = true;
    }
    return {};
}
}

int imageJobMain(const std::vector<std::string>&) {
    auto& channel = frames::helperChannel();
    protocol::Frame frame;
    std::string error;
    if (!channel.receive(frame, protocol::startupMs, &error) || frame.type != "bootstrap") {
        log::error("image-job", "No bootstrap frame", {{"reason", error}});
        return 1;
    }
    // Deliberately leaked: the heartbeat thread uses it until the process exits.
    Session& session = *new Session(channel);
    Job job;
    if (!parseJob(frame.payload, job)) {
        log::error("image-job", "Invalid bootstrap");
        session.send("failed", {{"code", "artifact_io"}, {"message", "Invalid image job request"}});
        return 1;
    }
    if (!session.send("ready", {{"nonce", job.nonce}, {"build", protocol::buildIdentity()}})) return 1;
    if (!channel.receive(frame, -1, &error) || frame.type != "activate") return 1;
    session.startHeartbeat();

    wic::Apartment com;
    Outcome outcome;
    Error result;
    if (!com.ok() || !wic::factory()) result = {"encoding_failed", "Windows Imaging Component is unavailable"};
    else result = run(job, outcome);
    if (result) {
        session.send("failed", {{"code", result.code}, {"message", result.message}});
        return 0;
    }
    json::Object checkpoint{{"bytes", (long long)outcome.bytes.size()},
                            {"sha256", job.previewOnly ? std::string() : crypto::Sha256::hexDigest(outcome.bytes)},
                            {"width", outcome.width},
                            {"height", outcome.height},
                            {"mime", job.outputKind == "dib" ? "image/x-dib" : "image/png"},
                            {"previewWritten", outcome.previewWritten}};
    outcome.bytes.clear();
    outcome.bytes.shrink_to_fit();
    if (!session.send("checkpoint", checkpoint)) return 1;
    if (!channel.receive(frame, -1, &error) || frame.type != "checkpointAccepted") return 1;
    return 0;
}
}
