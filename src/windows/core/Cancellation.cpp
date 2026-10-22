#include "core/Cancellation.h"

namespace putgd {
CancelFlag ItemCancellations::flag(const std::string& operationId, const std::string& itemId, const CancelFlag& operation) {
    std::lock_guard lock(mutex_);
    auto& flag = flags_[operationId][itemId];
    if (!flag) flag = std::make_shared<std::atomic_bool>(false);
    // Checked under the lock that cancelAll takes, so an item made after the
    // operation was cancelled cannot miss it.
    if (operation && operation->load()) flag->store(true);
    return flag;
}
void ItemCancellations::cancelItem(const std::string& operationId, const std::string& itemId) {
    std::lock_guard lock(mutex_);
    auto& flag = flags_[operationId][itemId];
    if (!flag) flag = std::make_shared<std::atomic_bool>(false);
    flag->store(true);
}
void ItemCancellations::cancelAll(const std::string& operationId) {
    std::lock_guard lock(mutex_);
    const auto it = flags_.find(operationId);
    if (it == flags_.end()) return;
    for (const auto& pair : it->second) pair.second->store(true);
}
void ItemCancellations::forget(const std::string& operationId) {
    std::lock_guard lock(mutex_);
    flags_.erase(operationId);
}
bool ItemCancellations::cancelled(const std::string& operationId, const std::string& itemId) const {
    std::lock_guard lock(mutex_);
    const auto it = flags_.find(operationId);
    if (it == flags_.end()) return false;
    const auto item = it->second.find(itemId);
    return item != it->second.end() && item->second->load();
}
}
