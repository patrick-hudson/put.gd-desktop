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
      tracker_(std::make_unique<scroll::ScrollTracker>(canvas_, opts_.tracker)), away_(opts_.away) {}
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
    const int rows = opts_.frameRows > 0 ? std::min(opts_.frameRows, height) : height;
    // A rest inside the frame (the bar inside the rectangle) is no edge to come in from.
    if (away_ && (away_->x < 0 || away_->y < 0 || away_->x >= width || away_->y >= height))
        points.push_back(entryPoint(*away_, opts_.entryInset, width, rows));
    for (int position = 0; position < 3; ++position) points.push_back(parkPoint(position, width, rows));
    return points;
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
    if (opts_.request) seenSerial_ = opts_.request().first;
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
    feed(*first);
    if (opts_.autoScroll) startAuto();
    progress(true);
    for (;;) {
        if (discard.load()) return done(SessionEnd::Discarded);
        if (stop.load()) {
            // Done on the way to the top: the image is the screen as it is now.
            if (phase_ == Phase::ToTop) endToTop(false, "the capture was finished on the way to the top");
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
        if (auto frame = source_.nextFrame(auto_ ? 15 : 40)) feed(*frame);
        else if (source_.lost()) return done(SessionEnd::SourceLost);
        if (sizeChanged_) return done(SessionEnd::FrameSizeChanged);
        if (capped_) return done(SessionEnd::Capped);
        if (clock_.nowMs() - lastActivity_ >= opts_.idleLimitMs) return done(SessionEnd::Idle);
        progress();
    }
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
    if (opts_.frameRows > 0 && opts_.frameRows < view.height) view.height = opts_.frameRows;
    // A visit's frames show the painted pointer over the page.
    if (blindUntil_ >= 0 && frame.timestampMs <= blindUntil_ && tracker_->totals().placed > 0) return;
    width_ = view.width;
    height_ = view.height;
    if (phase_ == Phase::ToTop) {
        observeToTop(view, frame.timestampMs);
        return;
    }
    feedTracker(view, frame.timestampMs);
}

void ScrollSession::feedTracker(const FrameView& view, int64_t t) {
    const TrackerHint hint = hintFor(t);
    const TrackResult r = tracker_->addFrame(view, hint);
    if (opts_.onFrame) opts_.onFrame(r, hint, t);
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
    const bool afterWait = stepSentAt_ >= 0 && t - stepSentAt_ >= responseWaitMs() * patience_;
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
                    lastMoveAt_ = t;
                    if (stepFirstMoveAt_ < 0 && stepSentAt_ >= 0) stepFirstMoveAt_ = t;
                    if (d > 0) movedDown_ = true;
                    // The page went up while auto-scroll turns it down: the user scrolled.
                    // (A frame found again after skipped ones went wherever the user had taken it.)
                    if (phase_ != Phase::Recover && d < -24 && !r.recovered) handBack(HandBack::Scrolled);
                }
                lastStill_ = false;
            } else {
                lastStill_ = true;
                if (afterWait) ++framesAfterWait_;
            }
            break;
        }
        case TrackStatus::NoMovement:
            // The page did not move: what the hints gave this stretch is still owed.
            if (chainHinted_) pending_ += attributed_;
            resetChain();
            lastStill_ = true;
            if (afterWait) ++framesAfterWait_;
            break;
        case TrackStatus::Skipped: lastStill_ = false; break;
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
    seenSerial_ = serial;
    lastActivity_ = now;
    if (on) {
        if (!auto_) startAuto();
        return;
    }
    if (phase_ == Phase::ToTop) endToTop(false, "auto-scroll was turned off on the way to the top");
    auto_ = false;
    phase_ = Phase::Off;
    pausedBy_ = HandBack::None;
    pending_ = 0;
    stepSentAt_ = -1;
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
        checkDigest_.clear();
        digest_.clear();
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
    if (opts_.blindVisits) blindUntil_ = INT64_MAX;
    if (opts_.pointer) {
        if (!injected(injector_.movePointer(at.x, at.y))) return false;
        pump(opts_.pointerSettleMs);
        if (!auto_ || stopping()) { leave(); return false; }
        if (pausedNow()) { handBack(HandBack::Pointer); return false; }
        if (!injected(injector_.movePointer(at.x, at.y))) return false;
    }
    if (!auto_ || stopping()) { leave(); return false; }
    if (!injected(injector_.scroll(notches))) return false;
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
        stepFirstMoveAt_ = lastMoveAt_ = -1;
        framesAfterWait_ = 0;
        lastStill_ = false;
        if (stats_.pxPerNotch > 0) pending_ += notches * stats_.pxPerNotch;
        phase_ = Phase::Settle;
        if (!visit(notches)) { stepSentAt_ = -1; pending_ = 0; if (auto_) phase_ = Phase::Step; return; }
        return;
    }
    if (phase_ != Phase::Settle && phase_ != Phase::Recover) return;
    if (stepFirstMoveAt_ >= 0) {
        if ((lastStill_ && now - lastMoveAt_ >= opts_.settleMs) || now - stepSentAt_ >= opts_.stepTimeoutMs) { settled(now); return; }
        // Most of the step has moved: the next goes now. A browser's smooth
        // scroll eases out for half a second and more; the frames of its tail
        // and of the next step overlap as well as any.
        const double expected = std::abs(stepNotches_) * stats_.pxPerNotch;
        if (phase_ == Phase::Settle && stillSteps_ == 0 && expected > 0 && double(stepMoved_) >= opts_.nextAtShare * expected && tracker_->state() != TrackState::Lost) {
            stats_.maxResponseMs = std::max(stats_.maxResponseMs, int(stepFirstMoveAt_ - stepSentAt_));
            if (stepCap_ > 0 && ++goodSteps_ >= 6) stepCap_ = 0;
            restedBefore_ = false;
            phase_ = Phase::Step;
            nextStepAt_ = now + opts_.stepGapMs;
        }
        return;
    }
    if (now - stepSentAt_ >= int64_t(responseWaitMs()) * patience_) stillStep(now);
}

