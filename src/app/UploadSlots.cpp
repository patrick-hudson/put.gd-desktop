#include "UploadSlots.h"
#include <chrono>
namespace putgd {
void UploadSlots::setCapacity(int capacity) {
    {
        std::lock_guard lock(mutex_);
        capacity_ = qMax(1, capacity);
    }
    changed_.notify_all();
}
bool UploadSlots::next(const Waiter& candidate) const {
    if (active_ >= capacity_) return false;
    for (const auto& other : waiting_) {
        const auto mine = held_.value(candidate.operation), theirs = held_.value(other.operation);
        if (theirs < mine || (theirs == mine && other.sequence < candidate.sequence)) return false;
    }
    return true;
}
bool UploadSlots::acquire(const QString& operation, const std::function<bool()>& stop) {
    std::unique_lock lock(mutex_);
    const Waiter self{operation, ++sequence_};
    waiting_.append(self);
    auto leave = [&] {
        for (qsizetype index = 0; index < waiting_.size(); ++index)
            if (waiting_[index].sequence == self.sequence) { waiting_.removeAt(index); break; }
    };
    for (;;) {
        if (closed_ || (stop && stop())) {
            leave();
            lock.unlock();
            changed_.notify_all();
            return false;
        }
        if (next(self)) {
            leave();
            ++active_;
            ++held_[operation];
            lock.unlock();
            changed_.notify_all();
            return true;
        }
        changed_.wait_for(lock, std::chrono::milliseconds(100));
    }
}
void UploadSlots::release(const QString& operation) {
    {
        std::lock_guard lock(mutex_);
        --active_;
        if (--held_[operation] <= 0) held_.remove(operation);
    }
    changed_.notify_all();
}
void UploadSlots::wake() { changed_.notify_all(); }
void UploadSlots::close() {
    {
        std::lock_guard lock(mutex_);
        closed_ = true;
    }
    changed_.notify_all();
}
QJsonObject UploadSlots::diagnostics() const {
    std::lock_guard lock(mutex_);
    return {{"capacity", capacity_}, {"active", active_}, {"waiting", int(waiting_.size())}, {"operations", int(held_.size())}};
}
bool DestinationLease::acquire(const std::function<bool()>& stop) {
    std::unique_lock lock(mutex_);
    const auto ticket = next_++;
    queue_.push_back(ticket);
    for (;;) {
        if (stop && stop()) {
            for (auto it = queue_.begin(); it != queue_.end(); ++it)
                if (*it == ticket) { queue_.erase(it); break; }
            lock.unlock();
            changed_.notify_all();
            return false;
        }
        if (!held_ && queue_.front() == ticket) {
            queue_.pop_front();
            held_ = true;
            return true;
        }
        changed_.wait_for(lock, std::chrono::milliseconds(100));
    }
}
void DestinationLease::release() {
    {
        std::lock_guard lock(mutex_);
        held_ = false;
    }
    changed_.notify_all();
}
}
