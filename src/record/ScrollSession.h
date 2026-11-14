// The scroll capture's session loop (#1515): every frame of the region goes to
// the tracker (core/ScrollTracker.h) as it arrives, whoever scrolls. Two
// drivers sit on top of it:
//
// - the user: nothing is injected, the pointer is not moved, and the image
//   grows as the page moves under the user's own wheel, touchpad, keys or
//   scrollbar;
// - put.gd (auto-scroll): wheel steps at a steady pace, each small enough
//   that the frames of its animation overlap well, with the pointer visiting
//   the rectangle for the step only (#1514). It hands the wheel back when the
//   user moves the mouse or scrolls, when it cannot find its place again, and
//   when nothing scrolls under it; it ends the capture at the end of the page.
//
// Switching between the two keeps the image: the tracker follows the page
// through both. Standard library only, behind small interfaces, so the same
// loop runs against the synthetic application in test_scroll_session and
// against the frame tap and a desktop injector in the helper. It blocks: the
// helper runs it on a worker thread.
#pragma once
#include "core/ScrollTracker.h"
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace putgd::record {
// What auto-scroll may do to the desktop: wheel notches and pointer motion,
// never buttons, never keys. Points are frame pixels of the captured
// rectangle; a point outside the frame is outside the rectangle on the screen.
struct RunInjector {
    virtual ~RunInjector() = default;
    // Positive scrolls the content down. False when the event could not be sent.
    virtual bool scroll(int notches) = 0;
    virtual bool movePointer(int x, int y) = 0;
    // The wheel has come to rest: said to the desktop where it can be said
    // (KWin's fake input: an axis stop). True when something was sent.
    virtual bool rest() { return false; }
    // Before the first injection. X11 waits here until the modifier keys are up
    // (Ctrl plus wheel zooms); false when they never were, or `stop` was set.
    virtual bool waitReady(const std::atomic<bool>& stop) { (void)stop; return true; }
    // Called while auto-scroll is paused by the mouse, every few ms. The only
    // injection allowed then is the helper's own one-time correction of a
    // pointer it pulled back from under the user's hand (#1514).
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
    // stream has sent: a damage-driven stream sends nothing while the screen
    // is still. Nothing when the source cannot give one.
    virtual std::optional<RunFrame> freshFrame(int timeoutMs) { (void)timeoutMs; return std::nullopt; }
};
struct RunClock {
    virtual ~RunClock() = default;
    virtual int64_t nowMs() = 0;
    virtual void sleepMs(int ms) = 0;
};
struct RunPoint { int x = 0, y = 0; };

// The canvas the tracker writes to, which the way to the top can empty: the
// image then starts again at the top of the page.
class SessionCanvas : public scroll::TrackerCanvas {
public:
    virtual void clear() = 0;
};

enum class Driver { User, Putgd };
const char* toString(Driver);
// Why auto-scroll handed the wheel back by itself.
enum class HandBack {
    None,
    Pointer,     // the user moved the mouse (#1514)
    Scrolled,    // the page moved against auto-scroll's direction: the user scrolled
    Lost,        // the tracker lost the page and a step back did not find it again
    NoMovement,  // nothing scrolled under the wheel at any of its points
};
const char* toString(HandBack);
// What the bar says, as one word an agent reads.
enum class SessionState { Waiting, Following, Lost, ScrollingToTop, AutoScrolling, Paused };
const char* toString(SessionState);

struct SessionProgress {
    SessionState state = SessionState::Waiting;
    Driver driver = Driver::User;
    HandBack pausedBy = HandBack::None;
    long long height = 0;      // rows of the image so far
    int placed = 0, skipped = 0;
    int steps = 0;             // wheel steps auto-scroll sent
    double pixelsPerNotch = 0;
    bool picture = false;      // the image has a frame: Done keeps something
    unsigned serial = 0;       // the last auto-scroll request the session has acted on
};

enum class SessionEnd {
    Finished,          // the stop flag: Done, the hotkey, Esc, scroll.stop
    Idle,              // nobody scrolled or used a control for idleLimitMs
    PageEnd,           // auto-scroll reached the end of the page
    Capped,            // the image reached its height cap
    Discarded,         // the discard flag (Cancel)
    NoFrames,          // the source never delivered a frame
    SourceLost,
    FrameSizeChanged,  // the frames changed size (the display changed)
    InjectorFailed,    // the desktop refused auto-scroll's input
    NotReady,          // auto-scroll: modifier keys stayed down (X11)
};
const char* toString(SessionEnd);

struct SessionStats {
    // The tracker's counts for the image (after the way to the top, the
    // image's tracker only).
    int frames = 0, placed = 0, noMovement = 0, skipped = 0, recovered = 0, longestSkipRun = 0;
    int byExpectation = 0, onHint = 0, pinnedRegions = 0;
    // Placed frames that moved, by who drove at the time.
    int movedByUser = 0, movedByPutgd = 0;
    // Every frame the source delivered, and how long the session read them:
    // the frame rate the desktop gives.
    int framesSeen = 0;
    int64_t readMs = 0;
    // Auto-scroll.
    int steps = 0, notches = 0, autoRuns = 0, handBacks = 0, stepsBack = 0;
    double pxPerNotch = 0;
    int maxResponseMs = 0;
    bool reachedEnd = false;   // auto-scroll proved the end of the page
    bool endedLost = false;    // the session ended while the tracker was lost
    bool capReached = false;
    bool topAsked = false, topReached = false;
    std::string toTopGaveUp;   // why the way to the top ended without proof
    int toTopSteps = 0;
    int64_t toTopMs = 0, elapsedMs = 0;
    long long top = 0, bottom = 0;  // the image's extent in page rows
    std::string lastError;
};

struct SessionOptions {
    scroll::TrackerOptions tracker;
    // The driver at the start.
    bool autoScroll = false;
    // Auto-scroll goes to the top of the page first, when it begins while
    // nothing has moved yet; the image then starts at the top.
    bool fromTop = false;
    // Only the frame's first rows are the page's: put.gd's bar sits inside
    // the rectangle below them (#1519). 0: all of them.
    int frameRows = 0;
    // A session that nobody touches (no frame placed with movement, no
    // command) for this long ends as Idle, the image kept.
    int idleLimitMs = 600000;
    int firstFrameTimeoutMs = 3000;
    // Auto-scroll's pace. A step is this share of the frame's height (frames
    // of a smooth scroll overlap the more, the smaller it is), at most
    // maxNotches. It is settled when the page has not moved for settleMs; a
    // step that moved nothing for responseMs (or responseFactor times the
    // slowest answer seen, at most responseMaxMs) is still.
    double stepFraction = 0.40;
    int maxNotches = 12;
    // The next step goes once the page has moved this share of what the step
    // asked (or has come to rest).
    double nextAtShare = 0.85;
    int settleMs = 150;
    int responseMs = 600;
    double responseFactor = 3.0;
    int responseMaxMs = 3000;
    int stepTimeoutMs = 3000;   // a step whose page never rests is taken as settled after this
    int stepGapMs = 40;
    // How long an animated wheel step is expected to take, for the hint.
    int animationMs = 300;
    // The way to the top: wheel steps up of toTopNotches, doubled while the
    // page moves up to toTopMaxNotches; the top is two checked steps that
    // moved nothing and a fresh frame that agrees. At most toTopLimitMs;
    // a page that never comes to rest for toTopRestlessMs gives up.
    int toTopNotches = 10, toTopMaxNotches = 40, toTopLimitMs = 90000, toTopRestlessMs = 15000;
    // The pointer: auto-scroll moves it into the rectangle for each wheel
    // step and back to `away` (its rest on a surface of the helper's) right
    // after. Without `away` it stays at the wheel point. beforeAuto, when
    // set, gives the rest for each run of auto-scroll instead.
    bool pointer = false;
    std::optional<RunPoint> away;
    // The frames show the pointer (Hyprland paints it whatever was asked):
    // frames from a wheel step's visit, and the next, do not go to the tracker.
    bool blindVisits = false;
    int entryInset = 48, pointerSettleMs = 60, leaveDelayMs = 30;
    // The user took the pointer, or auto-scroll is off on the helper's side:
    // nothing may be injected while it returns true (#1514).
    std::function<bool()> paused;
    // Asked once before auto-scroll's first injection of each run (the
    // helper puts up its rest and watch surfaces): where the pointer rests
    // between wheel steps. Must not block long.
    std::function<std::optional<RunPoint>()> beforeAuto;
    // Requests from other threads: auto-scroll on or off (a serial that
    // changes with every request, and the wish), and a count of commands
    // that changes with every one (they count against the idle bound).
    std::function<std::pair<unsigned, bool>()> request;
    std::function<unsigned()> activity;
    // After every frame and every change of state; on the loop's thread.
    std::function<void(const SessionProgress&)> onProgress;
    // Every frame's result, with its hint (diagnostics, tests).
    std::function<void(const scroll::TrackResult&, const scroll::TrackerHint&, int64_t t)> onFrame;
    // Every wheel step: notches, and where.
    std::function<void(int notches, RunPoint at)> onStep;
};

class ScrollSession {
public:
    ScrollSession(RunInjector&, RunFrames&, RunClock&, SessionCanvas&, SessionOptions);
    ~ScrollSession();
    ScrollSession(const ScrollSession&) = delete;
    ScrollSession& operator=(const ScrollSession&) = delete;
    // Runs until the stop or discard flag, the idle bound, the cap, the end
    // of the page under auto-scroll, or a failure. The tracker is finished on
    // every path that keeps an image (its pinned parts written once).
    SessionEnd run(const std::atomic<bool>& stop, const std::atomic<bool>& discard);
    const SessionStats& stats() const { return stats_; }
    const scroll::ScrollTracker& tracker() const { return *tracker_; }
    // Where the wheel is turned: just inside the edge the pointer rests
    // beyond, else the centre a third down, near the right edge, two thirds down.
    static RunPoint parkPoint(int position, int width, int height);
    static RunPoint entryPoint(RunPoint away, int entryInset, int width, int height);
    // Every point the wheel may be turned at, for a frame of this size.
    std::vector<RunPoint> wheelPoints(int width, int height) const;

private:
    enum class Phase { Off, ToTop, Step, Settle, Recover };
    void feed(const RunFrame&);
    void feedTracker(const scroll::FrameView&, int64_t t);
    void account(const scroll::TrackResult&, int64_t t);
    scroll::TrackerHint hintFor(int64_t t);
    void observeToTop(const scroll::FrameView&, int64_t t);
    // Waits up to ms, feeding whatever frames come.
    bool pump(int ms);
    void applyRequests();
    void startAuto();
    void handBack(HandBack why);
    void autoTick();
    void toTopTick();
    void endToTop(bool proved, const std::string& gaveUp);
    void resetTracker();
    bool visit(int notches);
    bool injected(bool sent);
    RunPoint wheelPoint() const;
    int planNotches() const;
    int responseWaitMs() const;
    void settled(int64_t t);
    void startRecover();
    int frameRowsFull() const { return frameH_; }
    void stillStep(int64_t t);
    SessionState state() const;
    void progress(bool force = false);
    bool pausedNow() const { return opts_.paused && opts_.paused(); }
    bool stopping() const { return (stop_ && stop_->load()) || (discard_ && discard_->load()); }
    void finishTracker();

    RunInjector& injector_;
    RunFrames& source_;
    RunClock& clock_;
    SessionCanvas& canvas_;
    SessionOptions opts_;
    std::unique_ptr<scroll::ScrollTracker> tracker_;
    SessionStats stats_;
    const std::atomic<bool>* stop_ = nullptr;
    const std::atomic<bool>* discard_ = nullptr;
    int width_ = 0, height_ = 0;  // the tracker's frames (after frameRows)
    int frameW_ = 0, frameH_ = 0;  // the source's frames
    int64_t firstFrameAt_ = -1;
    bool sizeChanged_ = false, capped_ = false, failed_ = false, notReady_ = false, pageEnd_ = false;
    int64_t start_ = 0, lastActivity_ = 0;
    unsigned seenSerial_ = 0, seenActivity_ = 0;
    bool readyChecked_ = false;
    // Driver, and where the pointer rests between its wheel steps.
    bool auto_ = false;
    std::optional<RunPoint> away_;
    // Blind: frames until this time are a visit's (blindVisits).
    int64_t blindUntil_ = -1;
    HandBack pausedBy_ = HandBack::None;
    Phase phase_ = Phase::Off;
    bool movedYet_ = false;      // a frame has been placed that moved
    bool movedDown_ = false;     // auto-scroll has moved the page in its direction
    int64_t nextStepAt_ = 0, stepSentAt_ = -1, stepFirstMoveAt_ = -1, lastMoveAt_ = -1;
    int stepNotches_ = 0, stillSteps_ = 0, patience_ = 1, wheelAt_ = 0, triesHere_ = 0;
    int recoverTries_ = 0, stepCap_ = 0, goodSteps_ = 0;
    int animationMs_ = 0;        // the longest recent step animation seen
    bool restedBefore_ = true, stepFromRest_ = true;
    long long stepMoved_ = 0;
    bool lastStill_ = false;
    int framesAfterWait_ = 0;    // frames placed or still that arrived after the step's response time
    // The hint's model of what auto-scroll's steps still owe in movement.
    double pending_ = 0, attributed_ = 0;
    long long emitted_ = 0;
    bool chainHinted_ = true;    // every frame since the tracker's last placed one had a hint
    int64_t lastHintAt_ = -1;
    // The way to the top: per-row digests of the last frame and the last check.
    std::vector<uint64_t> digest_, checkDigest_, scratchDigest_;
    std::vector<uint8_t> lastPixels_;
    int lastPixelsW_ = 0, lastPixelsH_ = 0;
    int64_t toTopStart_ = 0, toTopRested_ = 0, toTopSent_ = -1, toTopChangedAt_ = -1;
    int toTopNotchesNow_ = 0, toTopStill_ = 0;
    bool toTopChanged_ = false, toTopDone_ = false;
    // Progress.
    SessionProgress shown_;
    int64_t shownAt_ = -1;
};
}
