#pragma once
// The backend-neutral screen recording contract (docs/recording.md), ported from
// the Linux src/core/RecordingModel with the same JSON shapes, so putgd-cli and
// the MCP helper drive both residents alike. Every time value is an integer count
// of microseconds on the recorded-clip timeline (pauses removed); intervals are
// half-open [start, end). Geometry is in output-canvas pixels, fixed at start.
// No Windows headers and no media code: this is tested on Linux.
#include "common/Error.h"
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace putgd {
struct RecordingRect {
    long long x = 0, y = 0, w = 0, h = 0;
    bool empty() const { return w <= 0 || h <= 0; }
    bool operator==(const RecordingRect&) const = default;
    json::Array toJson() const { return {x, y, w, h}; }
};
struct RecordingTarget {
    std::string kind;            // monitor | desktop | region | window
    std::string identity;        // monitor index/name or window handle; empty for desktop/region
    std::optional<RecordingRect> rect; // region only: virtual-screen physical pixels
    json::Object toJson() const;
};
struct RecordingConfig {
    std::string format = "mp4"; // mp4 | webm | gif | webp
    std::string codec = "h264"; // mp4: h264 | h265; webm: vp9; gif/webp: empty
    int fps = 30;               // 1..60 capture rate
    bool audioSystem = false;   // the computer's playback
    bool audioMicrophone = false; // reserved; refused
    std::optional<int> maxEdge;
    long long budgetBytes = 0;  // set by the resident
    std::string mime() const;
    std::string extension() const { return format; }
    bool animation() const { return format == "gif" || format == "webp"; }
    json::Object toJson() const;
};
struct RecordingMask {
    std::string id, kind = "blur"; // blur | solid
    RecordingRect rect;
    long long start = 0, end = 0;
    int strength = 16;
    json::Object toJson() const;
};
struct RecordingEdit {
    long long trimStart = 0;
    std::optional<long long> trimEnd; // absent: the clip end
    std::vector<RecordingMask> masks;
    unsigned long long revision = 1;
    long long exportEnd(long long duration) const { return trimEnd ? *trimEnd : duration; }
    long long exportDuration(long long duration) const { return exportEnd(duration) - trimStart; }
    // True when an export is the working clip as recorded (no trim, no mask).
    bool unedited(long long duration) const { return trimStart == 0 && (!trimEnd || *trimEnd >= duration) && masks.empty(); }
    static std::optional<std::pair<long long, long long>> exportInterval(const RecordingMask&, long long trimStart, long long trimEnd);
    json::Object toJson() const;
};
struct RecordingLimits {
    long long animationMaxUs = 60000000;
    int animationMaxEdge = 1280;
};
struct RecordingSize { int w = 0, h = 0; bool empty() const { return w <= 0 || h <= 0; } };

namespace recording {
inline constexpr int maximumMasks = 64;
inline constexpr int maximumEditCommands = 100;
inline constexpr long long maximumClipUs = 24LL * 3600 * 1000000;
const std::vector<std::string>& states();
const std::vector<std::string>& targetKinds();
const std::vector<std::string>& formats();
std::vector<std::string> codecs(const std::string& format);
bool terminal(const std::string& state);
// Which session states accept which command; an invalid pair is recording_state.
Error checkTransition(const std::string& state, const std::string& command);
Error parseTarget(const json::Value&, RecordingTarget&);
// Accepts {format, codec?, fps?, audio?: {system?, microphone?}, maxEdge?}.
Error parseConfig(const json::Object&, RecordingConfig&);
// setTrim/resetTrim/addMask/updateMask/removeMask applied atomically; success
// advances the revision once. `newId` names added masks.
Error applyEditCommands(RecordingEdit&, const json::Array& commands, long long durationUs, RecordingSize canvas, int fps, const std::function<std::string()>& newId);
long long alignToFrame(long long us, int fps);
// Reads RecordingEdit::toJson's form back (the resident sends its edit to the
// helper with each export and preview); false on anything malformed.
bool parseEdit(const json::Object&, RecordingEdit&);
Error checkExportBudget(const RecordingConfig&, const RecordingEdit&, long long durationUs, RecordingSize canvas, const RecordingLimits&);
bool clampAnimationSize(RecordingConfig&, RecordingSize canvas, const RecordingLimits&);
RecordingSize outputSize(RecordingSize canvas, const std::optional<int>& maxEdge);
// Encoders want even dimensions: a capture area shrinks to even values rather
// than growing into unselected pixels.
RecordingRect evenArea(RecordingRect);
// H.264 bitrate for a live capture: about 0.1 bit per pixel per frame, 1.5 to 20 Mbit/s.
long long liveBitrate(int width, int height, int fps);
// Menu keys of the bar and the review: a format, or format/codec for formats with
// codecs; and their labels.
std::string formatKey(const RecordingConfig&);
std::string formatLabel(const std::string& key);
}
}
