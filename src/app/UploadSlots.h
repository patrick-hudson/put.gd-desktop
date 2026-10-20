#pragma once
#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
namespace putgd {
// Bounded upload slots shared by every operation (#28). One slot carries one file's
// transfer; a multipart upload's parts run in order inside its slot, so a huge file
// holds one slot however long it takes. A free slot goes to the waiting operation
// that holds the fewest slots, then to the earliest request, so a large batch never
// starves a newer action. Workers block here; the session thread never does.
class UploadSlots {
public:
    // Session thread: the user's "Uploads at once" setting. Raising it wakes waiters.
    void setCapacity(int);
    // Blocks until this operation may start one more transfer. Returns false, without
    // a slot, once `stop` returns true (checked at least every 100 ms) or after close().
    bool acquire(const QString& operation, const std::function<bool()>& stop);
    void release(const QString& operation);
    // Re-evaluate waiters now, after a cancellation flag changed.
    void wake();
    void close();
    QJsonObject diagnostics() const;
private:
    struct Waiter { QString operation; quint64 sequence; };
    bool next(const Waiter&) const;
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    QList<Waiter> waiting_;
    QHash<QString, int> held_;
    int capacity_ = 1, active_ = 0;
    quint64 sequence_ = 0;
    bool closed_ = false;
};
// One operation at a time for a destination that takes one request at a time,
// served in arrival order so queued actions keep their invocation order.
class DestinationLease {
public:
    // Blocks until this caller holds the lease; false, without it, once `stop` returns true.
    bool acquire(const std::function<bool()>& stop);
    void release();
private:
    std::mutex mutex_;
    std::condition_variable changed_;
    std::deque<quint64> queue_;
    quint64 next_ = 0;
    bool held_ = false;
};
}
