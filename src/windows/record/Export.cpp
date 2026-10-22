#include "record/Export.h"
#include "capture/Png.h"
#include "common/Files.h"
#include "core/RecordingFrames.h"
#include "record/Media.h"
#include <cmath>
#include <vector>

namespace putgd::record {
namespace {
// Frame times are 100-ns units in the clip; edits are microseconds.
long long toUs(long long time100ns) { return time100ns / 10; }
}

Error exportClip(const ExportRequest& r, ExportResult& result, const std::atomic_bool& cancel, const std::function<void(double)>& progress) {
    if (r.config.format != "mp4" || r.config.codec != "h264") return {"recording_unsupported", "This build exports MP4 (H.264) only", {{"format", r.config.format}}};
    const int fps = std::max(1, r.config.fps);
    const long long period = 10000000LL / fps;
    const auto [start, end] = recording::exportWindow(r.edit, r.durationUs, fps);
    if (end <= start) return {"invalid_request", "The retained interval is empty", {}};
    const auto size = recording::outputSize(r.canvas, r.config.maxEdge);
    media::ClipReader reader;
    if (auto e = reader.open(r.working, r.canvas.w, r.canvas.h)) return e;
    if (auto e = reader.seek(start)) return e;
    media::Mp4Writer writer;
    media::VideoFormat format{size.w, size.h, fps, recording::liveBitrate(size.w, size.h, fps), true};
    // The index first when the sink can write it that way; a plain MP4 otherwise.
    if (writer.open(r.output, format)) { format.faststart = false; if (auto e = writer.open(r.output, format)) return e; }
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
        if (progress) progress(double(us - start) / double(end - start));
    }
    if (error) { writer.abandon(); return error; }
    if (frames == 0) { writer.abandon(); return {"recording_export_failed", "No frame lies in the retained interval", {}}; }
    result.encoder = writer.encoder();
    if (auto e = writer.finish()) return {"recording_export_failed", e.message, e.details};
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
