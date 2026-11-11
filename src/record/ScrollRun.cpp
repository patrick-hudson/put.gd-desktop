#include "ScrollRun.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <utility>

namespace putgd::record {
using scroll::kNoHint;
using scroll::StepResult;
using scroll::StepStatus;

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

ScrollRun::ScrollRun(RunInjector& injector, RunFrames& source, RunClock& clock, RunOptions opts, scroll::RowSink sink)
    : injector_(injector), source_(source), clock_(clock), opts_(std::move(opts)), stitcher_(opts_.stitch, std::move(sink)) {}

// Rule 2: the horizontal centre, a third of the way down. Rule 7's two other
// places for a region whose centre does not scroll: near the right edge, then
// two thirds down.
RunPoint ScrollRun::parkPoint(int position, int width, int height) {
    if (position == 1) return {std::max(0, width - std::max(12, width / 8)), height / 3};
    if (position == 2) return {width / 2, 2 * height / 3};
    return {width / 2, height / 3};
}

bool ScrollRun::park() {
    const RunPoint at = parkPoint(stats_.parkPosition, last_.width, last_.height);
    return injector_.movePointer(at.x, at.y);
}

// The pointer's place is asserted again before every injection: the user may
// have moved the mouse, and the wheel goes to whatever is under the pointer.
bool ScrollRun::inject(int notches) {
    if (stopped()) return false;
    // On the way to the top of a desktop that paints the pointer into the
    // frames, the pointer is in the rectangle only for the wheel step itself.
    const bool visit = toTop_ && opts_.restAwayToTop && opts_.pointer && opts_.away && haveFrame_;
    // Stop found the pointer inside: no wheel step follows, the pointer leaves.
    const auto leave = [&] {
        parked_ = -1;
        return injector_.movePointer(opts_.away->x, opts_.away->y);
    };
    if (opts_.pointer && haveFrame_) {
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
    stats_.notchesInjected += std::abs(notches);
    lastInjectMs_ = clock_.nowMs();
    const bool sent = injector_.scroll(notches);
    return visit ? leave() && sent : sent;
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
    // The cadence is measured between frames taken one after the other in one
    // wait; the time the loop spent elsewhere between two waits is not a gap.
    lastFrameMs_ = -1;
    for (;;) {
        if (stopped()) return Wait::Stopped;
        const int64_t now = clock_.nowMs();
        if (now - startMs_ >= opts_.totalTimeLimitMs) return Wait::TimeLimit;
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
            // A frame from before the call began cannot show what the call waits for.
            if (!haveFrame_ || f->timestampMs >= start) {
                if (!responded) firstChangeMs_ = std::max(start, f->timestampMs);
                responded = true;
            }
        }
        std::swap(lastSig_, tmpSig_);
        last_ = f->view;
        haveFrame_ = true;
    }
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
        const RunPoint at = parkPoint(stats_.parkPosition, last_.width, last_.height);
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
        const int64_t now = clock_.nowMs();
        if (now - startMs_ >= opts_.totalTimeLimitMs) return Wait::TimeLimit;
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
std::optional<RunResult> ScrollRun::scrollToTop() {
    toTop_ = true;
    const int64_t begin = clock_.nowMs();
    const auto result = wayToTop();
    stats_.toTopMs = clock_.nowMs() - begin;
    toTop_ = false;
    return result;
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
    if (stats_.toTopSteps > 0 && !stopped() && injector_.rest()) {
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

RunResult ScrollRun::run(const std::atomic<bool>* stop) {
    stop_ = stop;
    startMs_ = clock_.nowMs();
    notchCap_ = opts_.maxNotchesPerStep;
    static const std::atomic<bool> never{false};
    if (!injector_.waitReady(stop ? *stop : never)) {
        if (!stopped()) stats_.lastError = "the modifier keys were never released";
        return finishWith(stopped() ? RunResult::Stopped : RunResult::NotReady);
    }
    if (opts_.fromTop)
        if (auto f = scrollToTop()) return finishWith(*f == RunResult::Stopped ? viewportAfterStop() : *f);

    // Rule 1: the first frame is grabbed with the pointer outside the region.
    if (opts_.pointer && opts_.away) {
        if (stopped()) return finishWith(RunResult::Stopped);
        if (!injector_.movePointer(opts_.away->x, opts_.away->y)) {
            stats_.lastError = "injector refused the pointer motion";
            return finishWith(RunResult::InjectorFailed);
        }
        parked_ = -1;
    }
    if (auto f = waitFailure(waitSettled(opts_.firstFrameTimeoutMs))) return finishWith(*f);
    if (stitcher_.addFrame(last_, lastSig_, kNoHint).status != StepStatus::First) {
        stats_.lastError = "the first frame was refused";
        return finishWith(RunResult::NoFrames);
    }
    progress(false);

    int notches = 1;
    bool endPending = false, moved = false;
    // The step before this one showed no movement. If this one moves, the
    // earlier wheel step may have arrived late and be part of what is seen, so
    // the distance says nothing about one notch and no distance is expected of
    // it. (Seen on Plasma through XWayland: a burst of notches landed after
    // the settle time; real desktop run of 2026-11-10.)
    bool afterStill = false;
    int oneNotchFailures = 0, stillAtPark = 0;

    for (;;) {
        if (stopped()) return finishWith(RunResult::Stopped);
        if (clock_.nowMs() - startMs_ >= opts_.totalTimeLimitMs) return finishWith(RunResult::TimeLimit);

        if (!inject(notches)) {
            stats_.lastError = "injector refused the scroll event";
            return finishWith(RunResult::InjectorFailed);
        }
        ++stats_.steps;
        const int64_t injectedAt = clock_.nowMs();
        if (auto f = waitFailure(waitSettled(opts_.stepTimeoutMs, responseWaitMs()))) return finishWith(*f);
        int64_t firstChange = firstChangeMs_;
        const bool hinted = stats_.pxPerNotch > 0 && !afterStill;
        const int hint = hinted ? int(std::lround(stats_.pxPerNotch * notches)) : kNoHint;
        const int unc = hint < 0 ? 0 : int(std::ceil(notches * (stats_.maxPxPerNotch - stats_.minPxPerNotch)));
        StepResult r = stitcher_.addFrame(last_, lastSig_, hint, unc);
        if (opts_.onStep) opts_.onStep(r, hint, notches);

        // Something changed in place (a hover effect under the pointer, a
        // caret) and the picture came to rest again before the response time
        // was over: the scroll itself may still be on its way.
        while (r.status == StepStatus::NoMovement) {
            const int left = responseWaitMs() - int(clock_.nowMs() - injectedAt);
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
        Screen screen = Screen::Same;
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

        if (r.status == StepStatus::NoMatch || r.status == StepStatus::Ambiguous) {
            // 1) Transient: the frame may not have been final (late repaint, overlay).
            ++stats_.retries;
            if (auto f = waitFailure(waitSettled(opts_.stepTimeoutMs))) return finishWith(*f);
            r = stitcher_.addFrame(last_, lastSig_, hint, unc);
            if (opts_.onStep) opts_.onStep(r, hint, notches);
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
                stats_.lastError = std::string("after scrolling back the last stitched frame was not found (") + scroll::toString(back.status) + ")";
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

        switch (r.status) {
            case StepStatus::Appended: {
                if (endPending) ++stats_.falseEnds;
                endPending = false;
                moved = true;
                oneNotchFailures = 0;
                if (r.lowConfidence) ++stats_.lowConfidenceSteps;
                if (!afterStill && firstChange >= 0) stats_.maxResponseMs = std::max(stats_.maxResponseMs, int(firstChange - injectedAt));
                // Learn px-per-notch from verified steps; a low-confidence step only
                // teaches it when nothing is known yet (and it had pixel evidence).
                const bool learn = !afterStill && (r.lowConfidence ? (hint < 0 && r.evidenceRows > 0) : true);
                afterStill = false;
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
                    if (stats_.parkPosition >= 2) {
                        stats_.lastError = "nothing under the selection scrolled at any park position";
                        return finishWith(RunResult::NoMovement);
                    }
                    ++stats_.parkPosition;
                    break;
                }
                if (endPending) {
                    // The end of the content is a claim about the screen: it
                    // takes a fresh frame, grabbed after the last injections,
                    // that shows what was there before them.
                    if (screen != Screen::Same) {
                        stats_.lastError = "the frame source gave no fresh frame after the last scroll steps, so the end of the content is not proven";
                        return finishWith(RunResult::LostTrack);
                    }
                    ++stats_.endConfirmations;
                    return finishWith(RunResult::EndOfContent);
                }
                // Possibly the end, a dropped event or a slow lazy load: confirm once.
                endPending = true;
                afterStill = true;
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
            default:
                stats_.lastError = std::string("stitcher returned ") + scroll::toString(r.status);
                return finishWith(RunResult::LostTrack);
        }
        progress(false);
    }
}
}
