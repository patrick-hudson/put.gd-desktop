#include "ScrollSession.h"
#include <algorithm>
#include <climits>
#include <cstdint>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <utility>

namespace putgd::record {
using scroll::FrameView;
using scroll::TrackerHint;
using scroll::TrackResult;
using scroll::TrackState;
using scroll::TrackStatus;

const char* toString(Driver d) { return d == Driver::Putgd ? "putgd" : "user"; }
const char* toString(HandBack h) {
    switch (h) {
        case HandBack::None: return "";
        case HandBack::Pointer: return "pointer";
        case HandBack::Scrolled: return "scrolled";
        case HandBack::Lost: return "lost";
        case HandBack::NoMovement: return "no_movement";
    }
    return "";
}
const char* toString(SessionState s) {
    switch (s) {
        case SessionState::Waiting: return "waiting";
        case SessionState::Following: return "following";
        case SessionState::Lost: return "lost";
        case SessionState::ScrollingToTop: return "scrollingToTop";
        case SessionState::AutoScrolling: return "autoScrolling";
        case SessionState::Paused: return "paused";
    }
    return "?";
}
const char* toString(SessionEnd e) {
    switch (e) {
        case SessionEnd::Finished: return "Finished";
        case SessionEnd::Idle: return "Idle";
        case SessionEnd::Forgotten: return "Forgotten";
        case SessionEnd::CanvasFailed: return "CanvasFailed";
        case SessionEnd::PageEnd: return "PageEnd";
        case SessionEnd::Capped: return "Capped";
        case SessionEnd::Discarded: return "Discarded";
        case SessionEnd::NoFrames: return "NoFrames";
        case SessionEnd::SourceLost: return "SourceLost";
        case SessionEnd::FrameSizeChanged: return "FrameSizeChanged";
        case SessionEnd::InjectorFailed: return "InjectorFailed";
        case SessionEnd::NotReady: return "NotReady";
    }
    return "?";
}

namespace {
// Per row of a frame, a hash of its middle columns (the sides hold scrollbars):
// the way to the top compares pictures with it, since the tracker does not see
// those frames.
void digestRows(const FrameView& f, std::vector<uint64_t>& out) {
    out.resize(size_t(std::max(0, f.height)));
    const int lo = f.width / 25, hi = f.width - lo;
    for (int y = 0; y < f.height; ++y) {
        const uint8_t* row = f.row(y);
        uint64_t h = 1469598103934665603ull;
        for (int x = lo; x < hi; x += 2) {
            uint32_t v;
            std::memcpy(&v, row + size_t(x) * 4, 4);
            h = (h ^ (v & 0xFFFFFFu)) * 1099511628211ull;
        }
        out[size_t(y)] = h;
    }
}
// Rows that differ; a caret or a cursor blink is a row or two.
int differingRows(const std::vector<uint64_t>& a, const std::vector<uint64_t>& b) {
    if (a.size() != b.size()) return INT_MAX;
    int n = 0;
    for (size_t i = 0; i < a.size(); ++i) n += a[i] != b[i];
    return n;
}
constexpr int samePictureRows = 2;
}

ScrollSession::ScrollSession(RunInjector& injector, RunFrames& source, RunClock& clock, SessionCanvas& canvas, SessionOptions opts)
    : injector_(injector), source_(source), clock_(clock), canvas_(canvas), opts_(std::move(opts)),
      tracker_(std::make_unique<scroll::ScrollTracker>(canvas_, opts_.tracker)), away_(opts_.away) {
    if (!opts_.excluded.empty()) tracker_->setExcluded(opts_.excluded);
}
ScrollSession::~ScrollSession() = default;

RunPoint ScrollSession::parkPoint(int position, int width, int height) {
    if (position == 1) return {std::max(0, width - std::max(12, width / 8)), height / 3};
    if (position == 2) return {width / 2, 2 * height / 3};
    return {width / 2, height / 3};
}
// The point just inside the edge the pointer rests beyond, level with it: the
// way into the rectangle stays short (on Plasma a pointer that goes back and
// forth over more than a hundred pixels several times a second is one its
// user shakes to find, and KWin blows it up).
RunPoint ScrollSession::entryPoint(RunPoint away, int entryInset, int width, int height) {
    const int inset = std::min({entryInset, width / 2, height / 2});
    return {std::clamp(away.x, inset, std::max(inset, width - 1 - inset)), std::clamp(away.y, inset, std::max(inset, height - 1 - inset))};
}
std::vector<RunPoint> ScrollSession::wheelPoints(int width, int height) const {
    std::vector<RunPoint> points;
    // Not over the bar inside the region: its rows are left out of the wheel's places.
    const int rows = !opts_.excluded.empty() && opts_.excluded.y > height / 4 ? std::min(opts_.excluded.y, height) : height;
    // A rest inside the frame (the bar inside the rectangle) is no edge to come in from.
    if (away_ && (away_->x < 0 || away_->y < 0 || away_->x >= width || away_->y >= height))
        points.push_back(entryPoint(*away_, opts_.entryInset, width, rows));
    for (int position = 0; position < 3; ++position) points.push_back(parkPoint(position, width, rows));
    return points;
}
int ScrollSession::centreIndex() const {
    // wheelPoints: the entry point first where the rest is outside the frame, then parkPoint(0).
    return int(wheelPoints(frameW_, frameRowsFull()).size()) - 3;
}
RunPoint ScrollSession::wheelPoint() const {
    const auto points = wheelPoints(frameW_, frameRowsFull());
    return points[size_t(std::clamp(wheelAt_, 0, int(points.size()) - 1))];
}

SessionEnd ScrollSession::run(const std::atomic<bool>& stop, const std::atomic<bool>& discard) {
    stop_ = &stop;
    discard_ = &discard;
    start_ = clock_.nowMs();
    lastActivity_ = start_;
    if (opts_.activity) seenActivity_ = opts_.activity();
    // An Auto-scroll asked for before the loop began (the request is read
    // here once) is acted on, not taken as already applied (CodeRabbit on #1528).
    bool autoAtStart = opts_.autoScroll;
    if (opts_.request) {
        const auto [serial, on] = opts_.request();
        seenSerial_ = serial;
        autoAtStart = on;
    }
    const auto done = [&](SessionEnd end) {
        stats_.elapsedMs = clock_.nowMs() - start_;
        if (end != SessionEnd::Discarded) finishTracker();
        return end;
    };
    std::optional<RunFrame> first;
    while (!first && !stopping() && !source_.lost() && clock_.nowMs() - start_ < opts_.firstFrameTimeoutMs) first = source_.nextFrame(100);
    if (!first) {
        stats_.elapsedMs = clock_.nowMs() - start_;
        if (discard.load()) return SessionEnd::Discarded;
        if (stop.load()) return SessionEnd::Finished;
        return source_.lost() ? SessionEnd::SourceLost : SessionEnd::NoFrames;
    }
    // Auto-scroll from the start: the pointer goes to its rest before the
    // first frame is taken into the image, as the published nightly did. A
    // desktop that paints the pointer into every frame (Hyprland) put the
    // arrow where the person had left it into the image's first screen, and
    // a hover under it shows too (#1515's acceptance). The person scrolling
    // themselves keeps the pointer: it is never moved for them.
    if (autoAtStart && opts_.pointer) {
        startAuto();
        if (auto_ && away_ && !stopping() && !pausedNow() && injector_.movePointer(away_->x, away_->y)) {
            // Only a frame from after the move: one already waiting in the
            // stream still shows the pointer where it was (the Omarchy guest
            // sends 3 to 4 frames a second, so one nearly always waits).
            // `first` stays the newest frame the source returned: an older
            // one's pixels belong to the stream again once another is taken.
            const int64_t from = clock_.nowMs() + std::max(opts_.pointerSettleMs, 100), until = from + 500;
            bool moved = false;
            while (!moved && !stopping() && !source_.lost() && clock_.nowMs() < until)
                if (auto frame = source_.nextFrame(int(until - clock_.nowMs()))) {
                    first = frame;
                    moved = frame->timestampMs >= from;
                }
            // A stream that sends only changes may have nothing new: the screen as it is.
            if (!moved && !stopping() && !source_.lost())
                if (auto fresh = source_.freshFrame(opts_.freshAtDoneMs > 0 ? opts_.freshAtDoneMs : 1500)) first = fresh;
        }
    }
    feed(*first);
    if (autoAtStart && !auto_ && !stopping() && pausedBy_ == HandBack::None) startAuto();
    progress(true);
    for (;;) {
        if (discard.load()) return done(SessionEnd::Discarded);
        if (stop.load()) {
            // Done on the way to the top: the image is the screen as it is now,
            // from a fresh frame as at every other end; the last one the
            // stream sent only if none comes (CodeRabbit on #1536).
            if (phase_ == Phase::ToTop) {
                if (opts_.freshAtDoneMs > 0 && !source_.lost())
                    if (auto fresh = source_.freshFrame(opts_.freshAtDoneMs)) feed(*fresh);
                endToTop(false, "the capture was finished on the way to the top");
            } else {
                takeScreenAtDone();
            }
            return done(SessionEnd::Finished);
        }
        applyRequests();
        if (auto_ && pausedNow()) handBack(HandBack::Pointer);
        if (auto_) {
            if (phase_ == Phase::ToTop) toTopTick();
            else autoTick();
        } else if (pausedBy_ == HandBack::Pointer) {
            injector_.held();
        }
        if (failed_) return done(SessionEnd::InjectorFailed);
        if (notReady_) return done(SessionEnd::NotReady);
        if (sizeChanged_) return done(SessionEnd::FrameSizeChanged);
        if (capped_) return done(SessionEnd::Capped);
        if (pageEnd_) return done(SessionEnd::PageEnd);
        // For a second after the mouse took the wheel back the loop comes
        // round every 10 ms: the one correction of a pointer moved from under
        // the hand goes out from injector_.held() (#1514).
        const bool correcting = !auto_ && pausedBy_ == HandBack::Pointer && clock_.nowMs() - handBackAt_ < 1000;
        if (auto frame = source_.nextFrame(auto_ ? 15 : correcting ? 10 : 40)) feed(*frame);
        else if (source_.lost()) return done(SessionEnd::SourceLost);
        // Nothing more can be written: the capture ends here, with a reason.
        if (canvas_.failed()) return done(SessionEnd::CanvasFailed);
        if (sizeChanged_) return done(SessionEnd::FrameSizeChanged);
        if (capped_) return done(SessionEnd::Capped);
        // The idle bound. A capture in which nothing ever moved (no placed
        // frame moved, by the user or by auto-scroll, over the whole session;
        // the way to the top is not the image's movement) was forgotten: its
        // one screen is not saved, uploaded or put in History.
        if (clock_.nowMs() - lastActivity_ >= opts_.idleLimitMs) return done(stats_.movedByUser + stats_.movedByPutgd == 0 ? SessionEnd::Forgotten : SessionEnd::Idle);
        progress();
    }
}

// Done: the screen as it is now, not only what the stream last sent. KWin's
// region stream once sent 92 frames identical to the pixel while Chrome went
// on scrolling for 220 px (#1534's live runs), and a person's Done then
// missed those rows; a fresh frame (the stream linked again, or the next grab)
// brings them.
void ScrollSession::takeScreenAtDone() {
    if (opts_.freshAtDoneMs <= 0 || source_.lost() || tracker_->totals().placed == 0) return;
    const long long before = tracker_->height();
    auto fresh = source_.freshFrame(opts_.freshAtDoneMs);
    if (!fresh) return;
    feed(*fresh);
    stats_.rowsAtDone = tracker_->height() - before;
}

void ScrollSession::finishTracker() {
    stats_.endedLost = tracker_->state() == TrackState::Lost;
    const auto totals = tracker_->finish();
    stats_.frames = totals.frames;
    stats_.placed = totals.placed;
    stats_.noMovement = totals.noMovement;
    stats_.skipped = totals.skipped;
    stats_.recovered = totals.recovered;
    stats_.longestSkipRun = totals.longestSkipRun;
    stats_.byExpectation = totals.byExpectation;
    stats_.onHint = totals.onHint;
    stats_.pinnedRegions = totals.pinnedRegions;
    stats_.capReached = totals.capReached || capped_;
    stats_.hiddenCells = totals.hiddenCells;
    stats_.hiddenRows = totals.hiddenRows;
    stats_.top = totals.top;
    stats_.bottom = totals.bottom;
}

void ScrollSession::feed(const RunFrame& frame) {
    FrameView view = frame.view;
    if (!view.data || view.width <= 0 || view.height <= 0) return;
    if (frameW_ == 0) {
        frameW_ = view.width;
        frameH_ = view.height;
    } else if (view.width != frameW_ || view.height != frameH_) {
        sizeChanged_ = true;
        return;
    }
    ++stats_.framesSeen;
    if (firstFrameAt_ < 0) firstFrameAt_ = frame.timestampMs;
    stats_.readMs = frame.timestampMs - firstFrameAt_;
    // A visit's frames show the painted pointer over the page.
    if (blindUntil_ >= 0 && frame.timestampMs <= blindUntil_ && tracker_->totals().placed > 0) return;
    width_ = view.width;
    height_ = view.height;
    if (phase_ == Phase::ToTop) {
        observeToTop(view, frame.timestampMs);
        return;
    }
    // While a way to the top may still begin, the latest screen is kept: it is
    // that way's first look and, should it end before another frame comes (a
    // stream that sends nothing while the page stands still), the screen the
    // image starts again with (CodeRabbit on #1528).
    if (opts_.fromTop && !toTopDone_ && !movedYet_) keepScreen(view);
    feedTracker(view, frame.timestampMs);
}

void ScrollSession::feedTracker(const FrameView& view, int64_t t) {
    ++framesSinceStep_;
    lastFrameAt_ = clock_.nowMs();
    const TrackerHint hint = hintFor(t);
    const TrackResult r = tracker_->addFrame(view, hint);
    // A frame that shows nothing of the page: fewer rows with texture than
    // the tracker needs as evidence, outside what it has learned as pinned (a
    // header, a footer), put.gd's bar inside the region and the side margins.
    // A header's flat background is no content, and the edges of its text the
    // mask does not quite cover are not either (#1543's review: a blank
    // stretch between a header and a footer was called the end of the page).
    lastFrameBlank_ = auto_ && r.texturedRows < opts_.tracker.minEvidenceRows &&
                      (r.status == TrackStatus::Placed || r.status == TrackStatus::NoMovement || r.status == TrackStatus::Skipped);
    if (opts_.onFrame) opts_.onFrame(r, hint, t);
    // The image grew where auto-scroll turns the page: its way back has helped.
    if (r.addedBottom > 0 || r.addedTop > 0) recoversWithoutGrowth_ = 0;
    account(r, t);
}

// The hint (#1515): auto-scroll's own wheel steps say how far the page will
// move, but not when, since an application animates a step over a few
// hundred milliseconds and the frames see it on the way. The tracker sums the
// hints of the frames since the one it last placed, so each frame gets the
// share of the movement still owed that its time since the frame before
// takes up of the step's expected animation; once that time is over, all of
// what is owed. A placed frame then says how far the page really moved, and
// what is owed is corrected by the difference. A person's scrolling, and the
// first step (the distance per notch is not known yet), give no hint.
TrackerHint ScrollSession::hintFor(int64_t t) {
    if (auto_ && phase_ == Phase::Settle && stats_.pxPerNotch <= 0 && stepSentAt_ >= 0 && lastStepMove_ > 0) {
        // The step that measures the distance per notch: how far it goes is
        // not known yet, but a smooth scroll only slows down, so the frame
        // after one that moved d moves 0 to d. Without it, a page that
        // repeats (the ruler fixture's 40 px rows) was ambiguous from the
        // step's last few pixels on: the tracker's own guess, the last
        // frame's speed times the frames since, only grew (#1534's engine).
        chainHinted_ = false;
        lastHintAt_ = t;
        return {true, int(lastStepMove_ / 2), int(lastStepMove_ / 2) + 4};
    }
    if (!auto_ || phase_ == Phase::Off || stats_.pxPerNotch <= 0) {
        chainHinted_ = false;
        lastHintAt_ = t;
        return {};
    }
    double share = 0;
    if (pending_ != 0 && stepSentAt_ >= 0) {
        const int64_t from = std::max(lastHintAt_, stepSentAt_);
        const int64_t end = stepSentAt_ + std::max(opts_.animationMs, animationMs_);
        if (t >= end || end <= from) share = pending_;
        else if (t > from) share = pending_ * double(t - from) / double(end - from);
    }
    pending_ -= share;
    attributed_ += share;
    lastHintAt_ = t;
    const long long target = std::llround(attributed_);
    const int offset = int(target - emitted_);
    emitted_ = target;
    return {true, offset, int(std::lround(0.35 * std::abs(share))) + 4};
}

void ScrollSession::account(const TrackResult& r, int64_t t) {
    const auto resetChain = [&] {
        emitted_ = 0;
        attributed_ = 0;
        chainHinted_ = true;
    };
    // The tracker lost the page and found it again: a frame placed, or one
    // proved where the last placed frame was. A frame let through as still
    // with nothing proved has not found it.
    if (r.state == TrackState::Lost) lostSincePlaced_ = true;
    else if (lostSincePlaced_ && (r.status == TrackStatus::Placed || (r.status == TrackStatus::NoMovement && !r.hintsKept))) {
        lostSincePlaced_ = false;
        if (!auto_ || phase_ == Phase::Off) ++stats_.foundAgain;
    }
    lastSkipped_ = r.status == TrackStatus::Skipped;
    // Whether this frame showed the page where the last placed frame had it.
    lastShowedStill_ = r.status == TrackStatus::NoMovement || (r.status == TrackStatus::Placed && r.offset == 0);
    // A jump up under auto-scroll is the user's scrolling only if the next
    // frame agrees: the page stays up, or goes on up. A frame that falls back
    // down onto auto-scroll's way was one picture out of place. On a starved
    // X server a grab caught the page half repainted, and the tracker placed
    // it 418 px up against a hint of 25 ± 13. The next frames were back on the
    // page's way, but the wheel had been handed back to nobody (#1535).
    const auto settleUp = [&](bool fellBack) {
        if (!upPending_) return;
        upPending_ = false;
        if (fellBack) upSinceStep_ = 0;
        else if (auto_ && phase_ != Phase::Off && phase_ != Phase::Recover) handBack(HandBack::Scrolled);
    };
    if (r.status == TrackStatus::NoMovement || (r.status == TrackStatus::Placed && r.offset <= 0)) settleUp(false);
    else if (r.status == TrackStatus::Placed) settleUp(true);
    switch (r.status) {
        case TrackStatus::Placed: {
            const long long d = r.offset;
            if (stepSentAt_ >= 0) {
                // What the page really did, against what the hints since the last placed frame said.
                pending_ -= chainHinted_ ? double(d) - double(emitted_) : double(d);
                if ((stepNotches_ > 0 && pending_ < 0) || (stepNotches_ < 0 && pending_ > 0)) pending_ = 0;
            }
            resetChain();
            if (d != 0) {
                movedYet_ = true;
                lastActivity_ = t;
                if (auto_) ++stats_.movedByPutgd;
                else ++stats_.movedByUser;
                if (auto_ && phase_ != Phase::Off) {
                    stepMoved_ += d;
                    lastStepMove_ = std::abs(d);
                    lastMoveAt_ = t;
                    if (stepFirstMoveAt_ < 0 && stepSentAt_ >= 0) stepFirstMoveAt_ = t;
                    if (d > 0) movedDown_ = true;
                    // The page went up while auto-scroll turns it down: the user scrolled.
                    // A person's wheel takes the page up over several frames and by a
                    // few notches; one jump back of a page that lays itself out anew
                    // (a header that shrinks, a banner) is not that. A frame found again
                    // after skipped ones went wherever the user had taken it.
                    if (d < 0 && phase_ != Phase::Recover && !r.recovered) upSinceStep_ -= d;
                    if (d < 0 && upSinceStep_ >= std::max(height_ / 4.0, 2 * stats_.pxPerNotch) && phase_ != Phase::Recover) upPending_ = true;
                }
                lastStill_ = false;
                stillRun_ = 0;
            } else {
                lastStill_ = true;
                ++stillRun_;
            }
            break;
        }
        case TrackStatus::NoMovement:
            // The page did not move. A frame proved still starts the tracker's
            // sum of hints again, so what the hints gave this stretch is owed
            // again. One still by the tracker's fallback (nothing proved) keeps
            // its sum for the next frame, and so does the session: handing it
            // out again counted it twice, and the tracker expected the page
            // further than the step could take it (#1543's review).
            if (!r.hintsKept) {
                if (chainHinted_) pending_ += attributed_;
                resetChain();
            }
            lastStill_ = true;
            ++stillRun_;
            break;
        case TrackStatus::Skipped:
            lastStill_ = false;
            stillRun_ = 0;
            break;
        case TrackStatus::CapReached: capped_ = true; break;
        case TrackStatus::Invalid: sizeChanged_ = true; break;
    }
}

bool ScrollSession::pump(int ms) {
    const int64_t until = clock_.nowMs() + ms;
    for (;;) {
        const int64_t left = until - clock_.nowMs();
        if (left <= 0) return true;
        if (auto frame = source_.nextFrame(int(left))) feed(*frame);
        else if (source_.lost()) return false;
    }
}

void ScrollSession::applyRequests() {
    const int64_t now = clock_.nowMs();
    if (opts_.activity) {
        const unsigned a = opts_.activity();
        if (a != seenActivity_) { seenActivity_ = a; lastActivity_ = now; }
    }
    if (!opts_.request) return;
    const auto [serial, on] = opts_.request();
    if (serial == seenSerial_) return;
    // Every request counts one. More than one since the last look, the latest
    // "on" while auto-scroll still runs here: an "off" came in between that
    // this loop never saw, and the main thread took the watch and the rest
    // down with it. It is taken as off, then on again, so that the run
    // prepares them again (beforeAuto) before its next step (CodeRabbit on
    // #1536).
    const bool missedOff = on && auto_ && serial - seenSerial_ >= 2;
    seenSerial_ = serial;
    lastActivity_ = now;
    if (on && !missedOff) {
        if (!auto_) startAuto();
        return;
    }
    if (phase_ == Phase::ToTop) endToTop(false, "auto-scroll was turned off on the way to the top");
    auto_ = false;
    phase_ = Phase::Off;
    pausedBy_ = HandBack::None;
    pending_ = 0;
    stepSentAt_ = -1;
    if (missedOff) {
        startAuto();
        return;
    }
    progress(true);
}

void ScrollSession::startAuto() {
    if (auto_ || stopping()) return;
    if (!readyChecked_) {
        readyChecked_ = true;
        if (!injector_.waitReady(*stop_)) {
            if (!stopping()) {
                notReady_ = true;
                stats_.lastError = "a modifier key stayed down, so no wheel step was sent";
            }
            return;
        }
    }
    // Lost: from here auto-scroll would only take the page further away. The
    // user brings it back where the image can find it again.
    if (tracker_->state() == TrackState::Lost) {
        pausedBy_ = HandBack::Lost;
        progress(true);
        return;
    }
    if (opts_.beforeAuto) {
        const size_t before = wheelPoints(frameW_, frameH_).size();
        away_ = opts_.beforeAuto();
        // The points moved with the rest: start again at the first.
        if (wheelPoints(frameW_, frameH_).size() != before) wheelAt_ = 0;
    }
    if (stopping()) return;
    auto_ = true;
    pausedBy_ = HandBack::None;
    recoversWithoutGrowth_ = 0;
    ++stats_.autoRuns;
    stillSteps_ = 0;
    patience_ = 1;
    recoverTries_ = 0;
    triesHere_ = 0;
    stepSentAt_ = -1;
    pending_ = 0;
    const int64_t now = clock_.nowMs();
    if (opts_.fromTop && !movedYet_ && !toTopDone_) {
        phase_ = Phase::ToTop;
        stats_.topAsked = true;
        toTopStart_ = toTopRested_ = now;
        toTopSent_ = -1;
        toTopNotchesNow_ = opts_.toTopNotches;
        toTopStill_ = 0;
        toTopPointsTried_ = 1;
        toTopMoved_ = false;
        checkDigest_.clear();
        // The screen as it is now is the way's first look.
        if (!lastPixels_.empty()) digestRows(FrameView{lastPixels_.data(), lastPixelsW_, lastPixelsH_, std::ptrdiff_t(lastPixelsW_) * 4}, digest_);
        else digest_.clear();
        nextStepAt_ = now;
    } else {
        phase_ = Phase::Step;
        nextStepAt_ = now;
    }
    progress(true);
}

void ScrollSession::handBack(HandBack why) {
    if (!auto_) return;
    if (phase_ == Phase::ToTop) endToTop(false, why == HandBack::Pointer ? "the way to the top was paused by the mouse" : "the way to the top was interrupted");
    auto_ = false;
    phase_ = Phase::Off;
    pausedBy_ = why;
    handBackAt_ = clock_.nowMs();
    ++stats_.handBacks;
    pending_ = 0;
    stepSentAt_ = -1;
    progress(true);
}

int ScrollSession::responseWaitMs() const {
    return std::clamp(std::max(opts_.responseMs, int(std::lround(opts_.responseFactor * stats_.maxResponseMs))), opts_.responseMs, opts_.responseMaxMs);
}

int ScrollSession::planNotches() const {
    // The first step learns the distance per notch; a still step is asked again with one.
    if (!movedDown_ || stillSteps_ > 0 || stats_.pxPerNotch <= 0) return 1;
    int n = int(std::floor(height_ * opts_.stepFraction / stats_.pxPerNotch));
    n = std::clamp(n, 1, opts_.maxNotches);
    if (stepCap_ > 0) n = std::min(n, stepCap_);
    return n;
}

bool ScrollSession::injected(bool sent) {
    if (sent) return true;
    if (pausedNow()) handBack(HandBack::Pointer);
    else if (!stopping()) {
        failed_ = true;
        stats_.lastError = "the desktop refused the scroll input";
    }
    return false;
}

// One wheel step with the pointer's visit (#1514): in, put there again once the
// application has seen it enter, the step, a moment for the application to take
// it with the pointer on it, and back to the rest. A pause ends the visit where
// it is: no wheel step and no way out follow, the pointer is the user's.
bool ScrollSession::visit(int notches) {
    const RunPoint at = wheelPoint();
    const auto leave = [&] {
        if (opts_.pointer && away_ && !pausedNow()) injector_.movePointer(away_->x, away_->y);
        if (opts_.blindVisits) blindUntil_ = clock_.nowMs() + 60;
    };
    // Every way out of a visit ends the blind interval, also one that sends
    // nothing more (a pause by the mouse, a refused injection): frames after
    // it are the page's again (CodeRabbit on #1528).
    const auto cutShort = [&] {
        if (opts_.blindVisits) blindUntil_ = clock_.nowMs() + 60;
        return false;
    };
    if (opts_.blindVisits) blindUntil_ = INT64_MAX;
    if (opts_.pointer) {
        if (!injected(injector_.movePointer(at.x, at.y))) return cutShort();
        pump(opts_.pointerSettleMs);
        if (!auto_ || stopping()) { leave(); return false; }
        if (pausedNow()) { handBack(HandBack::Pointer); return cutShort(); }
        if (!injected(injector_.movePointer(at.x, at.y))) return cutShort();
    }
    if (!auto_ || stopping()) { leave(); return false; }
    if (!injected(injector_.scroll(notches))) return cutShort();
    ++stats_.steps;
    stats_.notches += std::abs(notches);
    if (opts_.onStep) opts_.onStep(notches, at);
    if (opts_.pointer && away_) pump(opts_.leaveDelayMs);
    leave();
    return true;
}

void ScrollSession::autoTick() {
    const int64_t now = clock_.nowMs();
    if (phase_ == Phase::Step) {
        if (now < nextStepAt_) return;
        const int notches = planNotches();
        // Sent before the visit: the frames of the visit already belong to this step.
        stepSentAt_ = now;
        // Only a step that began with the page at rest measures the distance per notch.
        stepFromRest_ = restedBefore_;
        stepNotches_ = notches;
        stepMoved_ = 0;
        lastStepMove_ = 0;
        stepFirstMoveAt_ = lastMoveAt_ = -1;
        upSinceStep_ = 0;
        upPending_ = false;
        lastStill_ = false;
        stillRun_ = 0;
        if (stats_.pxPerNotch > 0) pending_ += notches * stats_.pxPerNotch;
        phase_ = Phase::Settle;
        if (!visit(notches)) { stepSentAt_ = -1; pending_ = 0; if (auto_) phase_ = Phase::Step; return; }
        return;
    }
    if (phase_ != Phase::Settle && phase_ != Phase::Recover) return;
    if (stepFirstMoveAt_ >= 0) {
        const bool quiet = opts_.restQuietMs > 0 && lastMoveAt_ >= 0 && now - lastMoveAt_ >= opts_.restQuietMs && now - lastFrameAt_ >= opts_.restQuietMs;
        if ((lastStill_ && stillRun_ >= opts_.restFrames && now - lastMoveAt_ >= opts_.settleMs) || quiet || now - stepSentAt_ >= opts_.stepTimeoutMs) { settled(now); return; }
        // Most of the step has moved: the next goes now. A browser's smooth
        // scroll eases out for half a second and more; the frames of its tail
        // and of the next step overlap as well as any.
        const double expected = std::abs(stepNotches_) * stats_.pxPerNotch;
        if (phase_ == Phase::Settle && opts_.nextAtShare > 0 && stillSteps_ == 0 && expected > 0 && double(stepMoved_) >= opts_.nextAtShare * expected && tracker_->state() != TrackState::Lost) {
            stats_.maxResponseMs = std::max(stats_.maxResponseMs, int(stepFirstMoveAt_ - stepSentAt_));
            if (stepCap_ > 0 && ++goodSteps_ >= 6) stepCap_ = 0;
            restedBefore_ = false;
            phase_ = Phase::Step;
            nextStepAt_ = now + opts_.stepGapMs;
        }
        return;
    }
    // While lost the tracker looks for the frame in the whole image and may
    // take a second a frame: a step back is judged on a few frames, not a time.
    if (phase_ == Phase::Recover && (now - stepSentAt_ < std::max<int64_t>(responseWaitMs(), opts_.recoverWaitMs) || framesSinceStep_ < 3)) {
        if (now - stepSentAt_ < 4 * int64_t(opts_.recoverWaitMs)) return;
    }
    if (now - stepSentAt_ >= int64_t(responseWaitMs()) * patience_) stillStep(now);
}

void ScrollSession::startRecover() {
    // Lost again and again with the image no taller: whatever the tracker
    // says it found in between was no way on (a page that keeps laying
    // itself out, a picture it cannot hold). The user takes it from here.
    if (phase_ != Phase::Recover && ++recoversWithoutGrowth_ > opts_.recoverTries) {
        recoversWithoutGrowth_ = 0;
        handBack(HandBack::Lost);
        return;
    }
    // Back by the last forward step, again at every try: the tracker finds
    // that picture in the image.
    if (phase_ != Phase::Recover) backNotches_ = std::max(1, std::abs(stepNotches_));
    phase_ = Phase::Recover;
    ++stats_.stepsBack;
    const int notches = -backNotches_;
    framesSinceStep_ = 0;
    stepSentAt_ = clock_.nowMs();
    stepNotches_ = notches;
    stepMoved_ = 0;
    lastStepMove_ = 0;
    stepFirstMoveAt_ = lastMoveAt_ = -1;
    lastStill_ = false;
    stillRun_ = 0;
    pending_ = stats_.pxPerNotch > 0 ? notches * stats_.pxPerNotch : 0;
    if (!visit(notches)) { stepSentAt_ = -1; pending_ = 0; if (auto_) phase_ = Phase::Step; }
}

// A step is over: what it owed is dropped, and the tracker forgets what the
// hints since its last placed frame still hold, or the next step's hints add
// to them (#1543's review). Not while lost: then that sum is where the page
// went.
void ScrollSession::dropOwed() {
    pending_ = 0;
    if (tracker_->state() == TrackState::Lost) return;
    // The step's last frame was skipped (one frame: the tracker is not lost
    // yet), so the tracker's reference has not advanced and where the page
    // went since it is not known. What the hints said stays with the tracker,
    // and with the session's chain, and the next step's add to it. Dropped, a
    // step that moved unseen leaves the next frame a step short, and on content
    // that repeats every step that is a place that fits: the image lost a
    // period (CodeRabbit on #1536). After a frame placed or taken still they
    // are dropped, as before: a step that moved nothing must not add to the
    // next (#1544).
    if (lastSkipped_) return;
    tracker_->forgetHints();
    emitted_ = 0;
    attributed_ = 0;
    chainHinted_ = true;
}

void ScrollSession::settled(int64_t now) {
    if (stepFirstMoveAt_ >= 0) stats_.maxResponseMs = std::max(stats_.maxResponseMs, int(stepFirstMoveAt_ - stepSentAt_));
    dropOwed();
    const bool measure = stepFromRest_;
    restedBefore_ = true;
    if (phase_ == Phase::Settle && measure && stepNotches_ > 0 && stepMoved_ > 0) {
        const double sample = double(stepMoved_) / stepNotches_;
        // A short step (the last, partial one at the end) keeps the distance per
        // notch, and so does one that moved far more than a notch can: a page
        // that laid itself out anew during the step (GitHub in Chrome gave
        // 540 px for a notch of 180, and every later step was one notch).
        if (stats_.pxPerNotch <= 0 || (sample >= 0.7 * stats_.pxPerNotch && sample <= 1.5 * stats_.pxPerNotch)) stats_.pxPerNotch = sample;
        if (lastMoveAt_ >= 0) animationMs_ = std::clamp(std::max(int(0.7 * animationMs_), int(lastMoveAt_ - stepSentAt_)), 120, 1500);
    }
    stillSteps_ = 0;
    patience_ = 1;
    if (tracker_->state() == TrackState::Lost) {
        if (phase_ == Phase::Recover && ++recoverTries_ >= opts_.recoverTries) { handBack(HandBack::Lost); return; }
        startRecover();
        return;
    }
    if (phase_ == Phase::Recover) {
        // Found again: smaller steps from here.
        recoverTries_ = 0;
        stepCap_ = std::max(1, planNotches() / 2);
        goodSteps_ = 0;
    } else if (stepCap_ > 0 && ++goodSteps_ >= 6) {
        stepCap_ = 0;
    }
    phase_ = Phase::Step;
    nextStepAt_ = now + opts_.stepGapMs;
}

void ScrollSession::stillStep(int64_t now) {
    dropOwed();
    if (phase_ == Phase::Recover) {
        if (tracker_->state() != TrackState::Lost) { recoverTries_ = 0; phase_ = Phase::Step; nextStepAt_ = now; return; }
        if (++recoverTries_ >= opts_.recoverTries) { handBack(HandBack::Lost); return; }
        startRecover();
        return;
    }
    if (tracker_->state() == TrackState::Lost) { startRecover(); return; }
    if (!movedDown_) {
        // Nothing has scrolled under the wheel yet: two single notches at each of its points.
        if (++triesHere_ >= 2) {
            triesHere_ = 0;
            if (++wheelAt_ >= int(wheelPoints(frameW_, frameRowsFull()).size())) {
                wheelAt_ = 0;
                handBack(HandBack::NoMovement);
                return;
            }
        }
        phase_ = Phase::Step;
        nextStepAt_ = now;
        return;
    }
    if (++stillSteps_ >= 2 && wheelAt_ != centreIndex()) {
        // Still twice, but not at the centre: the wheel may be over a part
        // that scrolls by itself once the page has moved (Python's
        // documentation has a side bar that sticks and then takes the wheel
        // at the left edge; a false end at 1,071 of 110,440 px, 2026-11-14).
        // The end is proved at the centre, which keeps the wheel from here on.
        wheelAt_ = centreIndex();
        stillSteps_ = 0;
        patience_ = 1;
        phase_ = Phase::Step;
        nextStepAt_ = now + opts_.stepGapMs;
        return;
    }
    if (stillSteps_ >= 2) {
        // The end of the page: twice the page did not move after a step. The
        // screen itself must agree, not only the stream: a stream that sent
        // nothing then (damage-driven, the screen still) has no frame from
        // after the steps, and one that went stale sends its last picture
        // again with new times (KWin resent it every second while Chrome went
        // on scrolling). Every end is taken from a fresh frame of the screen
        // (CodeRabbit on #1528).
        auto fresh = source_.freshFrame(2000);
        if (!fresh) {
            stats_.lastError = "the frame source gave no frame after the last wheel steps, so the end of the page is not proven";
            handBack(HandBack::NoMovement);
            return;
        }
        feed(*fresh);
        if (stepFirstMoveAt_ >= 0) { stillSteps_ = 0; return; }  // it had moved after all: settle as usual
        // Only a frame that shows the page where it was proves the end. A
        // stream that went quiet while the page moved (the restarted stream's
        // first frame came 56 s later showing the page elsewhere: a GitHub
        // pull request in Chrome ended at 37,096 of 44,491 px, 2026-11-14)
        // proves nothing.
        if (!lastShowedStill_) {
            stillSteps_ = 0;
            patience_ = 1;
            if (tracker_->state() == TrackState::Lost) startRecover();
            else { phase_ = Phase::Step; nextStepAt_ = now; }
            return;
        }
        // Frames that show nothing move nothing either: a blank stretch taller
        // than the region looks like the end, and the wheel may still be moving
        // the page under it. Not called the end of the page then (the capture
        // stops, as it cannot follow), and the result says so.
        if (lastFrameBlank_) stats_.endBlank = true;
        else stats_.reachedEnd = true;
        pageEnd_ = true;
        phase_ = Phase::Off;
        return;
    }
    // Once more, one notch with twice the patience: a busy page may answer late.
    patience_ = 2;
    phase_ = Phase::Step;
    nextStepAt_ = now + opts_.stepGapMs;
}

// The way to the top (fromTop): the frames are not the image's, so only
// their digests are compared; the image starts again at the top.
void ScrollSession::observeToTop(const FrameView& view, int64_t t) {
    digestRows(view, scratchDigest_);
    if (!digest_.empty() && toTopSent_ >= 0 && differingRows(scratchDigest_, digest_) > samePictureRows) {
        if (!toTopChanged_) stats_.maxResponseMs = std::max(stats_.maxResponseMs, int(t - toTopSent_));
        toTopChanged_ = true;
        toTopChangedAt_ = t;
    }
    std::swap(digest_, scratchDigest_);
    keepScreen(view);
}

void ScrollSession::keepScreen(const FrameView& view) {
    lastPixelsW_ = view.width;
    lastPixelsH_ = view.height;
    lastPixels_.resize(size_t(view.width) * view.height * 4);
    for (int y = 0; y < view.height; ++y) std::memcpy(&lastPixels_[size_t(y) * view.width * 4], view.row(y), size_t(view.width) * 4);
}

void ScrollSession::toTopTick() {
    const int64_t now = clock_.nowMs();
    if (toTopSent_ < 0) {
        if (now < nextStepAt_) return;
        if (now - toTopStart_ >= opts_.toTopLimitMs) { endToTop(false, "the page was still scrolling up when the time for the way to the top was over"); return; }
        if (now - toTopRested_ >= opts_.toTopRestlessMs) { endToTop(false, "the page never came to rest on the way to the top, so its top cannot be told"); return; }
        if (digest_.empty()) return;  // the first look at the screen
        if (checkDigest_.empty()) checkDigest_ = digest_;
        toTopSent_ = now;
        toTopChanged_ = false;
        toTopChangedAt_ = -1;
        if (!visit(-toTopNotchesNow_)) { toTopSent_ = -1; return; }
        ++stats_.toTopSteps;
        return;
    }
    const auto still = [&] {
        toTopSent_ = -1;
        if (toTopNotchesNow_ > opts_.toTopNotches) {
            // A larger step that moved nothing proves nothing: the plain step decides.
            toTopNotchesNow_ = opts_.toTopNotches;
            return;
        }
        if (++toTopStill_ < 2) { nextStepAt_ = now + 400; return; }
        // Twice still where no step of the way has moved the page: the wheel
        // may be over a part that takes no wheel, which says nothing of the
        // top. The top is proved only where the wheel is known to move the
        // page, or once every wheel point has been tried (one more still step
        // at each) and moved nothing: a page that does not scroll here, or
        // its top.
        const int points = int(wheelPoints(frameW_, frameRowsFull()).size());
        if (!toTopMoved_ && toTopPointsTried_ < points) {
            ++toTopPointsTried_;
            wheelAt_ = (wheelAt_ + 1) % points;
            toTopStill_ = 1;
            nextStepAt_ = now;
            return;
        }
        // Twice still: the screen itself must agree, not only the stream.
        auto fresh = source_.freshFrame(2000);
        if (!fresh) { endToTop(false, "the frame source gave no fresh frame after the last steps up, so the top of the page is not proven"); return; }
        feed(*fresh);
        if (phase_ != Phase::ToTop) return;
        if (differingRows(digest_, checkDigest_) > samePictureRows) { toTopStill_ = 0; toTopMoved_ = true; checkDigest_ = digest_; return; }
        endToTop(true, {});
    };
    if (toTopChanged_) {
        const bool rested = now - toTopChangedAt_ >= opts_.settleMs;
        if (!rested && now - toTopSent_ < opts_.stepTimeoutMs) return;
        if (differingRows(digest_, checkDigest_) <= samePictureRows) { still(); return; }
        // It moved: bigger steps while it keeps moving.
        toTopSent_ = -1;
        toTopStill_ = 0;
        toTopMoved_ = true;
        if (rested) toTopRested_ = now;
        checkDigest_ = digest_;
        toTopNotchesNow_ = std::min(2 * toTopNotchesNow_, std::max(opts_.toTopNotches, opts_.toTopMaxNotches));
        progress();
        return;
    }
    if (now - toTopSent_ >= responseWaitMs()) {
        toTopRested_ = now;
        still();
    }
}

void ScrollSession::endToTop(bool proved, const std::string& gaveUp) {
    if (phase_ != Phase::ToTop) return;
    const int64_t now = clock_.nowMs();
    stats_.toTopMs = now - toTopStart_;
    stats_.topReached = proved;
    if (!proved) stats_.toTopGaveUp = gaveUp;
    toTopDone_ = true;
    // An overscrolled view springs back where the desktop can say the wheel stopped.
    if (stats_.toTopSteps > 0 && auto_ && !stopping() && !pausedNow() && injector_.rest()) pump(opts_.settleMs * 2);
    phase_ = auto_ ? Phase::Step : Phase::Off;
    nextStepAt_ = now;
    // Where no step of the way moved the page, auto-scroll looks for a wheel
    // point that does, from the first.
    if (!toTopMoved_) wheelAt_ = 0;
    resetTracker();
    // The image starts with the screen as it is now.
    if (!lastPixels_.empty()) {
        const FrameView view{lastPixels_.data(), lastPixelsW_, lastPixelsH_, std::ptrdiff_t(lastPixelsW_) * 4};
        feedTracker(view, clock_.nowMs());
    }
    progress(true);
}

void ScrollSession::resetTracker() {
    canvas_.clear();
    tracker_ = std::make_unique<scroll::ScrollTracker>(canvas_, opts_.tracker);
    if (!opts_.excluded.empty()) tracker_->setExcluded(opts_.excluded);
    movedYet_ = false;
    // Known to move the page under the wheel only where a step of the way did.
    movedDown_ = toTopMoved_ && !digest_.empty();
    pending_ = 0;
    emitted_ = 0;
    attributed_ = 0;
    chainHinted_ = true;
}

SessionState ScrollSession::state() const {
    if (phase_ == Phase::ToTop) return SessionState::ScrollingToTop;
    if (tracker_->state() == TrackState::Lost) return SessionState::Lost;
    if (auto_) return SessionState::AutoScrolling;
    // Auto-scroll gave up lost and the user has brought the page back where
    // the image finds it: paused (by Lost) until Auto-scroll goes on.
    if (pausedBy_ != HandBack::None) return SessionState::Paused;
    if (!movedYet_) return SessionState::Waiting;
    return SessionState::Following;
}

void ScrollSession::progress(bool force) {
    const auto& totals = tracker_->totals();
    SessionProgress p;
    p.state = state();
    p.driver = auto_ ? Driver::Putgd : Driver::User;
    p.pausedBy = pausedBy_;
    p.height = tracker_->height();
    p.placed = totals.placed;
    p.skipped = totals.skipped;
    p.steps = stats_.steps;
    p.pixelsPerNotch = stats_.pxPerNotch;
    p.picture = totals.placed > 0;
    p.serial = seenSerial_;
    const int64_t now = clock_.nowMs();
    const bool changed = p.state != shown_.state || p.driver != shown_.driver || p.pausedBy != shown_.pausedBy || p.picture != shown_.picture;
    const bool grew = p.height != shown_.height || p.placed != shown_.placed || p.skipped != shown_.skipped || p.steps != shown_.steps;
    if (!force && !changed && !(grew && now - shownAt_ >= 100)) return;
    shown_ = p;
    shownAt_ = now;
    if (opts_.onProgress) opts_.onProgress(p);
}
}
