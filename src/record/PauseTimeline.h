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
}
