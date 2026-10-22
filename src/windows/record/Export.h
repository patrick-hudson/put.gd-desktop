#pragma once
// Exports and previews of the working clip in the recording helper: decode with
// Media Foundation, apply the edit (the retained interval on the frame grid,
// masks in source time, a downscale to maxEdge) with the portable rules in
// core/RecordingFrames, and encode. Preview and export use the same decode and
// the same rules, so they agree on timing and placement.
#include "core/Recording.h"
#include <atomic>
#include <functional>
#include <string>

namespace putgd::record {
struct ExportRequest {
    std::string working, output; // the working clip; an empty file the resident created
    RecordingSize canvas;
    long long durationUs = 0;
    RecordingEdit edit;
    RecordingConfig config;
};
struct ExportResult {
    long long bytes = 0, durationUs = 0;
    int frames = 0;
    std::string sha256, encoder;
    RecordingSize size;
};
// Errors: recording_unsupported (a format this build cannot write),
// recording_export_failed, cancelled.
Error exportClip(const ExportRequest&, ExportResult&, const std::atomic_bool& cancel, const std::function<void(double)>& progress);

struct PreviewResult { std::string png; long long frameTimeUs = 0; int width = 0, height = 0; };
// The frame at or after `timeUs` with the masks active then, at most `maxEdge`
// on its long side, as a sanitized PNG.
Error previewFrame(const std::string& working, RecordingSize canvas, const RecordingEdit&, long long timeUs, int maxEdge, PreviewResult&);
}