void ScrollSession::startRecover() {
    // Back by the last step: the tracker finds that picture in the image.
    phase_ = Phase::Recover;
    ++stats_.stepsBack;
    const int notches = -std::max(1, std::abs(stepNotches_));
    stepSentAt_ = clock_.nowMs();
    stepNotches_ = notches;
    stepMoved_ = 0;
    stepFirstMoveAt_ = lastMoveAt_ = -1;
    framesAfterWait_ = 0;
    lastStill_ = false;
    pending_ = stats_.pxPerNotch > 0 ? notches * stats_.pxPerNotch : 0;
    if (!visit(notches)) { stepSentAt_ = -1; pending_ = 0; if (auto_) phase_ = Phase::Step; }
}

void ScrollSession::settled(int64_t now) {
    if (stepFirstMoveAt_ >= 0) stats_.maxResponseMs = std::max(stats_.maxResponseMs, int(stepFirstMoveAt_ - stepSentAt_));
    pending_ = 0;
    const bool measure = stepFromRest_;
    restedBefore_ = true;
    if (phase_ == Phase::Settle && measure && stepNotches_ > 0 && stepMoved_ > 0) {
        const double sample = double(stepMoved_) / stepNotches_;
        // A short step (the last, partial one at the end) keeps the distance per notch.
        if (stats_.pxPerNotch <= 0 || sample >= 0.7 * stats_.pxPerNotch) stats_.pxPerNotch = sample;
        if (lastMoveAt_ >= 0) animationMs_ = std::clamp(std::max(int(0.7 * animationMs_), int(lastMoveAt_ - stepSentAt_)), 120, 1500);
    }
    stillSteps_ = 0;
    patience_ = 1;
    if (tracker_->state() == TrackState::Lost) {
        if (phase_ == Phase::Recover && ++recoverTries_ >= 2) { handBack(HandBack::Lost); return; }
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
    pending_ = 0;
    if (phase_ == Phase::Recover) {
        if (tracker_->state() != TrackState::Lost) { recoverTries_ = 0; phase_ = Phase::Step; nextStepAt_ = now; return; }
        if (++recoverTries_ >= 2) { handBack(HandBack::Lost); return; }
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
    if (++stillSteps_ >= 2) {
        // The end of the page: twice the page was where it was in frames from
        // after the step's response time. A stream that sent nothing then
        // (damage-driven, the screen still) is asked for a frame of the screen.
        if (framesAfterWait_ == 0) {
            auto fresh = source_.freshFrame(2000);
            if (!fresh) {
                stats_.lastError = "the frame source gave no frame after the last wheel steps, so the end of the page is not proven";
                handBack(HandBack::NoMovement);
                return;
            }
            feed(*fresh);
            if (stepFirstMoveAt_ >= 0) { stillSteps_ = 0; return; }  // it had moved after all: settle as usual
        }
        stats_.reachedEnd = true;
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
        // Twice still: the screen itself must agree, not only the stream.
        auto fresh = source_.freshFrame(2000);
        if (!fresh) { endToTop(false, "the frame source gave no fresh frame after the last steps up, so the top of the page is not proven"); return; }
        feed(*fresh);
        if (phase_ != Phase::ToTop) return;
        if (differingRows(digest_, checkDigest_) > samePictureRows) { toTopStill_ = 0; checkDigest_ = digest_; return; }
        endToTop(true, {});
    };
    if (toTopChanged_) {
        const bool rested = now - toTopChangedAt_ >= opts_.settleMs;
        if (!rested && now - toTopSent_ < opts_.stepTimeoutMs) return;
        if (differingRows(digest_, checkDigest_) <= samePictureRows) { still(); return; }
        // It moved: bigger steps while it keeps moving.
        toTopSent_ = -1;
        toTopStill_ = 0;
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
    movedYet_ = false;
    movedDown_ = stats_.toTopSteps > 0 && !digest_.empty();
    pending_ = 0;
    emitted_ = 0;
    attributed_ = 0;
    chainHinted_ = true;
}

SessionState ScrollSession::state() const {
    if (phase_ == Phase::ToTop) return SessionState::ScrollingToTop;
    if (tracker_->state() == TrackState::Lost) return SessionState::Lost;
    if (auto_) return SessionState::AutoScrolling;
    if (pausedBy_ == HandBack::Lost) return SessionState::Lost;
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
