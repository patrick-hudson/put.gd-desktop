// The scroll-capture control loop (#1376): scroll, wait for the picture to
// settle, stitch, repeat until the content stops moving. It is the research
// prototype's scroll_session plus the pointer rules of the issue's "Control
// loop" section and the optional scroll-to-top step before the first frame.
// Standard library only, behind three small interfaces, so the same loop runs
// against the synthetic application in test_scroll_stitcher and against the
// frame tap and a desktop injector in the helper. It blocks: the helper runs it
// on a worker thread.
#pragma once
#include "core/ScrollStitcher.h"
#include <atomic>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace putgd::record {
// What the loop may do to the desktop: wheel notches and pointer motion, never
// buttons, never keys. Points are frame pixels of the captured rectangle; a
// point outside the frame is outside the rectangle on the screen.
struct RunInjector {
    virtual ~RunInjector() = default;
    // Positive scrolls the content down. False when the event could not be sent.
    virtual bool scroll(int notches) = 0;
    virtual bool movePointer(int x, int y) = 0;
    // The wheel has come to rest: said to the desktop where it can be said
    // (KWin's fake input: an axis stop). True when something was sent, so the
    // picture may change once more. See ScrollRun::scrollToTop.
    virtual bool rest() { return false; }
    // Before the first injection. X11 waits here until the modifier keys are up
    // (Ctrl plus wheel zooms); false when they never were, or `stop` was set.
    virtual bool waitReady(const std::atomic<bool>& stop) { (void)stop; return true; }
    // Called while the loop waits in a pause, every 10 ms. The only injection
    // allowed then is the helper's own one-time correction of a pointer it
    // pulled back from under the user's hand (#1514).
    virtual void held() {}
};
struct RunFrame {
    scroll::FrameView view;    // valid until the next nextFrame() that returns a frame
    int64_t timestampMs = 0;   // the loop clock's time
};
struct RunFrames {
    virtual ~RunFrames() = default;
    // The newest frame, newer than the last one returned, or nothing after
    // timeoutMs. A stream that sends nothing while the screen is still is fine.
    virtual std::optional<RunFrame> nextFrame(int timeoutMs) = 0;
    // The source ended and no more frames will come.
    virtual bool lost() const { return false; }
    // A frame grabbed from the screen after this call began, whatever the
    // stream has or has not sent since: a damage-driven stream sends nothing
    // while the screen is still, delivers a single repaint late or not at all
    // (KWin's region stream, on a real desktop), and so looks exactly like
    // content that stopped moving. This is the loop's only proof of stillness:
    // without it a capture never ends as "end of content". Nothing when the
    // source cannot give one.
    virtual std::optional<RunFrame> freshFrame(int timeoutMs) { (void)timeoutMs; return std::nullopt; }
};
struct RunClock {
    virtual ~RunClock() = default;
    virtual int64_t nowMs() = 0;
    virtual void sleepMs(int ms) = 0;
};

