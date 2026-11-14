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
    // frame_source_unavailable, region_unavailable. The helper backend adds
    // message? (why, for details) and selection: "helper"|"shell"|"none" (who
    // selects the region here), and before its first probe has answered the
    // reasons scroll_not_probed, scroll_probe_pending and scroll_probe_failed.
    // These probe states are transient: refresh before deciding availability
    // and do not present them as a permanent desktop failure.
    virtual QJsonObject capabilities() const = 0;
    // Makes capabilities() current (a lazy helper probe); never starts capture.
    virtual void refreshCapabilities(std::function<void()> done) { done(); }
    // Unsolicited events, each {"id", "event", ...}:
    //   selected  {rect: [x, y, w, h]}                    the user finished the drag
    //   progress  {state: "scrollingToTop"|"capturing"|"paused", paused, pausedBy?: "pointer"|"request",
    //              height, steps, pixelsPerNotch}   paused: nothing is injected until resume
    //                                               (#1514); "pointer": the user moved the mouse
    //   finished  {path, bytes, sha256, width, height,
    //              endReason: "end_of_content"|"height_cap"|"stopped"|"lost_track"|"time_limit"|"top_not_reached",
    //              truncated, appliedMaxHeight, topReached?: false,
    //              stats: {steps, pixelsPerNotch, lowConfidenceSteps, elapsedMs}}
    //             end_of_content is the whole scroll area, top to bottom. With
    //             fromTop on, a capture whose scroll-to-top step could not
    //             prove the top carries topReached: false, whatever ended it;
    //             one that then ran to the end of the content ends as
    //             top_not_reached (truncated), never as end_of_content.
    //   cancelled {}                                      Cancel on the bar, Esc in the selector, or a
    //                                                     stop before the first frame was stitched
    //   failed    {error: {code, message, details}}
    // After finished, cancelled or failed the backend injects nothing more for the id.
    virtual void setEventSink(Event) = 0;
    // params: {rect?: [x, y, w, h] logical desktop pixels (X11: root pixels, as
    //          for a region recording), maxHeight, maxPixels?, fromTop,
    //          autoStart, selection: "helper"|"shell"}.
    // maxPixels bounds width x height of the image (the smallest of
    // limits.imagePixels, limits.imageBytes / 4 and, for the editor route,
    // EditorJob::admissionPixels): the helper knows the width only once frames
    // arrive, and reports the height it applied as appliedMaxHeight.
    // With no rect and selection "helper" the helper opens its region selector;
    // with selection "shell" (GNOME) the resident selects through RecordingShell
    // and sends `select`. autoStart begins capturing as soon as a rect is known.
    // The PNG is written under stagingDirectory (0700, owned by the resident).
    // Replies {state: "selecting"|"ready"|"capturing"} once the helper is up.
    virtual void prepare(const QString& id, const QJsonObject& params, const QString& stagingDirectory, Reply) = 0;
    // select {rect} | start | pause | resume | stop | cancel | overlay {visible,
    // rect?}. Reply only once the helper reached the state. pause and resume
    // answer {state: "capturing", paused, pausedBy?}; resume of a capture that
    // is not paused is scroll_state. stop keeps the image: its reply carries the
    // same object as the finished event (which is still sent). cancel deletes
    // the staging files. overlay hides or shows the helper's bar. On GNOME the
    // Shell has to move the bar out of the rectangle, which needs it on screen
    // first: prepare with autoStart false, `select`, place the bar through
    // RecordingShell::showFrame (and send overlay {visible: false} when it
    // could not go outside, else overlay {visible: true, rect} with where the
    // Shell put it, so the pointer rests on it between wheel steps), then `start`. With autoStart true there the bar
    // stays hidden for the whole capture.
    virtual void control(const QString& id, const QString& command, const QJsonObject& params, Reply) = 0;
    // A finished PNG survives helper exit. The resident validates and admits it,
    // routes it, then removes it. Releases helper ownership for the id.
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
