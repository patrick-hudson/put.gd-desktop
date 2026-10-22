#pragma once
// Media Foundation for the recording helper (putgd.exe --record). mfplat.dll and
// mfreadwrite.dll are loaded at run time by the helper only, so the resident,
// which shares this executable, never maps them. Only built-in Windows encoders
// are used: no codec is bundled.
#include "capture/Png.h"
#include "common/Error.h"
#include <cstdint>
#include <string>
#include <mutex>
#include <vector>
#include <windows.h>

struct IMFSinkWriter;
struct IMFSourceReader;

namespace putgd::media {
// Loads Media Foundation and runs MFStartup; MFShutdown in the destructor. One
// per process, on the main thread, before any other function here.
class Platform {
public:
    Platform();
    ~Platform();
    bool ok() const { return ok_; }
    const std::string& failure() const { return failure_; }
private:
    bool ok_ = false;
    std::string failure_;
};

struct VideoFormat {
    int width = 0, height = 0, fps = 30;
    long long bitrate = 0;
    bool hevc = false; // H.265 instead of H.264 (needs an HEVC encoder: hardware or the Store extension)
    // An AAC track from 16-bit stereo PCM at this rate (44100 or 48000); 0 for none.
    int audioRate = 0;
};

// An H.264 MP4 writer over a file the resident created (helpers never create
// files), or over memory for the probe. Frames are top-down BGRX rows; times are
// 100-ns units on the clip timeline. Not thread-safe: one thread opens, writes and
// finishes it (a COM multithreaded apartment).
class Mp4Writer {
public:
    Mp4Writer() = default;
    ~Mp4Writer();
    Mp4Writer(const Mp4Writer&) = delete;
    Mp4Writer& operator=(const Mp4Writer&) = delete;
    // Errors: recording_encoder_unavailable (no H.264 encoder accepts the format),
    // artifact_io (the output file).
    Error open(const std::string& path, const VideoFormat&);
    Error openInMemory(const VideoFormat&);
    Error write(const uint8_t* bits, size_t stride, long long time100ns, long long duration100ns);
    // Interleaved 16-bit stereo frames for the AAC track. The video and audio
    // threads may write at the same time; calls are serialized.
    Error writeAudio(const int16_t* samples, size_t frames, long long time100ns);
    // Finalizes the container (index written); the file is complete afterwards.
    Error finish();
    // Drops the writer without finalizing; the resident discards the file.
    void abandon();
    // The encoder transform's friendly name, as Media Foundation chose it.
    const std::string& encoder() const { return encoder_; }
private:
    Error start(void* byteStream, const VideoFormat&);
    IMFSinkWriter* writer_ = nullptr;
    DWORD stream_ = 0, audioStream_ = 0;
    std::mutex mutex_;
    VideoFormat format_;
    std::string encoder_;
};

// Decodes a clip's first video stream to top-down BGRX frames of the canvas size,
// in presentation order. The helper reads only its own working clip with it.
class ClipReader {
public:
    ClipReader() = default;
    ~ClipReader();
    ClipReader(const ClipReader&) = delete;
    ClipReader& operator=(const ClipReader&) = delete;
    Error open(const std::string& path, int width, int height);
    // Seeks to the keyframe at or before `us`; frames before `us` still follow.
    Error seek(long long us);
    // The next frame (`time100ns` is its presentation time); false at the end or on error.
    bool next(std::vector<uint8_t>& bgrx, long long& time100ns, Error& error);
    int width() const { return width_; }
    int height() const { return height_; }
private:
    IMFSourceReader* reader_ = nullptr;
    int width_ = 0, height_ = 0;
    long defaultStride_ = 0;
};

// Decodes a clip's first audio stream to interleaved 16-bit PCM at its own rate
// and channel count; `available` is false for a clip without one.
class AudioReader {
public:
    AudioReader() = default;
    ~AudioReader();
    AudioReader(const AudioReader&) = delete;
    AudioReader& operator=(const AudioReader&) = delete;
    Error open(const std::string& path);
    bool available() const { return reader_ != nullptr; }
    int rate() const { return rate_; }
    int channels() const { return channels_; }
    Error seek(long long us);
    bool next(std::vector<int16_t>& samples, long long& time100ns, Error& error);
private:
    IMFSourceReader* reader_ = nullptr;
    int rate_ = 0, channels_ = 0;
};
// The AAC encoder with a short silent track, in memory.
Error probeAac();

// Encodes two synthetic 320x240 frames to memory: registry presence is not proof
// that an encoder works. Fills the encoder name on success.
Error probeH264(std::string& encoder);
// The same check for H.265; most computers without a GPU encoder have none.
Error probeHevc(std::string& encoder);
}
