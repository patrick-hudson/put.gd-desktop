#include "resident/HistoryService.h"
#include "common/Files.h"
#include "common/Log.h"
#include "common/Paths.h"
#include "common/Sha256.h"
#include "common/Uuid.h"
#include "resident/Resident.h"
#include "common/Text.h"
#include <algorithm>
#include <shellapi.h>

namespace putgd {
namespace {
Error notReady() { return {"initializing", "Wait for resident journal recovery", {}}; }
bool in(const std::vector<std::string>& list, const std::string& v) { return std::find(list.begin(), list.end(), v) != list.end(); }
}
HistoryService::HistoryService(OperationService& operations) : operations_(operations) {
    Loop::instance().startTimer(60 * 60 * 1000, [this] { expireConfigured(); });
}
void HistoryService::notifyChanged() { if (onChanged) onChanged(); }
void HistoryService::shutdown() { for (auto& pair : active_) pair.second->store(true); }
void HistoryService::expireConfigured() {
    const int days = operations_.settingsSnapshot().historyRetentionDays;
    if (days <= 0 || !operations_.ready()) return;
    const auto before = files::isoUtcFromMs(files::parseIsoUtcMs(files::nowIsoUtc()) - (long long)days * 86400000LL);
    const auto root = operations_.dataRoot(), path = operations_.databasePath();
    operations_.background([root, path, before](Error& e) { HistoryStore store(root, path); e = store.expireBefore(before); return json::Object{}; }, [this](Error e, json::Object) { if (!e) notifyChanged(); });
}
void HistoryService::list(const HistoryQuery& query, JsonReply reply) {
    if (!operations_.ready()) { reply(notReady(), {}); return; }
    const auto root = operations_.dataRoot(), path = operations_.databasePath();
    operations_.background([root, path, query](Error& e) { HistoryStore store(root, path); auto page = store.list(query); e = page.error; return page.toJson(); }, reply);
}
void HistoryService::detail(const std::string& id, JsonReply reply) {
    if (!operations_.ready()) { reply(notReady(), {}); return; }
    const auto root = operations_.dataRoot(), path = operations_.databasePath();
    operations_.background([this, root, path, id](Error& e) {
        HistoryStore store(root, path);
        auto data = store.detail(id, &e);
        if (!e) {
            const auto receipt = StorageReceipt::fromJson(data.value("receipt").toObject());
            if (auto provider = operations_.providers().find(receipt.destination.providerId)) data["capabilities"] = provider->capabilities(receipt.destination).toJson();
        }
        return data;
    }, reply);
}
void HistoryService::clear(JsonReply reply) {
    if (!operations_.ready()) { reply(notReady(), {}); return; }
    const auto root = operations_.dataRoot(), path = operations_.databasePath();
    operations_.background([root, path](Error& e) { HistoryStore store(root, path); e = store.clear(); return json::Object{{"cleared", !e}, {"pendingDeletionTargetsRetained", true}}; },
        [this, reply](Error e, json::Object result) { if (!e) notifyChanged(); reply(e, result); });
}
void HistoryService::copyImage(const std::string& id, JsonReply reply) { if (!operations_.ready()) { reply(notReady(), {}); return; } operations_.copyHistoryImage(id, std::move(reply)); }
void HistoryService::copyLink(const std::string& id, JsonReply reply) {
    operations_.invalidateCopyEligibility();
    detail(id, [this, reply](Error e, json::Object data) {
        if (e) { reply(e, {}); return; }
        if (!data.value("linkAvailable").toBool()) { reply({"link_unavailable", "This recorded link is no longer available", {}}, {}); return; }
        operations_.copyHistoryUrl(data.value("receipt").toObject().value("directUrl").toString(), reply);
    });
}
void HistoryService::openLink(const std::string& id, JsonReply reply) {
    detail(id, [reply](Error e, json::Object data) {
        if (e) { reply(e, {}); return; }
        if (!data.value("linkAvailable").toBool()) { reply({"link_unavailable", "This recorded link is no longer available", {}}, {}); return; }
        const auto url = data.value("receipt").toObject().value("directUrl").toString();
        const auto parsed = text::parseUrl(url);
        if (!parsed.valid || (parsed.scheme != "https" && parsed.scheme != "http")) { reply({"link_unavailable", "The recorded link is not an http(s) URL", {}}, {}); return; }
        const auto code = reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", paths::wide(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL));
        if (code <= 32) { reply({"link_not_opened", "No application opened the link", {{"status", double(code)}}}, {}); return; }
        reply({}, {{"route", "launcher"}, {"opened", true}, {"fallbackFrom", json::Value()}});
    });
}
void HistoryService::preview(const std::string& id, JsonReply reply) {
    detail(id, [this, reply](Error e, json::Object data) {
        if (e) { reply(e, {}); return; }
        const auto path = data.value("previewPath").toString();
        if (!path.empty() && files::isRegularFile(path)) { reply({}, {{"state", "ready"}, {"previewPath", path}, {"source", "cache"}}); return; }
        // Remake from the retained artifact or an unchanged local copy, in the image job.
        Artifact artifact = Artifact::fromJson(data);
        artifact.id = data.value("id").toString();
        const auto operationId = data.value("operationId").toString();
        artifact.path = paths::join(paths::join(paths::join(operations_.dataRoot(), "artifacts"), operationId), artifact.id);
        std::string source = "artifact";
        const auto local = data.value("local").toObject();
        if (!files::isRegularFile(artifact.path) && local.value("status") == "saved" && files::isRegularFile(local.value("path").toString())) { artifact.path = local.value("path").toString(); source = "local-copy"; }
        if (!files::isRegularFile(artifact.path)) { reply({"preview_unavailable", "Neither the retained artifact nor an unchanged local copy is available", {}}, {}); return; }
        const auto snapshot = operations_.settingsSnapshot();
        operations_.background([this, artifact, snapshot](Error& e) {
            ArtifactStore store(operations_.dataRoot());
            const auto made = store.preview(artifact, snapshot.previewCacheBytes, snapshot.limits.imagePixels);
            if (made.empty()) e = {"preview_unavailable", "No preview could be made from this file", {}};
            return json::Object{{"previewPath", made}};
        }, [reply, source](Error e, json::Object result) { if (e) reply(e, {}); else reply({}, {{"state", "made"}, {"previewPath", result.value("previewPath")}, {"source", source}}); });
    });
}
json::Object HistoryService::prepare(const std::vector<std::string>& ids, Error& e) const {
    if (ids.empty() || ids.size() > 1000) { e = {"invalid_selection", "Select between 1 and 1000 confirmed uploads", {}}; return {}; }
    HistoryStore store(operations_.dataRoot(), operations_.databasePath());
    json::Array targets; std::set<std::string> seen;
    for (const auto& id : ids) {
        if (seen.count(id)) { e = {"invalid_selection", "Duplicate history ID", {}}; return {}; }
        seen.insert(id);
        auto data = store.detail(id, &e); if (e) return {};
        if (data.value("uploadStatus") != "uploaded") { e = {"delete_target_invalid", "Only confirmed uploaded entries can be deleted", {{"historyId", id}}}; return {}; }
        const auto remote = data.value("remoteStatus").toString();
        if (in({"pending", "deleting", "unknown", "changed", "hidden_version_retained"}, remote)) { e = {"delete_target_changed", "Resolve or retry the existing deletion identity", {{"historyId", id}}}; return {}; }
        auto receipt = StorageReceipt::fromJson(data.value("receipt").toObject());
        if (auto error = receipt.validate()) { e = error; return {}; }
        auto provider = operations_.providers().find(receipt.destination.providerId);
        if (!provider) { e = {"provider_unavailable", "Restore access to the original destination", {{"historyId", id}}}; return {}; }
        const auto caps = provider->capabilities(receipt.destination);
        if (!caps.remove || !caps.stat || (!caps.conditionalDelete && !caps.versionAddressing) || caps.deletionEffect.empty()) { e = {"unsupported_capability", "Provider cannot safely delete this recorded object", {{"historyId", id}}}; return {}; }
        targets.push_back(json::Object{{"historyId", id}, {"receipt", receipt.toJson()}, {"effect", caps.deletionEffect}, {"name", data.value("name")}});
    }
    const auto digest = crypto::Sha256::hexDigest(json::serialize(targets));
    return {{"targets", targets}, {"acknowledgement", digest}, {"localCopiesPreserved", true}, {"cacheNotice", "Origin deletion does not invalidate browser or CDN caches"}};
}
void HistoryService::prepareDelete(const std::vector<std::string>& ids, JsonReply reply) {
    if (!operations_.ready()) { reply(notReady(), {}); return; }
    operations_.background([this, ids](Error& e) { return prepare(ids, e); }, reply);
}
void HistoryService::deleteConfirmed(const std::vector<std::string>& ids, const std::string& acknowledgement, JsonReply reply) {
    if (!operations_.ready()) { reply(notReady(), {}); return; }
    operations_.background([this, ids, acknowledgement](Error& e) {
        auto preview = prepare(ids, e); if (e) return json::Object{};
        if (acknowledgement.empty() || preview.value("acknowledgement") != acknowledgement.c_str()) { e = {"delete_confirmation_changed", "Acknowledge the current exact target set and effect", {}}; return json::Object{}; }
        json::Array items;
        for (auto target : preview.value("targets").toArray()) { auto item = target.toObject(); item.remove("name"); item["status"] = "pending"; item["progress"] = json::Object{{"schemaVersion", 1}}; items.push_back(item); }
        json::Object doc{{"id", uuid::create()}, {"created", files::nowIsoUtc()}, {"state", "accepted"}, {"acknowledgement", acknowledgement}, {"items", items}, {"cancelled", false}};
        HistoryStore store(operations_.dataRoot(), operations_.databasePath());
        e = store.saveDeletion(doc);
        return doc;
    }, [this, reply](Error e, json::Object doc) {
        if (e) { reply(e, {}); return; }
        notifyChanged();
        const auto id = doc.value("id").toString();
        auto flag = std::make_shared<std::atomic_bool>(false);
        active_[id] = flag;
        reply({}, {{"deleteOperationId", id}, {"deletion", doc}});
        runDelete(doc, flag);
    });
}
void HistoryService::runDelete(json::Object doc, CancelFlag cancelled) {
    const auto root = operations_.dataRoot(), path = operations_.databasePath(), id = doc.value("id").toString();
    operations_.background([this, doc, cancelled, root, path](Error& error) mutable {
        HistoryStore store(root, path);
        doc["state"] = "running";
        if ((error = store.saveDeletion(doc))) return doc;
        Loop::instance().post([this] { notifyChanged(); });
        auto items = doc.value("items").toArray();
        for (size_t index = 0; index < items.size(); ++index) {
            auto item = items[index].toObject();
            const auto status = item.value("status").toString();
            if (status == "removed" || status == "already_missing" || status == "changed") continue;
            if (cancelled->load()) { doc["cancelled"] = true; break; }
            const auto receipt = StorageReceipt::fromJson(item.value("receipt").toObject());
            auto provider = operations_.providers().find(receipt.destination.providerId);
            Error e; std::string outcome; json::Object progress = item.value("progress").toObject();
            if (!provider) { e = {"provider_unavailable", "Restore access to the recorded destination", {}}; outcome = "failed"; }
            else {
                auto lock = operations_.objectLock(receipt.destination, receipt.objectReference, {});
                std::lock_guard guard(*lock);
                const bool driverContinues = progress.contains("phase");
                auto stat = driverContinues ? StatResult{"own", receipt, {}} : provider->stat(receipt, cancelled);
                if (stat.status == "missing") outcome = "already_missing";
                else if (stat.status == "foreign") outcome = "changed";
                else if (stat.status != "own" || stat.error) { outcome = "unknown"; e = stat.error ? stat.error : Error{"delete_unknown", "Recorded object identity could not be reconciled", {}}; }
                else if (stat.receipt.publicationId != receipt.publicationId || stat.receipt.objectReference.value("key") != receipt.objectReference.value("key") || stat.receipt.destination.toJson() != receipt.destination.toJson()) { outcome = "changed"; e = {"delete_target_changed", "The recorded target has changed", {}}; }
                else {
                    item["status"] = "deleting"; items[index] = item; doc["items"] = items;
                    if ((error = store.saveDeletion(doc))) return doc;
                    Loop::instance().post([this] { notifyChanged(); });
                    auto checkpoint = [&](const json::Object& checkpointProgress) -> Error {
                        if (!json::integer(checkpointProgress.value("schemaVersion"), 1, 1e9)) return {"delete_checkpoint_invalid", "Driver progress requires a version", {}};
                        auto proposedItems = items; auto target = proposedItems[index].toObject(); target["progress"] = checkpointProgress; proposedItems[index] = target;
                        auto proposed = doc; proposed["items"] = proposedItems;
                        HistoryStore durable(root, path);
                        const auto saveError = durable.saveDeletion(proposed);
                        if (!saveError) { items = std::move(proposedItems); doc = std::move(proposed); Loop::instance().post([this] { notifyChanged(); }); }
                        return saveError;
                    };
                    auto result = provider->remove({doc.value("id").toString(), receipt, progress}, checkpoint, cancelled);
                    outcome = result.status == "missing" ? "already_missing" : result.status;
                    progress = result.progress.empty() ? items[index].toObject().value("progress").toObject() : result.progress;
                    e = result.error;
                    if (!in({"removed", "already_missing", "changed", "failed", "unknown", "hidden_version_retained", "version_retained", "unsupported_safe_removal"}, outcome)) { outcome = "unknown"; e = {"delete_unknown", "Provider returned an unrecognized deletion result", {}}; }
                }
            }
            item = items[index].toObject(); item["status"] = outcome; item["progress"] = progress; item["error"] = e.toJson(); items[index] = item; doc["items"] = items;
            if ((error = store.saveDeletion(doc))) return doc;
            Loop::instance().post([this] { notifyChanged(); });
        }
        doc["state"] = cancelled->load() ? "cancelled" : "completed"; doc["items"] = items;
        error = store.saveDeletion(doc);
        return doc;
    }, [this, id](Error e, json::Object) { if (e) log::warning("history", "Deletion journal write failed", {{"deleteOperationId", id}, {"code", e.code}}); active_.erase(id); notifyChanged(); });
}
void HistoryService::inspectDelete(const std::string& id, JsonReply reply) {
    const auto root = operations_.dataRoot(), path = operations_.databasePath();
    operations_.background([root, path, id](Error& e) { HistoryStore store(root, path); return store.deletion(id, &e); }, reply);
}
void HistoryService::retryDelete(const std::string& id, JsonReply reply) {
    if (active_.count(id)) { reply({"delete_busy", "Deletion is active", {}}, {}); return; }
    inspectDelete(id, [this, reply](Error e, json::Object doc) {
        if (e) { reply(e, {}); return; }
        const auto id = doc.value("id").toString();
        if (active_.count(id)) { reply({"delete_busy", "Deletion is active", {}}, {}); return; }
        auto flag = std::make_shared<std::atomic_bool>(false);
        active_[id] = flag;
        reply({}, {{"deleteOperationId", id}, {"retryAccepted", true}});
        runDelete(doc, flag);
    });
}
void HistoryService::cancelDelete(const std::string& id, JsonReply reply) {
    const auto it = active_.find(id);
    if (it == active_.end()) { reply({"delete_not_active", "Deletion is not active", {}}, {}); return; }
    it->second->store(true);
    reply({}, {{"deleteOperationId", id}, {"cancellationRequested", true}, {"effect", "Already sent deletion cannot be rolled back"}});
}

// ---- sharing ---------------------------------------------------------------------
void HistoryService::inspectSharing(const std::string& id, JsonReply reply) {
    if (!operations_.ready()) { reply(notReady(), {}); return; }
    const auto stopped = stoppedSharing_.find(id);
    if (stopped != stoppedSharing_.end()) { reply({}, stopped->second); return; }
    const auto root = operations_.dataRoot(), path = operations_.databasePath();
    operations_.background([root, path, id](Error& e) { HistoryStore store(root, path); return store.sharing(id, &e); }, std::move(reply));
}
void HistoryService::changeSharing(const std::string& historyId, const std::string& action, const json::Object& options, JsonReply reply) {
    if (!operations_.ready()) { reply(notReady(), {}); return; }
    if (!in({"create", "revoke", "inspect"}, action)) { reply({"invalid_request", "Invalid sharing action", {}}, {}); return; }
    for (const auto& pair : options) if (pair.first != "expirationDays" && pair.first != "passwordReference") { reply({"invalid_request", "Sharing options contain unsupported fields", {}}, {}); return; }
    if (options.contains("expirationDays") && !json::integer(options.value("expirationDays"), 0, 365)) { reply({"invalid_request", "Invalid link expiration", {}}, {}); return; }
    if (options.contains("passwordReference") && !options.value("passwordReference").isNull() && (!options.value("passwordReference").isString() || options.value("passwordReference").stringRef().empty() || options.value("passwordReference").stringRef().size() > 256)) { reply({"invalid_request", "Use a keychain password reference", {}}, {}); return; }
    if (action != "inspect") operations_.invalidateCopyEligibility();
    const auto root = operations_.dataRoot(), path = operations_.databasePath();
    std::map<std::string, json::Object> stopped;
    for (const auto& pair : stoppedSharing_) if (pair.second.value("historyId") == historyId.c_str()) stopped.insert(pair);
    operations_.background([this, root, path, historyId, action, options, stopped](Error& e) {
        HistoryStore store(root, path);
        auto item = store.detail(historyId, &e); if (e) return json::Object{};
        auto receipt = StorageReceipt::fromJson(item.value("receipt").toObject());
        auto provider = operations_.providers().find(receipt.destination.providerId);
        if (receipt.validate() || !provider || !provider->capabilities(receipt.destination).sharing) { e = {"sharing_unsupported", "This provider cannot manage share links", {}}; return json::Object{}; }
        auto lock = operations_.objectLock(receipt.destination, receipt.objectReference, {}); std::lock_guard guard(*lock);
        item = store.detail(historyId, &e); if (e) return json::Object{};
        const auto previousId = item.value("shareOperationId").toString();
        if (stopped.count(previousId) && action == "inspect") {
            // A failed terminal write left a durable pending intent. Commit an unknown,
            // URL-cleared outcome before permitting a new read-only reconciliation.
            e = store.saveSharing(stopped.at(previousId)); if (e) return json::Object{};
            item = store.detail(historyId, &e); if (e) return json::Object{};
        }
        if (item.value("uploadStatus") != "uploaded" || in({"pending", "deleting", "unknown", "removed", "already_missing", "changed"}, item.value("remoteStatus").toString())) { e = {"share_unavailable", "This item has no available confirmed upload", {}}; return json::Object{}; }
        const auto shareStatus = item.value("shareStatus").toString();
        if (shareStatus == "pending" || (action != "inspect" && shareStatus == "unknown")) { e = {"share_result_unknown", "Wait for or inspect the previous sharing result before another mutation", {}}; return json::Object{}; }
        receipt = StorageReceipt::fromJson(item.value("receipt").toObject()); if (action == "revoke") receipt.directUrl.clear();
        json::Object doc{{"id", uuid::create()}, {"historyId", historyId}, {"action", action}, {"options", options}, {"state", "accepted"}, {"status", "pending"}, {"receipt", receipt.toJson()}, {"progress", json::Object{}}};
        e = store.saveSharing(doc); return doc;
    }, [this, reply, stopped](Error e, json::Object doc) {
        if (e) { reply(e, {}); return; }
        for (const auto& pair : stopped) stoppedSharing_.erase(pair.first);
        const auto id = doc.value("id").toString();
        auto cancelled = std::make_shared<std::atomic_bool>(false); active_[id] = cancelled; notifyChanged();
        reply({}, {{"shareOperationId", id}, {"sharing", doc}});
        runSharing(doc, cancelled);
    });
}
void HistoryService::recordStoppedSharing(json::Object doc, const Error& error) {
    doc["state"] = "interrupted"; doc["status"] = "unknown"; doc["error"] = error.toJson();
    auto receipt = doc.value("receipt").toObject(); receipt["directUrl"] = ""; doc["receipt"] = receipt;
    stoppedSharing_[doc.value("id").toString()] = doc; notifyChanged();
}
void HistoryService::runSharing(json::Object doc, CancelFlag cancelled) {
    const auto root = operations_.dataRoot(), path = operations_.databasePath(), id = doc.value("id").toString();
    operations_.background([this, root, path, doc, cancelled](Error& e) mutable {
        HistoryStore store(root, path);
        auto receipt = StorageReceipt::fromJson(doc.value("receipt").toObject());
        auto provider = operations_.providers().find(receipt.destination.providerId);
        if (!provider) { e = {"provider_unavailable", "Restore access to the original destination", {}}; return doc; }
        auto lock = operations_.objectLock(receipt.destination, receipt.objectReference, {}); std::lock_guard guard(*lock);
        doc["state"] = "running"; if ((e = store.saveSharing(doc))) return doc;
        Loop::instance().post([this] { notifyChanged(); });
        auto checkpoint = [root, path, &doc](const json::Object& progress) -> Error {
            auto proposed = doc; proposed["progress"] = progress; HistoryStore durable(root, path);
            const auto error = durable.saveSharing(proposed); if (!error) doc = std::move(proposed); return error;
        };
        ShareRequest request{doc.value("id").toString(), doc.value("action").toString(), receipt, doc.value("options").toObject(), {}, cancelled};
        auto result = provider->share(request, checkpoint, cancelled);
        if (!shareResultMatchesReceipt(result, receipt)) { result.status = "unknown"; result.receipt = receipt; result.expiresAt.clear(); result.error = {"share_identity_changed", "Sharing did not confirm the recorded publication identity", {}}; }
        if (!result.expiresAt.empty()) doc["expiresAt"] = result.expiresAt;
        if (result.status != "shared") result.receipt.directUrl.clear();
        doc["state"] = "completed"; doc["status"] = result.status; doc["receipt"] = result.receipt.toJson(); doc["progress"] = result.progress; doc["error"] = result.error.toJson();
        e = store.saveSharing(doc); return doc;
    }, [this, id](Error e, json::Object doc) {
        if (e) recordStoppedSharing(doc, e);
        else log::info("history", "Sharing finished", {{"shareOperationId", id}, {"action", doc.value("action")}, {"status", doc.value("status")}, {"error", doc.value("error")["code"]}});
        active_.erase(id); notifyChanged();
    });
}
}
