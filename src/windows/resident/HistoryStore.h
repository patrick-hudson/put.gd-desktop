#pragma once
// The durable journal and history ledger in SQLite (winsqlite3): the same tables,
// row shapes and public record fields as the Linux HistoryStore, so CLI/MCP
// clients see identical history.list / history.detail / operation.* results.
// One instance per worker thread; never share a connection across threads.
#include "common/Sqlite.h"
#include "core/Operation.h"
#include "resident/Provider.h"
#include <set>
#include <vector>

namespace putgd {
struct HistoryQuery { int limit = 50, offset = 0; std::string search, category, mime, profileId, uploadStatus, localStatus, remoteStatus, from, to; };
struct HistoryPage { json::Array records; long long total = 0; Error error; json::Object toJson() const { return {{"records", records}, {"total", double(total)}}; } };
class HistoryStore {
public:
    HistoryStore(std::string dataRoot, std::string path);
    Error error() const { return error_; }
    Error saveOperation(const Operation&, const ProviderRegistry* = nullptr);
    Error checkpointOperation(const Operation&, const std::set<std::string>& changedItems, const ProviderRegistry* = nullptr);
    Operation operation(const std::string& id, Error* = nullptr) const;
    // Every operation still in an active state at startup; interrupted uploads become unknown.
    std::vector<Operation> recover(Error* = nullptr);
    HistoryPage list(const HistoryQuery&) const;
    json::Object detail(const std::string& id, Error* = nullptr) const;
    json::Array recent(int limit, Error* = nullptr) const;
    Error expireBefore(const std::string& timestamp);
    Error clear(); // preserves identities retained by in-progress deletion
    Error saveDeletion(const json::Object&);
    json::Object deletion(const std::string& id, Error* = nullptr) const;
    // Sharing journal and the item's effective receipt/status in one transaction (docs/proton-drive.md).
    Error saveSharing(const json::Object& doc);
    json::Object sharing(const std::string& id, Error* = nullptr) const;
    // Merges remote-management fields into an item's state and history row.
    Error updateRemote(const std::string& historyId, const json::Object& fields);
    std::vector<std::string> activeDeletionHistoryIds() const;
    static std::string publicationIdentity(const StorageReceipt&, const ProviderRegistry&);
private:
    Error initialize();
    Error persistOperation(const Operation&, const ProviderRegistry*, const std::set<std::string>* changed);
    json::Object publicRecord(json::Object data) const;
    std::string root_, path_;
    mutable sqlite::Database db_;
    Error error_;
};
}