struct RunPoint { int x = 0, y = 0; };
struct RunProgress {
    bool scrollingToTop = false;
    int height = 0;            // output rows so far
    int steps = 0;
    double pixelsPerNotch = 0;
};
struct RunOptions {
    scroll::StitchOptions stitch;
    int settleMs = 120;               // no change in the moving band for this long = settled
    int settleMaxChangedRows = 2;
    // A stream that delivers a frame only every 150 ms or so (a slow guest)
    // shows two frames of one scroll animation further apart than settleMs,
    // and the first of them was taken for the settled picture: the distance
    // per notch learned from it was wrong and the rest of the movement landed
    // in the next step (lab guests, 2026-11-10). The settle time is therefore
    // at least settleCadenceFactor times the longest recent gap between two
    // frames, at most settleMaxMs. Gaps over cadenceIdleMs are a stream at rest.
    double settleCadenceFactor = 2.0;
    int settleMaxMs = 600;
    int cadenceIdleMs = 400;
    // How long the picture gets to begin to change after an injection before
    // the step counts as still. An application repaints when it gets to it and
    // a compositor's stream delivers that repaint later still (KWin 6.3: 40 to
    // 120 ms after the application painted), so stillness inside the settle
    // time alone says nothing. The wait is the larger of responseMs and
    // responseFactor times the slowest response this run has seen, at most
    // responseMaxMs.
    int responseMs = 500;
    double responseFactor = 3.0;
    int responseMaxMs = 3000;
    int stepTimeoutMs = 1500;         // hard limit per settle wait
    int firstFrameTimeoutMs = 2000;
    double targetStepFraction = 0.60; // of the moving band
    double maxStepFraction = 0.75;
    int maxNotchesPerStep = 64;
    int endConfirmDelayMs = 400;      // a still step is at least this old before the confirming injection
    int totalTimeLimitMs = 180000;
    bool allowScrollBack = true;
    int extraScrollBacks = 2;         // ways back sent again when the first did not reach the last stitched frame
    double shortStepFraction = 0.7;   // offset < this * hint: a final, partial step; keep px-per-notch
    // Pointer rules. With `pointer` off the loop never moves the pointer (the
    // prototype's behaviour, and what the synthetic tests use).
    bool pointer = false;
    // A point outside the rectangle to rest the pointer on while the first
    // frame is grabbed, so no hover effect is in it. Unset: it stays where it is.
    std::optional<RunPoint> away;
    // Rows kept between the parked pointer and the first appended row.
    int hoverMarginRows = 24;
    // When the pointer arrives at a new place it is put there twice, this far
    // apart, before the first wheel step: the application under it has to see
    // it enter first. (On GNOME a step sent right after the first motion went
    // nowhere; lab probe of 2026-11-10.)
    int pointerSettleMs = 60;
    // Step 0: wheel up until the top is proved: two checked steps in a row
    // that moved nothing, and a fresh frame that agrees. The way up takes what
    // the page needs, at most toTopLimitMs and at most half of the total limit,
    // so that a capture can still follow. (It was 15 s whatever the page: a
    // 27,000 px text view on a Hyprland guest needed 17 s, and the capture
    // then began at line 78 and ended as "end of content"; final pass of
    // 2026-11-11.) When the top cannot be proved the way up gives up, the
    // capture goes on from where the page is, and RunStats::reachedTop is
    // false with the reason in RunStats::toTopGaveUp: never silently.
    bool fromTop = false;
    int toTopNotches = 10;
    int toTopLimitMs = 90000;
    // While the page keeps moving the wheel steps grow, doubled at every check
    // that found it moved, up to this many notches. A larger step that moved
    // nothing proves nothing (an application may refuse an oversized wheel
    // event): the top is proved with steps of toTopNotches only.
    int toTopMaxNotches = 40;
    // Between two checks the next wheel step is sent as soon as the picture
    // begins to change, without waiting for it to settle; every
    // toTopCheckEvery-th step is a check: the picture settles and is compared
    // with the one at the check before. A check that finds the page where it
    // was although the steps in between looked like movement (something
    // changes in place: a pointer painted into the frames, a hover effect)
    // ends the quick steps for this run. 1: every step is a check.
    int toTopCheckEvery = 4;
    // A page that never comes to rest (a video, an animation) cannot be told
    // from one that still scrolls: when no check has found the picture settled
    // for this long, the way up gives up.
    int toTopRestlessMs = 15000;
    // The frames show the mouse pointer whatever the stream was asked for
    // (Hyprland composites a software cursor into the output, and every copy
    // of the output has it; lab guest of 2026-11-11, where Stop on the way to
    // the top returned the viewport with the arrow in it). On the way to the
    // top the pointer then goes back to `away` right behind every wheel step
    // and comes in again for the next, so the viewport Stop keeps is read
    // with the pointer outside. A Stop that arrives between the pointer's
    // entry and its wheel step sends no wheel step; the pointer still leaves,
    // which is the one motion that can follow a Stop. Needs `away`.
    bool restAwayToTop = false;
    // The same for every wheel step of the capture, on every desktop: the
    // pointer is inside the rectangle only to deliver the wheel step and
    // waits at `away` while the frames are read. A pointer that rests on the
    // page makes the page answer it: a browser shows the address of the link
    // under it in a panel at the bottom of the viewport, in the very rows
    // the next step appends, and every capture of a web page had that panel
    // in it several times (live pages, acceptance of 2026-11-12). A visit of
    // pointerSettleMs is shorter than the time a browser waits before it
    // shows that panel or a tooltip. Needs `away`; without one (the rectangle
    // covers its whole output) the pointer stays parked as before.
    bool restAway = false;
    // With restAway: how far inside the rectangle's edge the pointer comes in
    // to turn the wheel (see ScrollRun::wheelPoint), and how long it stays
    // after the wheel step before it leaves again. Both together stay under
    // the time a browser waits before it shows a link's address (80 ms after
    // the pointer last moved).
    int entryInset = 48;
    int leaveDelayMs = 0;
    // After the pointer has left for the first frame, what it had called up
    // while it rested on the page (the user's own pointer before the capture,
    // or the way to the top without restAway) gets this long to go away.
    int awayLingerMs = 0;
    // After a way up that moved the page: this many single steps down may move
    // nothing (an overscrolled view takes them) before the capture gives up.
    int stillStepsAfterTop = 8;
    // Stop on the way to the top: fresh frames asked for, at most, until one
    // agrees with the frame before it.
    int stopLooks = 6;
    // Asked when the stop flag ends the way to the top: true when no image is
    // wanted (Cancel), so the loop does not wait for the viewport to settle.
    std::function<bool()> discarded;
    // The user has taken the pointer, or an agent asked for a pause (#1514).
    // While it returns true the loop sends nothing, no pointer motion and no
    // wheel step, and reads no frame: it stops where it is, between any two
    // injections, and waits. On Resume it looks at the screen again before it
    // goes on (ScrollRun::resync): a page that is where it was, or that moved
    // to where its frame can be placed, goes on; one that cannot be placed
    // ends the capture as lost track with the exact image so far. A pause is
    // not counted in totalTimeLimitMs; one that lasts pauseLimitMs ends the
    // capture as Stop, the image kept.
    std::function<bool()> paused;
    int pauseLimitMs = 600000;
    // Every stitch attempt with the hint and notches used (diagnostics, tests).
    std::function<void(const scroll::StepResult&, int hint, int notches)> onStep;
    // After the first frame and after every step; called on the loop's thread.
    std::function<void(const RunProgress&)> onProgress;
};

