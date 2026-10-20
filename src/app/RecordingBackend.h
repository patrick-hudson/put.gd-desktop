#pragma once
#include "core/RecordingModel.h"
#include <functional>
#include <memory>

namespace putgd {
namespace recording {
// The production helper's reply deadlines (RecordingHelperBackend). A session
// client waits past them (LocalServer::replyDeadlineMs), so the resident's own
// answer, recording_helper_timeout included, always reaches the caller.
inline constexpr int helperStartupMs = 10000, helperCommandMs = 15000, helperStopMs = 2 * helperCommandMs, helperPrepareMs = 600000, helperExportMs = 600000;
}
// The resident's view of a recording backend. The production backend
// supervises the transient putgd-record helper; tests inject a fake. Replies
// and events may arrive on any thread; RecordingService marshals them.
// Capability/status inspection must not start capture or permission prompts.
class RecordingBackend {
public:
    using Reply = std::function<void(Error, QJsonObject)>;
    using Event = std::function<void(QJsonObject)>;
    virtual ~RecordingBackend() = default;
    // {available, reason?, targets, formats, codecs, audio:{system,microphone}, overlayExclusion}
    virtual QJsonObject capabilities() const = 0;
    // Makes capabilities() current (a lazy helper probe); never starts capture.
    virtual void refreshCapabilities(std::function<void()> done) { done(); }
    // Unsolicited events: {"id", "event": "stopped"|"failed"|"progress", ...}.
    virtual void setEventSink(Event) = 0;
    // Shows ready bounds/controls and asks for any consent. Result: {canvas:[w,h]}.
    virtual void prepare(const QString& id, const RecordingTarget&, const RecordingConfig&, const QString& stagingDirectory, Reply) = 0;
    // start | pause | resume | stop | cancel | raise | overlay. Reply only once the
    // helper reached the state. stop replies {durationUs, canvas:[w,h]}.
    virtual void control(const QString& id, const QString& command, const QJsonObject& params, Reply) = 0;
    // One frame at clip time with trim/masks applied: {png: base64, timeUs}.
    virtual void preview(const QString& id, const RecordingEdit&, const RecordingConfig&, qint64 timeUs, int maxEdge, Reply) = 0;
    // Writes the final file to outputPath: {bytes, durationUs}.
    virtual void exportClip(const QString& id, const RecordingEdit&, const RecordingConfig&, const QString& outputPath, Reply) = 0;
    // Pushes the authoritative edit (and config) to the helper's review window.
    // limits: {animationSeconds, animationEdge} so the review can warn before an export is refused.
    virtual void setEdit(const QString&, const RecordingEdit&, const RecordingConfig&, const QJsonObject& limits) { Q_UNUSED(limits); }
    // Releases helper/staging ownership for the id without an upload.
    virtual void discard(const QString& id) = 0;
    // The process that owns the session's windows (the control bar), or 0.
    virtual qint64 processId(const QString&) const { return 0; }
};
// Presentation the helper cannot do itself where the compositor keeps clients
// from placing their own surfaces. On GNOME the put.gd Shell extension runs the
// region selector, draws the ready ring and outside dimming as Shell actors and
// moves the helper's control bar out of the recorded area. Unset elsewhere,
// where the helper selects and places everything (X11, KDE layer-shell).
class RecordingShell {
public:
    using Reply = std::function<void(Error, QJsonObject)>;
    virtual ~RecordingShell() = default;
    // True when a recording region can be chosen through this shell now.
    virtual bool selectsRegions() const = 0;
    // When it cannot, why, if the shell knows (an extension that is off, or an
    // older one still loaded after an upgrade); empty leaves the region to the helper.
    virtual Error regionError() const { return {}; }
    // Replies {rect: [x, y, w, h]} in logical desktop pixels, or cancelled.
    virtual void selectRegion(Reply) = 0;
    // Closes an open selector; its pending selectRegion reply reports cancelled.
    virtual void cancelSelection() = 0;
    // {rect, kind, state: ready|recording|paused, pid}: ring, dimming (recording
    // and paused) and bar placement. Replies {bar: {placed, outside, rect}}.
    virtual void showFrame(const QJsonObject&, Reply) = 0;
    virtual void hideFrame() = 0;
};
class UnavailableRecordingBackend final : public RecordingBackend {
public:
    explicit UnavailableRecordingBackend(QString reason = "recording_helper_missing") : reason_(std::move(reason)) {}
    QJsonObject capabilities() const override;
    void setEventSink(Event) override {}
    void prepare(const QString&, const RecordingTarget&, const RecordingConfig&, const QString&, Reply reply) override { reply(unavailable(), {}); }
    void control(const QString&, const QString&, const QJsonObject&, Reply reply) override { reply(unavailable(), {}); }
    void preview(const QString&, const RecordingEdit&, const RecordingConfig&, qint64, int, Reply reply) override { reply(unavailable(), {}); }
    void exportClip(const QString&, const RecordingEdit&, const RecordingConfig&, const QString&, Reply reply) override { reply(unavailable(), {}); }
    void discard(const QString&) override {}
private:
    Error unavailable() const { return {"recording_unavailable", "Screen recording is not available on this installation", {{"reason", reason_}}}; }
    QString reason_;
};
}
