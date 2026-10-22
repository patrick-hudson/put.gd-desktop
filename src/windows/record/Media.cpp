#include "record/Media.h"
#include "common/Log.h"
#include "common/Paths.h"
#include <codecapi.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mftransform.h>
#include <shlwapi.h>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace putgd::media {
namespace {
using wic::Com;
// MF_MPEG4SINK_MOOV_BEFORE_MDAT (Windows 8 and later; missing from mingw's headers):
// the index goes first, so a shared link plays before the whole file arrives.
const GUID moovBeforeMdat{0xf672e3ac, 0xe1e6, 0x4f10, {0xb5, 0xec, 0x5f, 0x3b, 0x30, 0x82, 0x88, 0x16}};

struct Api {
    HMODULE plat = nullptr, readwrite = nullptr;
    HRESULT(WINAPI* startup)(ULONG, DWORD) = nullptr;
    HRESULT(WINAPI* shutdown)() = nullptr;
    HRESULT(WINAPI* createMediaType)(IMFMediaType**) = nullptr;
    HRESULT(WINAPI* createSample)(IMFSample**) = nullptr;
    HRESULT(WINAPI* createMemoryBuffer)(DWORD, IMFMediaBuffer**) = nullptr;
    HRESULT(WINAPI* copyImage)(BYTE*, LONG, const BYTE*, LONG, DWORD, DWORD) = nullptr;
    HRESULT(WINAPI* createAttributes)(IMFAttributes**, UINT32) = nullptr;
    HRESULT(WINAPI* byteStreamOnStream)(IStream*, IMFByteStream**) = nullptr;
    HRESULT(WINAPI* sinkWriterFromUrl)(LPCWSTR, IMFByteStream*, IMFAttributes*, IMFSinkWriter**) = nullptr;
    HRESULT(WINAPI* sourceReaderFromUrl)(LPCWSTR, IMFAttributes*, IMFSourceReader**) = nullptr;
};
Api api;
template <class F> bool resolve(HMODULE module, const char* name, F& out) {
    out = reinterpret_cast<F>(reinterpret_cast<void*>(GetProcAddress(module, name)));
    return out != nullptr;
}
std::string hex(HRESULT hr) { char b[16]; wsprintfA(b, "0x%08lX", static_cast<unsigned long>(hr)); return b; }
Error encoderError(const char* step, HRESULT hr) {
    return {"recording_encoder_unavailable", "No built-in H.264 encoder accepted this recording", {{"step", step}, {"hresult", hex(hr)}}};
}
std::string narrowName(const wchar_t* w) { return w ? paths::narrow(w) : std::string(); }
}

