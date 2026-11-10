#pragma once
#include "core/Error.h"
#include <QJsonObject>
#include <QString>
#include <functional>

namespace putgd {
namespace scroll {
// The production helper's reply deadlines (ScrollHelperBackend). A session
// client waits past them (LocalServer::replyDeadlineMs), so the resident's own
// answer always reaches the caller.
inline constexpr int helperStartupMs = 10000, helperCommandMs = 15000, helperStopMs = 2 * helperCommandMs;
// The whole capture, selection excluded: the control loop's own limit plus slack.
inline constexpr int captureLimitMs = 180000;
}
// The resident's view of a scroll-capture backend (#1376). The production
// backend supervises a putgd-record helper running a scroll session; tests
// inject a fake. Replies and events may arrive on any thread;
// ScrollCaptureService marshals them. Capability inspection must not start
// capture, show a window or inject input.
class ScrollBackend {
public:
    using Reply = std::function<void(Error, QJsonObject)>;
    using Event = std::function<void(QJsonObject)>;
    virtual ~ScrollBackend() = default;
    // {available, reason?, injector: "xtest"|"mutter-remote-desktop"|"kwin-fake-input"|"wlr-virtual-pointer",
    //  frames: "ximagesrc"|"mutter-screencast"|"kde-screencast"|"image-copy", maxHeight}
    // reason is one of recording_helper_missing, injection_unavailable,
    // frame_source_unavailable, region_unavailable.
    virtual QJsonObject capabilities() const = 0;
    // Makes capabilities() current (a lazy helper probe); never starts capture.
    virtual void refreshCapabilities(std::function<void()> done) { done(); }
    // Unsolicited events, each {"id", "event", ...}:
    //   selected  {rect: [x, y, w, h]}                    the user finished the drag
    //   progress  {state: "scrollingToTop"|"capturing", height, steps, pixelsPerNotch}
    //   finished  {path, bytes, sha256, width, height,
    //              endReason: "end_of_content"|"height_cap"|"stopped"|"lost_track"|"time_limit",
    //              truncated, appliedMaxHeight,
    //              stats: {steps, pixelsPerNotch, lowConfidenceSteps, elapsedMs}}
    //   cancelled {}                                      Cancel on the bar, or Esc in the selector
    //   failed    {error: {code, message, details}}
    // After finished, cancelled or failed the backend injects nothing more for the id.
    virtual void setEventSink(Event) = 0;
    // params: {rect?: [x, y, w, h] logical desktop pixels, maxHeight, fromTop,
    //          imagePixels, imageBytes (resident snapshot budgets; imagePixels also
    //          includes the editor admission limit on that route),
    //          autoStart, selection: "helper"|"shell"}.
    // With no rect and selection "helper" the helper opens its region selector;
    // with selection "shell" (GNOME) the resident selects through RecordingShell
    // and sends `select`. autoStart begins capturing as soon as a rect is known.
    // The PNG is written under stagingDirectory (0700, owned by the resident).
    // Replies {state: "selecting"|"ready"|"capturing"} once the helper is up.
    virtual void prepare(const QString& id, const QJsonObject& params, const QString& stagingDirectory, Reply) = 0;
    // select {rect} | start | stop | cancel. Reply only once the helper reached
    // the state. stop keeps the image: its reply carries the same object as the
    // finished event (which is still sent). cancel deletes the staging files.
    virtual void control(const QString& id, const QString& command, const QJsonObject& params, Reply) = 0;
    // Releases helper and staging ownership for the id.
    virtual void discard(const QString& id) = 0;
    // The process that owns the session's windows (the bar), or 0.
    virtual qint64 processId(const QString&) const { return 0; }
};
class UnavailableScrollBackend final : public ScrollBackend {
public:
    explicit UnavailableScrollBackend(QString reason = "recording_helper_missing") : reason_(std::move(reason)) {}
    QJsonObject capabilities() const override { return {{"available", false}, {"reason", reason_}}; }
    void setEventSink(Event) override {}
    void prepare(const QString&, const QJsonObject&, const QString&, Reply reply) override { reply(unavailable(), {}); }
    void control(const QString&, const QString&, const QJsonObject&, Reply reply) override { reply(unavailable(), {}); }
    void discard(const QString&) override {}
private:
    Error unavailable() const { return {"scroll_unavailable", "Scrolling capture is not available on this installation", {{"reason", reason_}}}; }
    QString reason_;
};
}
