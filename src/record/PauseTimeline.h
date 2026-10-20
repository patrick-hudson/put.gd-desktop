#pragma once
#include <cstdint>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

namespace putgd::record {
// The paused intervals of one recording, in pipeline running time, shared by
// the video and audio branches. Both drop what was captured while paused and
// shift what follows by the same paused time, so a pause/resume leaves the
// tracks where they were relative to each other (#130). Each branch used to
// measure its own gap from the first buffer after the resume, which differed
// by up to a frame and by the audio buffer size between the two.
class PauseTimeline {
public:
    // Opens an interval at `running`; ignored while one is open.
    void pause(std::int64_t running);
    // Closes the open interval at `running` (at least where it opened).
    void resume(std::int64_t running);
    void reset();
    bool paused() const;
    // A buffer covering [pts, pts + duration): nullopt when any of it falls in a
    // paused interval (or after an open one), else pts minus the paused time
    // before it. Video frames pass duration 0.
    std::optional<std::int64_t> map(std::int64_t pts, std::int64_t duration) const;
    // Total closed paused time.
    std::int64_t pausedTotal() const;
private:
    mutable std::mutex mutex_;
    std::vector<std::pair<std::int64_t, std::int64_t>> closed_;
    std::int64_t open_ = -1;
};

// Keeps audio timestamps from running away. pulsesrc on the monitor of a
// suspended null sink (a VM without a sound card) can stamp its buffers about
// 2^32 ring-buffer segments in the future (#129). The muxer then never takes
// them, the audio queue reads as full, and the audio branch blocks for good.
// A buffer stamped more than kFutureNs ahead of the pipeline's running time is
// re-anchored to its arrival (running time minus its duration), and the same
// correction carries over to the buffers after it; a correction that would put
// later buffers more than kBehindNs in the past is dropped again.
class AudioClockGuard {
public:
    static constexpr std::int64_t kFutureNs = 1'000'000'000, kBehindNs = 5'000'000'000;
    std::int64_t apply(std::int64_t pts, std::int64_t duration, std::int64_t runningNow);
    std::int64_t correction() const { return correction_; }
    int corrections() const { return corrections_; }
    void reset() { correction_ = 0; corrections_ = 0; }
private:
    std::int64_t correction_ = 0;
    int corrections_ = 0;
};
}
