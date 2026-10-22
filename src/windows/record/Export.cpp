#include "record/Export.h"
#include "capture/Png.h"
#include "common/Files.h"
#include "common/Paths.h"
#include "core/Audio.h"
#include "core/Mp4.h"
#include "core/RecordingFrames.h"
#include "record/Gif.h"
#include "record/Media.h"
#include <cmath>
#include <vector>

namespace putgd::record {
namespace {
// Frame times are 100-ns units in the clip; edits are microseconds.
long long toUs(long long time100ns) { return time100ns / 10; }

// Moves moov ahead of the media data in place (as qt-faststart does): the bytes
// from the first mdat up to moov move forward by moov's size, moov goes where
// mdat began, and every chunk offset grows by the same amount. The file keeps
// its length. A layout this does not recognise is left as written.
// The working clip's system audio cut to the retained interval and rebased to
// the export's zero: packets before the start lose their head, a hole becomes
// silence, and the track ends with the video.
class AudioCut {
public:
    Error open(const std::string& working, long long startUs, long long endUs) {
        if (auto e = reader_.open(working)) return e;
        if (!reader_.available()) return {};
        if (!audio::encoderRate(reader_.rate())) return {"recording_export_failed", "The working clip's audio rate is not one the AAC encoder takes", {{"rate", reader_.rate()}}};
        startUs_ = startUs;
        timeline_ = audio::Timeline(reader_.rate());
        endFrame_ = audio::Timeline::frameAt((endUs - startUs) * 10, reader_.rate());
        return reader_.seek(startUs);
    }
    bool available() const { return reader_.available(); }
    int rate() const { return reader_.rate(); }
    // Writes the track up to `us` on the export timeline (the end once `us` reaches it).
    Error writeUntil(media::Mp4Writer& writer, long long us) {
        const long long until = std::min(endFrame_, audio::Timeline::frameAt(us * 10, timeline_.rate()));
        while (!ended_ && timeline_.written() < until) {
            long long time = 0;
            Error error;
            if (!reader_.next(packet_, time, error)) { if (error) return error; ended_ = true; break; }
            audio::toStereo16(packet_.data(), packet_.size() / size_t(reader_.channels()), reader_.channels(), stereo_);
            if (auto e = write(writer, timeline_.place(audio::Timeline::frameAt(time - startUs_ * 10, timeline_.rate()), stereo_))) return e;
        }
        if (ended_ && until == endFrame_) return write(writer, timeline_.fillTo(endFrame_));
        return {};
    }
private:
    Error write(media::Mp4Writer& writer, audio::Timeline::Piece piece) {
        // Nothing past the end of the video.
        const long long room = std::max(0LL, endFrame_ - piece.start);
        if ((long long)piece.samples.size() / 2 > room) piece.samples.resize(size_t(room) * 2);
        if (piece.samples.empty()) return {};
        return writer.writeAudio(piece.samples.data(), piece.samples.size() / 2, piece.start * 10000000 / timeline_.rate());
    }
    media::AudioReader reader_;
    audio::Timeline timeline_{48000};
    long long startUs_ = 0, endFrame_ = 0;
    bool ended_ = false;
    std::vector<int16_t> packet_, stereo_;
};

Error faststart(const std::string& path) {
    HANDLE file = CreateFileW(paths::wide(path).c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE) return {"recording_export_failed", "Cannot reopen the export", {}};
    struct Closer { HANDLE h; ~Closer() { CloseHandle(h); } } closer{file};
    LARGE_INTEGER length{};
    GetFileSizeEx(file, &length);
    auto readAt = [file](uint64_t at, size_t n) {
        std::string bytes(n, '\0');
        LARGE_INTEGER position; position.QuadPart = LONGLONG(at);
        DWORD got = 0;
        if (!SetFilePointerEx(file, position, nullptr, FILE_BEGIN) || !ReadFile(file, bytes.data(), DWORD(n), &got, nullptr)) return std::string();
        bytes.resize(got);
        return bytes;
    };
    auto writeAt = [file](uint64_t at, const std::string& bytes) {
        LARGE_INTEGER position; position.QuadPart = LONGLONG(at);
        DWORD put = 0;
        return SetFilePointerEx(file, position, nullptr, FILE_BEGIN) && WriteFile(file, bytes.data(), DWORD(bytes.size()), &put, nullptr) && put == bytes.size();
    };
    const auto boxes = mp4::topLevel(uint64_t(length.QuadPart), readAt);
    size_t mdat = boxes.size(), moov = boxes.size();
    for (size_t i = 0; i < boxes.size(); ++i) { if (boxes[i].type == "mdat" && mdat == boxes.size()) mdat = i; if (boxes[i].type == "moov") moov = i; }
    if (mdat == boxes.size() || moov == boxes.size() || moov < mdat || moov + 1 != boxes.size() || boxes[moov].size > 64ull * 1024 * 1024) return {};
    std::string index = readAt(boxes[moov].offset, size_t(boxes[moov].size));
    if (index.size() != boxes[moov].size || !mp4::shiftChunkOffsets(index, boxes[moov].size)) return {};
    const uint64_t shift = boxes[moov].size, begin = boxes[mdat].offset;
    constexpr uint64_t chunk = 1 << 20;
    for (uint64_t end = boxes[moov].offset; end > begin;) {
        const uint64_t n = std::min(chunk, end - begin);
        const auto bytes = readAt(end - n, size_t(n));
        if (bytes.size() != n || !writeAt(end - n + shift, bytes)) return {"recording_export_failed", "Cannot reorder the export", {}};
        end -= n;
    }
    if (!writeAt(begin, index) || !FlushFileBuffers(file)) return {"recording_export_failed", "Cannot reorder the export", {}};
    return {};
}
}

// GIF: frames at min(fps, 15) on the kept part's timeline, each tick showing the
// frame at or after it (as the Linux export's fps filter does), delays from the
// rounded tick times so they never drift.
Error exportGif(const ExportRequest& r, ExportResult& result, const std::atomic_bool& cancel, const std::function<void(double)>& progress) {
    const int fps = std::max(1, r.config.fps), animationFps = std::min(fps, 15);
    const auto [start, end] = recording::exportWindow(r.edit, r.durationUs, fps);
    if (end <= start) return {"invalid_request", "The retained interval is empty", {}};
    const auto size = recording::outputSize(r.canvas, r.config.maxEdge);
    const long long retained = end - start;
    const double step = 1000000.0 / animationFps;
    const long long ticks = std::max(1LL, (long long)std::ceil(double(retained) / step - 1e-6));
    auto centiseconds = [&](long long tick) { return std::llround(std::min(double(retained), double(tick) * step) / 10000.0); };
    media::ClipReader reader;
    if (auto e = reader.open(r.working, r.canvas.w, r.canvas.h)) return e;
    if (auto e = reader.seek(start)) return e;
    GifWriter writer;
    if (auto e = writer.open(r.output, size.w, size.h)) return e;
    std::vector<uint8_t> frame, scaled(size_t(size.w) * size.h * 4);
    long long time = 0, tick = 0;
    const long long half = 1000000 / fps / 2;
    bool have = false;
    Error error;
    auto emit = [&]() -> Error {
        const auto delay = int(centiseconds(tick + 1) - centiseconds(tick));
        if (auto e = writer.write(scaled.data(), size_t(size.w) * 4, delay)) return e;
        ++tick;
        if (progress) progress(double(tick) / double(ticks));
        return {};
    };
    while (tick < ticks && reader.next(frame, time, error)) {
        if (cancel) return {"cancelled", "The export was cancelled", {}};
        const long long us = time / 10;
        if (us + half < start) continue;
        if (us >= end) break;
        const recording::Frame source{frame.data(), r.canvas.w, r.canvas.h, size_t(r.canvas.w) * 4};
        recording::applyMasks(source, r.edit, us);
        recording::resample(source, {scaled.data(), size.w, size.h, size_t(size.w) * 4});
        have = true;
        while (tick < ticks && double(tick) * step <= double(us - start) + double(half))
            if (auto e = emit()) return e;
    }
    if (error) return error;
    if (!have) return {"recording_export_failed", "No frame lies in the retained interval", {}};
    // The kept part's last ticks hold its last frame.
    while (tick < ticks) if (auto e = emit()) return e;
    if (auto e = writer.finish()) return e;
    result.sha256 = files::sha256(r.output, &result.bytes);
    if (result.sha256.empty() || result.bytes < 1) return {"recording_export_failed", "The export produced no file", {}};
    result.frames = int(ticks);
    result.durationUs = retained;
    result.size = size;
    result.encoder = "WIC GIF";
    return {};
}

Error exportClip(const ExportRequest& r, ExportResult& result, const std::atomic_bool& cancel, const std::function<void(double)>& progress) {
    if (r.config.format == "gif") return exportGif(r, result, cancel, progress);
    if (r.config.format != "mp4" || (r.config.codec != "h264" && r.config.codec != "h265")) return {"recording_unsupported", "This build exports MP4 (H.264 or H.265) and GIF", {{"format", r.config.format}}};
    const int fps = std::max(1, r.config.fps);
    const long long period = 10000000LL / fps;
    const auto [start, end] = recording::exportWindow(r.edit, r.durationUs, fps);
    if (end <= start) return {"invalid_request", "The retained interval is empty", {}};
    const auto size = recording::outputSize(r.canvas, r.config.maxEdge);
    media::ClipReader reader;
    if (auto e = reader.open(r.working, r.canvas.w, r.canvas.h)) return e;
    if (auto e = reader.seek(start)) return e;
    AudioCut audio;
    if (r.config.audioSystem) if (auto e = audio.open(r.working, start, end)) return e;
    media::Mp4Writer writer;
    if (auto e = writer.open(r.output, {size.w, size.h, fps, recording::liveBitrate(size.w, size.h, fps), r.config.codec == "h265", audio.available() ? audio.rate() : 0})) return e;
    std::vector<uint8_t> frame, scaled(size_t(size.w) * size.h * 4);
    long long time = 0;
    Error error;
    int frames = 0;
    const bool scale = size.w != r.canvas.w || size.h != r.canvas.h;
    while (reader.next(frame, time, error)) {
        if (cancel) { writer.abandon(); return {"cancelled", "The export was cancelled", {}}; }
        const long long us = toUs(time);
        // Frames sit on the capture's frame grid; half a frame of slack absorbs rounding.
        if (us + 1000000 / fps / 2 < start) continue;
        if (us >= end) break;
        const recording::Frame source{frame.data(), r.canvas.w, r.canvas.h, size_t(r.canvas.w) * 4};
        recording::applyMasks(source, r.edit, us);
        const uint8_t* bits = frame.data();
        if (scale) { recording::resample(source, {scaled.data(), size.w, size.h, size_t(size.w) * 4}); bits = scaled.data(); }
        const long long index = std::llround(double(time - start * 10) / double(period));
        if (auto e = writer.write(bits, size_t(size.w) * 4, std::max(0LL, index) * period, period)) { writer.abandon(); return {"recording_export_failed", e.message, e.details}; }
        ++frames;
        // Audio interleaved with the video, so the muxer holds little of either.
        if (audio.available()) if (auto e = audio.writeUntil(writer, (std::max(0LL, index) + 1) * period / 10)) { writer.abandon(); return {"recording_export_failed", e.message, e.details}; }
        if (progress) progress(double(us - start) / double(end - start));
    }
    if (error) { writer.abandon(); return error; }
    if (frames == 0) { writer.abandon(); return {"recording_export_failed", "No frame lies in the retained interval", {}}; }
    if (audio.available()) if (auto e = audio.writeUntil(writer, end - start)) { writer.abandon(); return {"recording_export_failed", e.message, e.details}; }
    result.encoder = writer.encoder();
    result.audio = audio.available();
    if (auto e = writer.finish()) return {"recording_export_failed", e.message, e.details};
    if (auto e = faststart(r.output)) return e;
    result.sha256 = files::sha256(r.output, &result.bytes);
    if (result.sha256.empty() || result.bytes < 1) return {"recording_export_failed", "The export produced no file", {}};
    result.frames = frames;
    result.durationUs = (long long)frames * period / 10;
    result.size = size;
    if (progress) progress(1.0);
    return {};
}

Error previewFrame(const std::string& working, RecordingSize canvas, const RecordingEdit& edit, long long timeUs, int maxEdge, PreviewResult& result) {
    media::ClipReader reader;
    if (auto e = reader.open(working, canvas.w, canvas.h)) return e;
    if (auto e = reader.seek(timeUs)) return e;
    std::vector<uint8_t> frame, last;
    long long time = 0, lastTime = -1;
    Error error;
    bool found = false;
    // The frame at or after the requested time; the clip's last frame past its end.
    while (reader.next(frame, time, error)) {
        if (time + 5 >= timeUs * 10) { found = true; break; }
        last.swap(frame); lastTime = time;
    }
    if (error) return error;
    if (!found) { if (lastTime < 0) return {"recording_export_failed", "The clip has no frame at this time", {}}; frame.swap(last); time = lastTime; }
    const recording::Frame source{frame.data(), canvas.w, canvas.h, size_t(canvas.w) * 4};
    recording::applyMasks(source, edit, toUs(time));
    const auto size = recording::outputSize(canvas, maxEdge);
    std::vector<uint8_t> scaled(size_t(size.w) * size.h * 4);
    recording::resample(source, {scaled.data(), size.w, size.h, size_t(size.w) * 4});
    if (auto e = wic::encodePixels(scaled.data(), size.w, size.h, size_t(size.w) * 4, false, result.png)) return {"recording_export_failed", "Cannot encode the preview", {{"cause", e.code}}};
    result.frameTimeUs = toUs(time); result.width = size.w; result.height = size.h;
    return {};
}
}
