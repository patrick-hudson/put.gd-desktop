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
// A capture has no time limit of its own: one that nobody touches (no frame
// placed with movement, no command) for this long ends as Done, the image kept.
inline constexpr int idleLimitMs = 600000;
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
    //   progress  {state: "waiting"|"following"|"lost"|"scrollingToTop"|"autoScrolling"|"paused"|"resuming",
    //              driver: "user"|"putgd", paused, pausedBy?: "pointer"|"scrolled"|"lost"|"no_movement",
    //              height, placed, skipped, steps, pixelsPerNotch}
    //             driver: who turns the page now (#1515); paused: auto-scroll
    //             handed the wheel back by itself, pausedBy why (#1514).
    //   finished  {path, bytes, sha256, width, height,
    //              endReason: "end_of_content"|"top_not_reached"|"height_cap"|"done"|"lost_track",
    //              truncated, appliedMaxHeight, topReached?: false, note?,
    //              scrolledBy: "user"|"putgd"|"both"|"none", framesPlaced, framesSkipped, endedLost, reachedEnd,
    //              stats: {steps, pixelsPerNotch, lowConfidenceSteps, elapsedMs, frames, recovered, pinnedRegions, framesPerSecond}}
    //             end_of_content: auto-scroll proved the end of the page and
    //             no frame was skipped. top_not_reached: the same, but the way
    //             to the top (fromTop) could not prove the top. done: finished
    //             (Done, stop, the hotkey, Esc, the idle bound, or auto-scroll
    //             at the end after skipped frames) and whole as far as it was
    //             scrolled. lost_track: finished while the tracker was lost.
    //             truncated is height_cap, lost_track and top_not_reached.
    //   cancelled {}                                      Cancel on the bar, Esc in the selector, or a
    //                                                     stop before the first frame was stitched
    //   failed    {error: {code, message, details}}
    // After finished, cancelled or failed the backend injects nothing more for the id.
    virtual void setEventSink(Event) = 0;
    // params: {rect?: [x, y, w, h] logical desktop pixels (X11: root pixels, as
    //          for a region recording), maxHeight, maxPixels?, fromTop,
    //          autoStart, selection: "helper"|"shell", driver: "user"|"putgd",
    //          finishKey?: the key that finishes the capture, for the bar}.
    // driver is who turns the page at the start; fromTop applies to auto-scroll only.
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
    // select {rect} | start | autoScroll {on} | pause | resume | stop | cancel |
    // overlay {visible, rect?, inside?}. Reply only once the helper reached the
    // state. autoScroll turns put.gd's own scrolling on or off (pause and resume
    // are the same, off and on); it answers {state: "capturing", driver, paused,
    // pausedBy?}. stop is Done and keeps the image: its reply carries the
    // same object as the finished event (which is still sent). cancel deletes
    // the staging files. overlay hides or shows the helper's bar. On GNOME the
    // Shell has to move the bar out of the rectangle, which needs it on screen
    // first: prepare with autoStart false, `select`, place the bar through
    // RecordingShell::showFrame and send overlay {visible: true, rect, inside}
    // with where the Shell put it (inside: in the rectangle's corner, its rows
    // kept out of the image; the pointer rests on it between auto-scroll's
    // wheel steps), or overlay {visible: false} when it placed none, then
    // `start`. With autoStart true there the bar stays hidden for the whole capture.
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
