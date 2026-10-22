// The resident: single instance per user session, tray, shortcuts, clipboard
// owner, journal and the JSON session pipe. Launching again forwards a view
// request to the running resident, as the Linux launcher does (docs/cli.md).
#include "common/Files.h"
#include "common/Log.h"
#include "common/Modes.h"
#include "common/Paths.h"
#include "common/Pipe.h"
#include "common/Text.h"
#include "common/Uuid.h"
#include "resident/Clipboard.h"
#include "resident/CredentialStore.h"
#include "resident/HistoryService.h"
#include "resident/Hotkeys.h"
#include "resident/LoginStartup.h"
#include "resident/OperationService.h"
#include "resident/ProtonProvider.h"
#include "resident/Resident.h"
#include "resident/S3Provider.h"
#include "resident/Session.h"
#include "resident/SettingsService.h"
#include "resident/SetupService.h"
#include "resident/Tray.h"
#include "resident/UiProcess.h"
#include "resident/UpdateService.h"
#include <shellapi.h>

namespace putgd {
namespace {
// Backblaze B2 has no Windows provider yet. Configurations naming it still
// validate so a shared config file loads; its destinations report unavailability.
class UnavailableProvider final : public StorageProvider {
public:
    explicit UnavailableProvider(std::string id) : id_(std::move(id)) {}
    std::string id() const override { return id_; }
    bool available() const override { return false; }
    std::string unavailableReason() const override { return "not_available_on_windows"; }
    ProviderCapabilities capabilities(const Destination&) const override { return {}; }
    std::string serializationKey(const Destination&, const json::Object& r, const std::string& c) const override { return json::serialize(r) + c; }
    Error validateSettings(const json::Object&) const override { return {}; }
    Error readiness(const Destination&) const override { return unavailable(); }
    NameCheck checkName(const Destination&, const std::string&, CancelFlag) override { return {"inconclusive", {}, {}, unavailable()}; }
    UploadResult upload(const UploadRequest&, ProgressReport, CancelFlag) override { return {"failed", {}, unavailable()}; }
    StatResult stat(const StorageReceipt&, CancelFlag) override { return {"unknown", {}, unavailable()}; }
    StatResult reconcile(const UploadRequest&, CancelFlag) override { return {"unknown", {}, unavailable()}; }
    DeleteResult remove(const DeleteRequest&, DurableCheckpoint, CancelFlag) override { return {"failed", {}, unavailable()}; }
    json::Object testDestination(const Destination&, DurableCheckpoint, Error& e) override { e = unavailable(); return {}; }
private:
    Error unavailable() const { return {"provider_unavailable", "The " + id_ + " destination is not available in the Windows build", {{"provider", id_}}}; }
    std::string id_;
};
std::string completionMessage(const Operation& op, bool hasLink) {
    const auto& items = op.items;
    if (op.state == "completed" && hasLink) return op.copyStatus == "copied" ? "Link copied to the clipboard" : items.size() == 1 ? "Upload finished" : std::to_string(items.size()) + " files uploaded";
    if (op.state == "completed" && !items.empty() && items.front().uploadStatus == "local_only") return items.size() == 1 ? "Saved " + items.front().artifact.name : std::to_string(items.size()) + " files saved";
    if (op.state == "completed") return "Finished";
    if (op.state == "partial") return "Some files did not upload; open Recent activity to retry";
    if (op.state == "cancelled") return "Cancelled";
    return op.error ? op.error.message : "Failed";
}
int forwardView(const std::string& pipePath, const std::string& view) {
    const auto id = uuid::create();
    const auto request = json::serialize(json::Object{{"schemaVersion", 1}, {"requestId", id}, {"session", pipePath}, {"method", "ui.show"}, {"params", json::Object{{"view", view}}}});
    std::string error;
    const auto reply = pipe::request(pipePath, request, 15000, &error);
    if (!reply || reply->size() > 16 * 1024) return 1;
    const auto object = json::parseObject(*reply);
    return object && object->value("ok").toBool() && object->value("requestId") == id.c_str() ? 0 : 1;
}
}

int residentMain(const std::vector<std::string>& args) {
    bool background = false; std::string view;
    for (size_t i = 0; i < args.size(); ++i) {
        const auto& a = args[i];
        if (a == "--background") background = true;
        else if (a == "--history") view = "history"; else if (a == "--activity") view = "activity"; else if (a == "--settings") view = "settings"; else if (a == "--about") view = "about";
    }
    const auto pipePath = paths::sessionPipePath();
    const auto probe = pipe::probe(pipePath, 500);
    if (probe == pipe::Probe::Connected) { if (background) return 0; return forwardView(pipePath, view.empty() ? "settings" : view); }
    if (probe == pipe::Probe::Unknown) return 1;

    const auto root = paths::dataRoot();
    files::makePrivateDirectory(root);
    log::initialize(paths::join(root, "logs"), "putgd");
    auto& loop = Loop::instance();
    CredentialStore credentials;
    ProviderRegistry providers;
    providers.add(std::make_shared<S3Provider>(credentials));
    auto proton = std::make_shared<ProtonProvider>(root);
    providers.add(proton);
    providers.add(std::make_shared<UnavailableProvider>("backblaze-b2"));
    SettingsService settings(paths::configPath(), [&providers](const std::string& id, const json::Object& s) { return providers.validate(id, s); },
                             [&providers](const std::string& id, const json::Object& c) { return providers.validateCredentials(id, c); });
    const auto loaded = settings.load();
    if (loaded.error) log::warning("settings", "Configuration not accepted at startup; defaults are active", {{"code", loaded.error.code}});
    log::setDebugEnabled(settings.snapshot().debugLogging);
    log::prune(paths::join(root, "logs"), settings.snapshot().logDays, settings.snapshot().logBytes);
    Clipboard clipboard(loop.hwnd());
    Hotkeys hotkeys(loop.hwnd());
    Tray tray(loop.hwnd());
    OperationService operations(root, settings, providers, clipboard);
    SetupService setup(settings, credentials, providers, operations);
    HistoryService history(operations);
    UpdateService updates(root, settings);
    UiProcess ui(pipePath, {});
    Session session(settings, operations, history, setup, updates, ui, paths::join(root, "logs"));
    if (auto e = session.listen(pipePath)) { log::error("session", "Cannot serve the session pipe", {{"code", e.code}}); tray.notify("put.gd", "Another put.gd is already running in this session.", true); return 1; }
    log::info("app", "Resident started", {{"version", protocol::buildIdentity()}, {"pipe", pipePath}, {"sqlite", sqlite::libraryVersion()}});

    auto applyShortcuts = [&] {
        std::map<std::string, std::string> desired;
        const auto snapshot = settings.snapshot();
        for (const auto& name : Settings::actionNames()) desired[name] = snapshot.action(name).shortcut;
        hotkeys.apply(desired);
    };
    auto applySettings = [&] {
        applyShortcuts();
        log::setDebugEnabled(settings.snapshot().debugLogging);
        if (auto e = startup::apply(paths::executablePath(), settings.snapshot().launchAtLogin)) log::warning("startup", "Login startup not applied", {{"code", e.code}});
        ui.invalidate("settings");
    };
    settings.onChanged(applySettings);
    applySettings();
    settings.startWatching();

    bool quitting = false;
    auto quit = [&] {
        if (quitting) return;
        quitting = true;
        log::info("app", "Resident quitting");
        ui.stop(); proton->shutdown(); operations.shutdown(); history.shutdown(); session.stop(); settings.stopWatching();
        loop.quit(0);
    };
    session.onQuit = quit;
    auto showView = [&](const std::string& v) { ui.show(v, [v](Error e) { if (e) log::warning("ui", "View not shown", {{"view", v}, {"code", e.code}}); }); };
    auto invoke = [&](const std::string& action, json::Object options = {}) {
        operations.invoke(action, options, [&tray, action](Error e, json::Object) {
            if (e && e.code != "busy_selecting" && e.code != "editor_active") tray.notify("put.gd", e.message, true);
        });
    };
    tray.onCommand([&](UINT command) {
        switch (command) {
        case Tray::CmdCaptureRegion: invoke("region"); break;
        case Tray::CmdCaptureEdit: invoke("edit"); break;
        case Tray::CmdCaptureWindow: invoke("window"); break;
        case Tray::CmdCaptureDesktop: invoke("desktop"); break;
        case Tray::CmdUploadClipboard: invoke("clipboard"); break;
        case Tray::CmdEditClipboard: invoke("edit", {{"source", "clipboard"}}); break;
        case Tray::CmdEditFile: showView("editFile"); break;
        case Tray::CmdSettings: showView("settings"); break;
        case Tray::CmdHistory: showView("history"); break;
        case Tray::CmdActivity: showView("activity"); break;
        case Tray::CmdAbout: showView("about"); break;
        case Tray::CmdQuit: quit(); break;
        default: break;
        }
    });
    loop.setMessageHook([&](UINT message, WPARAM wParam, LPARAM lParam, LRESULT& result) {
        if (message == WM_HOTKEY) { const auto action = hotkeys.actionFor(wParam); if (!action.empty()) invoke(action); return true; }
        if (message == WM_CLIPBOARDUPDATE) { clipboard.handleUpdate(); return true; }
        if (tray.handleMessage(message, wParam, lParam)) return true;
        if (message == WM_QUERYENDSESSION) { result = TRUE; return true; }
        if (message == WM_ENDSESSION) { if (wParam) quit(); return true; }
        return false;
    });
    operations.onActivityChanged = [&](bool active, bool uploading) { tray.setTint(uploading ? Tray::Tint::Uploading : active ? Tray::Tint::Busy : Tray::Tint::Idle); };
    bool invalidationPending = false;
    auto invalidateOperations = [&] {
        if (invalidationPending) return;
        invalidationPending = true;
        loop.singleShot(500, [&] { invalidationPending = false; ui.invalidate("operations"); });
    };
    operations.onChanged = [&](const std::string&) { invalidateOperations(); ui.invalidate("history"); };
    operations.onObservationsChanged = invalidateOperations;
    operations.onResultReady = [&](const Operation& op) {
        if (!settings.snapshot().notifications) return;
        std::string link; for (const auto& item : op.items) if (item.linkAvailable()) { link = item.receipt.directUrl; break; }
        const bool warning = op.state != "completed";
        if (op.state == "cancelled" && !op.error) return;
        tray.notify("put.gd — " + op.state, completionMessage(op, !link.empty()), warning, link);
    };
    history.onChanged = [&] { ui.invalidate("history"); };
    setup.onChanged = [&](const std::string&) { ui.invalidate("setup"); };
    updates.onUpdateAvailable = [&](const json::Object& r) { if (settings.snapshot().notifications) tray.notify("put.gd — update available", "put.gd " + r.value("latest").toString() + " is available. Settings → Updates has the download.", false, r.value("notesUrl").toString()); ui.invalidate("update"); };
    ui.onShortcutRecording = [&](bool active, std::function<void(Error)> reply) { if (active) hotkeys.pause(); else hotkeys.resume(); reply({}); };
    ui.onProcessChanged = [&](unsigned long pid) { operations.setUiProcessId(pid); };
    operations.shortcutStatus = [&] { return hotkeys.status(); };

    const bool firstStart = settings.firstStart();
    operations.initialize([&, view, background, firstStart] {
        if (!operations.ready()) tray.notify("put.gd", "The upload journal is unavailable; captures cannot start. See the log for details.", true);
        loop.singleShot(3000, [&] { setup.unlockSavedCredentials(); });
        updates.start();
        if (!view.empty()) showView(view);
        else if (firstStart && !background) showView("settings");
    });
    const int code = loop.run();
    loop.joinBackground();
    log::info("app", "Resident stopped", {{"code", code}});
    return code;
}
}
