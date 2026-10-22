#include "resident/HistoryStore.h"
#include "common/Files.h"
#include "common/Paths.h"
#include "common/Sha256.h"
#include "common/Text.h"
#include "common/Uuid.h"

namespace putgd {
namespace {
Error sqlError() { return {"history_io", "Persistent journal could not be read or committed", {}}; }
std::string encode(const json::Object& o) { return json::serialize(o); }
json::Object decode(const std::string& bytes) { return json::parseObject(bytes).value_or(json::Object{}); }
std::string previewPathFor(const std::string& root, const std::string& item) { return paths::join(paths::join(root, "previews"), item + ".png"); }
std::string artifactPathFor(const std::string& root, const std::string& op, const std::string& item) { return paths::join(paths::join(paths::join(root, "artifacts"), op), item); }
}
std::string HistoryStore::publicationIdentity(const StorageReceipt& receipt, const ProviderRegistry& providers) {
    if (receipt.validate()) return {};
    const auto provider = providers.find(receipt.destination.providerId);
    if (!provider) return {};
    const auto key = provider->serializationKey(receipt.destination, receipt.objectReference, {});
    if (key.empty()) return {};
    return crypto::Sha256::hexDigest(json::serialize(json::Array{receipt.destination.providerId, key}));
}
HistoryStore::HistoryStore(std::string root, std::string path) : root_(std::move(root)), path_(std::move(path)) { error_ = initialize(); }
Error HistoryStore::initialize() {
    if (!sqlite::available()) return {"history_unavailable", "winsqlite3.dll is not available on this Windows installation", {}};
    if (!db_.open(path_)) return sqlError();
    sqlite::Statement version(db_, "PRAGMA user_version");
    if (!version.step()) return sqlError();
    const long long current = version.integer(0);
    if (current == 7) return {};
    if (current != 0) return {"history_schema", "Unsupported history schema version; keep the file for recovery", {{"version", double(current)}}};
    const char* statements[] = {
        "CREATE TABLE history(id TEXT PRIMARY KEY, operation_id TEXT NOT NULL,item_id TEXT NOT NULL,created TEXT NOT NULL,name TEXT NOT NULL,mime TEXT NOT NULL,category TEXT NOT NULL,profile_id TEXT NOT NULL,upload_status TEXT NOT NULL,local_status TEXT NOT NULL,remote_status TEXT NOT NULL,data BLOB NOT NULL,publication_identity TEXT NOT NULL DEFAULT '',UNIQUE(operation_id,item_id))",
        "CREATE INDEX history_created ON history(created DESC,id)",
        "CREATE INDEX history_filters ON history(category,mime,profile_id,upload_status,local_status,remote_status)",
        "CREATE INDEX history_publication ON history(publication_identity,upload_status)",
        "CREATE TABLE operations(id TEXT PRIMARY KEY,data BLOB NOT NULL)",
        "CREATE INDEX operations_created ON operations(json_extract(data,'$.created') DESC)",
        "CREATE TABLE deletions(id TEXT PRIMARY KEY,data BLOB NOT NULL)",
        "CREATE TABLE deletion_targets(deletion_id TEXT NOT NULL,history_id TEXT NOT NULL,active INTEGER NOT NULL,PRIMARY KEY(deletion_id,history_id))",
        "CREATE TABLE item_state(id TEXT PRIMARY KEY,operation_id TEXT NOT NULL,remote_status TEXT NOT NULL,data BLOB NOT NULL,publication_identity TEXT NOT NULL DEFAULT '',upload_status TEXT NOT NULL)",
        "CREATE INDEX item_state_operation ON item_state(operation_id)",
        "CREATE INDEX item_state_publication ON item_state(publication_identity,upload_status)",
        "CREATE TABLE cleared_history(id TEXT PRIMARY KEY)",
        "CREATE TABLE sharing_operations(id TEXT PRIMARY KEY,history_id TEXT NOT NULL,data BLOB NOT NULL)",
        "CREATE INDEX sharing_history ON sharing_operations(history_id)",
        "PRAGMA user_version=7"};
    if (!db_.begin()) return sqlError();
    for (const auto* sql : statements) if (!db_.exec(sql)) { db_.rollback(); return sqlError(); }
    if (!db_.commit()) { db_.rollback(); return sqlError(); }
    return {};
}
json::Object HistoryStore::publicRecord(json::Object data) const {
    if (data.contains("shareReceipt")) data["receipt"] = data.take("shareReceipt");
    // A link past its confirmed expiry no longer opens, so it is not offered for copying.
    const auto expires = files::parseIsoUtcMs(data.value("shareExpiresAt").toString());
    data["linkAvailable"] = OperationItem::fromJson(data).linkAvailable() && !(expires >= 0 && expires <= files::parseIsoUtcMs(files::nowIsoUtc()));
    // Previews move with the data root; only the tail is trusted.
    const auto preview = data.value("previewPath").toString();
    if (!preview.empty()) data["previewPath"] = previewPathFor(root_, data.value("id").toString());
    return data;
}
Error HistoryStore::saveOperation(const Operation& op, const ProviderRegistry* providers) { return persistOperation(op, providers, nullptr); }
Error HistoryStore::checkpointOperation(const Operation& op, const std::set<std::string>& changed, const ProviderRegistry* providers) { return persistOperation(op, providers, &changed); }
Error HistoryStore::persistOperation(const Operation& op, const ProviderRegistry* providers, const std::set<std::string>* changed) {
    if (error_) return error_;
    if (!db_.begin()) return sqlError();
    auto fail = [&] { db_.rollback(); return sqlError(); };
    { sqlite::Statement q(db_, "INSERT INTO operations VALUES(?,?) ON CONFLICT(id) DO UPDATE SET data=excluded.data"); q.bind(1, op.id).bindBlob(2, encode(op.toJson())); if (!q.exec()) return fail(); }
    for (const auto& item : op.items) {
        if (changed && !changed->count(item.artifact.id)) continue;
        std::string identity; bool newlyUploaded = true; json::Object remote;
        { sqlite::Statement q(db_, "SELECT data,publication_identity,upload_status FROM item_state WHERE id=?"); q.bind(1, item.artifact.id);
          if (q.step()) { identity = q.text(1); newlyUploaded = q.text(2) != "uploaded"; remote = decode(q.blob(0)); } else if (q.failed()) return fail(); }
        if (providers && item.uploadStatus == "uploaded") identity = publicationIdentity(item.receipt, *providers);
        if (!remote.contains("remoteStatus")) remote["remoteStatus"] = item.remoteStatus;
        { sqlite::Statement q(db_, "INSERT INTO item_state VALUES(?,?,?,?,?,?) ON CONFLICT(id) DO UPDATE SET remote_status=excluded.remote_status,data=excluded.data,publication_identity=excluded.publication_identity,upload_status=excluded.upload_status");
          q.bind(1, item.artifact.id).bind(2, op.id).bind(3, remote.value("remoteStatus").toString()).bindBlob(4, encode(remote)).bind(5, identity).bind(6, item.uploadStatus); if (!q.exec()) return fail(); }
        bool cleared = false;
        { sqlite::Statement q(db_, "SELECT 1 FROM cleared_history WHERE id=?"); q.bind(1, item.artifact.id); cleared = q.step(); if (q.failed()) return fail(); }
        if (!cleared) {
            auto data = item.toJson();
            for (const auto& pair : remote) data[pair.first] = pair.second;
            if (remote.contains("shareReceipt")) data["receipt"] = remote.value("shareReceipt");
            auto artifact = data.take("artifact").toObject(); artifact.remove("path");
            for (const auto& pair : artifact) data[pair.first] = pair.second;
            data["operationId"] = op.id; data["itemId"] = item.artifact.id; data["id"] = item.artifact.id; data["created"] = op.created; data["profileId"] = op.destination.profileId;
            sqlite::Statement q(db_, "INSERT INTO history(id,operation_id,item_id,created,name,mime,category,profile_id,upload_status,local_status,remote_status,data,publication_identity) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?) ON CONFLICT(id) DO UPDATE SET upload_status=excluded.upload_status,local_status=excluded.local_status,remote_status=excluded.remote_status,data=excluded.data,publication_identity=excluded.publication_identity");
            q.bind(1, item.artifact.id).bind(2, op.id).bind(3, item.artifact.id).bind(4, op.created).bind(5, item.artifact.name).bind(6, item.artifact.mime).bind(7, item.artifact.category).bind(8, op.destination.profileId)
             .bind(9, item.uploadStatus).bind(10, item.local.status).bind(11, remote.value("remoteStatus").toString()).bindBlob(12, encode(data)).bind(13, identity);
            if (!q.exec()) return fail();
        }
        if (newlyUploaded && item.uploadStatus == "uploaded" && item.condition == "match" && !identity.empty()) {
            // An explicit overwrite supersedes older records of the same object.
            std::vector<std::pair<std::string, json::Object>> older;
            { sqlite::Statement q(db_, "SELECT id,data FROM item_state WHERE publication_identity=? AND id<>? AND upload_status='uploaded' AND remote_status NOT IN ('removed','already_missing','changed','hidden_version_retained','superseded')");
              q.bind(1, identity).bind(2, item.artifact.id); while (q.step()) older.emplace_back(q.text(0), decode(q.blob(1))); if (q.failed()) return fail(); }
            for (auto& previous : older) {
                previous.second["remoteStatus"] = "superseded"; previous.second["supersededBy"] = item.artifact.id;
                sqlite::Statement s(db_, "UPDATE item_state SET remote_status='superseded',data=? WHERE id=?"); s.bindBlob(1, encode(previous.second)).bind(2, previous.first); if (!s.exec()) return fail();
                sqlite::Statement h(db_, "SELECT data FROM history WHERE id=?"); h.bind(1, previous.first);
                if (h.step()) { auto data = decode(h.blob(0)); data["remoteStatus"] = "superseded"; data["supersededBy"] = item.artifact.id;
                    sqlite::Statement u(db_, "UPDATE history SET remote_status='superseded',data=? WHERE id=?"); u.bindBlob(1, encode(data)).bind(2, previous.first); if (!u.exec()) return fail(); }
            }
        }
    }
    if (!db_.commit()) return fail();
    return {};
}
Operation HistoryStore::operation(const std::string& id, Error* error) const {
    if (error_) { if (error) *error = error_; return {}; }
    sqlite::Statement q(db_, "SELECT data FROM operations WHERE id=?"); q.bind(1, id);
    if (!q.step()) { if (error) *error = q.failed() ? sqlError() : Error{"operation_not_found", "No operation has this identifier", {}}; return {}; }
    auto op = Operation::fromJson(decode(q.blob(0)));
    // Remote-management state lives in item_state; hydrate it like the Linux store.
    for (auto& item : op.items) {
        sqlite::Statement s(db_, "SELECT data FROM item_state WHERE id=?"); s.bind(1, item.artifact.id);
        if (s.step()) { const auto state = decode(s.blob(0)); if (state.contains("remoteStatus")) item.remoteStatus = state.value("remoteStatus").toString();
            if (state.contains("shareReceipt")) item.receipt = StorageReceipt::fromJson(state.value("shareReceipt").toObject());
            if (state.contains("shareStatus")) item.shareStatus = state.value("shareStatus").toString();
            if (state.contains("shareIntent")) item.shareIntent = state.value("shareIntent").toString();
            if (state.contains("shareIntentOptions")) item.shareIntentOptions = state.value("shareIntentOptions").toObject(); }
        if (!item.artifact.path.empty()) item.artifact.path = artifactPathFor(root_, op.id, item.artifact.id);
    }
    return op;
}
std::vector<Operation> HistoryStore::recover(Error* error) {
    std::vector<Operation> result;
    if (error_) { if (error) *error = error_; return result; }
    std::vector<std::string> ids;
    { sqlite::Statement q(db_, "SELECT id FROM operations WHERE json_extract(data,'$.state') IN ('accepted','acquiring','editing','stored','running')"); while (q.step()) ids.push_back(q.text(0)); if (q.failed()) { if (error) *error = sqlError(); return result; } }
    for (const auto& id : ids) { Error e; auto op = operation(id, &e); if (!e) result.push_back(std::move(op)); }
    return result;
}
HistoryPage HistoryStore::list(const HistoryQuery& query) const {
    HistoryPage r;
    if (error_) { r.error = error_; return r; }
    if (query.limit < 1 || query.limit > 200 || query.offset < 0 || query.search.size() > 256) { r.error = {"invalid_query", "History page must be between 1 and 200 records", {}}; return r; }
    std::string where = " WHERE 1=1"; std::vector<std::string> values;
    auto filter = [&](const char* column, const std::string& value) { if (!value.empty()) { where += std::string(" AND ") + column + "=?"; values.push_back(value); } };
    filter("category", query.category); filter("mime", query.mime); filter("profile_id", query.profileId); filter("upload_status", query.uploadStatus); filter("local_status", query.localStatus); filter("remote_status", query.remoteStatus);
    if (!query.from.empty()) { where += " AND created>=?"; values.push_back(query.from); }
    if (!query.to.empty()) { where += " AND created<=?"; values.push_back(query.to); }
    if (!query.search.empty()) { where += " AND name LIKE ? ESCAPE '\\'"; auto escaped = text::replace(query.search, "\\", "\\\\"); escaped = text::replace(escaped, "%", "\\%"); escaped = text::replace(escaped, "_", "\\_"); values.push_back("%" + escaped + "%"); }
    { sqlite::Statement q(db_, "SELECT count(*) FROM history" + where); for (size_t i = 0; i < values.size(); ++i) q.bind(int(i + 1), values[i]); if (!q.step()) { r.error = sqlError(); return r; } r.total = q.integer(0); }
    sqlite::Statement q(db_, "SELECT data FROM history" + where + " ORDER BY created DESC,id LIMIT ? OFFSET ?");
    for (size_t i = 0; i < values.size(); ++i) q.bind(int(i + 1), values[i]);
    q.bind(int(values.size() + 1), (long long)query.limit).bind(int(values.size() + 2), (long long)query.offset);
    while (q.step()) r.records.push_back(publicRecord(decode(q.blob(0))));
    if (q.failed()) r.error = sqlError();
    return r;
}
json::Object HistoryStore::detail(const std::string& id, Error* error) const {
    if (error_) { if (error) *error = error_; return {}; }
    sqlite::Statement q(db_, "SELECT data FROM history WHERE id=?"); q.bind(1, id);
    if (!q.step()) { if (error) *error = q.failed() ? sqlError() : Error{"history_not_found", "History record not found", {}}; return {}; }
    return publicRecord(decode(q.blob(0)));
}
json::Array HistoryStore::recent(int limit, Error* error) const {
    json::Array result;
    if (error_) { if (error) *error = error_; return result; }
    if (limit < 1 || limit > 10000) { if (error) *error = {"invalid_query", "Invalid recent activity limit", {}}; return result; }
    sqlite::Statement q(db_, "SELECT id FROM operations ORDER BY json_extract(data,'$.created') DESC,id LIMIT ?"); q.bind(1, (long long)limit);
    std::vector<std::string> ids; while (q.step()) ids.push_back(q.text(0));
    if (q.failed()) { if (error) *error = sqlError(); return result; }
    for (const auto& id : ids) { Error e; auto op = operation(id, &e); if (!e) result.push_back(op.publicJson()); }
    return result;
}
std::vector<std::string> HistoryStore::activeDeletionHistoryIds() const {
    std::vector<std::string> ids;
    sqlite::Statement q(db_, "SELECT history_id FROM deletion_targets WHERE active=1"); while (q.step()) ids.push_back(q.text(0));
    return ids;
}
Error HistoryStore::expireBefore(const std::string& timestamp) {
    if (error_) return error_;
    sqlite::Statement q(db_, "DELETE FROM history WHERE created<? AND upload_status<>'uploading' AND id NOT IN (SELECT history_id FROM deletion_targets WHERE active=1)"); q.bind(1, timestamp);
    return q.exec() ? Error{} : sqlError();
}
Error HistoryStore::clear() {
    if (error_) return error_;
    if (!db_.begin()) return sqlError();
    const bool ok = db_.exec("INSERT OR IGNORE INTO cleared_history(id) SELECT id FROM history") && db_.exec("DELETE FROM history WHERE id NOT IN (SELECT history_id FROM deletion_targets WHERE active=1)");
    if (!ok || !db_.commit()) { db_.rollback(); return sqlError(); }
    return {};
}
Error HistoryStore::saveDeletion(const json::Object& doc) {
    if (error_) return error_;
    if (!db_.begin()) return sqlError();
    auto fail = [&] { db_.rollback(); return sqlError(); };
    const auto id = doc.value("id").toString();
    { sqlite::Statement q(db_, "INSERT INTO deletions VALUES(?,?) ON CONFLICT(id) DO UPDATE SET data=excluded.data"); q.bind(1, id).bindBlob(2, encode(doc)); if (!q.exec()) return fail(); }
    const bool active = doc.value("state") == "accepted" || doc.value("state") == "running";
    for (const auto& value : doc.value("items").toArray()) {
        const auto item = value.toObject();
        const auto historyId = item.value("historyId").toString();
        const auto status = item.value("status").toString();
        const bool itemActive = active && (status == "pending" || status == "deleting" || status == "unknown");
        { sqlite::Statement q(db_, "INSERT INTO deletion_targets VALUES(?,?,?) ON CONFLICT(deletion_id,history_id) DO UPDATE SET active=excluded.active"); q.bind(1, id).bind(2, historyId).bind(3, (long long)(itemActive ? 1 : 0)); if (!q.exec()) return fail(); }
        if (status == "pending" && doc.value("state") == "accepted") continue;
        json::Object fields{{"remoteStatus", status == "pending" ? std::string("pending") : status}, {"deleteOperationId", id}, {"deleteProgress", item.value("progress")}, {"remoteError", item.value("error")}};
        sqlite::Statement s(db_, "SELECT data FROM item_state WHERE id=?"); s.bind(1, historyId);
        if (s.step()) {
            auto state = decode(s.blob(0)); for (const auto& pair : fields) state[pair.first] = pair.second;
            sqlite::Statement u(db_, "UPDATE item_state SET remote_status=?,data=? WHERE id=?"); u.bind(1, fields.value("remoteStatus").toString()).bindBlob(2, encode(state)).bind(3, historyId); if (!u.exec()) return fail();
        }
        sqlite::Statement h(db_, "SELECT data FROM history WHERE id=?"); h.bind(1, historyId);
        if (h.step()) {
            auto data = decode(h.blob(0)); for (const auto& pair : fields) data[pair.first] = pair.second;
            sqlite::Statement u(db_, "UPDATE history SET remote_status=?,data=? WHERE id=?"); u.bind(1, fields.value("remoteStatus").toString()).bindBlob(2, encode(data)).bind(3, historyId); if (!u.exec()) return fail();
        }
    }
    if (!db_.commit()) return fail();
    return {};
}
json::Object HistoryStore::deletion(const std::string& id, Error* error) const {
    if (error_) { if (error) *error = error_; return {}; }
    sqlite::Statement q(db_, "SELECT data FROM deletions WHERE id=?"); q.bind(1, id);
    if (!q.step()) { if (error) *error = q.failed() ? sqlError() : Error{"deletion_not_found", "No deletion operation has this identifier", {}}; return {}; }
    return decode(q.blob(0));
}
json::Object HistoryStore::sharing(const std::string& id, Error* error) const {
    if (error_) { if (error) *error = error_; return {}; }
    sqlite::Statement q(db_, "SELECT data FROM sharing_operations WHERE id=?"); q.bind(1, id);
    if (!q.step()) { if (error) *error = q.failed() ? sqlError() : Error{"share_not_found", "No sharing operation has this identifier", {}}; return {}; }
    return decode(q.blob(0));
}
Error HistoryStore::saveSharing(const json::Object& doc) {
    if (error_) return error_;
    const auto id = doc.value("id").toString(), historyId = doc.value("historyId").toString();
    const auto receipt = StorageReceipt::fromJson(doc.value("receipt").toObject());
    if (id.empty() || historyId.empty() || receipt.validate()) return {"share_journal_invalid", "Incomplete sharing intent", {}};
    if (!db_.begin()) return sqlError();
    auto fail = [&] { db_.rollback(); return sqlError(); };
    json::Object remote;
    { sqlite::Statement q(db_, "SELECT data FROM item_state WHERE id=?"); q.bind(1, historyId);
      if (!q.step()) { db_.rollback(); return q.failed() ? sqlError() : Error{"history_not_found", "Uploaded item ledger not found", {}}; }
      remote = decode(q.blob(0)); }
    // Remember a create until it is confirmed shared or explicitly abandoned.
    // A read-only inspection that proves private resolves uncertainty, not intent.
    auto intent = remote.value("shareIntent").toString();
    const auto status = doc.value("status").toString(), action = doc.value("action").toString();
    if (action == "create") { intent = "create"; remote["shareIntentOptions"] = doc.value("options").toObject(); }
    if (action == "revoke" || status == "shared" || status == "revoked" || status == "changed") intent.clear();
    if (intent.empty()) remote["shareIntentOptions"] = json::Object{};
    // A confirmed result replaces the recorded link expiry; an uncertain one keeps the last confirmed value.
    if (status == "shared" || status == "private" || status == "revoked" || status == "changed") { if (doc.value("expiresAt").isString()) remote["shareExpiresAt"] = doc.value("expiresAt"); else remote.remove("shareExpiresAt"); }
    remote["shareIntent"] = intent; remote["shareStatus"] = status; remote["shareReceipt"] = doc.value("receipt"); remote["shareOperationId"] = id; remote["shareProgress"] = doc.value("progress");
    { sqlite::Statement q(db_, "INSERT INTO sharing_operations VALUES(?,?,?) ON CONFLICT(id) DO UPDATE SET data=excluded.data"); q.bind(1, id).bind(2, historyId).bindBlob(3, encode(doc)); if (!q.exec()) return fail(); }
    { sqlite::Statement u(db_, "UPDATE item_state SET data=? WHERE id=?"); u.bindBlob(1, encode(remote)).bind(2, historyId); if (!u.exec()) return fail(); }
    { sqlite::Statement h(db_, "SELECT data FROM history WHERE id=?"); h.bind(1, historyId);
      if (h.step()) { auto data = decode(h.blob(0)); for (const auto& pair : remote) data[pair.first] = pair.second; if (!remote.contains("shareExpiresAt")) data.remove("shareExpiresAt"); data["receipt"] = doc.value("receipt");
          sqlite::Statement u(db_, "UPDATE history SET data=? WHERE id=?"); u.bindBlob(1, encode(data)).bind(2, historyId); if (!u.exec()) return fail(); }
      else if (h.failed()) return fail(); }
    if (!db_.commit()) return fail();
    return {};
}
Error HistoryStore::updateRemote(const std::string& historyId, const json::Object& fields) {
    if (error_) return error_;
    if (!db_.begin()) return sqlError();
    auto fail = [&] { db_.rollback(); return sqlError(); };
    for (const char* table : {"item_state", "history"}) {
        sqlite::Statement s(db_, std::string("SELECT data FROM ") + table + " WHERE id=?"); s.bind(1, historyId);
        if (!s.step()) { if (s.failed()) return fail(); continue; }
        auto data = decode(s.blob(0)); for (const auto& pair : fields) data[pair.first] = pair.second;
        sqlite::Statement u(db_, std::string("UPDATE ") + table + " SET remote_status=?,data=? WHERE id=?");
        u.bind(1, data.value("remoteStatus").toString("not_requested")).bindBlob(2, encode(data)).bind(3, historyId); if (!u.exec()) return fail();
    }
    if (!db_.commit()) return fail();
    return {};
}
}
