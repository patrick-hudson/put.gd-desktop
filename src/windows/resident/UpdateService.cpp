#include "resident/UpdateService.h"
#include "common/BuildInfo.h"
#include "common/Files.h"
#include "common/Log.h"
#include "common/Paths.h"
#include "common/Process.h"
#include "common/Text.h"
#include "common/Uuid.h"
#include "core/UpdatePlan.h"
#include "resident/Resident.h"
#include "resident/UpdateInstall.h"
#include <cstdio>
#include <windows.h>

namespace putgd {
namespace {
constexpr long long dayMs = 24LL * 3600 * 1000;
struct Version { int major = 0, minor = 0, patch = 0; int kind = 2; int number = 0; bool valid = false; }; // kind: 0 nightly, 1 rc, 2 release
Version parseVersion(const std::string& text) {
    Version v;
    const auto dash = text.find('-');
    const auto base = text.substr(0, dash);
    if (std::sscanf(base.c_str(), "%d.%d.%d", &v.major, &v.minor, &v.patch) != 3) return v;
    if (dash != std::string::npos) {
        const auto rest = text.substr(dash + 1);
        // "nightly.N", a bare "nightly" (calendar builds), "rc.N" and local "dev.N" stamps.
        if (text::startsWith(rest, "nightly")) { v.kind = 0; v.number = rest.size() > 8 ? std::atoi(rest.c_str() + 8) : 0; }
        else if (text::startsWith(rest, "rc")) { v.kind = 1; v.number = rest.size() > 3 ? std::atoi(rest.c_str() + 3) : 0; }
        else if (text::startsWith(rest, "dev.")) { v.kind = 0; v.number = std::atoi(rest.c_str() + 4); }
        else return v;
    }
    v.valid = true;
    return v;
}
std::string feedBase() {
    wchar_t buffer[2048]; const DWORD n = GetEnvironmentVariableW(L"PUTGD_UPDATE_FEED_URL", buffer, 2048);
    return n > 0 && n < 2048 ? paths::narrow(buffer) : std::string("https://app.put.gd/update/v1/");
}
}
std::string UpdateService::buildChannel() { return text::contains(build::version, "nightly") || text::contains(build::version, "dev.") ? "nightly" : "stable"; }
int UpdateService::compareVersions(const std::string& a, const std::string& b) {
    const auto x = parseVersion(a), y = parseVersion(b);
    if (!x.valid || !y.valid) return 0;
    if (x.major != y.major) return x.major < y.major ? -1 : 1;
    if (x.minor != y.minor) return x.minor < y.minor ? -1 : 1;
    if (x.patch != y.patch) return x.patch < y.patch ? -1 : 1;
    if (x.kind != y.kind) return x.kind < y.kind ? -1 : 1;
    if (x.number != y.number) return x.number < y.number ? -1 : 1;
    return 0;
}
int UpdateService::childMain(const std::vector<std::string>& args) {
    const std::string channel = args.size() > 1 ? args[1] : buildChannel();
    const std::string outputPath = args.size() > 2 ? args[2] : std::string();
    auto emit = [&outputPath](const json::Object& o) {
        const auto line = json::serialize(o) + "\n";
        if (!outputPath.empty()) files::writeAtomically(outputPath, line);
        else { std::fwrite(line.data(), 1, line.size(), stdout); std::fflush(stdout); }
    };
    if (channel != "stable" && channel != "nightly") { emit({{"ok", false}, {"error", Error{"update_check_unavailable", "Unknown update channel", {}}.toJson()}}); return 1; }
    update::Transfer transfer;
    const auto body = update::httpGet(feedBase() + channel + ".json", 256 * 1024, transfer);
    if (!transfer.ok()) { emit({{"ok", false}, {"error", Error{transfer.tooLarge ? "update_feed_invalid" : "update_feed_unavailable", "The update feed could not be fetched", {{"httpStatus", double(transfer.status)}}}.toJson()}}); return 1; }
    const auto feed = json::parseObject(body);
    if (!feed || !json::integer(feed->value("schema"), 1, 1) || feed->value("channel") != channel.c_str()) { emit({{"ok", false}, {"error", Error{"update_feed_invalid", "The update feed is not a valid version-1 document", {}}.toJson()}}); return 1; }
    json::Object result{{"channel", channel}, {"generatedAt", feed->value("generatedAt")}};
    const auto& release = feed->value("release");
    if (release.isObject()) {
        const auto version = release["version"].toString();
        if (release["tag"].toString() != "v" + version || (channel == "nightly") != text::contains(version, "nightly")) { emit({{"ok", false}, {"error", Error{"update_feed_invalid", "The feed names a release outside its channel", {}}.toJson()}}); return 1; }
        result["latest"] = version; result["tag"] = release["tag"]; result["publishedAt"] = release["publishedAt"]; result["notesUrl"] = release["notesUrl"];
        result["assets"] = release["assets"];
    } else result["latest"] = json::Value();
    emit({{"ok", true}, {"result", result}});
    return 0;
}
UpdateService::UpdateService(std::string root, SettingsService& settings) : root_(std::move(root)), statePath_(paths::join(root_, "update-check.json")), settings_(settings) { load(); finishRestartedUpdate(); }
std::string UpdateService::channel() const { const auto s = settings_.snapshot().updateChannel; return s == "build" ? buildChannel() : s; }
void UpdateService::load() {
    const auto bytes = files::read(statePath_, 256 * 1024);
    if (!bytes) return;
    const auto o = json::parseObject(*bytes).value_or(json::Object{});
    result_ = o.value("result").toObject(); notified_ = o.value("notified").toObject(); checkedAt_ = o.value("checkedAt").toString(); attemptedAt_ = o.value("attemptedAt").toString();
}
void UpdateService::save() const { files::writeAtomically(statePath_, json::serialize(json::Object{{"result", result_}, {"notified", notified_}, {"checkedAt", checkedAt_}, {"attemptedAt", attemptedAt_}})); }
void UpdateService::schedule() {
    if (timer_) { Loop::instance().stopTimer(timer_); timer_ = 0; }
    if (!settings_.snapshot().checkForUpdates) { nextCheckAt_.clear(); return; }
    const long long last = checkedAt_.empty() ? -1 : files::parseIsoUtcMs(checkedAt_);
    const long long now = files::parseIsoUtcMs(files::nowIsoUtc());
    long long delay = last < 0 ? 180000 : std::max(60000LL, last + dayMs - now);
    nextCheckAt_ = files::isoUtcFromMs(now + delay);
    timer_ = Loop::instance().startTimer(int(std::min(delay, 3600000LL)), [this] { timer_ = 0; const long long n = files::parseIsoUtcMs(files::nowIsoUtc()); if (files::parseIsoUtcMs(nextCheckAt_) <= n) check(false, {}); else schedule(); }, false);
}
void UpdateService::start() { schedule(); settings_.onChanged([this] { schedule(); }); }
json::Object UpdateService::status() const {
    const auto snapshot = settings_.snapshot();
    const auto current = channel();
    const auto here = update::installation();
    const auto unavailable = installUnavailable();
    json::Object build{{"version", build::version}, {"kind", here.msi ? "msi" : "portable"}, {"channel", buildChannel()}, {"packageTarget", here.target},
        {"supported", unavailable.empty()}, {"reason", unavailable.empty() ? json::Value() : json::Value(unavailable)}};
    std::string state = "not_checked";
    if (result_.value("channel") == current.c_str()) {
        if (!result_.value("latest").isString()) state = "no_release";
        else state = compareVersions(result_.value("latest").stringRef(), build::version) > 0 ? "update_available" : "up_to_date";
    }
    return {{"build", build}, {"check", snapshot.checkForUpdates}, {"channelSetting", snapshot.updateChannel}, {"channel", current}, {"enabled", snapshot.checkForUpdates}, {"notifies", true},
        {"checking", checking_}, {"attemptedAt", attemptedAt_}, {"checkedAt", checkedAt_}, {"nextCheckAt", nextCheckAt_}, {"result", result_}, {"state", state}, {"error", lastError_.toJson()},
        {"install", installStatus()}};
}
void UpdateService::check(bool manual, std::function<void(Error, json::Object)> reply) {
    if (checking_) { if (reply) reply({"update_check_busy", "An update check is already running", {}}, {}); return; }
    checking_ = true;
    attemptedAt_ = files::nowIsoUtc();
    const auto channelName = channel();
    auto exitCode = std::make_shared<int>(-1);
    const auto outputPath = paths::join(root_, "update-check-" + uuid::create() + ".json");
    Loop::instance().background([exitCode, channelName, outputPath] {
        auto child = process::start(paths::executablePath(), {"--check-update", channelName, outputPath}, true);
        if (!child) return;
        child->channel->close();
        child->waitExit(90000);
        if (child->running()) child->terminate();
        *exitCode = child->exitCode();
    }, [this, exitCode, reply, channelName, manual, outputPath] {
        checking_ = false;
        // The child writes one JSON object to the private output file.
        const auto bytes = files::read(outputPath, 256 * 1024).value_or(std::string());
        files::remove(outputPath);
        const auto parsed = json::parseObject(bytes);
        if (!parsed || !parsed->value("ok").isBool()) lastError_ = {"update_check_failed", "The update check did not complete", {{"exitCode", double(*exitCode)}}};
        else if (!parsed->value("ok").toBool()) lastError_ = Error::fromJson(parsed->value("error").toObject());
        else {
            lastError_ = {}; result_ = parsed->value("result").toObject(); checkedAt_ = files::nowIsoUtc();
            if (result_.value("latest").isString() && compareVersions(result_.value("latest").stringRef(), build::version) > 0 && notified_.value(channelName) != result_.value("latest") && onUpdateAvailable) {
                notified_[channelName] = result_.value("latest"); onUpdateAvailable(result_);
            }
        }
        save();
        schedule();
        log::info("updates", "Update check finished", {{"channel", channelName}, {"manual", manual}, {"error", lastError_.code}});
        if (reply) { if (lastError_) reply(lastError_, {}); else reply({}, status()); }
    });
}
// ---- installing (#404) --------------------------------------------------------
std::string UpdateService::installUnavailable() const {
    const auto own = update::releaseChannel(build::version);
    if (own.empty() || std::string(build::dirty) == "true") return "development_build";
    if (channel() != own) return "switch_unsupported";
    return {};
}
bool UpdateService::restartRequired() const {
    const auto installed = installState_.value("installed").toObject();
    const auto applied = installed.value("applied").toObject();
    return installed.value("version").isString() && installed.value("version") != build::version && installed.value("previousVersion") == build::version &&
        (applied.empty() || applied.value("ok").toBool());
}
json::Object UpdateService::installStatus() const {
    auto status = install_;
    if (!status.contains("state")) status["state"] = "idle";
    const auto unavailable = installUnavailable();
    status["available"] = unavailable.empty();
    status["reason"] = unavailable.empty() ? json::Value() : json::Value(unavailable);
    status["kind"] = "update";
    status["running"] = installing_;
    status["restartRequired"] = restartRequired();
    status["installed"] = installState_.value("installed").isObject() ? installState_.value("installed") : json::Value();
    status["backup"] = installState_.value("backup")["path"].isString() ? installState_.value("backup")["path"] : json::Value();
    return status;
}
void UpdateService::install(const json::Object& p, std::function<void(Error, json::Object)> reply) {
    for (const auto& key : p.keys()) if (key != "confirm" && key != "channel" && key != "method") { reply({"invalid_request", "update.install takes confirm, method and channel", {}}, {}); return; }
    // Nothing installs without an explicit yes.
    if (p.value("confirm") != json::Value(true)) { reply({"update_confirm_required", "Installing an update needs confirm: true", {}}, {}); return; }
    const auto method = p.value("method").toString("auto");
    if ((p.contains("method") && !p.value("method").isString()) || (method != "auto" && method != "download")) { reply({"invalid_request", "method is auto or download on Windows", {}}, {}); return; }
    if (p.contains("channel") && !p.value("channel").isString()) { reply({"invalid_request", "channel is stable or nightly", {}}, {}); return; }
    if (const auto reason = installUnavailable(); !reason.empty()) {
        reply({"update_install_unavailable", reason == "development_build" ? "Only release builds install updates" : "Windows builds install updates on their own channel only; switching channels is not supported yet", {{"reason", reason}}}, {}); return;
    }
    if (p.contains("channel") && p.value("channel") != channel().c_str()) { reply({"update_switch_confirm_required", "channel must be the channel put.gd watches, " + channel(), {{"channel", channel()}}}, {}); return; }
    if (installing_) { reply({"update_install_busy", "An update is already being installed", {}}, {}); return; }
    if (restartRequired()) { reply({"update_restart_required", "Restart put.gd to finish the update that is already installed", {{"installed", installState_.value("installed")["version"]}}}, {}); return; }
    if (busy) if (const auto reason = busy(); !reason.empty()) { reply({"update_install_busy", reason, {}}, {}); return; }
    installing_ = true;
    install_ = {{"state", "running"}, {"phase", "starting"}, {"method", method}, {"bytes", 0}, {"total", 0}, {"startedAt", files::nowIsoUtc()}};
    const auto channelName = channel();
    const auto outputPath = paths::join(root_, "update-install-" + uuid::create() + ".json");
    auto exitCode = std::make_shared<int>(-1);
    log::info("updates", "Installing an update", {{"channel", channelName}, {"kind", update::installation().msi ? "msi" : "portable"}});
    progressTimer_ = Loop::instance().startTimer(500, [this, outputPath] {
        const auto o = json::parseObject(files::read(outputPath, 64 * 1024).value_or(std::string()));
        if (!o || !o->value("progress").isObject()) return;
        const auto progress = o->value("progress").toObject();
        if (progress.value("phase") == install_.value("phase") && progress.value("bytes") == install_.value("bytes")) return;
        for (const auto* key : {"phase", "bytes", "total"}) install_[key] = progress.value(key);
        if (onChanged) onChanged();
    });
    Loop::instance().background([exitCode, channelName, outputPath] {
        auto child = process::start(paths::executablePath(), {"--install-update", channelName, outputPath}, true);
        if (!child) return;
        child->channel->close();
        child->waitExit(30 * 60 * 1000);
        if (child->running()) child->terminate();
        *exitCode = child->exitCode();
    }, [this, exitCode, outputPath] { installFinished(*exitCode, outputPath); });
    if (onChanged) onChanged();
    reply({}, status());
}
void UpdateService::installFinished(int exitCode, const std::string& outputPath) {
    Loop::instance().stopTimer(progressTimer_); progressTimer_ = 0;
    installing_ = false;
    const auto parsed = json::parseObject(files::read(outputPath, 256 * 1024).value_or(std::string()));
    files::remove(outputPath);
    installState_ = update::readState();
    install_["finishedAt"] = files::nowIsoUtc();
    if (parsed && parsed->value("ok").toBool()) {
        install_["state"] = "installed"; install_["phase"] = "done"; install_["result"] = parsed->value("result"); install_.remove("error");
        log::info("updates", "Update installed; it starts when put.gd restarts", {{"version", parsed->value("result")["version"]}, {"method", parsed->value("result")["method"]}});
    } else {
        const auto error = parsed && parsed->value("error").isObject() ? Error::fromJson(parsed->value("error").toObject())
                                                                       : Error{"update_install_failed", "The update install stopped without a result", {{"exitCode", double(exitCode)}}};
        install_["state"] = "failed"; install_["error"] = error.toJson(); install_.remove("result");
        log::warning("updates", "Update install failed", {{"code", error.code}, {"details", error.details}});
    }
    if (onChanged) onChanged();
}
void UpdateService::restart(const json::Object& p, std::function<void(Error, json::Object)> reply) {
    if (p.size() != 1 || p.value("confirm") != json::Value(true)) { reply({"update_confirm_required", "Restarting needs confirm: true", {}}, {}); return; }
    if (!restartRequired()) { reply({"update_restart_unavailable", "No installed update is waiting for a restart", {}}, {}); return; }
    if (installing_) { reply({"update_install_busy", "An update is being installed", {}}, {}); return; }
    if (busy) if (const auto reason = busy(); !reason.empty()) { reply({"update_install_busy", reason, {}}, {}); return; }
    const auto installed = installState_.value("installed").toObject();
    const auto pid = std::to_string(GetCurrentProcessId());
    bool started = false;
    if (installed.value("method") == "msi") {
        // msiexec replaces putgd.exe, so the copy that runs it lives outside the installation.
        const auto helper = paths::join(paths::join(root_, "updates"), "putgd-update-helper-" + uuid::create().substr(0, 8) + ".exe");
        started = !files::copyFile(paths::executablePath(), helper) && update::startIndependent(helper, {"--apply-update", installed.value("package").toString(), pid});
    } else {
        started = update::startIndependent(paths::join(installed.value("directory").toString(), "putgd.exe"), {"--background", "--after-update", pid});
    }
    if (!started || !quit) { reply({"update_restart_failed", "Cannot start put.gd again", {}}, {}); return; }
    log::info("updates", "Restarting into the installed update", {{"version", installed.value("version")}, {"method", installed.value("method")}});
    install_["state"] = "restarting";
    reply({}, {{"restarting", true}, {"version", installed.value("version")}});
    Loop::instance().singleShot(0, [this] { quit(); });
}
void UpdateService::finishRestartedUpdate() {
    installState_ = update::readState();
    auto installed = installState_.value("installed").toObject();
    if (installed.empty()) return;
    const auto applied = installed.value("applied").toObject();
    if (installed.value("version") == build::version) {
        // The new version runs: record it once, then remove the backup, the staged MSI and its helper.
        if (!installed.contains("completedAt")) {
            installed["completedAt"] = files::nowIsoUtc(); installed.remove("pending");
            installState_["installed"] = installed;
            log::info("updates", "Updated", {{"from", installed.value("previousVersion")}, {"to", installed.value("version")}, {"method", installed.value("method")}});
        }
        install_ = {{"state", "installed"}, {"phase", "done"}, {"result", installed}};
        Loop::instance().singleShot(15000, [this] {
            const auto backup = installState_.value("backup")["path"].toString();
            if (!backup.empty() && text::contains(paths::fileName(backup), ".putgd-update-backup-")) files::removeDirectoryRecursively(backup);
            const auto updates = paths::join(root_, "updates");
            for (const auto& name : files::list(updates))
                if (text::startsWith(name, "putgd-update-helper-") || text::endsWith(name, ".msi") || text::endsWith(name, ".part")) files::remove(paths::join(updates, name));
            installState_.remove("backup");
            auto record = installState_.value("installed").toObject(); record.remove("package");
            installState_["installed"] = record;
            update::writeState(installState_);
        });
    } else if (!applied.empty() && !applied.value("ok").toBool()) {
        // The MSI failed and Windows Installer rolled it back: this version runs again.
        install_ = {{"state", "failed"}, {"phase", "done"},
            {"error", Error{"update_install_failed", "The installer didn't finish; put.gd " + std::string(build::version) + " is still installed", {{"exitCode", applied.value("exitCode")}, {"log", applied.value("log")}}}.toJson()}};
        installState_.remove("installed");
    } else if (installed.value("previousVersion") != build::version) {
        // A record from a version that isn't this one's predecessor (installed by hand since).
        installState_.remove("installed"); installState_.remove("backup");
    }
    update::writeState(installState_);
}
}
