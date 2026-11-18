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
// #1613: a stretch of source time left out of the output, kept in the edit so
// it stays visible and restorable in review. Half-open [start, end).
struct RecordingCut {
    QString id;
    qint64 start = 0, end = 0;
    QJsonObject toJson() const;  // {id, start, end}
    bool operator==(const RecordingCut&) const = default;
};
struct RecordingEdit {
    qint64 trimStart = 0;
    std::optional<qint64> trimEnd;  // absent: the clip end
    QList<RecordingMask> masks;
    // #1613: sorted by start, pairwise disjoint and never touching (addCut and
    // updateCut merge), inside [0, duration). Covers stay in source time; the
    // output drops whatever a cut or the trim leaves out.
    QList<RecordingCut> cuts;
    quint64 revision = 1;  // advances on every accepted mutation
    // #1613: whether the resident's history can step back or forward from this
    // revision. Set by applyEditCommands from its history; serialized so the
    // review window can enable Undo and Redo; not part of an undo snapshot's meaning.
    bool canUndo = false, canRedo = false;
    qint64 exportEnd(qint64 duration) const { return trimEnd ? *trimEnd : duration; }
    // The output's length: the trimmed interval minus every cut inside it
    // (recording::KeptSpans::outputDuration).
    qint64 exportDuration(qint64 duration) const;
    QJsonObject toJson() const;  // also {cuts: [...], canUndo, canRedo}
};
// #1613: the resident's undo history for one review session. Each entry is the
// whole edit before an accepted batch; never serialized, never sent to the helper.
struct RecordingEditHistory {
    QList<RecordingEdit> undo, redo;
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
constexpr int maximumCuts = 64;
constexpr int maximumUndoSteps = 200;  // the oldest step is dropped past this
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
// Applies setTrim/resetTrim/addMask/updateMask/removeMask/addCut/updateCut/
// removeCut atomically; on error the edit (and the history) is unchanged.
// Success advances the revision once.
// #1613 cuts: addCut {start, end} and updateCut {cutId, start, end} clamp to
// [0, duration], merge with every cut they overlap or touch (an addCut that
// merges takes a new id; updateCut keeps its id and absorbs the others) and
// refuse with recording_cut_empty when fewer than one frame (1/fps) of the
// trimmed interval would stay kept; setTrim refuses the same way. removeCut
// {cutId}; an unknown id is recording_cut_unknown. At most maximumCuts.
// #1613 history: with a history, every accepted batch pushes the edit it
// replaced onto history->undo (dropping the oldest past maximumUndoSteps) and
// clears history->redo. A batch that is exactly [{op: "undo"}] or
// [{op: "redo"}] steps the history instead: the edit becomes the snapshot,
// its revision still advances past the current one, and the step it left goes
// on the other stack; nothing to step is recording_history_empty. undo/redo
// mixed with other commands, or without a history, is invalid_request.
// canUndo/canRedo are set from the history after every success.
Error applyEditCommands(RecordingEdit&, const QJsonArray& commands, qint64 durationUs, const QSize& canvas, int fps, RecordingEditHistory* history = nullptr);
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
