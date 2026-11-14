#include "ScrollRun.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <utility>

namespace putgd::record {
using scroll::kNoHint;
using scroll::StepResult;
using scroll::StepStatus;
namespace {
const char* const layoutNote = "the layout inside the selection changed during the capture; the image ends with the rows from before the change";
const char* const paceNote = "a part of the selection scrolls at a pace of its own (a minimap, a column with a parallax effect), so no single image shows it beside the page; the image ends with the rows from before it moved";
const char* noteFor(const StepResult& r) { return r.ownPace ? paceNote : layoutNote; }
}

const char* toString(RunResult r) {
    switch (r) {
        case RunResult::EndOfContent: return "EndOfContent";
        case RunResult::TopNotReached: return "TopNotReached";
        case RunResult::LimitReached: return "LimitReached";
        case RunResult::Stopped: return "Stopped";
        case RunResult::TimeLimit: return "TimeLimit";
        case RunResult::LostTrack: return "LostTrack";
        case RunResult::NoMovement: return "NoMovement";
        case RunResult::InjectorFailed: return "InjectorFailed";
        case RunResult::NoFrames: return "NoFrames";
        case RunResult::SourceLost: return "SourceLost";
        case RunResult::LayoutChanged: return "LayoutChanged";
        case RunResult::NotReady: return "NotReady";
    }
    return "?";
}

ScrollRun::ScrollRun(RunInjector& injector, RunFrames& source, RunClock& clock, RunOptions opts, scroll::RowSink sink, scroll::RowTakeBack takeBack)
    : injector_(injector), source_(source), clock_(clock), opts_(std::move(opts)), stitcher_(opts_.stitch, std::move(sink), std::move(takeBack)) {}

// Rule 2: the horizontal centre, a third of the way down. Rule 7's two other
// places for a region whose centre does not scroll: near the right edge, then
// two thirds down.
RunPoint ScrollRun::parkPoint(int position, int width, int height) {
    if (position == 1) return {std::max(0, width - std::max(12, width / 8)), height / 3};
    if (position == 2) return {width / 2, 2 * height / 3};
    return {width / 2, height / 3};
}

// Where the wheel is turned. A pointer that waits outside the rectangle
// (restAway) comes in by the shortest way first: the point entryInset inside
// the edge it waits beyond. On Plasma a pointer that goes back and forth over
// more than a hundred pixels several times a second is a pointer its user
// shakes to find it, and KWin blows it up to many times its size for as long
// as that lasts (the capture's own visits did that on a real desktop,
// 2026-11-12, with the stream's frames coming in bursts 350 ms apart while it
// was up). Where nothing scrolls at that edge the classic places follow.
RunPoint ScrollRun::entryPoint(RunPoint away, int entryInset, int width, int height) {
    const int inset = std::min({entryInset, width / 2, height / 2});
    return {std::clamp(away.x, inset, std::max(inset, width - 1 - inset)), std::clamp(away.y, inset, std::max(inset, height - 1 - inset))};
}
RunPoint ScrollRun::wheelPoint() const {
    if (!atEntry_ || !opts_.away) return parkPoint(stats_.parkPosition, last_.width, last_.height);
    return entryPoint(*opts_.away, opts_.entryInset, last_.width, last_.height);
}

bool ScrollRun::park() {
    holdOff();
    const RunPoint at = wheelPoint();
    return sent(injector_.movePointer(at.x, at.y));
}

// The pointer's place is asserted again before every injection: the user may
// have moved the mouse, and the wheel goes to whatever is under the pointer.
// A pause (the user took the pointer) ends the visit wherever it is: no wheel
// step and no way out follow, because the pointer is the user's now.
bool ScrollRun::inject(int notches) {
    if (stopped()) return false;
    holdOff();
    // On the way to the top of a desktop that paints the pointer into the
    // frames, the pointer is in the rectangle only for the wheel step itself.
    const bool visit = (opts_.restAway || (toTop_ && opts_.restAwayToTop)) && opts_.pointer && opts_.away && haveFrame_;
    // Stop found the pointer inside: no wheel step follows, the pointer leaves.
    const auto leave = [&] {
        if (!stopped()) holdOff();
        parked_ = -1;
        return sent(injector_.movePointer(opts_.away->x, opts_.away->y));
    };
    atEntry_ = atEntry_ && opts_.restAway && opts_.pointer && opts_.away;
    if (opts_.pointer && haveFrame_) {
        // Rows a frame shows above the parked pointer's margin are under the
        // pointer's influence unless it leaves again right after the wheel step.
        stitcher_.setTrustedFrom(visit ? 0 : wheelPoint().y + opts_.hoverMarginRows);
        stitcher_.setWheelColumn(wheelPoint().x);
        if (!park()) return false;
        if (parked_ != stats_.parkPosition) {
            // It just arrived here: let the application see it enter, then say it again.
            parked_ = stats_.parkPosition;
            clock_.sleepMs(opts_.pointerSettleMs);
            if (stopped()) { if (visit) leave(); return false; }
            if (!park()) return false;
        }
    }
    if (stopped()) { if (visit) leave(); return false; }
    holdOff();
    stats_.notchesInjected += std::abs(notches);
    lastInjectMs_ = clock_.nowMs();
    const bool wheel = sent(injector_.scroll(notches));
    if (wheel) { stepNotches_ += notches; ++stepInjections_; }
    if (!visit) return wheel;
    // The application gets the wheel step with the pointer still on it: a
    // step whose pointer had left before the application came to it was lost
    // or went to what was under the pointer by then.
    if (wheel && opts_.leaveDelayMs > 0) clock_.sleepMs(opts_.leaveDelayMs);
    return leave() && wheel;
}

ScrollRun::Screen ScrollRun::screenIsAsSeen() {
    if (!haveFrame_) return Screen::Unknown;
    std::optional<RunFrame> fresh = source_.freshFrame(opts_.firstFrameTimeoutMs);
    if (!fresh) {
        // No proof either way: the caller must not make "the content ended" of it.
        ++stats_.unprovenStill;
        return Screen::Unknown;
    }
    ++stats_.framesSeen;
    ++stats_.freshFrames;
    if (stitcher_.hasFrame() && (fresh->view.width != referenceWidth_ || fresh->view.height != referenceHeight_)) { layoutChanged_ = true; return Screen::Unknown; }
    tmpSig_.compute(fresh->view, opts_.stitch);
    bool same = lastSig_.height == tmpSig_.height && lastSig_.width == tmpSig_.width;
    if (same) {
        int top = 0, bottom = tmpSig_.height;
        if (stitcher_.hasFrame()) {
            top = stitcher_.movingBandTop();
            bottom = stitcher_.movingBandBottom();
        }
        same = !scroll::framesDiffer(lastSig_, tmpSig_, top, bottom, opts_.stitch, opts_.settleMaxChangedRows);
    }
    std::swap(lastSig_, tmpSig_);
    last_ = fresh->view;
    if (!same) ++stats_.quietStream;
    return same ? Screen::Same : Screen::Moved;
}

int ScrollRun::responseWaitMs() const {
    const int learned = int(std::lround(opts_.responseFactor * stats_.maxResponseMs));
    return std::clamp(std::max(opts_.responseMs, learned), opts_.settleMs, std::max(opts_.settleMs, opts_.responseMaxMs));
}

int ScrollRun::settleNeedMs() const {
    const int byCadence = int(std::lround(opts_.settleCadenceFactor * cadenceMs_));
    return std::clamp(byCadence, opts_.settleMs, std::max(opts_.settleMs, opts_.settleMaxMs));
}

void ScrollRun::progress(bool toTop) {
    if (!opts_.onProgress) return;
    opts_.onProgress({toTop, stitcher_.hasFrame() ? stitcher_.outputHeight() : 0, stats_.steps, stats_.pxPerNotch});
}

RunResult ScrollRun::finishWith(RunResult r) {
    // A refused injection after Stop is the stop, not an injector failure.
    if (r == RunResult::InjectorFailed && stopped()) r = RunResult::Stopped;
    // "End of content" says the whole scroll area, top to bottom.
    if (r == RunResult::EndOfContent && opts_.fromTop && !stats_.reachedTop) r = RunResult::TopNotReached;
    if (layoutChanged_) r = RunResult::LayoutChanged;
    stitcher_.finish();
    stats_.elapsedMs = clock_.nowMs() - startMs_;
    stats_.outputRows = stitcher_.hasFrame() ? stitcher_.outputHeight() : 0;
    return r;
}

std::optional<RunResult> ScrollRun::waitFailure(Wait w) {
    switch (w) {
        case Wait::Stopped: return RunResult::Stopped;
        case Wait::TimeLimit: return RunResult::TimeLimit;
        case Wait::NoFrame: stats_.lastError = "the frame source delivered no frame"; return RunResult::NoFrames;
        case Wait::LayoutChanged: return RunResult::LayoutChanged;
        case Wait::Lost: stats_.lastError = "the frame source ended"; return RunResult::SourceLost;
        case Wait::Settled: case Wait::TimedOut: case Wait::Changed: break;
    }
    return std::nullopt;
}

// Settled = no frame changed versus its predecessor for settleMs. "Changed" is
// decided on row signatures of the moving band: more than settleMaxChangedRows
// rows fail the loose same-y rule, more than the strict budget changed at all,
// or any margin row (scrollbar) changed. A caret, spinner or hover effect
// confined to two of eight bands therefore never keeps the frame "moving".
// With quietMs the picture is first given that long to begin to change: until
// a frame that arrived after the call began differs from its predecessor,
// only quietMs without one counts as settled.
ScrollRun::Wait ScrollRun::waitSettled(int timeoutMs, int quietMs) {
    const int64_t start = clock_.nowMs();
    const int quiet = std::max(quietMs, settleNeedMs());
    const int64_t deadline = start + std::max(timeoutMs, quiet);
    int64_t lastChange = start;  // at least settleMs after the injection
    bool responded = false;
    firstChangeMs_ = -1;
    // A response wait counts from the wheel step, not from this call: the
    // pointer stays a moment after the step (leaveDelayMs), and a page that
    // scrolls at once has moved by the time the call begins. Its one changed
    // frame was then "from before the call", nothing changed after it, and
    // every step waited out the whole response time (1.6 s a step on Plasma
    // with a browser that does not animate its scrolling, 2026-11-12).
    const int64_t since = quietMs > 0 && lastInjectMs_ >= 0 ? std::min(start, lastInjectMs_) : start;
    // The cadence is measured between frames taken one after the other in one
    // wait; the time the loop spent elsewhere between two waits is not a gap.
    lastFrameMs_ = -1;
    for (;;) {
        if (stopped()) return Wait::Stopped;
        // Frames read while the user has the pointer may show what it does to the page.
        if (!afterStop_) holdOff();
        const int64_t now = clock_.nowMs();
        if (activeMs() >= opts_.totalTimeLimitMs) return Wait::TimeLimit;
        const int need = responded ? settleNeedMs() : quiet;
        if (haveFrame_ && now - lastChange >= need) return Wait::Settled;
        if (source_.lost()) return Wait::Lost;
        if (now >= deadline) {
            if (!haveFrame_) return Wait::NoFrame;
            ++stats_.settleTimeouts;
            return Wait::TimedOut;
        }
        const int64_t wakeAt = haveFrame_ ? std::min(lastChange + need, deadline) : deadline;
        // In slices, so that Stop is seen soon during a long response wait.
        const int wait = int(std::clamp<int64_t>(wakeAt - now, 1, 100));
        std::optional<RunFrame> f = source_.nextFrame(wait);
        if (!f) continue;
        if (stitcher_.hasFrame() && (f->view.width != referenceWidth_ || f->view.height != referenceHeight_)) return Wait::LayoutChanged;
        ++stats_.framesSeen;
        if (lastFrameMs_ >= 0 && f->timestampMs > lastFrameMs_ && f->timestampMs - lastFrameMs_ <= opts_.cadenceIdleMs)
            cadenceMs_ = std::max(0.9 * cadenceMs_, double(f->timestampMs - lastFrameMs_));
        lastFrameMs_ = f->timestampMs;
        tmpSig_.compute(f->view, opts_.stitch);
        bool changed = true;
        if (haveFrame_ && lastSig_.height == tmpSig_.height && lastSig_.width == tmpSig_.width) {
            int top = 0, bottom = tmpSig_.height;
            if (stitcher_.hasFrame()) {
                top = stitcher_.movingBandTop();
                bottom = stitcher_.movingBandBottom();
            }
            changed = scroll::framesDiffer(lastSig_, tmpSig_, top, bottom, opts_.stitch, opts_.settleMaxChangedRows);
        }
        if (changed) {
            lastChange = std::max(lastChange, f->timestampMs);
            // A frame from before the call began (before the wheel step, for a
            // response wait) cannot show what the call waits for.
            if (!haveFrame_ || f->timestampMs >= since) {
                if (!responded) firstChangeMs_ = std::max(start, f->timestampMs);
                responded = true;
            }
        }
        std::swap(lastSig_, tmpSig_);
        last_ = f->view;
        haveFrame_ = true;
    }
}

// The end of the content, proved. A still step says only that the picture did
// not move in the time it was given. A page that works through its wheel
// notches one after another and paints once none is waiting is still for as
// long as it is busy: two such steps ended a capture as complete with 1,020 of
// 24,000 rows, the page still working through what it had been sent (the
// tester's non-fixed fixture on Cinnamon, 2026-11-12). A page answers its wheel
// steps in the order they came. So a notch up: once the picture has moved up,
// everything sent before it has been answered, and none of it took the page
// past what the image ends on. Then one notch more than that down: a page at
// its end comes back to exactly the picture it ended on; one that is not goes
// a notch further (a step that was lost, or content that has loaded
// meanwhile), and that step goes on as any other. A step up that is not
// answered is sent again, twice as large and with twice the patience (a view
// overscrolled at its edge spends the first steps back on that: GTK 4 fed a
// continuous axis keeps up to 100 px), twice; when none is answered the end
// is not proved.
ScrollRun::EndProof ScrollRun::proveEnd(StepResult& found, std::optional<RunResult>& fail) {
    const scroll::FrameSignatures atEnd = lastSig_;
    const int64_t stillAt = lastInjectMs_;
    StepResult s;
    const auto look = [&](int notches, int hint, int unc) {
        s = stitcher_.addFrame(last_, lastSig_, hint, unc);
        if (opts_.onStep) opts_.onStep(s, hint, notches);
        // The stream may have sent nothing: the screen itself.
        if (s.status == StepStatus::NoMovement && screenIsAsSeen() == Screen::Moved) {
            s = stitcher_.addFrame(last_, lastSig_, hint, unc);
            if (opts_.onStep) opts_.onStep(s, hint, notches);
        }
    };
    const auto refused = [&] {
        stats_.lastError = "injector refused the scroll event";
        fail = RunResult::InjectorFailed;
        return EndProof::Unproved;
    };
    const auto failed = [&](Wait w) {
        fail = waitFailure(w);
        return fail.has_value();
    };
    // A fresh frame of another size (screenIsAsSeen).
    const auto resized = [&] {
        if (layoutChanged_) fail = RunResult::LayoutChanged;
        return layoutChanged_;
    };
    int up = 0, back = 0;
    bool answered = false, placed = false;
    for (int attempt = 0; attempt < 3 && !answered; ++attempt) {
        const int step = 1 << attempt;
        if (!inject(-step)) return refused();
        up += step;
        const int patience = std::min(std::max(opts_.settleMs, opts_.responseMaxMs), responseWaitMs() << attempt);
        if (failed(waitSettled(opts_.stepTimeoutMs, patience))) return EndProof::Unproved;
        look(-step, kNoHint, 0);
        if (resized()) return EndProof::Unproved;
        switch (s.status) {
            case StepStatus::NoMovement: break;  // not answered yet
            case StepStatus::Reversed: answered = placed = true; back = -s.offset; break;
            case StepStatus::NoMatch: case StepStatus::Ambiguous: answered = true; break;  // moved, the pixels cannot say where to
            default:
                // Further down than the picture showed (or the cap, or a change
                // of layout): the page had not answered every step down. How long
                // it took is the least its answers take.
                if (s.status == StepStatus::Appended && firstChangeMs_ >= 0 && stillAt >= 0)
                    stats_.maxResponseMs = std::max(stats_.maxResponseMs, int(firstChangeMs_ - stillAt));
                found = s;
                return EndProof::Moved;
        }
    }
    if (!answered) return EndProof::Unproved;
    const auto further_down = [&] {
        return (s.status == StepStatus::Appended && s.appendedRows > 0) || s.status == StepStatus::LimitReached || s.status == StepStatus::LayoutChanged || s.status == StepStatus::InvalidFrame;
    };
    // From where the step up went, one notch more than it is further down than
    // the stitcher can place in one frame where a notch is near half the band
    // (a short selection): back down by as much first, then the one notch more.
    const int band = stitcher_.movingBandBottom() - stitcher_.movingBandTop();
    if (placed && (up + 1) * std::max(1.0, stats_.pxPerNotch) > opts_.maxStepFraction * band) {
        if (!inject(up)) return refused();
        if (failed(waitSettled(opts_.stepTimeoutMs, responseWaitMs()))) return EndProof::Unproved;
        look(up, back, int(std::ceil(up * (stats_.maxPxPerNotch - stats_.minPxPerNotch))));
        if (resized()) return EndProof::Unproved;
        if (further_down()) {
            found = s;
            return EndProof::Moved;
        }
        if (s.status == StepStatus::Appended) back -= s.offset;  // no new rows: where it ended, or above
        else if (s.status != StepStatus::NoMovement) return EndProof::Unproved;
        placed = back > 0;
        up = 0;
    }
    if (!inject(up + 1)) return refused();
    if (failed(waitSettled(opts_.stepTimeoutMs, responseWaitMs()))) return EndProof::Unproved;
    int top = 0, bottom = lastSig_.height;
    if (stitcher_.hasFrame()) {
        top = stitcher_.movingBandTop();
        bottom = stitcher_.movingBandBottom();
    }
    // Exactly the picture it ended on: the stitcher is told where that is (it
    // keeps its reference and its footer from the frames it was given).
    const bool same = atEnd.height == lastSig_.height && atEnd.width == lastSig_.width && !scroll::framesDiffer(atEnd, lastSig_, top, bottom, opts_.stitch, opts_.settleMaxChangedRows);
    // Otherwise, where a page that is not at its end would be: a notch below where it ended.
    const int further = (placed ? back : 0) + std::max(1, int(std::lround(stats_.pxPerNotch)));
    look(up + 1, same ? (placed ? back : 0) : further, same ? 0 : int(std::ceil((up + 1) * (stats_.maxPxPerNotch - stats_.minPxPerNotch))));
    if (resized()) return EndProof::Unproved;
    // Back where it ended, as the stitcher places it, and nothing new below.
    if (placed ? s.status == StepStatus::Appended && s.appendedRows == 0 && s.offset == back : s.status == StepStatus::NoMovement) return EndProof::Proved;
    if (further_down()) {
        found = s;
        return EndProof::Moved;
    }
    return EndProof::Unproved;
}

int ScrollRun::planNotches() const {
    const double px = stats_.pxPerNotch;
    if (px <= 0) return 1;
    const int band = stitcher_.movingBandBottom() - stitcher_.movingBandTop();
    double maxPx = opts_.maxStepFraction * band;
    double targetPx = opts_.targetStepFraction * band;
    if (opts_.pointer) {
        // Rule 4: the appended rows are the frame's last rows; keep them below
        // the parked pointer so a hover effect under it never reaches the image.
        const RunPoint at = wheelPoint();
        maxPx = std::min(maxPx, double(std::max(0, stitcher_.movingBandBottom() - at.y - opts_.hoverMarginRows)));
        targetPx = std::min(targetPx, maxPx);
    }
    // Keep informative rows of the reference in the overlap when it has any, so
    // that a blank/flat lower part does not force a hint-only (blind) step.
    const int evidence = stitcher_.maxEvidenceStep();
    if (evidence >= 0) {
        maxPx = std::min(maxPx, double(evidence));
        targetPx = std::min(targetPx, maxPx);
    } else if (stats_.minPxPerNotch > 0 && stats_.maxPxPerNotch > 1.03 * stats_.minPxPerNotch) {
        // Blind (hint-only) step and the app's px-per-notch varies: keep it to one
        // notch so the unverifiable error stays as small as possible.
        return 1;
    }
    int n = int(std::lround(targetPx / px));
    n = std::max(1, n);
    while (n > 1 && n * px > maxPx) --n;
    n = std::min({n, notchCap_, opts_.maxNotchesPerStep});
    return std::max(1, n);
}

// The quick steps of the way up: only whether the picture has begun to change.
// A frame from before the call cannot show what the call waits for.
ScrollRun::Wait ScrollRun::waitChanged(int quietMs) {
    const int64_t start = clock_.nowMs();
    const int64_t deadline = start + quietMs;
    beforeSig_ = lastSig_;
    for (;;) {
        if (stopped()) return Wait::Stopped;
        holdOff();
        const int64_t now = clock_.nowMs();
        if (activeMs() >= opts_.totalTimeLimitMs) return Wait::TimeLimit;
        if (source_.lost()) return Wait::Lost;
        if (now >= deadline) return Wait::Settled;
        std::optional<RunFrame> f = source_.nextFrame(int(std::clamp<int64_t>(deadline - now, 1, 100)));
        if (!f) continue;
        ++stats_.framesSeen;
        tmpSig_.compute(f->view, opts_.stitch);
        const bool changed = f->timestampMs >= start && scroll::framesDiffer(beforeSig_, tmpSig_, 0, tmpSig_.height, opts_.stitch, opts_.settleMaxChangedRows);
        std::swap(lastSig_, tmpSig_);
        last_ = f->view;
        if (changed) return Wait::Changed;
    }
}

// Step 0. No stitching here, only frame comparison: the capture has not begun.
std::optional<RunResult> ScrollRun::scrollToTop(bool resumed) {
    // Also when a pause interrupts it: the time is counted and the flag cleared.
    struct Done {
        ScrollRun& run;
        int64_t begin, before;
        ~Done() { run.stats_.toTopMs = before + run.clock_.nowMs() - begin; run.toTop_ = false; }
    } done{*this, clock_.nowMs(), resumed ? stats_.toTopMs : 0};
    toTop_ = true;
    return wayToTop();
}

std::optional<RunResult> ScrollRun::wayToTop() {
    if (auto f = waitFailure(waitSettled(opts_.firstFrameTimeoutMs))) return f;
    settledSig_ = lastSig_;
    progress(true);
    const int64_t begin = clock_.nowMs();
    const int64_t limit = std::min<int64_t>(opts_.toTopLimitMs, opts_.totalTimeLimitMs / 2);
    // When a check last found the picture at rest.
    int64_t rested = begin;
    // A wheel step can go nowhere (the first one after the pointer arrived was
    // lost on Plasma; real desktop run of 2026-11-10), so one still step is not
    // the top: like the end of the content, it takes two in a row.
    int still = 0;
    int notches = opts_.toTopNotches;
    // Quick steps sent since the last check.
    int unchecked = 0;
    bool quick = opts_.toTopCheckEvery > 1;
    for (;;) {
        // Giving up is said (RunStats::toTopGaveUp), and the capture goes on
        // from here: decided right after a check, when the picture has had
        // its time to answer the last step and to settle.
        const int64_t now = clock_.nowMs();
        if (unchecked == 0 && now - begin >= limit) {
            stats_.toTopGaveUp = "the page was still scrolling up when the time for the way to the top was over";
            break;
        }
        if (unchecked == 0 && now - rested >= opts_.toTopRestlessMs) {
            stats_.toTopGaveUp = "the page never came to rest on the way to the top, so its top cannot be told";
            break;
        }
        if (!inject(-notches)) {
            if (!stopped()) stats_.lastError = "injector refused the scroll-to-top event";
            return stopped() ? RunResult::Stopped : RunResult::InjectorFailed;
        }
        ++stats_.toTopSteps;
        const int64_t injectedAt = clock_.nowMs();
        Wait w;
        if (quick && still == 0 && unchecked + 1 < opts_.toTopCheckEvery) {
            w = waitChanged(responseWaitMs());
            if (auto f = waitFailure(w)) return f;
            if (settledSig_.height != lastSig_.height || settledSig_.width != lastSig_.width) return RunResult::LayoutChanged;
            if (w == Wait::Changed) {
                // On its way, as far as one frame can tell: the next step at once.
                ++unchecked;
                continue;
            }
            // Nothing changed in the whole response time: that is a check.
            w = waitSettled(opts_.stepTimeoutMs);
        } else {
            w = waitSettled(opts_.stepTimeoutMs, responseWaitMs());
        }
        if (auto f = waitFailure(w)) return f;
        if (settledSig_.height != lastSig_.height || settledSig_.width != lastSig_.width) return RunResult::LayoutChanged;
        if (w == Wait::Settled) rested = clock_.nowMs();
        const int sinceCheck = std::exchange(unchecked, 0);
        if (!scroll::framesDiffer(settledSig_, lastSig_, 0, lastSig_.height, opts_.stitch, opts_.settleMaxChangedRows)) {
            // The page is where the last check found it. Quick steps in between
            // that looked like movement were none: every step is checked from here on.
            if (sinceCheck > 0) quick = false;
            if (notches > opts_.toTopNotches) {
                // A larger step that moved nothing proves nothing: the plain step decides.
                notches = opts_.toTopNotches;
                continue;
            }
            if (++still >= 2) {
                // The stream may only have gone quiet: look at the screen itself.
                const Screen screen = screenIsAsSeen();
                if (screen == Screen::Moved) {
                    still = 0;
                    stats_.toTopMoved = true;
                    settledSig_ = lastSig_;
                    continue;
                }
                stats_.reachedTop = screen == Screen::Same;
                if (!stats_.reachedTop) stats_.toTopGaveUp = "the frame source gave no fresh frame after the last steps up, so the top of the page is not proven";
                break;
            }
            const int age = int(clock_.nowMs() - injectedAt);
            if (age < opts_.endConfirmDelayMs) clock_.sleepMs(opts_.endConfirmDelayMs - age);
            continue;
        }
        if (firstChangeMs_ >= 0) stats_.maxResponseMs = std::max(stats_.maxResponseMs, int(firstChangeMs_ - injectedAt));
        still = 0;
        stats_.toTopMoved = true;
        settledSig_ = lastSig_;
        notches = std::min(2 * notches, std::max(opts_.toTopNotches, opts_.toTopMaxNotches));
        progress(true);
    }
    // Wheel steps against the top can leave the view overscrolled, with its
    // edge effect in the picture and the first steps down spent on it (GTK 4
    // fed a continuous axis). Where the desktop can be told that the wheel has
    // stopped, the view springs back before the first frame is taken.
    if (stats_.toTopSteps > 0 && !stopped() && (holdOff(), injector_.rest())) {
        lastInjectMs_ = clock_.nowMs();
        if (auto f = waitFailure(waitSettled(opts_.stepTimeoutMs, responseWaitMs()))) return f;
    }
    return std::nullopt;
}

// Stop while the page was scrolling to its top: nothing more is injected, and
// the picture on the screen when it has come to rest is the image (one
// viewport, wherever the page then is). The last wheel steps may not have
// reached the screen yet, so they get their response time; and what the stream
// last sent may be older than what the screen shows (KWin's region stream sent
// nothing at all for two batches of wheel steps, and Stop returned the viewport
// from before them; real desktop run of 2026-11-10), so the image is a frame
// grabbed from the screen after the last injection that a second look agrees
// with. The pointer stays where it was parked, so a hover effect under it can
// be in this image; moving it away would be an injection after Stop. Where the
// compositor paints the pointer itself into the frames (restAwayToTop) it was
// never left in the rectangle between two wheel steps, so it is not in this one.
RunResult ScrollRun::viewportAfterStop() {
    if (!haveFrame_ || (opts_.discarded && opts_.discarded())) return RunResult::Stopped;
    afterStop_ = true;
    const int sinceInjection = lastInjectMs_ < 0 ? responseWaitMs() : int(clock_.nowMs() - lastInjectMs_);
    Wait w = waitSettled(opts_.stepTimeoutMs, std::max(0, responseWaitMs() - sinceInjection));
    // A fresh frame that differs from the last one seen is itself only one
    // look at a page that may still be moving: it is asked again until two
    // agree, a few times at most.
    bool proven = false;
    for (int look = 0; !proven && (w == Wait::Settled || w == Wait::TimedOut) && look < opts_.stopLooks; ++look) {
        const Screen screen = screenIsAsSeen();
        if (screen == Screen::Unknown) break;
        proven = screen == Screen::Same;
        if (!proven) w = waitSettled(opts_.stepTimeoutMs);
    }
    afterStop_ = false;
    if (w != Wait::Settled && w != Wait::TimedOut) return RunResult::Stopped;
    stats_.stopViewportProven = proven;
    if (stitcher_.addFrame(last_, lastSig_, kNoHint).status == StepStatus::First) stats_.stoppedAtTop = true;
    return RunResult::Stopped;
}

// A pause: nothing is sent until it ends. Stop and Cancel end it at once; so
// does a pause that lasts pauseLimitMs (as Stop: the image is kept), so that a
// capture nobody comes back to does not hold the session for ever.
std::optional<RunResult> ScrollRun::holdPaused() {
    const int64_t began = clock_.nowMs();
    ++stats_.pauses;
    std::optional<RunResult> end;
    while (!end) {
        if (stopped()) end = RunResult::Stopped;
        else if (!pausedNow()) break;
        else if (clock_.nowMs() - began >= opts_.pauseLimitMs) {
            stats_.pauseExpired = true;
            end = RunResult::Stopped;
        } else clock_.sleepMs(50);
    }
    const int64_t held = clock_.nowMs() - began;
    pausedMs_ += held;
    stats_.pausedMs += held;
    return end;
}

// Resume looks at the screen before anything more is sent: the user may have
// scrolled the page, and the step the pause interrupted may have been seen
// halfway or not at all. The pointer goes back to its place outside, what it
// did to the page gets its time to go, and the picture at rest is placed
// against the last stitched frame by its pixels alone (no hint: where the
// wheel would have taken the page says nothing once the user had it). Where
// the page is, or one placed frame down or up, the capture goes on from there.
// Where it cannot be placed and the pause came after one plain wheel step,
// that step is scrolled back and the last stitched frame looked for where it
// was; otherwise the capture ends as lost track with the exact image so far.
std::optional<RunResult> ScrollRun::resync(StepResult& seen) {
    const int inFlight = stepInjections_ == 1 ? stepNotches_ : 0;
    stepNotches_ = stepInjections_ = 0;
    if (opts_.pointer && opts_.away) {
        holdOff();
        if (!sent(injector_.movePointer(opts_.away->x, opts_.away->y))) {
            stats_.lastError = "injector refused the pointer motion";
            return RunResult::InjectorFailed;
        }
        parked_ = -1;
        if (opts_.awayLingerMs > 0) clock_.sleepMs(opts_.awayLingerMs);
    }
    const auto look = [&](int hint) -> std::optional<RunResult> {
        if (auto f = waitFailure(waitSettled(opts_.stepTimeoutMs, responseWaitMs()))) return f;
        // The stream may have sent nothing while the loop was not reading.
        screenIsAsSeen();
        if (layoutChanged_) return RunResult::LayoutChanged;
        seen = stitcher_.addFrame(last_, lastSig_, hint);
        if (opts_.onStep) opts_.onStep(seen, hint, 0);
        return std::nullopt;
    };
    const auto placed = [&] { return seen.status == StepStatus::NoMovement || seen.status == StepStatus::Appended || seen.status == StepStatus::Reversed; };
    if (auto f = look(kNoHint)) return f;
    if (!placed() && (seen.status == StepStatus::NoMatch || seen.status == StepStatus::Ambiguous)) {
        // A hover effect that had not gone yet: once more.
        if (auto f = look(kNoHint)) return f;
    }
    if (!placed() && inFlight != 0 && (seen.status == StepStatus::NoMatch || seen.status == StepStatus::Ambiguous)) {
        if (!inject(-inFlight)) {
            stats_.lastError = "injector refused the scroll-back event";
            return RunResult::InjectorFailed;
        }
        ++stats_.scrollBacks;
        stepNotches_ = stepInjections_ = 0;
        if (auto f = look(0)) return f;
    }
    switch (seen.status) {
        case StepStatus::NoMovement: case StepStatus::Appended: case StepStatus::Reversed: return std::nullopt;
        case StepStatus::LimitReached: return RunResult::LimitReached;
        case StepStatus::InvalidFrame: stats_.lastError = "the frame size changed"; return RunResult::LayoutChanged;
        case StepStatus::LayoutChanged: stats_.lastError = noteFor(seen); return RunResult::LostTrack;
        default:
            stats_.lastError = "the page moved while the capture was paused and its place could not be found again";
            return RunResult::LostTrack;
    }
}

std::optional<RunResult> ScrollRun::start(bool toTop) {
    bool resumed = false;
    for (;;) {
        try {
            if (toTop)
                if (auto f = scrollToTop(resumed)) return *f == RunResult::Stopped ? viewportAfterStop() : *f;
            // Rule 1: the first frame is grabbed with the pointer outside the region.
            if (opts_.pointer && opts_.away) {
                if (stopped()) return RunResult::Stopped;
                holdOff();
                if (!sent(injector_.movePointer(opts_.away->x, opts_.away->y))) {
                    stats_.lastError = "injector refused the pointer motion";
                    return RunResult::InjectorFailed;
                }
                parked_ = -1;
                if (opts_.awayLingerMs > 0) clock_.sleepMs(opts_.awayLingerMs);
                if (stopped()) return RunResult::Stopped;
            }
            if (auto f = waitFailure(waitSettled(opts_.firstFrameTimeoutMs))) return f;
            holdOff();
            referenceWidth_ = last_.width; referenceHeight_ = last_.height;
            if (stitcher_.addFrame(last_, lastSig_, kNoHint).status != StepStatus::First) {
                stats_.lastError = "the first frame was refused";
                return RunResult::NoFrames;
            }
            stepNotches_ = stepInjections_ = 0;
            return std::nullopt;
        } catch (const Interrupted&) {
            // Nothing is stitched yet: Stop keeps the viewport as on the way up,
            // and Resume goes up again from wherever the user left the page.
            if (auto end = holdPaused()) return *end == RunResult::Stopped ? viewportAfterStop() : *end;
            resumed = toTop;
            toTop = opts_.fromTop;
        }
    }
}

RunResult ScrollRun::run(const std::atomic<bool>* stop) {
    stop_ = stop;
    startMs_ = clock_.nowMs();
    notchCap_ = opts_.maxNotchesPerStep;
    atEntry_ = opts_.restAway && opts_.pointer && opts_.away.has_value();
    static const std::atomic<bool> never{false};
    if (!injector_.waitReady(stop ? *stop : never)) {
        if (!stopped()) stats_.lastError = "the modifier keys were never released";
        return finishWith(stopped() ? RunResult::Stopped : RunResult::NotReady);
    }
    if (auto f = start(opts_.fromTop)) return finishWith(*f);
    progress(false);

    int notches = 1;
    bool endPending = false, moved = false;
    // How far the last step that moved fell short of the way its notches ask
    // (0: it went all of it, or no way was known). A short step is the last,
    // partial one at the end of the content, or a step seen halfway whose rest
    // comes with the next: that step then moves more than it asks, so its
    // distance teaches nothing about one notch, and its hint is that much less
    // sure. A page between tables that repeat had a step of 60 px for 200
    // asked and then one of 340 taken for 196, beside the hint, in an image
    // that ended "end of content"; Chromium on Plasma learned 309 px a notch
    // where a notch is 180 from such a pair, and every later step looked short
    // against that (2026-11-12).
    int owed = 0;
    // The step before this one showed no movement. If this one moves, the
    // earlier wheel step may have arrived late and be part of what is seen, so
    // the distance says nothing about one notch and no distance is expected of
    // it. (Seen on Plasma through XWayland: a burst of notches landed after
    // the settle time; real desktop run of 2026-11-10.)
    bool afterStill = false;
    int oneNotchFailures = 0, stillAtPark = 0;

    for (;;) {
        try {
        if (stopped()) return finishWith(RunResult::Stopped);
        if (activeMs() >= opts_.totalTimeLimitMs) return finishWith(RunResult::TimeLimit);

        stepNotches_ = stepInjections_ = 0;
        if (!inject(notches)) {
            stats_.lastError = "injector refused the scroll event";
            return finishWith(RunResult::InjectorFailed);
        }
        ++stats_.steps;
        const int64_t injectedAt = clock_.nowMs();
        // The step that would confirm an end gets twice the time to be
        // answered: a page may show the first notch of a step at once and the
        // rest only once it has worked through them all (see proveEnd).
        const int answerMs = endPending ? std::min(std::max(opts_.settleMs, opts_.responseMaxMs), 2 * responseWaitMs()) : responseWaitMs();
        if (auto f = waitFailure(waitSettled(opts_.stepTimeoutMs, answerMs))) return finishWith(*f);
        int64_t firstChange = firstChangeMs_;
        const bool hinted = stats_.pxPerNotch > 0 && !afterStill;
        const int hint = hinted ? int(std::lround(stats_.pxPerNotch * notches)) : kNoHint;
        const int unc = hint < 0 ? 0 : int(std::ceil(notches * (stats_.maxPxPerNotch - stats_.minPxPerNotch))) + owed;
        StepResult r = stitcher_.addFrame(last_, lastSig_, hint, unc);
        if (opts_.onStep) opts_.onStep(r, hint, notches);

        // Something changed in place (a hover effect under the pointer, a
        // caret) and the picture came to rest again before the response time
        // was over: the scroll itself may still be on its way.
        while (r.status == StepStatus::NoMovement) {
            const int left = answerMs - int(clock_.nowMs() - injectedAt);
            if (left <= 0) break;
            if (auto f = waitFailure(waitSettled(opts_.stepTimeoutMs, left))) return finishWith(*f);
            if (firstChangeMs_ < 0) break;  // nothing changed in all that time
            if (firstChange < 0) firstChange = firstChangeMs_;
            r = stitcher_.addFrame(last_, lastSig_, hint, unc);
            if (opts_.onStep) opts_.onStep(r, hint, notches);
        }

        // Stillness is checked against a fresh frame before anything is made of
        // it (the end of the content, "nothing scrolls"): a stream that sent
        // nothing must not end a capture as if it were complete. Checked at
        // the first still step already, while the content is one step away
        // and still overlaps what was stitched.
        Screen screen = Screen::Unknown;
        const auto checkStillness = [&] {
            if (r.status == StepStatus::NoMovement) {
                screen = screenIsAsSeen();
                if (screen == Screen::Moved) {
                    firstChange = -1;  // when it moved is not known
                    r = stitcher_.addFrame(last_, lastSig_, hint, unc);
                    if (opts_.onStep) opts_.onStep(r, hint, notches);
                    // The fresh frame differs in place only: it is the proof that nothing scrolled.
                    if (r.status == StepStatus::NoMovement) screen = Screen::Same;
                }
            }
        };
        checkStillness();
        if (layoutChanged_) return finishWith(RunResult::LayoutChanged);

        if (r.status == StepStatus::NoMatch || r.status == StepStatus::Ambiguous) {
            // 1) Transient: the frame may not have been final (late repaint, overlay).
            ++stats_.retries;
            if (auto f = waitFailure(waitSettled(opts_.stepTimeoutMs))) return finishWith(*f);
            r = stitcher_.addFrame(last_, lastSig_, hint, unc);
            if (opts_.onStep) opts_.onStep(r, hint, notches);
            checkStillness();
            if (layoutChanged_) return finishWith(RunResult::LayoutChanged);
        }

        if (r.status == StepStatus::NoMatch || r.status == StepStatus::Ambiguous) {
            const bool ambiguous = r.status == StepStatus::Ambiguous;
            if (ambiguous && stats_.pxPerNotch <= 0) {
                stats_.lastError = "first step is ambiguous (periodic or blank content) and no px-per-notch is known";
                return finishWith(RunResult::LostTrack);
            }
            if (!opts_.allowScrollBack) {
                stats_.lastError = "step could not be matched; scrolling further down cannot reconnect to the "
                                   "last stitched frame without scrolling back";
                return finishWith(RunResult::LostTrack);
            }
            // 2) Scroll back by the same amount, re-find the reference, halve the step.
            if (!inject(-notches)) {
                stats_.lastError = "injector refused the scroll-back event";
                return finishWith(RunResult::InjectorFailed);
            }
            ++stats_.scrollBacks;
            if (auto f = waitFailure(waitSettled(opts_.stepTimeoutMs, responseWaitMs()))) return finishWith(*f);
            StepResult back = stitcher_.addFrame(last_, lastSig_, 0);
            if (opts_.onStep) opts_.onStep(back, 0, -notches);
            if ((back.status == StepStatus::NoMatch || back.status == StepStatus::Ambiguous) && screenIsAsSeen() == Screen::Moved) {
                // The stream had not sent the way back.
                back = stitcher_.addFrame(last_, lastSig_, 0);
                if (opts_.onStep) opts_.onStep(back, 0, -notches);
            }
            // Wheel steps of an earlier burst may have landed after their
            // settle time (a slow guest), and the page is then further down
            // than one way back reaches: the same way back again, and the
            // pixels alone say where the page is (no hint).
            for (int again = 0; again < opts_.extraScrollBacks && (back.status == StepStatus::NoMatch || back.status == StepStatus::Ambiguous); ++again) {
                if (!inject(-notches)) {
                    stats_.lastError = "injector refused the scroll-back event";
                    return finishWith(RunResult::InjectorFailed);
                }
                ++stats_.scrollBacks;
                if (auto f = waitFailure(waitSettled(opts_.stepTimeoutMs, responseWaitMs()))) return finishWith(*f);
                back = stitcher_.addFrame(last_, lastSig_, kNoHint);
                if (opts_.onStep) opts_.onStep(back, kNoHint, -notches);
                if ((back.status == StepStatus::NoMatch || back.status == StepStatus::Ambiguous) && screenIsAsSeen() == Screen::Moved) {
                    back = stitcher_.addFrame(last_, lastSig_, kNoHint);
                    if (opts_.onStep) opts_.onStep(back, kNoHint, -notches);
                }
            }
            if (back.status != StepStatus::NoMovement && back.status != StepStatus::Appended && back.status != StepStatus::Reversed) {
                stats_.lastError = back.status == StepStatus::LayoutChanged ? noteFor(back) : std::string("after scrolling back the last stitched frame was not found (") + scroll::toString(back.status) + ")";
                return finishWith(back.status == StepStatus::LimitReached ? RunResult::LimitReached
                                  : back.status == StepStatus::InvalidFrame ? RunResult::LayoutChanged : RunResult::LostTrack);
            }
            if (back.status == StepStatus::Reversed) ++stats_.reversed;
            if (notches == 1 && ++oneNotchFailures >= 2) {
                stats_.lastError = "a single notch scrolls further than the overlap allows; enlarge the region";
                return finishWith(RunResult::LostTrack);
            }
            notchCap_ = std::max(1, notches / 2);
            verifiedRun_ = 0;
            ++stats_.halvings;
            afterStill = false;
            notches = planNotches();
            progress(false);
            continue;
        }

        for (bool again = true; again;) {
        again = false;
        switch (r.status) {
            case StepStatus::Appended: {
                if (endPending) ++stats_.falseEnds;
                endPending = false;
                moved = true;
                // Only a step that added rows is progress. One that found its
                // way back to rows already in the image (after a way back that
                // went too far) is not: counted as progress, a place where one
                // wheel step takes the picture further than a viewport, there
                // and back again, kept the loop going round until the time
                // limit (a page that lays itself out anew under the viewport;
                // Chromium, real desktop, 2026-11-12).
                if (r.appendedRows > 0) oneNotchFailures = 0;
                if (r.lowConfidence) ++stats_.lowConfidenceSteps;
                if (!afterStill && firstChange >= 0) stats_.maxResponseMs = std::max(stats_.maxResponseMs, int(firstChange - injectedAt));
                // Learn px-per-notch from verified steps; a low-confidence step only
                // teaches it when nothing is known yet (and it had pixel evidence).
                const bool learn = !afterStill && owed == 0 && (r.lowConfidence ? (hint < 0 && r.evidenceRows > 0) : true);
                afterStill = false;
                owed = hint >= 0 && r.offset < opts_.shortStepFraction * hint ? hint - r.offset : 0;
                if (learn && r.offset > 0 && (hint < 0 || r.offset >= opts_.shortStepFraction * hint)) {
                    const double px = double(r.offset) / notches;
                    stats_.pxPerNotch = px;
                    stats_.minPxPerNotch = stats_.minPxPerNotch > 0 ? std::min(stats_.minPxPerNotch, px) : px;
                    stats_.maxPxPerNotch = std::max(stats_.maxPxPerNotch, px);
                }
                // A halved cap recovers after a run of verified steps.
                if (!r.lowConfidence && ++verifiedRun_ >= 3 && notchCap_ < opts_.maxNotchesPerStep) {
                    notchCap_ = std::min(opts_.maxNotchesPerStep, notchCap_ * 2);
                    verifiedRun_ = 0;
                }
                if (r.lowConfidence) verifiedRun_ = 0;
                notches = planNotches();
                break;
            }
            case StepStatus::LimitReached:
                return finishWith(RunResult::LimitReached);
            case StepStatus::NoMovement: {
                // The response time is over, and the screen itself was as the
                // stream showed it (Same) or could not be asked (Unknown).
                const int age = int(clock_.nowMs() - injectedAt);
                if (opts_.pointer && !moved) {
                    // Nothing has scrolled down in this capture yet, so this is
                    // not the end of anything. A single step can be lost, and
                    // wheel-up steps against a page already at its top leave a
                    // GTK 4 view overscrolled by up to 100 px when the wheel
                    // arrives as a continuous axis (KWin's fake input): the
                    // first steps down are spent on that and move nothing (two
                    // and most of a third at 37.5 px a step; real desktop run
                    // of 2026-11-10, where two still steps after the way up
                    // were reported as the end of a 1,500-line file).
                    afterStill = true;
                    if (age < opts_.endConfirmDelayMs) clock_.sleepMs(opts_.endConfirmDelayMs - age);
                    ++stillAtPark;
                    if (stats_.toTopMoved) {
                        // It scrolled up, so it scrolls: stay here and keep trying.
                        if (stillAtPark < opts_.stillStepsAfterTop) break;
                        stats_.lastError = "the page scrolled up to its top and then did not scroll down";
                        return finishWith(RunResult::LostTrack);
                    }
                    // Rule 7: the centre may be a part that does not scroll: each
                    // place gets two steps (the first one four after wheel-up
                    // steps), then the two other park positions.
                    if (stillAtPark < (stats_.parkPosition == 0 && stats_.toTopSteps > 0 ? 4 : 2)) break;
                    stillAtPark = 0;
                    if (atEntry_) {
                        // Nothing scrolls just inside the edge: the classic places, the centre first.
                        atEntry_ = false;
                    } else {
                        if (stats_.parkPosition >= 2) {
                            stats_.lastError = "nothing under the selection scrolled at any park position";
                            return finishWith(RunResult::NoMovement);
                        }
                        ++stats_.parkPosition;
                    }
                    if (opts_.fromTop) {
                        // No downward step was committed: replace the pending first
                        // viewport with the top proved at the new pointer target.
                        stats_.reachedTop = false;
                        stitcher_.restartBeforeMovement();
                        if (auto f = start(true)) return finishWith(*f);
                    }
                    break;
                }
                // The end of the content is a claim about the screen: it
                // takes a fresh frame, grabbed after the last injections,
                // that shows what was there before them.
                if (endPending && screen != Screen::Same) {
                    stats_.lastError = "the frame source gave no fresh frame after the last scroll steps, so the end of the content is not proven";
                    return finishWith(RunResult::LostTrack);
                }
                if (screen == Screen::Same && moved && (stats_.falseEnds > 0 || owed == 0 || stats_.maxResponseMs > settleNeedMs())) {
                    if (age < opts_.endConfirmDelayMs) clock_.sleepMs(opts_.endConfirmDelayMs - age);
                    // A page that runs out of content stops partway through a
                    // step. One that moved all of its last step and then nothing
                    // at all, one that was still before and then moved after
                    // all, or one whose answer to a step takes longer than the
                    // picture's settle time (so that a short step may have been
                    // seen halfway) may only not have answered yet: a lost
                    // step, a page still busy with earlier ones. A step back up
                    // and down again tells (proveEnd).
                    std::optional<RunResult> fail;
                    StepResult found;
                    const EndProof proof = proveEnd(found, fail);
                    if (fail) return finishWith(*fail);
                    if (proof == EndProof::Proved) {
                        ++stats_.endConfirmations;
                        return finishWith(RunResult::EndOfContent);
                    }
                    if (proof == EndProof::Unproved) {
                        stats_.lastError = "the page did not answer a wheel step up and back down, so the end of the content is not proven";
                        return finishWith(RunResult::LostTrack);
                    }
                    // Not the end: the step that found the page further down goes on
                    // as any other, its distance teaching nothing about one notch.
                    r = found;
                    endPending = true;
                    afterStill = true;
                    again = true;
                    break;
                }
                if (endPending) {
                    ++stats_.endConfirmations;
                    return finishWith(RunResult::EndOfContent);
                }
                // Possibly the end, a dropped event or a slow lazy load: confirm
                // once, with one notch (more is only more for a busy page to
                // work through before it can answer).
                endPending = true;
                afterStill = true;
                notches = 1;
                if (age < opts_.endConfirmDelayMs) clock_.sleepMs(opts_.endConfirmDelayMs - age);
                break;
            }
            case StepStatus::Reversed:
                moved = true;
                ++stats_.reversed;
                break;
            case StepStatus::InvalidFrame:
                stats_.lastError = "the frame size changed";
                return finishWith(RunResult::LayoutChanged);
            case StepStatus::LayoutChanged:
                // The frames kept their size, so the display did not change:
                // what was proved before is an exact image and is kept.
                stats_.lastError = noteFor(r);
                return finishWith(RunResult::LostTrack);
            default:
                stats_.lastError = std::string("stitcher returned ") + scroll::toString(r.status);
                return finishWith(RunResult::LostTrack);
        }
        }
        progress(false);
        } catch (const Interrupted&) {
            // Paused (#1514). What this step had sent is looked at again on Resume.
            StepResult seen;
            std::optional<RunResult> end;
            for (bool waiting = true; waiting;) {
                try {
                    end = holdPaused();
                    if (!end && stitcher_.hasFrame()) end = resync(seen);
                    waiting = false;
                } catch (const Interrupted&) {}
            }
            if (end) return finishWith(*end);
            if (!stitcher_.hasFrame()) {
                // Rule 7 was taking the first viewport again.
                if (auto f = start(opts_.fromTop)) return finishWith(*f);
            } else if (seen.status == StepStatus::Appended) {
                moved = true;
                if (seen.appendedRows > 0) oneNotchFailures = 0;
            }
            // The next step is placed by its pixels and teaches nothing about
            // one notch; an end that was pending is proved anew.
            afterStill = true;
            endPending = false;
            owed = 0;
            notches = planNotches();
            progress(false);
        }
    }
}
}
