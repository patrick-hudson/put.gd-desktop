#pragma once
// Offline history browsing, explicit link copying and separately identified
// remote deletion (docs/architecture.md). Sharing methods answer honestly that
// no Windows provider manages links.
#include "resident/OperationService.h"

namespace putgd {
class HistoryService {
public:
    explicit HistoryService(OperationService&);
    void list(const HistoryQuery&, JsonReply);
    void detail(const std::string& id, JsonReply);
    void clear(JsonReply);
    void copyImage(const std::string& id, JsonReply);
    void copyLink(const std::string& id, JsonReply);
    void openLink(const std::string& id, JsonReply);
    void preview(const std::string& id, JsonReply);
    void prepareDelete(const std::vector<std::string>& ids, JsonReply);
    void deleteConfirmed(const std::vector<std::string>& ids, const std::string& acknowledgement, JsonReply);
    void inspectDelete(const std::string& id, JsonReply);
    void retryDelete(const std::string& id, JsonReply);
    void cancelDelete(const std::string& id, JsonReply);
    void expireConfigured();
    void shutdown();
    std::function<void()> onChanged;
private:
    json::Object prepare(const std::vector<std::string>& ids, Error&) const;
    void runDelete(json::Object doc, CancelFlag);
    void notifyChanged();
    OperationService& operations_;
    std::map<std::string, CancelFlag> active_;
};
}
