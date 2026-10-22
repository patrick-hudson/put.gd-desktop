#include "resident/Session.h"
#include "common/Files.h"
#include "common/Log.h"
#include "common/Paths.h"
#include "common/Text.h"
#include "common/Uuid.h"
#include "core/SanitizedPng.h"
#include "resident/Resident.h"
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
// A store-only ZIP writer for the diagnostics package (no compression library needed).
class Zip {
public:
    void add(const std::string& name, const std::string& bytes) {
        const auto crc = png::crc32(bytes);
        const uint32_t offset = uint32_t(data_.size());
        append(data_, "PK\x03\x04", 4); u16(data_, 20); u16(data_, 0x0800); u16(data_, 0); u16(data_, 0); u16(data_, 0); u32(data_, uint32_t(crc)); u32(data_, uint32_t(bytes.size())); u32(data_, uint32_t(bytes.size()));
        u16(data_, uint16_t(name.size())); u16(data_, 0); data_ += name; data_ += bytes;
        append(central_, "PK\x01\x02", 4); u16(central_, 20); u16(central_, 20); u16(central_, 0x0800); u16(central_, 0); u16(central_, 0); u16(central_, 0); u32(central_, uint32_t(crc)); u32(central_, uint32_t(bytes.size())); u32(central_, uint32_t(bytes.size()));
        u16(central_, uint16_t(name.size())); u16(central_, 0); u16(central_, 0); u16(central_, 0); u16(central_, 0); u32(central_, 0); u32(central_, offset); central_ += name;
        ++entries_;
    }
    std::string finish() const {
        std::string out = data_ + central_;
        append(out, "PK\x05\x06", 4); u16(out, 0); u16(out, 0); u16(out, uint16_t(entries_)); u16(out, uint16_t(entries_)); u32(out, uint32_t(central_.size())); u32(out, uint32_t(data_.size())); u16(out, 0);
        return out;
    }
private:
    static void append(std::string& s, const char* p, size_t n) { s.append(p, n); }
    static void u16(std::string& s, uint16_t v) { s += char(v & 0xFF); s += char(v >> 8); }
    static void u32(std::string& s, uint32_t v) { for (int i = 0; i < 4; ++i) s += char((v >> (8 * i)) & 0xFF); }
    std::string data_, central_; int entries_ = 0;
};
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
            if (!object) { reply(json::serialize(response("", {"invalid_json", "Request must be a JSON object", {}}, {}))); return; }
            const auto id = object->value("requestId").toString();
            dispatch(*object, [reply, id](Error e, json::Object result) { reply(json::serialize(response(id, e, result))); });
        });
    });
    if (!ok) return {"session_unavailable", "Cannot listen on the user session pipe", {}};
    return {};
}
void Session::stop() { server_.stop(); }
json::Object Session::snapshot() const {
    return {{"build", protocol::buildIdentity()}, {"residentEpoch", epoch_}, {"operationsReady", operations_.ready()},
        {"settings", json::Object{{"document", settings_.document()}, {"metadata", settings_.metadata()}}}, {"diagnostics", operations_.diagnostics()}};
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
    if (startsWith(method, "recording.")) {
        // Field checks as on Linux (LocalServer); the session checks state and revision.
        const auto command = method.substr(10);
        bool ok = true;
        if (command == "capabilities") ok = p.empty();
        else if (command == "prepare") ok = fields(p, {"target", "format"}, {"codec", "fps", "audio", "maxEdge"}) && p.value("target").isObject() && stringField(p, "format", 8) && (!p.contains("codec") || p.value("codec").isString()) && (!p.contains("audio") || p.value("audio").isObject());
        else if (command == "start" || command == "pause" || command == "resume" || command == "stop" || command == "cancel" || command == "inspect" || command == "raise") ok = fields(p, {"id"}) && stringField(p, "id");
        else if (command == "select") ok = fields(p, {"id", "rect"}) && stringField(p, "id") && p.value("rect").isArray() && p.value("rect").arrayRef().size() == 4;
        else if (command == "overlay") ok = fields(p, {"id", "visible"}) && stringField(p, "id") && p.value("visible").isBool();
        else if (command == "edit") ok = fields(p, {"id", "revision", "commands"}) && stringField(p, "id") && p.value("revision").isNumber() && p.value("commands").isArray() && p.value("commands").arrayRef().size() <= 100;
        else if (command == "preview") ok = fields(p, {"id", "time"}, {"maxEdge"}) && stringField(p, "id") && p.value("time").isNumber() && (!p.contains("maxEdge") || p.value("maxEdge").isNumber());
        else if (command == "review") ok = stringField(p, "id") && stringField(p, "action", 32);
        else if (command == "saveCopy") ok = fields(p, {"id", "path"}, {"overwrite"}) && stringField(p, "id") && stringField(p, "path", 4096) && (!p.contains("overwrite") || p.value("overwrite").isBool());
        else if (command == "upload") ok = fields(p, {"id", "revision"}) && stringField(p, "id") && p.value("revision").isNumber();
        else { reply({"unknown_method", "Unknown recording method", {}}, {}); return; }
        if (!ok) { reply(invalid(), {}); return; }
        operations_.recordingCommand(command, p, reply); return;
    }
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
    if (method == "diagnostics.bundle") { diagnosticsBundle(p, reply); return; }
    if (method == "diagnostics.uploadTarget") { reply({}, {{"available", false}, {"reason", "unsupported"}, {"message", "Diagnostics packages are saved locally in the Windows build; attach the file by hand"}}); return; }
    if (method == "diagnostics.reveal") {
        if (!fields(p, {"path"}) || !stringField(p, "path", 4096) || !paths::isAbsolute(p.value("path").stringRef()) || !text::startsWith(paths::fileName(p.value("path").stringRef()), "putgd-diagnostics-")) { reply(invalid(), {}); return; }
        if (!files::isRegularFile(p.value("path").stringRef())) { reply({"not_found", "No saved diagnostics package at this path", {}}, {}); return; }
        ShellExecuteW(nullptr, L"open", L"explorer.exe", paths::wide("/select,\"" + p.value("path").stringRef() + "\"").c_str(), nullptr, SW_SHOWNORMAL);
        reply({}, {{"revealed", true}}); return;
    }
    if (!p.empty()) { reply(invalid(), {}); return; }
    if (method == "settings.get") reply({}, {{"document", settings_.document()}, {"metadata", settings_.metadata()}});
    else if (method == "settings.developmentTemplate") reply({}, {{"document", Settings::developmentTemplate(paths::documentsFolder())}, {"explicitImportRequired", true}});
    else if (method == "settings.reload") { const auto result = settings_.reload(); reply(result.error, settings_.metadata()); }
    else if (method == "capabilities") reply({}, operations_.capabilities());
    else if (method == "diagnostics") { auto d = operations_.diagnostics(); d["ui"] = ui_.diagnostics(); d["updates"] = updates_.status(); reply({}, d); }
    else if (method == "history.clear") history_.clear(reply);
    else if (method == "cache.clear") operations_.cleanup(false, reply);
    else if (method == "logs.clear") { log::clearAll(logs_); reply({}, {{"cleared", true}}); }
    else if (method == "update.status") reply({}, updates_.status());
    else reply({"unknown_method", "Unknown session method", {}}, {});
}
void Session::diagnosticsBundle(const json::Object& p, JsonReply reply) {
    if (!fields(p, {}, {"operation", "since", "includeNames", "output", "preview", "notify", "upload", "profileId", "copyLink"})) { reply(invalid(), {}); return; }
    const bool preview = p.value("preview").toBool(false), includeNames = p.value("includeNames").toBool(false);
    std::string output = p.value("output").toString();
    if (!output.empty() && !paths::isAbsolute(output)) { reply({"invalid_request", "output must be an absolute .zip path or folder", {}}, {}); return; }
    const auto snapshot = settings_.snapshot();
    auto folder = output.empty() ? paths::join(snapshot.archive.root, "diagnostics") : (text::endsWith(text::lower(output), ".zip") ? paths::parent(output) : output);
    auto stamp = files::nowIsoUtc(); for (char& c : stamp) if (c == '-' || c == ':') c = '\0';
    std::string compact; for (const char c : stamp) if (c != '\0' && c != '.') compact += c; // yyyymmddThhmmssmmmZ
    compact = compact.substr(0, 15) + "Z";
    const auto file = output.empty() || !text::endsWith(text::lower(output), ".zip") ? paths::join(folder, "putgd-diagnostics-" + compact + ".zip") : output;
    const auto redact = [includeNames](json::Object op) {
        if (includeNames) return op;
        auto items = op.value("items").toArray();
        for (auto& v : items) { auto item = v.toObject(); auto artifact = item.value("artifact").toObject(); artifact["name"] = "<redacted>"; item["artifact"] = artifact; item["candidate"] = "<redacted>";
            auto receipt = item.value("receipt").toObject(); receipt["directUrl"] = receipt.value("directUrl").toString().empty() ? "" : "<redacted>"; item["receipt"] = receipt; auto local = item.value("local").toObject(); local["path"] = local.value("path").toString().empty() ? "" : "<redacted>"; item["local"] = local; v = item; }
        op["items"] = items; return op;
    };
    operations_.recent([this, reply, preview, file, folder, redact, snapshot](Error e, json::Object recent) {
        if (e) { reply(e, {}); return; }
        Zip zip;
        json::Object diagnostics = operations_.diagnostics(); diagnostics["updates"] = updates_.status(); diagnostics["ui"] = ui_.diagnostics();
        zip.add("diagnostics.json", json::pretty(json::Value(diagnostics)));
        auto document = settings_.document(); zip.add("configuration.json", json::pretty(json::Value(json::Object{{"document", document}, {"metadata", settings_.metadata()}})));
        json::Array operations; for (const auto& op : recent.value("operations").toArray()) operations.push_back(redact(op.toObject()));
        zip.add("operations.json", json::pretty(json::Value(operations)));
        int logs = 0;
        for (const auto& name : files::list(logs_)) if (text::endsWith(name, ".log")) { if (auto bytes = files::read(paths::join(logs_, name), 8 * 1024 * 1024)) { zip.add("logs/" + name, *bytes); ++logs; } }
        const auto bytes = zip.finish();
        json::Object result{{"path", file}, {"bytes", double(bytes.size())}, {"operations", double(operations.size())}, {"logFiles", logs}, {"redaction", "names-hashed"}, {"preview", preview}};
        if (preview) { reply({}, result); return; }
        if (!files::makeDirectories(folder)) { reply({"diagnostics_not_saved", "Cannot create the diagnostics folder", {}}, {}); return; }
        if (auto error = files::writeAtomically(file, bytes, false)) { reply({"diagnostics_not_saved", "Cannot write the diagnostics package", {}}, {}); return; }
        result["uploadError"] = "Uploading diagnostics packages is not available in the Windows build";
        log::info("diagnostics", "Diagnostics package saved", {{"bytes", double(bytes.size())}});
        reply({}, result);
    });
}
}
