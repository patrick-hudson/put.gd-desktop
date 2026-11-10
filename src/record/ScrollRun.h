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
    // Before the first injection. X11 waits here until the modifier keys are up
    // (Ctrl plus wheel zooms); false when they never were, or `stop` was set.
    virtual bool waitReady(const std::atomic<bool>& stop) { (void)stop; return true; }
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
    // A frame grabbed anew from the screen, whatever the stream has or has not
    // sent since: a damage-driven stream can go quiet while the screen changes
    // (KWin's region stream did, on a real desktop), and a quiet stream looks
    // exactly like content that stopped moving. Nothing when the source cannot.
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
    int stepTimeoutMs = 1500;         // hard limit per settle wait
    int firstFrameTimeoutMs = 2000;
    double targetStepFraction = 0.60; // of the moving band
    double maxStepFraction = 0.75;
    int maxNotchesPerStep = 64;
    int endConfirmDelayMs = 400;      // wait before the confirming injection at a suspected end
    int totalTimeLimitMs = 180000;
    bool allowScrollBack = true;
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
    // Step 0: wheel up in steps of this many notches until two settled frames
    // in a row are the same, for at most toTopLimitMs (inside the total limit).
    bool fromTop = false;
    int toTopNotches = 10;
    int toTopLimitMs = 15000;
    // Every stitch attempt with the hint and notches used (diagnostics, tests).
    std::function<void(const scroll::StepResult&, int hint, int notches)> onStep;
    // After the first frame and after every step; called on the loop's thread.
    std::function<void(const RunProgress&)> onProgress;
};

enum class RunResult {
    EndOfContent,    // the content stopped moving: the whole scroll area is in the image
    LimitReached,    // maxOutputRows
    Stopped,         // the stop flag
    TimeLimit,
    LostTrack,       // a step could not be matched or recovered; the image so far is exact
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
    int toTopSteps = 0;
    bool toTopMoved = false, reachedTop = false;
    int parkPosition = 0;        // 0 centre and a third down, 1 near the right edge, 2 two thirds down
    int64_t elapsedMs = 0;
    int outputRows = 0;
    std::string lastError;
};

class ScrollRun {
public:
    ScrollRun(RunInjector&, RunFrames&, RunClock&, RunOptions = {}, scroll::RowSink = {});
    // Runs to completion. The stitcher is finished on every path, so the image
    // so far never has a gap. `stop` ends the loop before its next injection.
    RunResult run(const std::atomic<bool>* stop = nullptr);
    const RunStats& stats() const { return stats_; }
    scroll::ScrollStitcher& stitcher() { return stitcher_; }
    // The park positions of rule 2 and rule 7 for a frame of this size.
    static RunPoint parkPoint(int position, int width, int height);
private:
    enum class Wait { Settled, TimedOut, Stopped, TimeLimit, NoFrame, Lost };
    Wait waitSettled(int timeoutMs);
    int planNotches() const;
    bool inject(int notches);
    bool park();
    // Before a conclusion drawn from stillness: whether a fresh frame agrees
    // with the last one seen. When it does not, it becomes the last frame.
    bool screenIsAsSeen();
    bool stopped() const { return stop_ && stop_->load(); }
    std::optional<RunResult> waitFailure(Wait);
    std::optional<RunResult> scrollToTop();
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
    bool haveFrame_ = false;
    scroll::FrameView last_{};
    scroll::FrameSignatures lastSig_, tmpSig_, settledSig_;
    int notchCap_ = 0, verifiedRun_ = 0;
    int parked_ = -1;  // the park position the pointer was last put at, -1 elsewhere
};
}
