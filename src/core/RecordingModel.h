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
    QJsonObject toJson() const;
};
struct RecordingAudio {
    bool system = false;      // the computer's playback (default sink monitor)
    bool microphone = false;  // reserved; not offered by this delivery
};
struct RecordingConfig {
    QString format = "mp4";  // mp4 | webm | gif | webp
    QString codec = "h264";  // mp4: h264 | h265; webm: vp9; gif/webp: empty
    int fps = 30;            // 1..60 capture rate; animations may export at a lower rate
    RecordingAudio audio;
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
namespace recording {
constexpr int maximumMasks = 64;
constexpr int maximumEditCommands = 100;
constexpr qint64 maximumClipUs = 24LL * 3600 * 1000000;
QStringList states();
QStringList targetKinds();
QStringList formats();
QStringList codecs(const QString& format);
bool terminal(const QString& state);
// Which session states accept which command; an invalid pair is recording_state.
Error checkTransition(const QString& state, const QString& command);
Error parseTarget(const QJsonValue&, RecordingTarget&);
// Accepts {format, codec?, fps?, audio?: {system?, microphone?}, maxEdge?}.
Error parseConfig(const QJsonObject&, RecordingConfig&);
// Applies setTrim/resetTrim/addMask/updateMask/removeMask atomically; on error
// the edit is unchanged. Success advances the revision once.
Error applyEditCommands(RecordingEdit&, const QJsonArray& commands, qint64 durationUs, const QSize& canvas, int fps);
// Nearest frame boundary at the given rate; the export side rounds identically.
qint64 alignToFrame(qint64 us, int fps);
// Animation exports must fit the configured duration and dimension budget.
Error checkExportBudget(const RecordingConfig&, const RecordingEdit&, qint64 durationUs, const QSize& canvas, const RecordingLimits&);
// GIF/WebP outputs larger than the animation edge limit get maxEdge set to it
// (the user chose the format, not the pixel count). True when changed.
bool clampAnimationSize(RecordingConfig&, const QSize& canvas, const RecordingLimits&);
QSize outputSize(const QSize& canvas, const std::optional<int>& maxEdge);
}
}