enum class RunResult {
    EndOfContent,    // the content stopped moving: the whole scroll area is in the image
    TopNotReached,   // the content stopped moving, but scroll-to-top had not proved the top: the image starts partway down
    LimitReached,    // maxOutputRows
    Stopped,         // the stop flag
    TimeLimit,
    LostTrack,       // a step could not be matched or recovered, the end could not be proven, or the layout inside the frame changed; the image so far is exact
    NoMovement,      // nothing scrolled at any park position (two single notches at each)
    InjectorFailed,
    NoFrames,        // the source never delivered a frame
    SourceLost,
    LayoutChanged,   // the frame size changed
    NotReady,        // waitReady refused (modifier keys held)
};
const char* toString(RunResult);

struct RunStats {
    int steps = 0;               // injections followed by a stitch attempt
    int notchesInjected = 0;     // absolute, scroll-back and scroll-to-top included
    double pxPerNotch = 0;       // last measured
    double minPxPerNotch = 0, maxPxPerNotch = 0;
    int retries = 0, scrollBacks = 0, halvings = 0;
    int endConfirmations = 0, falseEnds = 0, reversed = 0;
    int lowConfidenceSteps = 0, settleTimeouts = 0, framesSeen = 0;
    int quietStream = 0;         // a fresh frame showed movement the stream had not sent
    int freshFrames = 0;         // fresh frames asked for and had
    int unprovenStill = 0;       // still steps for which no fresh frame could be had
    int maxResponseMs = 0;       // the slowest injection-to-first-change seen on a moving step
    bool stoppedAtTop = false;   // Stop arrived while scrolling to the top: the image is the viewport then
    bool stopViewportProven = false;  // that viewport is a fresh frame a second look agreed with
    int toTopSteps = 0;
    int pauses = 0;              // times the loop stopped for a pause
    int64_t pausedMs = 0;        // how long it waited in them
    bool pauseExpired = false;   // a pause lasted RunOptions::pauseLimitMs and ended the capture as Stop
    // reachedTop: the top was proved. With fromTop on and reachedTop false the
    // image does not start at the top of the page, whatever ended the capture
    // (the end of the content is then TopNotReached, never EndOfContent).
    bool toTopMoved = false, reachedTop = false;
    int64_t toTopMs = 0;         // how long the way up took
    std::string toTopGaveUp;     // why the way up ended without proof; empty when it was proved, stopped or failed
    int parkPosition = 0;        // 0 centre and a third down, 1 near the right edge, 2 two thirds down
    int64_t elapsedMs = 0;
    int outputRows = 0;
    std::string lastError;
};

