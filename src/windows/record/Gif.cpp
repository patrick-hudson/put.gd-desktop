#include "record/Gif.h"
#include "common/Paths.h"
#include <algorithm>
#include <shlwapi.h>
#include <vector>

namespace putgd::record {
namespace {
using wic::Com;
Error failed(const char* step, HRESULT hr) {
    char code[16]; wsprintfA(code, "0x%08lX", static_cast<unsigned long>(hr));
    return {"recording_export_failed", "The GIF encoder failed", {{"step", step}, {"hresult", code}}};
}
HRESULT setBytes(IWICMetadataQueryWriter* writer, const wchar_t* name, const unsigned char* bytes, ULONG count) {
    PROPVARIANT value; PropVariantInit(&value);
    value.vt = VT_UI1 | VT_VECTOR;
    value.caub.cElems = count;
    value.caub.pElems = const_cast<unsigned char*>(bytes);
    return writer->SetMetadataByName(name, &value);
}
HRESULT setShort(IWICMetadataQueryWriter* writer, const wchar_t* name, USHORT number) {
    PROPVARIANT value; PropVariantInit(&value);
    value.vt = VT_UI2; value.uiVal = number;
    return writer->SetMetadataByName(name, &value);
}
HRESULT setByte(IWICMetadataQueryWriter* writer, const wchar_t* name, UCHAR number) {
    PROPVARIANT value; PropVariantInit(&value);
    value.vt = VT_UI1; value.bVal = number;
    return writer->SetMetadataByName(name, &value);
}
}

GifWriter::~GifWriter() = default;
Error GifWriter::open(const std::string& path, int width, int height) {
    const auto wide = paths::wide(path);
    const DWORD attributes = GetFileAttributesW(wide.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) return {"recording_export_failed", "The export file is not a regular file", {}};
    Com<IStream> stream;
    // The existing, resident-created file; never created here.
    if (FAILED(SHCreateStreamOnFileEx(wide.c_str(), STGM_READWRITE | STGM_SHARE_EXCLUSIVE, FILE_ATTRIBUTE_NORMAL, FALSE, nullptr, stream.put()))) return {"recording_export_failed", "Cannot open the export file", {}};
    ULARGE_INTEGER zero{}; stream->SetSize(zero);
    return start(stream.get(), width, height);
}
Error GifWriter::openInMemory(int width, int height) {
    Com<IStream> stream;
    *stream.put() = SHCreateMemStream(nullptr, 0);
    if (!stream) return {"recording_export_failed", "Cannot create a memory stream", {}};
    return start(stream.get(), width, height);
}
Error GifWriter::start(IStream* stream, int width, int height) {
    auto* factory = wic::factory();
    if (!factory) return {"recording_export_failed", "Windows Imaging Component is unavailable", {}};
    stream->AddRef();
    *stream_.put() = stream;
    width_ = width; height_ = height;
    HRESULT hr = factory->CreateEncoder(GUID_ContainerFormatGif, nullptr, encoder_.put());
    if (FAILED(hr)) return failed("encoder", hr);
    hr = encoder_->Initialize(stream, WICBitmapEncoderNoCache);
    if (FAILED(hr)) return failed("initialize", hr);
    // Loop forever (NETSCAPE2.0 application extension) on a screen of the frame size.
    Com<IWICMetadataQueryWriter> metadata;
    if (SUCCEEDED(encoder_->GetMetadataQueryWriter(metadata.put()))) {
        static const unsigned char application[] = {'N', 'E', 'T', 'S', 'C', 'A', 'P', 'E', '2', '.', '0'};
        static const unsigned char loop[] = {3, 1, 0, 0, 0};
        setBytes(metadata.get(), L"/appext/application", application, sizeof application);
        setBytes(metadata.get(), L"/appext/data", loop, sizeof loop);
        setShort(metadata.get(), L"/logscrdesc/Width", USHORT(width));
        setShort(metadata.get(), L"/logscrdesc/Height", USHORT(height));
    }
    return {};
}
Error GifWriter::write(const uint8_t* bits, size_t stride, int delayCs) {
    auto* factory = wic::factory();
    Com<IWICBitmap> source;
    HRESULT hr = factory->CreateBitmapFromMemory(UINT(width_), UINT(height_), GUID_WICPixelFormat32bppBGR, UINT(stride), UINT(stride * size_t(height_)), const_cast<BYTE*>(bits), source.put());
    if (FAILED(hr)) return failed("bitmap", hr);
    Com<IWICPalette> palette;
    if (FAILED(hr = factory->CreatePalette(palette.put())) || FAILED(hr = palette->InitializeFromBitmap(source.get(), 256, FALSE))) return failed("palette", hr);
    Com<IWICFormatConverter> indexed;
    if (FAILED(hr = factory->CreateFormatConverter(indexed.put())) ||
        FAILED(hr = indexed->Initialize(source.get(), GUID_WICPixelFormat8bppIndexed, WICBitmapDitherTypeNone, palette.get(), 0.0, WICBitmapPaletteTypeCustom))) return failed("quantize", hr);
    Com<IWICBitmapFrameEncode> frame;
    if (FAILED(hr = encoder_->CreateNewFrame(frame.put(), nullptr)) || FAILED(hr = frame->Initialize(nullptr))) return failed("frame", hr);
    frame->SetSize(UINT(width_), UINT(height_));
    WICPixelFormatGUID format = GUID_WICPixelFormat8bppIndexed;
    frame->SetPixelFormat(&format);
    frame->SetPalette(palette.get());
    Com<IWICMetadataQueryWriter> metadata;
    if (SUCCEEDED(frame->GetMetadataQueryWriter(metadata.put()))) {
        setShort(metadata.get(), L"/grctlext/Delay", USHORT(std::max(2, delayCs)));
        setByte(metadata.get(), L"/grctlext/Disposal", 1); // keep: every frame covers the whole screen
    }
    if (FAILED(hr = frame->WriteSource(indexed.get(), nullptr)) || FAILED(hr = frame->Commit())) return failed("write", hr);
    return {};
}
Error GifWriter::finish() {
    if (finished_) return {};
    finished_ = true;
    HRESULT hr = encoder_->Commit();
    if (FAILED(hr)) return failed("commit", hr);
    if (FAILED(hr = stream_->Commit(STGC_DEFAULT))) return failed("flush", hr);
    // The file is opened exclusively; let it go so it can be hashed and moved.
    encoder_.reset();
    stream_.reset();
    return {};
}

Error probeGif() {
    GifWriter writer;
    if (auto e = writer.openInMemory(32, 24)) return e;
    std::vector<uint8_t> frame(size_t(32) * 24 * 4);
    for (int i = 0; i < 2; ++i) {
        for (size_t p = 0; p < frame.size(); p += 4) { frame[p] = uint8_t(p * 3 + i * 70); frame[p + 1] = uint8_t(p / 96); frame[p + 2] = uint8_t(i * 200); frame[p + 3] = 255; }
        if (auto e = writer.write(frame.data(), 32 * 4, 7)) return e;
    }
    return writer.finish();
}
}
