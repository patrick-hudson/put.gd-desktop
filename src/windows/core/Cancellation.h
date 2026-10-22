#pragma once
// Cancellation flags for an operation's items (docs/cli.md, operation.cancel with
// itemId). Every item's provider requests poll their own flag, so cancelling one
// file stops only its requests; cancelling the operation sets every item's flag.
// Thread-safe: the owner thread cancels while worker threads read the flags.
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace putgd {
using CancelFlag = std::shared_ptr<std::atomic_bool>;
class ItemCancellations {
public:
    // The item's flag, made on first use. It is already set when the item was
    // cancelled earlier or `operation` (the operation's own flag) is set.
    CancelFlag flag(const std::string& operationId, const std::string& itemId, const CancelFlag& operation);
    // Sets the item's flag, making it when the item has not reached a provider yet.
    void cancelItem(const std::string& operationId, const std::string& itemId);
    // Sets every item flag of the operation. Call after setting the operation's own
    // flag: an item flag made later sees that flag in flag().
    void cancelAll(const std::string& operationId);
    // Drops the operation's flags once it stopped running (a Retry starts afresh).
    void forget(const std::string& operationId);
    bool cancelled(const std::string& operationId, const std::string& itemId) const;
private:
    mutable std::mutex mutex_;
    std::map<std::string, std::map<std::string, CancelFlag>> flags_;
};
}