class ScrollRun {
public:
    ScrollRun(RunInjector&, RunFrames&, RunClock&, RunOptions = {}, scroll::RowSink = {}, scroll::RowTakeBack = {});
    // Runs to completion. The stitcher is finished on every path, so the image
    // so far never has a gap. `stop` ends the loop before its next injection.
    RunResult run(const std::atomic<bool>* stop = nullptr);
    const RunStats& stats() const { return stats_; }
    scroll::ScrollStitcher& stitcher() { return stitcher_; }
    // The park positions of rule 2 and rule 7 for a frame of this size.
    static RunPoint parkPoint(int position, int width, int height);
    // Rule 3's point just inside the edge the pointer waits beyond (restAway).
    static RunPoint entryPoint(RunPoint away, int entryInset, int width, int height);
private:
    // Thrown from wherever the loop is when it finds itself paused, and caught
    // where the loop can wait and look again (start(), run()): the pause takes
    // effect between any two injections, inside a visit and inside a wait.
    struct Interrupted {};
    bool pausedNow() const { return opts_.paused && opts_.paused(); }
    void holdOff() const { if (pausedNow()) throw Interrupted{}; }
    // An injection the desktop refused because the pause had closed the gate first.
    bool sent(bool ok) const { if (!ok) holdOff(); return ok; }
    // Waits while paused: Stopped for Stop, Cancel or a pause that lasted pauseLimitMs.
    std::optional<RunResult> holdPaused();
    // After a pause: where the page is now, against the last stitched frame.
    std::optional<RunResult> resync(scroll::StepResult& seen);
    // The way to the top (toTop) and the first viewport; after a pause there,
    // both again. The result is final (Stop on the way up already kept its viewport).
    std::optional<RunResult> start(bool toTop);
    // Time the capture ran, its pauses left out.
    int64_t activeMs() const { return clock_.nowMs() - startMs_ - pausedMs_; }
    enum class Wait { Settled, TimedOut, Stopped, TimeLimit, NoFrame, Lost, Changed, LayoutChanged };
    // quietMs: with no change at all since the call began, this long counts as
    // settled (the response wait after an injection); 0: the settle time.
    Wait waitSettled(int timeoutMs, int quietMs = 0);
    // Until a frame from after the call began differs from the picture as it
    // was then (Changed), or quietMs have passed without one (Settled).
    Wait waitChanged(int quietMs);
    int responseWaitMs() const;
    int planNotches() const;
    bool inject(int notches);
    bool park();
    RunPoint wheelPoint() const;
    // Before a conclusion drawn from stillness: whether a fresh frame agrees
    // with the last one seen. Moved: it does not, and it is now the last frame.
    // Unknown: the source gave none, so nothing is proven.
    enum class Screen { Same, Moved, Unknown };
    Screen screenIsAsSeen();
    // After Stop on the way to the top only a discarded capture (Cancel) ends the last waits.
    bool stopped() const { return afterStop_ ? (opts_.discarded && opts_.discarded()) : (stop_ && stop_->load()); }
    std::optional<RunResult> waitFailure(Wait);
    // After a still step: the end of the content proved, the page found
    // further down (`found` is that step), or neither. `fail` ends the run.
    enum class EndProof { Proved, Moved, Unproved };
    EndProof proveEnd(scroll::StepResult& found, std::optional<RunResult>& fail);
    std::optional<RunResult> scrollToTop(bool resumed = false);
    std::optional<RunResult> wayToTop();
    RunResult viewportAfterStop();
    void progress(bool toTop);
    RunResult finishWith(RunResult);

    RunInjector& injector_;
    RunFrames& source_;
    RunClock& clock_;
    RunOptions opts_;
    scroll::ScrollStitcher stitcher_;
    RunStats stats_;
    const std::atomic<bool>* stop_ = nullptr;
    int64_t startMs_ = 0;
    int referenceWidth_ = 0, referenceHeight_ = 0;
    bool layoutChanged_ = false;
    bool haveFrame_ = false;
    scroll::FrameView last_{};
    scroll::FrameSignatures lastSig_, tmpSig_, settledSig_, beforeSig_;
    int notchCap_ = 0, verifiedRun_ = 0;
    int parked_ = -1;  // the park position the pointer was last put at, -1 elsewhere
    bool atEntry_ = false;  // restAway: the wheel is turned just inside the edge the pointer waits beyond
    // The last waitSettled: when its first changed frame arrived (-1: none did).
    int64_t firstChangeMs_ = -1;
    // The stream's cadence: when the last frame arrived, and the longest recent gap between two.
    int64_t lastFrameMs_ = -1;
    double cadenceMs_ = 0;
    int settleNeedMs() const;
    bool afterStop_ = false;   // the viewport is being taken after Stop during scroll-to-top
    bool toTop_ = false;       // the page is on its way to the top (step 0)
    int64_t lastInjectMs_ = -1;  // when the last wheel step was sent
    int64_t pausedMs_ = 0;
    // Wheel steps of the current step of the capture, and how many: a pause
    // that comes after one plain wheel step can scroll that step back to find
    // the last stitched frame again (resync).
    int stepNotches_ = 0, stepInjections_ = 0;
};
}
