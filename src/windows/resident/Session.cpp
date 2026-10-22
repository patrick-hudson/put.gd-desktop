#include "resident/Session.h"
#include "common/Files.h"
#include "common/Log.h"
#include "common/Paths.h"
#include "common/Text.h"
#include "common/Uuid.h"
#include "core/SanitizedPng.h"
#include "resident/Diagnostics.h"
#include "resident/Resident.h"
#include "resident/RunMarker.h"
#include <algorithm>
#include <set>
#include <shellapi.h>

namespace putgd {
namespace {
Error invalid() { return {"invalid_request", "Invalid request fields or parameter types", {}}; }
bool fields(const json::Object& object, const std::vector<std::string>& required, const std::vector<std::string>& optional = {}) {
    for (const auto& pair : object) if (std::find(required.begin(), required.end(), pair.first) == required.end() && std::find(optional.begin(), optional.end(), pair.first) == optional.end()) return false;
    for (const auto& key : required) if (!object.contains(key)) return false;
    return true;
}
bool stringField(const json::Object& object, const std::string& key, size_t max = 256) { const auto& v = object.value(key); return v.isString() && !v.stringRef().empty() && v.stringRef().size() <= max; }
bool startsWith(const std::string& s, const char* p) { return text::startsWith(s, p); }
}
Session::Session(SettingsService& settings, OperationService& operations, HistoryService& history, SetupService& setup, UpdateService& updates, UiProcess& ui, std::string logs)
    : epoch_(uuid::create()), logs_(std::move(logs)), settings_(settings), operations_(operations), history_(history), setup_(setup), updates_(updates), ui_(ui) {}
json::Object Session::response(const std::string& id, const Error& error, const json::Object& result) {
    return {{"schemaVersion", 1}, {"requestId", id}, {"ok", !error}, {"result", error ? json::Object{} : result}, {"error", error.toJson()}};
}
Error Session::listen(const std::string& path) {
    const auto probe = pipe::probe(path, 200);
    if (probe == pipe::Probe::Connected) return {"session_running", "A resident process already owns this session", {}};
    const bool ok = server_.listen(path, [this](std::string request, std::function<void(std::string)> reply) {
        Loop::instance().post([this, request = std::move(request), reply = std::move(reply)] {
            const auto object = json::parseObject(request);
            if (!object) { log::info("session", "Session request rejected", {{"code", "invalid_json"}}); reply(json::serialize(response("", {"invalid_json", "Request must be a JSON object", {}}, {}))); return; }
            const auto id = object->value("requestId").toString();
            const auto method = object->value("method").toString().substr(0, 64);
            dispatch(*object, [reply, id, method](Error e, json::Object result) {
                // Malformed or misdirected requests; parameters are never written.
                if (e.code == "invalid_request" || e.code == "unsupported_version" || e.code == "wrong_session" || e.code == "unknown_method")
                    log::info("session", "Session request rejected", {{"method", method}, {"code", e.code}});
                reply(json::serialize(response(id, e, result)));
            });
        });
    });
    if (!ok) return {"session_unavailable", "Cannot listen on the user session pipe", {}};
    return {};
}
void Session::stop() { server_.stop(); }
json::Object Session::snapshot() const {
    return {{"build", protocol::buildIdentity()}, {"residentEpoch", epoch_}, {"operationsReady", operations_.ready()},
        {"settings", json::Object{{"document", settings_.document()}, {"metadata", settings_.metadata()}}}, {"diagnostics", diagnosticsDocument()}};
}
json::Object Session::diagnosticsDocument() const {
    auto d = operations_.diagnostics();
    d["ui"] = ui_.diagnostics(); d["updates"] = updates_.status();
    const auto logging = log::status();
    d["logging"] = logging; d["loggingDegraded"] = logging.value("degraded");
    d["uncleanExit"] = runmarker::report();
    if (diagnostics_) d["crashDumps"] = diagnostics_->crashDumps();
    d["locations"] = json::Object{{"config", json::Object{{"path", paths::parent(settings_.path())}}}, {"data", json::Object{{"path", operations_.dataRoot()}}}};
    return d;
}
void Session::dispatch(const json::Object& request, JsonReply reply) {
    if (!fields(request, {"schemaVersion", "requestId", "session", "method", "params"}) || !stringField(request, "requestId", 128) || !stringField(request, "method", 64) || !request.value("params").isObject() || !request.value("session").isString()) { reply(invalid(), {}); return; }
    if (!json::integer(request.value("schemaVersion"), 1, 1)) { reply({"unsupported_version", "Request schemaVersion must be 1", {}}, {}); return; }
    if (request.value("session").stringRef() != server_.path()) { reply({"wrong_session", "Request targets a different resident session", {}}, {}); return; }
    const auto method = request.value("method").stringRef();
    const auto p = request.value("params").toObject();
    if (!operations_.ready() && (startsWith(method, "operation.") || startsWith(method, "history.") || startsWith(method, "delete.") || startsWith(method, "share.") || method == "cache.clear")) { reply({"initializing", "Wait for resident journal recovery", {}}, {}); return; }
    if (method == "ui.quit") { if (!p.empty()) { reply(invalid(), {}); return; } reply({}, {{"quitting", true}}); Loop::instance().singleShot(0, [this] { if (onQuit) onQuit(); }); return; }
    if (method == "ui.show") {
        if (!fields(p, {"view"}) || !p.value("view").isString()) { reply(invalid(), {}); return; }
        const auto view = p.value("view").stringRef();
        ui_.show(view, [reply, view](Error e) { reply(e, {{"shown", view}}); });
        return;
    }
    if (method == "ui.snapshot") { if (!p.empty()) { reply(invalid(), {}); return; } reply({}, snapshot()); return; }
    if (startsWith(method, "ui.")) { reply({"ui_unavailable", "This window command is not available in the Windows build", {{"method", method}}}, {}); return; }
    if (startsWith(method, "recording.")) { reply({"recording_unavailable", "Screen recording is not available in the Windows build yet", {}}, {}); return; }
    if (method == "action.invoke") {
        if (!fields(p, {"action"}, {"source", "path", "mode"}) || !stringField(p, "action") || (p.contains("source") && !stringField(p, "source", 32)) || (p.contains("path") && !stringField(p, "path", 4096))) { reply(invalid(), {}); return; }
        auto options = p; options.remove("action"); operations_.invoke(p.value("action").stringRef(), options, reply); return;
    }
    if (method == "editor.open") {
        if (!fields(p, {}, {"source", "path"}) || (p.contains("source") && !stringField(p, "source", 32)) || (p.contains("path") && !stringField(p, "path", 4096))) { reply(invalid(), {}); return; }
        operations_.invoke("edit", p, reply); return;
    }
    if (method == "editor.fileDialog") { reply({"invalid_request", "The editor's file dialog cannot be driven in the Windows build", {}}, {}); return; }
    if (startsWith(method, "editor.")) {
        if (!stringField(p, "id")) { reply(invalid(), {}); return; }
        const auto command = method.substr(7);
        if (!std::set<std::string>{"inspect", "apply", "preview", "saveCopy", "submit", "cancel", "raise"}.count(command)) { reply({"unknown_method", "Unknown editor method", {}}, {}); return; }
        auto params = p; params.remove("id"); operations_.editorCommand(p.value("id").stringRef(), command, params, reply); return;
    }
    if (method == "operation.recent") { if (!p.empty()) reply(invalid(), {}); else operations_.recent(reply); return; }
    if (startsWith(method, "operation.")) {
        if (!fields(p, {"id"}, method == "operation.cancel" ? std::vector<std::string>{"itemId"} : std::vector<std::string>{}) || !stringField(p, "id")) { reply(invalid(), {}); return; }
        const auto id = p.value("id").stringRef();
        if (method == "operation.inspect") operations_.inspect(id, reply);
        else if (method == "operation.cancel") operations_.cancel(id, p.value("itemId").toString(), reply);
        else if (method == "operation.retry") operations_.retry(id, reply);
        else if (method == "operation.copy") operations_.copyResult(id, reply);
        else if (method == "operation.retryLocalSave") operations_.retryLocalSave(id, reply);
        else reply({"unknown_method", "Unknown operation method", {}}, {});
        return;
    }
    if (method == "history.share" || method == "history.revokeLink" || method == "history.inspectLink") {
        if (!fields(p, {"id"}) || !stringField(p, "id")) { reply(invalid(), {}); return; }
        history_.changeSharing(p.value("id").stringRef(), method == "history.share" ? "create" : method == "history.revokeLink" ? "revoke" : "inspect", {}, reply); return;
    }
    if (method == "share.inspect") { if (!fields(p, {"id"}) || !stringField(p, "id")) { reply(invalid(), {}); return; } history_.inspectSharing(p.value("id").stringRef(), reply); return; }
    if (method == "history.list") {
        const std::vector<std::string> allowed{"limit", "offset", "search", "category", "mime", "profileId", "uploadStatus", "localStatus", "remoteStatus", "from", "to"};
        if (!fields(p, {}, allowed)) { reply(invalid(), {}); return; }
        for (const auto& pair : p) {
            if (pair.first == "limit" || pair.first == "offset") { if (!json::integer(pair.second, 0, 2147483647.0)) { reply(invalid(), {}); return; } }
            else if (!pair.second.isString() || pair.second.stringRef().size() > 256) { reply(invalid(), {}); return; }
        }
        HistoryQuery q; q.limit = p.value("limit").toInt(50); q.offset = p.value("offset").toInt(0);
        q.search = p.value("search").toString(); q.category = p.value("category").toString(); q.mime = p.value("mime").toString(); q.profileId = p.value("profileId").toString();
        q.uploadStatus = p.value("uploadStatus").toString(); q.localStatus = p.value("localStatus").toString(); q.remoteStatus = p.value("remoteStatus").toString(); q.from = p.value("from").toString(); q.to = p.value("to").toString();
        history_.list(q, reply); return;
    }
    if (method == "history.detail" || method == "history.copyLink" || method == "history.copyImage" || method == "history.openLink" || method == "history.preview" ||
        (startsWith(method, "delete.") && method != "delete.prepare" && method != "delete.confirm")) {
        if (!fields(p, {"id"}) || !stringField(p, "id")) { reply(invalid(), {}); return; }
        const auto id = p.value("id").stringRef();
        if (method == "history.detail") history_.detail(id, reply);
        else if (method == "history.copyLink") history_.copyLink(id, reply);
        else if (method == "history.copyImage") history_.copyImage(id, reply);
        else if (method == "history.openLink") history_.openLink(id, reply);
        else if (method == "history.preview") history_.preview(id, reply);
        else if (method == "delete.inspect") history_.inspectDelete(id, reply);
        else if (method == "delete.retry") history_.retryDelete(id, reply);
        else if (method == "delete.cancel") history_.cancelDelete(id, reply);
        else reply({"unknown_method", "Unknown deletion method", {}}, {});
        return;
    }
    if (method == "delete.prepare" || method == "delete.confirm") {
        const bool confirm = method == "delete.confirm";
        if (!fields(p, confirm ? std::vector<std::string>{"ids", "acknowledgement"} : std::vector<std::string>{"ids"}) || !p.value("ids").isArray() || p.value("ids").arrayRef().size() > 1000 || (confirm && !stringField(p, "acknowledgement", 64))) { reply(invalid(), {}); return; }
        std::vector<std::string> ids;
        for (const auto& id : p.value("ids").arrayRef()) { if (!id.isString() || id.stringRef().empty() || id.stringRef().size() > 128) { reply(invalid(), {}); return; } ids.push_back(id.stringRef()); }
        if (confirm) history_.deleteConfirmed(ids, p.value("acknowledgement").stringRef(), reply); else history_.prepareDelete(ids, reply);
        return;
    }
    if (method == "setup.connectAccount" || method == "setup.inspectAccount" || method == "setup.createFolder" || method == "setup.storeSharePassword") {
        const bool hasInput = method == "setup.createFolder" || method == "setup.storeSharePassword";
        if (!fields(p, hasInput ? std::vector<std::string>{"profileId", "expectedRevision", "input"} : std::vector<std::string>{"profileId", "expectedRevision"}) || !stringField(p, "profileId") || !p.value("expectedRevision").isString()) { reply(invalid(), {}); return; }
        setup_.start(method.substr(6), p.value("profileId").stringRef(), p.value("expectedRevision").stringRef(), p.value("input").toObject(), reply); return;
    }
    if (method == "setup.storeCredentials" || method == "setup.unlockCredentials" || method == "setup.testDestination") {
        const bool store = method == "setup.storeCredentials";
        if (!fields(p, store ? std::vector<std::string>{"profileId", "expectedRevision", "credentials"} : std::vector<std::string>{"profileId", "expectedRevision"}) || !stringField(p, "profileId") || !p.value("expectedRevision").isString() || (store && !p.value("credentials").isObject())) { reply(invalid(), {}); return; }
        setup_.start(method.substr(6), p.value("profileId").stringRef(), p.value("expectedRevision").stringRef(), p.value("credentials").toObject(), reply); return;
    }
    if (method == "setup.credentialHint" || method == "setup.revealCredentialSecret") {
        if (!fields(p, {"profileId", "expectedRevision"}) || !stringField(p, "profileId") || !p.value("expectedRevision").isString()) { reply(invalid(), {}); return; }
        if (method == "setup.credentialHint") setup_.credentialHint(p.value("profileId").stringRef(), p.value("expectedRevision").stringRef(), reply);
        else setup_.revealCredentialSecret(p.value("profileId").stringRef(), p.value("expectedRevision").stringRef(), reply);
        return;
    }
    if (method == "setup.inspect") { if (!fields(p, {"id"}) || !stringField(p, "id")) { reply(invalid(), {}); return; } setup_.inspect(p.value("id").stringRef(), reply); return; }
    if (method == "settings.save") {
        if (!fields(p, {"document", "expectedRevision"}) || !p.value("document").isObject() || !p.value("expectedRevision").isString()) { reply(invalid(), {}); return; }
        const auto result = settings_.save(p.value("document").objectRef(), p.value("expectedRevision").stringRef());
        reply(result.error, {{"document", settings_.document()}, {"metadata", settings_.metadata()}}); return;
    }
    if (method == "settings.patch") {
        if (!fields(p, {"patch", "expectedRevision"}) || !p.value("patch").isObject() || !p.value("expectedRevision").isString()) { reply(invalid(), {}); return; }
        const auto result = settings_.patch(p.value("patch").objectRef(), p.value("expectedRevision").stringRef());
        reply(result.error, settings_.metadata()); return;
    }
    if (method == "settings.reorderProfiles") {
        if (!fields(p, {"order", "expectedRevision"}) || !p.value("order").isArray() || !p.value("expectedRevision").isString()) { reply(invalid(), {}); return; }
        json::Object reordered;
        if (auto e = Settings::reorderProfiles(settings_.document(), p.value("order").arrayRef(), reordered)) { reply(e, {}); return; }
        const auto result = settings_.save(reordered, p.value("expectedRevision").stringRef());
        reply(result.error, {{"document", settings_.document()}, {"metadata", settings_.metadata()}}); return;
    }
    if (method == "settings.importDevelopment") {
        if (!fields(p, {"expectedRevision"}) || !p.value("expectedRevision").isString()) { reply(invalid(), {}); return; }
        const auto result = settings_.save(Settings::developmentTemplate(paths::documentsFolder()), p.value("expectedRevision").stringRef());
        reply(result.error, settings_.metadata()); return;
    }
    if (method == "settings.validate") {
        if (!fields(p, {"document"}) || !p.value("document").isObject()) { reply(invalid(), {}); return; }
        const auto error = settings_.validate(p.value("document").objectRef()); reply(error, {{"valid", !error}}); return;
    }
    if (method == "update.check") { if (!p.empty()) { reply(invalid(), {}); return; } updates_.check(true, reply); return; }
    if (method == "update.install" || method == "update.restart" || method == "update.openSoftwareUpdates") {
        reply({"update_check_unavailable", "Windows builds are updated by replacing the executables; use the download link in Settings → Updates", {}}, {}); return;
    }
    if (method == "diagnostics.bundle" || method == "diagnostics.uploadTarget" || method == "diagnostics.reveal") {
        if (!diagnostics_) { reply({"initializing", "Diagnostics are not ready yet", {}}, {}); return; }
        if (method == "diagnostics.bundle") diagnostics_->bundle(p, reply);
        else if (method == "diagnostics.uploadTarget") diagnostics_->uploadTarget(p, reply);
        else diagnostics_->reveal(p, reply);
        return;
    }
    if (!p.empty()) { reply(invalid(), {}); return; }
    if (method == "settings.get") reply({}, {{"document", settings_.document()}, {"metadata", settings_.metadata()}});
    else if (method == "settings.developmentTemplate") reply({}, {{"document", Settings::developmentTemplate(paths::documentsFolder())}, {"explicitImportRequired", true}});
    else if (method == "settings.reload") { const auto result = settings_.reload(); reply(result.error, settings_.metadata()); }
    else if (method == "capabilities") reply({}, operations_.capabilities());
    else if (method == "diagnostics") reply({}, diagnosticsDocument());
    else if (method == "history.clear") history_.clear(reply);
    else if (method == "cache.clear") operations_.cleanup(false, reply);
    else if (method == "logs.clear") { auto removed = log::clearAll(); removed["cleared"] = true; reply({}, removed); }
    else if (method == "update.status") reply({}, updates_.status());
    else reply({"unknown_method", "Unknown session method", {}}, {});
}
}
