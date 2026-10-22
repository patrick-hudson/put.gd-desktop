#include "resident/Diagnostics.h"
#include "common/Files.h"
#include "common/Log.h"
#include "common/Paths.h"
#include "common/Protocol.h"
#include "common/Sha256.h"
#include "common/Text.h"
#include "core/LogFormat.h"
#include "resident/CrashDumps.h"
#include "resident/Environment.h"
#include "resident/HistoryStore.h"
#include "resident/Resident.h"
#include <bcrypt.h>
#include <shellapi.h>

namespace putgd {
namespace {
constexpr long long debugLoggingMs = 24LL * 60 * 60 * 1000;
std::string randomKey() {
    std::string key(32, '\0');
    if (BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(key.data()), ULONG(key.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) key = crypto::hex(crypto::Sha256::digest(files::nowIsoUtc() + std::to_string(GetTickCount64())));
    return key;
}
std::string environmentVariable(const wchar_t* name) {
    wchar_t buffer[1024]; const DWORD n = GetEnvironmentVariableW(name, buffer, 1024);
    return n && n < 1024 ? paths::narrow(std::wstring(buffer, n)) : std::string();
}
long long modifiedMs(const std::string& path) {
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (!GetFileAttributesExW(paths::wide(path).c_str(), GetFileExInfoStandard, &data)) return 0;
    return (long long)((((unsigned long long)data.ftLastWriteTime.dwHighDateTime << 32) | data.ftLastWriteTime.dwLowDateTime) - 116444736000000000ULL) / 10000;
}
std::string uniqueName(const std::string& folder, long long now) {
    const auto base = diagnostics::baseName(now);
    auto path = paths::join(folder, base + ".zip");
    for (int index = 2; files::exists(path) && index < 1000; ++index) path = paths::join(folder, base + "-" + std::to_string(index) + ".zip");
    return path;
}
std::string defaultFolder(const Settings& s) { return paths::join(s.archive.root, "diagnostics"); }
}
DiagnosticsService::DiagnosticsService(std::string dataRoot, SettingsService& settings, OperationService& operations, SetupService& setup, Tray& tray)
    : root_(std::move(dataRoot)), settings_(settings), operations_(operations), setup_(setup), tray_(tray) {}
bool DiagnosticsService::trayAvailable() const { return tray_.available(); }
json::Object DiagnosticsService::crashDumps() const { return crashdumps::status(root_); }
void DiagnosticsService::applySettings() {
    const auto s = settings_.snapshot();
    log::setRetention(s.logDays, s.logBytes);
    crashdumps::apply(s.crashDumps, root_);
    // Debug logging turns itself off 24 hours after it was turned on; a restart does not extend it.
    const auto file = paths::join(root_, "debug-logging.json");
    if (debugTimer_) { Loop::instance().stopTimer(debugTimer_); debugTimer_ = 0; }
    if (!s.debugLogging) { files::remove(file); log::setDebugEnabled(false); return; }
    const auto now = log::nowMs();
    auto enabledAt = files::parseIsoUtcMs(json::parseObject(files::read(file, 4096).value_or(std::string())).value_or(json::Object{}).value("enabledAt").toString());
    if (enabledAt < 0 || enabledAt > now) { enabledAt = now; files::writeAtomically(file, json::serialize(json::Object{{"enabledAt", files::isoUtcFromMs(now)}})); }
    const auto expires = enabledAt + debugLoggingMs;
    if (now >= expires) {
        log::setDebugEnabled(false);
        if (debugOffPending_) return;
        debugOffPending_ = true;
        // Not from inside a settings callback: the patch notifies the observers again.
        Loop::instance().singleShot(0, [this] {
            debugOffPending_ = false;
            const auto result = settings_.patch({{"preferences", json::Object{{"diagnostics", json::Object{{"debugLogging", false}}}}}}, settings_.revision());
            if (result.error) log::warning("settings", "Debug logging could not be turned off", {{"code", result.error.code}});
            else log::info("settings", "Debug logging turned off after 24 hours");
        });
        return;
    }
    log::setDebugEnabled(true, files::isoUtcFromMs(expires));
    debugTimer_ = Loop::instance().startTimer(int(std::min<long long>(expires - now + 1000, 0x7FFFFFFF)), [this] { debugTimer_ = 0; applySettings(); }, false);
}
void DiagnosticsService::destinationTested(const std::string& profileId) { tested_[profileId] = log::nowMs(); }
void DiagnosticsService::bundle(const json::Object& params, JsonReply reply) {
    diagnostics::Request request;
    if (auto e = diagnostics::parse(params, request)) { reply(e, {}); return; }
    if (running_) { reply({"diagnostics_busy", "A diagnostics package is already being created; try again when it finishes", {}}, {}); return; }
    if (!operations_.ready()) { reply({"initializing", "Wait for resident journal recovery", {}}, {}); return; }
    const auto snapshot = settings_.snapshot();
    auto in = std::make_shared<diagnostics::Inputs>();
    in->build = protocol::buildIdentity();
    in->settings = snapshot.document;
    in->diagnostics = document ? document() : operations_.diagnostics();
    in->crashDumps = crashdumps::status(root_);
    in->home = environmentVariable(L"USERPROFILE");
    in->roots = {{root_, "<data>"}, {snapshot.archive.root, "<archive>"}};
    if (const auto temp = environmentVariable(L"TEMP"); !temp.empty()) in->roots.push_back({temp, "<runtime>"});
    in->defaultFolder = defaultFolder(snapshot);
    const bool tray = tray_.available();
    const auto database = operations_.databasePath(), root = root_, logs = log::directory();
    running_ = true;
    operations_.background([in, request, tray, database, root, logs](Error& error) -> json::Object {
        in->nowMs = log::nowMs();
        in->key = randomKey();
        in->environment = environment::collect(root, tray);
        HistoryStore store(root, database);
        if (store.error()) { error = store.error(); return {}; }
        std::string created;
        if (!request.operationId.empty()) {
            auto op = store.operation(request.operationId, &error);
            if (error) return {};
            created = op.created;
            in->operations.push_back(std::move(op));
        }
        const auto from = logformat::iso(diagnostics::windowStart(request, in->nowMs, created));
        if (request.operationId.empty()) { in->operations = store.operationsSince(from, {"failed", "partial", "interrupted"}, diagnostics::maximumOperations, &error); if (error) return {}; }
        // An uploaded earlier package stays out of later ones, its log records too.
        Error ignored;
        for (const auto& op : store.operationsSince(from, {"completed"}, 100000, &ignored)) if (op.action == diagnostics::operationAction) in->uploadedPackages.insert(op.id);
        for (const auto& name : files::list(logs)) if (logformat::isFileName(name)) if (auto bytes = files::read(paths::join(logs, name), 64LL * 1024 * 1024)) in->logs.push_back({name, std::move(*bytes)});
        // Where it goes: the default folder belongs to put.gd; any other is the user's.
        std::string path, folder; bool inDefault = false;
        if (request.output.empty()) { folder = in->defaultFolder; inDefault = true; }
        else if (files::isDirectory(request.output)) folder = request.output;
        else if (files::exists(request.output)) { error = {"diagnostics_output_exists", "A file already exists at the output path; choose another name or a folder", {{"path", request.output}}}; return {}; }
        else if (!text::endsWith(text::lower(request.output), ".zip")) { error = {"invalid_request", "The output path must be a folder or end in .zip", {}}; return {}; }
        else if (!files::isDirectory(paths::parent(request.output))) { error = {"diagnostics_output_unavailable", "The output folder does not exist", {{"path", paths::parent(request.output)}}}; return {}; }
        else { folder = paths::parent(request.output); path = request.output; }
        in->previewFolder = folder; in->previewDefaultFolder = inDefault;
        auto pkg = diagnostics::build(*in, request);
        if (request.preview) return pkg.preview;
        if (inDefault && !files::makePrivateDirectory(folder)) { error = {"diagnostics_output_unavailable", "Cannot create the diagnostics folder", {{"path", folder}}}; return {}; }
        if (path.empty()) path = uniqueName(folder, in->nowMs);
        if (files::writeAtomically(path, pkg.zip, true)) { error = {"diagnostics_output_unavailable", "Cannot write the diagnostics package", {{"path", path}}}; return {}; }
        int pruned = 0;
        if (inDefault) {
            std::vector<diagnostics::Saved> saved;
            for (const auto& name : files::list(folder)) if (diagnostics::isPackageName(name)) saved.push_back({name, modifiedMs(paths::join(folder, name))});
            for (const auto& name : diagnostics::pruned(saved, in->nowMs)) if (name != paths::fileName(path) && files::remove(paths::join(folder, name))) ++pruned;
        }
        return {{"path", path}, {"bytes", double(pkg.zip.size())}, {"sha256", crypto::Sha256::hexDigest(pkg.zip)}, {"redaction", request.includeNames ? "names" : "hashed"},
            {"files", json::stringArray(diagnostics::fileNames(pkg))}, {"operations", double(pkg.operationList.size())}, {"logRecords", double(pkg.records)},
            {"droppedLogRecords", double(pkg.droppedRecords)}, {"defaultFolder", inDefault}, {"pruned", pruned}};
    }, [this, request, reply](Error error, json::Object result) {
        running_ = false;
        if (request.preview) { reply(error, result); return; }
        if (error) { log::warning("diagnostics", "Diagnostics package failed", {{"error", error.toJson()}}); reply(error, {}); return; }
        saved_.insert(text::lower(result.value("path").toString()));
        log::info("diagnostics", "Diagnostics package saved", {{"path", result.value("path")}, {"bytes", result.value("bytes")}, {"redaction", result.value("redaction")}, {"operations", result.value("operations")}, {"logRecords", result.value("logRecords")}});
        // The local copy exists first; an upload that fails or is skipped never changes it.
        if (!request.upload) { if (request.notify) notifySaved(result); reply({}, result); return; }
        upload(result, request, [this, reply, notify = request.notify](Error e, json::Object r) { if (notify) notifySaved(r); reply(e, r); });
    });
}
void DiagnosticsService::notifySaved(const json::Object& result) {
    const auto path = result.value("path").toString();
    std::string text = paths::fileName(path) + " (" + text::byteSize(result.value("bytes").toInteger()) + ") in " + paths::parent(path);
    if (result.contains("link")) text += result.value("linkCopied").toBool() ? ". Uploaded; the link is on the clipboard." : ". Uploaded.";
    else if (result.contains("uploadError")) text += ". Not uploaded: " + result.value("uploadError")["message"].toString();
    tray_.notify("put.gd \xE2\x80\x94 diagnostics saved", text + " Click to show it in its folder.", false, [this, path] { reveal({{"path", path}}, [](Error, json::Object) {}); });
}
void DiagnosticsService::check(const std::string& profileId, std::function<void(Error, json::Object)> done) {
    const auto snapshot = settings_.snapshot();
    const auto found = snapshot.profiles.find(profileId);
    if (found == snapshot.profiles.end()) { done({"profile_not_found", "No configured destination has this identifier", {{"profileId", profileId}}}, {}); return; }
    const auto profile = found->second;
    const auto target = diagnostics::target(profileId, profile);
    const auto revision = settings_.revision();
    const auto settings = profile.value("settings").toObject(), credentials = profile.value("credentials").toObject();
    const auto root = root_, database = operations_.databasePath();
    operations_.background([root, database, profileId, settings, credentials](Error& error) -> json::Object {
        HistoryStore store(root, database);
        const auto last = store.lastUpload(profileId, &error);
        // A failure under different settings or credentials says nothing about them now.
        if (error || !last || last->destination.settings != settings || last->destination.credentialReference != credentials) return {};
        Error failure = last->error;
        for (const auto& item : last->items) if (item.uploadStatus == "failed" && item.error) failure = item.error;
        return {{"operationId", last->id}, {"state", last->state}, {"created", last->created}, {"error", failure.toJson()}};
    }, [this, profileId, revision, target, done](Error error, json::Object last) {
        if (error) { done(error, {}); return; }
        const auto lastError = last.value("error").toObject();
        const auto tested = tested_.count(profileId) ? tested_[profileId] : -1;
        const auto failedAt = files::parseIsoUtcMs(last.value("created").toString());
        const bool testedSince = tested >= 0 && failedAt >= 0 && tested > failedAt;
        const auto state = last.value("state").toString();
        if (!testedSince && (state == "failed" || state == "partial") && diagnostics::credentialOrConfigurationError(lastError.value("code").toString())) {
            done({"diagnostics_destination_failing", "The last upload to this destination failed with a credential or configuration error, so the package was not uploaded. Fix the destination in Settings, then upload it.",
                {{"profileId", profileId}, {"operationId", last.value("operationId")}, {"lastError", lastError.value("code")}}}, target);
            return;
        }
        const auto previous = checked_.find(profileId);
        if (previous != checked_.end() && previous->second.revision == revision && log::nowMs() - previous->second.at < diagnostics::checkReuseMs) { done({}, target); return; }
        setup_.testNow(profileId, [this, profileId, revision, target, done](Error failed) {
            if (failed) { checked_.erase(profileId); done({"diagnostics_destination_unavailable", "Test connection failed for this destination, so the package was not uploaded", {{"profileId", profileId}, {"error", failed.toJson()}}}, target); return; }
            checked_[profileId] = {revision, log::nowMs()};
            done({}, target);
        });
    });
}
void DiagnosticsService::uploadTarget(const json::Object& p, JsonReply reply) {
    for (const auto& pair : p) if (pair.first != "profileId") { reply({"invalid_request", "diagnostics.uploadTarget takes an optional profileId", {}}, {}); return; }
    if (p.contains("profileId") && (!p.value("profileId").isString() || p.value("profileId").stringRef().empty() || p.value("profileId").stringRef().size() > 64)) { reply({"invalid_request", "diagnostics.uploadTarget takes an optional profileId", {}}, {}); return; }
    const auto profileId = p.contains("profileId") ? p.value("profileId").stringRef() : diagnostics::defaultUploadProfile(settings_.snapshot());
    if (profileId.empty()) { reply({}, {{"available", false}, {"reason", "diagnostics_no_destination"}, {"message", "No upload destination is set up"}}); return; }
    check(profileId, [reply, profileId](Error error, json::Object target) {
        if (error.code == "profile_not_found") { reply(error, {}); return; }
        if (error) { target["profileId"] = profileId; target["available"] = false; target["reason"] = error.code; target["message"] = error.message; target["details"] = error.details; reply({}, target); return; }
        target["available"] = true; reply({}, target);
    });
}
void DiagnosticsService::upload(json::Object result, const diagnostics::Request& request, JsonReply reply) {
    const auto profileId = request.profileId.empty() ? diagnostics::defaultUploadProfile(settings_.snapshot()) : request.profileId;
    // A skipped or failed upload is part of a successful reply: the package is saved.
    auto settle = [reply](json::Object r, const Error& error) {
        if (error) { r["uploadError"] = error.toJson(); log::warning("diagnostics", "Diagnostics package not uploaded", {{"error", error.toJson()}}); }
        reply({}, r);
    };
    if (profileId.empty()) { settle(result, {"diagnostics_no_destination", "No upload destination is set up, so the package was saved locally only", {}}); return; }
    const auto path = result.value("path").toString();
    const bool copyLink = request.copyLink;
    check(profileId, [this, result, settle, profileId, path, copyLink](Error error, json::Object target) mutable {
        if (error) { settle(result, error); return; }
        result["upload"] = target;
        operations_.uploadDiagnostics(path, profileId, [result, settle](Error accepted) { if (accepted) settle(result, accepted); },
            [this, result, settle, copyLink](const Operation& op) mutable {
                auto upload = result.value("upload").toObject();
                upload["operationId"] = op.id; upload["state"] = op.state;
                const auto item = op.items.empty() ? OperationItem{} : op.items.front();
                if (!item.candidate.empty()) upload["name"] = item.candidate;
                if (!item.artifact.id.empty()) upload["historyId"] = item.artifact.id;
                result["upload"] = upload;
                if (op.state != "completed" || item.uploadStatus != "uploaded") {
                    auto e = item.error ? item.error : op.error ? op.error : Error{"diagnostics_upload_failed", "The package was not uploaded", {}};
                    e.details["operationId"] = op.id; settle(result, e); return;
                }
                if (!item.linkAvailable()) { settle(result, {"diagnostics_link_unavailable", "The package was uploaded, but this destination gives it no link", {{"operationId", op.id}}}); return; }
                result["link"] = item.receipt.directUrl;
                log::info("diagnostics", "Diagnostics package uploaded", {{"operationId", op.id}, {"provider", op.destination.providerId}, {"profileId", op.destination.profileId}});
                if (!copyLink) { settle(result, {}); return; }
                operations_.copyHistoryUrl(item.receipt.directUrl, [result, settle](Error copied, json::Object) mutable {
                    result["linkCopied"] = !copied; if (copied) result["copyError"] = copied.toJson();
                    settle(result, {});
                });
            });
    });
}
void DiagnosticsService::reveal(const json::Object& p, JsonReply reply) {
    if (p.size() != 1 || !p.value("path").isString() || p.value("path").stringRef().empty() || p.value("path").stringRef().size() > 4096 || !paths::isAbsolute(p.value("path").stringRef())) { reply({"invalid_request", "diagnostics.reveal takes an absolute path", {}}, {}); return; }
    const auto& path = p.value("path").stringRef();
    const auto name = paths::fileName(path);
    // put.gd's own package names, or any path this resident saved a package to.
    if (!(diagnostics::isPackageName(name) || saved_.count(text::lower(path)))) { reply({"invalid_request", "Show in folder accepts a diagnostics package put.gd saved", {}}, {}); return; }
    if (!files::isRegularFile(path)) { log::warning("diagnostics", "Show in folder refused", {{"reason", "diagnostics_package_missing"}}); reply({"diagnostics_package_missing", "The diagnostics package is no longer at this path", {}}, {}); return; }
    ShellExecuteW(nullptr, L"open", L"explorer.exe", paths::wide("/select,\"" + path + "\"").c_str(), nullptr, SW_SHOWNORMAL);
    reply({}, {{"revealed", true}});
}
}
