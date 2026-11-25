#pragma once
#include <cstdint>

namespace putgd::record {
// Whether a frame source that grabs at a fixed rate (ximagesrc) and the loop
// that takes its frames keep up with the rate asked for (#2064). Judged over
// windows of a second while the loop reads: the source is slow when it sent
// under four fifths of the rate, the loop when a quarter of the frames that
// came were replaced unread in the mailbox. Two slow windows in a row, and
// the source should go down to its lower rate. No GStreamer here, so the
// rule is tested on its own.
class FramePace {
public:
    enum class Verdict { Keep, SourceSlow, LoopSlow };
    static constexpr int windowMs = 1000;
    static constexpr int slowWindows = 2;
    explicit FramePace(int fps = 0) : fps_(fps) {}
    // A new window from `now` (watching began, the loop asked for a fresh
    // frame and dropped what was there, the source was restarted).
    void begin(int64_t now) { from_ = now; arrived_ = missed_ = 0; }
    void restart(int fps, int64_t now) { fps_ = fps; bad_ = 0; begin(now); }
    // The source sent a frame.
    void arrived() { ++arrived_; }
    // The loop took a frame at `now`; `missed` frames came before it and
    // were replaced unread.
    Verdict taken(int64_t now, uint64_t missed) {
        missed_ += missed;
        if (from_ < 0 || now - from_ < windowMs) return Verdict::Keep;
        const double sent = double(arrived_) * 1000.0 / double(now - from_);
        Verdict verdict = Verdict::Keep;
        if (sent < 0.8 * fps_) verdict = Verdict::SourceSlow;
        else if (4 * missed_ >= arrived_ && missed_ > 0) verdict = Verdict::LoopSlow;
        lastSent_ = sent;
        lastMissedShare_ = arrived_ ? double(missed_) / double(arrived_) : 0;
        bad_ = verdict == Verdict::Keep ? 0 : bad_ + 1;
        begin(now);
        return bad_ >= slowWindows ? verdict : Verdict::Keep;
    }
    // The last whole window: frames a second the source sent, the share the loop missed.
    double lastSent() const { return lastSent_; }
    double lastMissedShare() const { return lastMissedShare_; }
private:
    int fps_ = 0;
    int64_t from_ = -1;
    uint64_t arrived_ = 0, missed_ = 0;
    int bad_ = 0;
    double lastSent_ = 0, lastMissedShare_ = 0;
};
}