Platform::Platform() {
    api.plat = LoadLibraryExW(L"mfplat.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    api.readwrite = LoadLibraryExW(L"mfreadwrite.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    // Windows N editions ship without Media Foundation until the Media Feature Pack is installed.
    if (!api.plat || !api.readwrite) { failure_ = "media_foundation_missing"; return; }
    const bool resolved = resolve(api.plat, "MFStartup", api.startup) && resolve(api.plat, "MFShutdown", api.shutdown) &&
        resolve(api.plat, "MFCreateMediaType", api.createMediaType) && resolve(api.plat, "MFCreateSample", api.createSample) &&
        resolve(api.plat, "MFCreateMemoryBuffer", api.createMemoryBuffer) && resolve(api.plat, "MFCopyImage", api.copyImage) &&
        resolve(api.plat, "MFCreateAttributes", api.createAttributes) && resolve(api.plat, "MFCreateMFByteStreamOnStream", api.byteStreamOnStream) &&
        resolve(api.readwrite, "MFCreateSinkWriterFromURL", api.sinkWriterFromUrl) && resolve(api.readwrite, "MFCreateSourceReaderFromURL", api.sourceReaderFromUrl);
    if (!resolved) { failure_ = "media_foundation_incomplete"; return; }
    if (FAILED(api.startup(MF_VERSION, MFSTARTUP_LITE))) { failure_ = "media_foundation_startup"; return; }
    ok_ = true;
}
Platform::~Platform() { if (ok_) api.shutdown(); }

Mp4Writer::~Mp4Writer() { abandon(); }
void Mp4Writer::abandon() { if (writer_) { writer_->Release(); writer_ = nullptr; } }
Error Mp4Writer::open(const std::string& path, const VideoFormat& format) {
    const auto wide = paths::wide(path);
    const DWORD attributes = GetFileAttributesW(wide.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) return {"artifact_io", "The working clip file is not a regular file", {}};
    // Opens the existing, resident-created file; never creates one.
    Com<IStream> stream;
    if (FAILED(SHCreateStreamOnFileEx(wide.c_str(), STGM_READWRITE | STGM_SHARE_EXCLUSIVE, FILE_ATTRIBUTE_NORMAL, FALSE, nullptr, stream.put()))) return {"artifact_io", "Cannot open the working clip file", {}};
    Com<IMFByteStream> bytes;
    if (FAILED(api.byteStreamOnStream(stream.get(), bytes.put()))) return {"artifact_io", "Cannot open the working clip file", {}};
    return start(bytes.get(), format);
}
Error Mp4Writer::openInMemory(const VideoFormat& format) {
    Com<IStream> stream;
    *stream.put() = SHCreateMemStream(nullptr, 0);
    if (!stream) return {"recording_encoder_unavailable", "Cannot create a memory stream", {}};
    Com<IMFByteStream> bytes;
    if (FAILED(api.byteStreamOnStream(stream.get(), bytes.put()))) return {"recording_encoder_unavailable", "Cannot create a memory byte stream", {}};
    return start(bytes.get(), format);
}
Error Mp4Writer::start(void* byteStreamPointer, const VideoFormat& format) {
    auto* byteStream = static_cast<IMFByteStream*>(byteStreamPointer);
    abandon();
    format_ = format;
    Error last;
    // A GPU encoder first; Media Foundation's software encoder when the GPU's refuses.
    for (const bool hardware : {true, false}) {
        Com<IMFAttributes> attributes;
        if (FAILED(api.createAttributes(attributes.put(), 4))) return encoderError("attributes", E_FAIL);
        attributes->SetGUID(MF_TRANSCODE_CONTAINERTYPE, MFTranscodeContainerType_MPEG4);
        attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, hardware ? TRUE : FALSE);
        if (format.faststart) attributes->SetUINT32(moovBeforeMdat, TRUE);
        byteStream->SetCurrentPosition(0);
        byteStream->SetLength(0);
        IMFSinkWriter* writer = nullptr;
        HRESULT hr = api.sinkWriterFromUrl(L".mp4", byteStream, attributes.get(), &writer);
        if (FAILED(hr)) { last = encoderError("sinkWriter", hr); continue; }
        Com<IMFMediaType> output, input;
        api.createMediaType(output.put());
        output->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        output->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
        output->SetUINT32(MF_MT_AVG_BITRATE, UINT32(format.bitrate));
        output->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        output->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Main);
        MFSetAttributeSize(output.get(), MF_MT_FRAME_SIZE, UINT32(format.width), UINT32(format.height));
        MFSetAttributeRatio(output.get(), MF_MT_FRAME_RATE, UINT32(format.fps), 1);
        MFSetAttributeRatio(output.get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        DWORD stream = 0;
        hr = writer->AddStream(output.get(), &stream);
        if (FAILED(hr)) { writer->Release(); last = encoderError("outputType", hr); continue; }
        api.createMediaType(input.put());
        input->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        input->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
        input->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        // A positive stride: rows are top-down, as a DIB section with negative height has them.
        input->SetUINT32(MF_MT_DEFAULT_STRIDE, UINT32(format.width * 4));
        MFSetAttributeSize(input.get(), MF_MT_FRAME_SIZE, UINT32(format.width), UINT32(format.height));
        MFSetAttributeRatio(input.get(), MF_MT_FRAME_RATE, UINT32(format.fps), 1);
        MFSetAttributeRatio(input.get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        hr = writer->SetInputMediaType(stream, input.get(), nullptr);
        if (FAILED(hr)) { writer->Release(); last = encoderError("inputType", hr); continue; }
        hr = writer->BeginWriting();
        if (FAILED(hr)) { writer->Release(); last = encoderError("beginWriting", hr); continue; }
        writer_ = writer; stream_ = stream;
        encoder_.clear();
        Com<IMFSinkWriterEx> extended;
        if (SUCCEEDED(writer->QueryInterface(IID_IMFSinkWriterEx, reinterpret_cast<void**>(extended.put())))) {
            for (DWORD index = 0;; ++index) {
                GUID category{}; Com<IMFTransform> transform;
                if (FAILED(extended->GetTransformForStream(stream, index, &category, transform.put()))) break;
                if (category != MFT_CATEGORY_VIDEO_ENCODER) continue;
                // Hardware MFTs carry a hardware URL and usually a friendly name; Microsoft's
                // software encoder carries neither.
                Com<IMFAttributes> facts;
                wchar_t* name = nullptr; UINT32 length = 0; UINT32 urlLength = 0;
                const bool hasFacts = SUCCEEDED(transform->GetAttributes(facts.put())) && facts;
                const bool gpu = hasFacts && SUCCEEDED(facts->GetStringLength(MFT_ENUM_HARDWARE_URL_Attribute, &urlLength));
                if (hasFacts && SUCCEEDED(facts->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &name, &length))) { encoder_ = narrowName(name); CoTaskMemFree(name); }
                if (encoder_.empty()) encoder_ = gpu ? "hardware H.264 encoder" : "Microsoft H.264 encoder (software)";
                break;
            }
        }
        if (encoder_.empty()) encoder_ = "Media Foundation H.264";
        return {};
    }
    return last ? last : encoderError("unknown", E_FAIL);
}
Error Mp4Writer::write(const uint8_t* bits, size_t stride, long long time, long long duration) {
    if (!writer_) return {"recording_failed", "The encoder is not open", {}};
    const DWORD row = DWORD(format_.width) * 4, length = row * DWORD(format_.height);
    Com<IMFMediaBuffer> buffer;
    if (FAILED(api.createMemoryBuffer(length, buffer.put()))) return {"recording_failed", "Out of memory for a video frame", {}};
    BYTE* destination = nullptr;
    if (FAILED(buffer->Lock(&destination, nullptr, nullptr))) return {"recording_failed", "Cannot lock a video frame", {}};
    const HRESULT copied = api.copyImage(destination, LONG(row), bits, LONG(stride), row, DWORD(format_.height));
    buffer->Unlock();
    if (FAILED(copied)) return {"recording_failed", "Cannot copy a video frame", {}};
    buffer->SetCurrentLength(length);
    Com<IMFSample> sample;
    if (FAILED(api.createSample(sample.put()))) return {"recording_failed", "Out of memory for a video frame", {}};
    sample->AddBuffer(buffer.get());
    sample->SetSampleTime(time);
    sample->SetSampleDuration(duration);
    const HRESULT hr = writer_->WriteSample(stream_, sample.get());
    if (FAILED(hr)) return {"recording_failed", "The encoder refused a frame", {{"hresult", hex(hr)}}};
    return {};
}
Error Mp4Writer::finish() {
    if (!writer_) return {"recording_failed", "The encoder is not open", {}};
    const HRESULT hr = writer_->Finalize();
    abandon();
    if (FAILED(hr)) return {"recording_failed", "The recording could not be finalized", {{"hresult", hex(hr)}}};
    return {};
}

