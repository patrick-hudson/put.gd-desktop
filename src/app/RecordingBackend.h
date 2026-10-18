#pragma once
#include "core/RecordingModel.h"
#include <functional>
#include <memory>

namespace putgd {
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
    // Releases helper/staging ownership for the id without an upload.
    virtual void discard(const QString& id) = 0;
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
