#pragma once
// Animated GIF through the GIF encoder built into Windows (WIC): one local
// palette per frame from WIC's median-cut palette, without dithering,
// per-frame delays in hundredths of a second and the NETSCAPE2.0 loop extension.
// Frames are top-down BGRX rows. The encoder writes into a file the resident
// created (helpers never create files), or into memory for the probe.
#include "capture/Png.h"
#include "common/Error.h"
#include <cstdint>
#include <string>

namespace putgd::record {
class GifWriter {
public:
    GifWriter() = default;
    ~GifWriter();
    GifWriter(const GifWriter&) = delete;
    GifWriter& operator=(const GifWriter&) = delete;
    // Errors: recording_export_failed (no GIF encoder, the file).
    Error open(const std::string& path, int width, int height);
    Error openInMemory(int width, int height);
    // `delayCs`: how long the frame shows, in hundredths of a second (at least 2:
    // browsers stretch shorter delays to 10).
    Error write(const uint8_t* bits, size_t stride, int delayCs);
    Error finish();
private:
    Error start(IStream*, int width, int height);
    wic::Com<IStream> stream_;
    wic::Com<IWICBitmapEncoder> encoder_;
    int width_ = 0, height_ = 0;
    bool finished_ = false;
};
// Encodes two small frames to memory: the encoder works, not only exists.
Error probeGif();
}
