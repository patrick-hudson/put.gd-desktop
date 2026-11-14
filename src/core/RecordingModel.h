#pragma once
#include "Error.h"
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QRect>
#include <QSize>
#include <QStringList>
#include <optional>
#include <utility>

// Backend-neutral recording contract shared by the resident, the recording
// helper, the CLI and the MCP helper. Every time value is an integer count of
// microseconds on the recorded-clip timeline: pauses are already removed, and
// intervals are half-open [start, end). Geometry is in output-canvas pixels,
// fixed when the recording starts. No media library is involved here.
namespace putgd {
struct RecordingTarget {
    QString kind;      // monitor | desktop | region | window
    QString identity;  // monitor output name or window identity; empty for desktop/region
    QRect rect;        // region only: logical desktop rectangle
    // A window on KDE/GNOME Wayland as the resident resolved it before the helper
    // starts: {identity, title, appId, rect: logical frame [x, y, w, h]}. Never
    // parsed from a request; the helper receives it beside the target.
    QJsonObject window;
    QJsonObject toJson() const;
};
struct RecordingAudio {
    bool system = false;      // the computer's playback (default sink monitor)
    bool microphone = false;  // an input device, mixed with the system audio into the one track (#1364)
    // The input to open: a PulseAudio/PipeWire source name, or empty for the
    // system default input. Never an output's monitor.
    QString microphoneDevice;
    bool any() const { return system || microphone; }
    QJsonObject toJson() const;
};
struct RecordingConfig {
    QString format = "mp4";  // mp4 | webm | gif | webp
    QString codec = "h264";  // mp4: h264 | h265; webm: vp9; gif/webp: empty
    int fps = 30;            // 1..60 capture rate; animations may export at a lower rate
    bool adaptiveFps = true; // the helper may lower the capture rate when the machine can't keep up (#984)
    RecordingAudio audio;
    // A ring at the pointer on every click, rendered into the clip (#1364).
    bool clickHighlights = false;
    std::optional<int> maxEdge;  // explicit user downscale of the output's long edge
    qint64 budgetBytes = 0;      // set by the resident: stop before the admitted file limit
    QString mime() const;
    QString extension() const;
    bool animation() const { return format == "gif" || format == "webp"; }
    bool silent() const { return animation(); }
    QJsonObject toJson() const;
};
struct RecordingMask {
    QString id;
    QString kind = "blur";  // blur | solid
    QRect rect;             // canvas pixels
    qint64 start = 0, end = 0;
    int strength = 16;      // blur radius; ignored for solid
    QJsonObject toJson() const;
};
struct RecordingEdit {
    qint64 trimStart = 0;
    std::optional<qint64> trimEnd;  // absent: the clip end
    QList<RecordingMask> masks;
    quint64 revision = 1;  // advances on every accepted mutation
    qint64 exportEnd(qint64 duration) const { return trimEnd ? *trimEnd : duration; }
    qint64 exportDuration(qint64 duration) const { return exportEnd(duration) - trimStart; }
    // A mask's interval in output time after trimming; absent when it lies
    // wholly outside the retained interval.
    static std::optional<std::pair<qint64, qint64>> exportInterval(const RecordingMask&, qint64 trimStart, qint64 trimEnd);
    QJsonObject toJson() const;
};
struct RecordingLimits {
    qint64 animationMaxUs = 60000000;  // retained duration allowed for GIF/WebP
    int animationMaxEdge = 1280;       // output long edge allowed for GIF/WebP
};
// The resident's session states (#1434). recording::stateName() is the string
// in session replies, the capabilities' `states` and the log.
enum class RecordingState { Preparing, Ready, Starting, Recording, Paused, Stopping, Reviewing, Exporting, Submitted, Cancelled, Failed };
// The helper's phase of its one session (putgd-record's RecordSession): Idle
// before prepare, Recorded once the clip is in review, Exporting while an export
// runs. Each phase stands for one resident state (recording::residentState).
enum class HelperPhase { Idle, Preparing, Ready, Starting, Recording, Paused, Stopping, Recorded, Exporting, Cancelled, Failed };
namespace recording {
constexpr int maximumMasks = 64;
constexpr int maximumEditCommands = 100;
constexpr qint64 maximumClipUs = 24LL * 3600 * 1000000;
QStringList states();
QString stateName(RecordingState);
QStringList targetKinds();
QStringList formats();
QStringList codecs(const QString& format);
bool terminal(RecordingState);
// Which steps a session takes (#1434): the from/to table that
// RecordingService::transition() checks. Failed and Cancelled are reached from
// every state that is not terminal, Submitted only from Exporting (Upload).
bool allowedStep(RecordingState from, RecordingState to);
// Which session states accept which command; an invalid pair is recording_state.
Error checkTransition(RecordingState, const QString& command);
QString phaseName(HelperPhase);
// The resident state a helper phase stands for; Idle has none.
std::optional<RecordingState> residentState(HelperPhase);
// Which commands the helper accepts in each phase: for every command the
// resident's table also names, exactly the phases whose resident state it
// allows. savedCopy, notice and click, which the resident sends on its own
// account, are accepted from Ready on. A refusal is recording_state.
Error checkHelperCommand(HelperPhase, const QString& command);
Error parseTarget(const QJsonValue&, RecordingTarget&);
// Accepts {format, codec?, fps?, adaptiveFps?, audio?: {system?, microphone?, microphoneDevice?}, clickHighlights?, maxEdge?}.
Error parseConfig(const QJsonObject&, RecordingConfig&);
// Applies setTrim/resetTrim/addMask/updateMask/removeMask atomically; on error
// the edit is unchanged. Success advances the revision once.
Error applyEditCommands(RecordingEdit&, const QJsonArray& commands, qint64 durationUs, const QSize& canvas, int fps);
// A source name for audio.microphoneDevice: a string of at most 256 characters without control characters (empty: the default input).
bool validMicrophoneDevice(const QJsonValue&);
// An output's monitor by its name; it is never opened as a microphone.
bool monitorSourceName(const QString& name);
// Animation exports must fit the configured duration and dimension budget.
Error checkExportBudget(const RecordingConfig&, const RecordingEdit&, qint64 durationUs, const QSize& canvas, const RecordingLimits&);
// GIF/WebP outputs larger than the animation edge limit get maxEdge set to it
// (the user chose the format, not the pixel count). True when changed.
bool clampAnimationSize(RecordingConfig&, const QSize& canvas, const RecordingLimits&);
QSize outputSize(const QSize& canvas, const std::optional<int>& maxEdge);
}
}
