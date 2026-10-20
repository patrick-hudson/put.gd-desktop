#include "PauseTimeline.h"
#include <algorithm>

namespace putgd::record {
void PauseTimeline::pause(std::int64_t running) {
    std::lock_guard lock(mutex_);
    if (open_ < 0) open_ = std::max<std::int64_t>(0, running);
}
void PauseTimeline::resume(std::int64_t running) {
    std::lock_guard lock(mutex_);
    if (open_ < 0) return;
    closed_.emplace_back(open_, std::max(open_, running));
    open_ = -1;
}
void PauseTimeline::reset() {
    std::lock_guard lock(mutex_);
    closed_.clear();
    open_ = -1;
}
bool PauseTimeline::paused() const {
    std::lock_guard lock(mutex_);
    return open_ >= 0;
}
std::optional<std::int64_t> PauseTimeline::map(std::int64_t pts, std::int64_t duration) const {
    std::lock_guard lock(mutex_);
    const std::int64_t end = pts + std::max<std::int64_t>(0, duration);
    if (open_ >= 0 && std::max(end, pts + 1) > open_) return std::nullopt;
    std::int64_t shift = 0;
    for (const auto& [from, to] : closed_) {
        if (pts >= to) shift += to - from;
        else if (std::max(end, pts + 1) > from) return std::nullopt;
    }
    return pts - shift;
}
std::int64_t PauseTimeline::pausedTotal() const {
    std::lock_guard lock(mutex_);
    std::int64_t total = 0;
    for (const auto& [from, to] : closed_) total += to - from;
    return total;
}
}