ClipReader::~ClipReader() { if (reader_) reader_->Release(); }
Error ClipReader::open(const std::string& path, int width, int height) {
    width_ = width; height_ = height;
    Com<IMFAttributes> attributes;
    if (FAILED(api.createAttributes(attributes.put(), 1))) return {"recording_export_failed", "Cannot prepare the decoder", {}};
    // Colour conversion and the decoder's padded height cropped back to the canvas.
    attributes->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
    HRESULT hr = api.sourceReaderFromUrl(paths::wide(path).c_str(), attributes.get(), &reader_);
    if (FAILED(hr)) return {"recording_export_failed", "Cannot open the working clip", {{"hresult", hex(hr)}}};
    reader_->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE);
    reader_->SetStreamSelection(MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE);
    Com<IMFMediaType> type;
    api.createMediaType(type.put());
    type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    MFSetAttributeSize(type.get(), MF_MT_FRAME_SIZE, UINT32(width), UINT32(height));
    hr = reader_->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, type.get());
    if (FAILED(hr)) return {"recording_export_failed", "The decoder cannot produce frames of the canvas size", {{"hresult", hex(hr)}}};
    Com<IMFMediaType> current;
    if (SUCCEEDED(reader_->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, current.put()))) {
        UINT32 stride = 0;
        if (SUCCEEDED(current->GetUINT32(MF_MT_DEFAULT_STRIDE, &stride))) defaultStride_ = LONG(stride);
    }
    if (defaultStride_ == 0) defaultStride_ = LONG(width) * 4;
    return {};
}
Error ClipReader::seek(long long us) {
    PROPVARIANT position; PropVariantInit(&position);
    position.vt = VT_I8; position.hVal.QuadPart = us * 10;
    const HRESULT hr = reader_->SetCurrentPosition(GUID_NULL, position);
    if (FAILED(hr)) return {"recording_export_failed", "Cannot seek in the working clip", {{"hresult", hex(hr)}}};
    return {};
}
bool ClipReader::next(std::vector<uint8_t>& bgrx, long long& time, Error& error) {
    while (true) {
        DWORD stream = 0, flags = 0; LONGLONG timestamp = 0;
        Com<IMFSample> sample;
        const HRESULT hr = reader_->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &stream, &flags, &timestamp, sample.put());
        if (FAILED(hr)) { error = {"recording_export_failed", "Cannot decode the working clip", {{"hresult", hex(hr)}}}; return false; }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) return false;
        if (!sample) continue; // a stream tick
        Com<IMFMediaBuffer> buffer;
        if (FAILED(sample->ConvertToContiguousBuffer(buffer.put()))) { error = {"recording_export_failed", "Cannot read a decoded frame", {}}; return false; }
        bgrx.resize(size_t(width_) * height_ * 4);
        const size_t row = size_t(width_) * 4;
        // Lock2D gives the top row and a signed pitch whatever the buffer's orientation.
        Com<IMF2DBuffer> twoD;
        BYTE* top = nullptr; LONG pitch = 0;
        bool locked2D = SUCCEEDED(buffer->QueryInterface(IID_IMF2DBuffer, reinterpret_cast<void**>(twoD.put()))) && SUCCEEDED(twoD->Lock2D(&top, &pitch));
        BYTE* base = nullptr; DWORD length = 0;
        if (!locked2D) {
            if (FAILED(buffer->Lock(&base, nullptr, &length))) { error = {"recording_export_failed", "Cannot read a decoded frame", {}}; return false; }
            pitch = defaultStride_;
            top = pitch < 0 ? base + size_t(-pitch) * size_t(height_ - 1) : base;
            if (length < DWORD(std::abs(pitch)) * DWORD(height_ - 1) + DWORD(row)) { buffer->Unlock(); error = {"recording_export_failed", "A decoded frame is too small", {}}; return false; }
        }
        for (int y = 0; y < height_; ++y) {
            const BYTE* source = top + (long long)pitch * y;
            uint8_t* target = bgrx.data() + row * size_t(y);
            std::memcpy(target, source, row);
            for (size_t x = 3; x < row; x += 4) target[x] = 255;
        }
        if (locked2D) twoD->Unlock2D(); else buffer->Unlock();
        time = timestamp;
        return true;
    }
}

Error probeH264(std::string& encoder) {
    VideoFormat format{320, 240, 30, 1500000};
    Mp4Writer writer;
    if (auto e = writer.openInMemory(format)) return e;
    std::vector<uint8_t> frame(size_t(format.width) * format.height * 4);
    for (int i = 0; i < 2; ++i) {
        for (size_t p = 0; p < frame.size(); p += 4) { frame[p] = uint8_t(p * 7 + i * 90); frame[p + 1] = uint8_t(p / 1280); frame[p + 2] = uint8_t(i * 128); frame[p + 3] = 255; }
        if (auto e = writer.write(frame.data(), size_t(format.width) * 4, i * 333333LL, 333333LL)) return {"recording_encoder_unavailable", e.message, e.details};
    }
    encoder = writer.encoder();
    if (auto e = writer.finish()) return {"recording_encoder_unavailable", e.message, e.details};
    return {};
}
}
