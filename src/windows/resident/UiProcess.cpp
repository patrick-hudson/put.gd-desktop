#include "resident/UiProcess.h"
#include "common/Log.h"
#include "core/UiState.h"
#include "resident/Resident.h"

namespace putgd {
namespace {
constexpr int viewDeadlineMs = 12000;
bool knownView(const std::string& v) { return v == "settings" || v == "history" || v == "activity" || v == "about" || v == "editFile"; }
}
UiProcess::UiProcess(std::string session, std::string epoch) : session_(std::move(session)), epoch_(std::move(epoch)) {}
UiProcess::~UiProcess() { stop(); }
json::Object UiProcess::diagnostics() const {
    return {{"running", running()}, {"state", phase()}, {"pid", double(pid())}, {"generation", double(generation_)}, {"pendingViews", double(pending_.size())},
            {"pendingCommands", double(commands_.size())}, {"checkpointBytes", double(state_.empty() ? 0 : json::serialize(state_).size())}, {"lastExit", lastExit_}};
}
std::string UiProcess::phase() const { return !helper_ ? "stopped" : closing_ ? "closing" : ready_ ? "ready" : "starting"; }
json::Object UiProcess::viewState() const { return uistate::viewState(state_, phase()); }
void UiProcess::failCommands(const Error& error) {
    auto commands = std::move(commands_);
    commands_.clear();
    for (auto& [id, command] : commands) { if (command.timer) Loop::instance().stopTimer(command.timer); command.reply(error, {}); }
}
void UiProcess::command(const std::string& name, const json::Object& params, CommandReply reply) {
    // Commands act on a window the user already opened; they never start the helper.
    if (!helper_ || !ready_ || closing_) { reply({"ui_view_closed", "Open put.gd's window before driving it.", {}}, {}); return; }
    if (commands_.size() >= uistate::maximumOutstandingCommands) { reply({"ui_busy", "The window is still answering earlier commands; try again", {}}, {}); return; }
    const auto id = std::to_string(++commandSerial_);
    if (!helper_->send("command", json::Object{{"version", uistate::commandVersion}, {"requestId", id}, {"command", name}, {"params", params}})) {
        reply({"ui_unavailable", "The put.gd window could not be reached", {}}, {});
        return;
    }
    Command pending;
    pending.reply = std::move(reply);
    pending.timer = Loop::instance().startTimer(uistate::commandDeadlineMs, [this, id] {
        const auto it = commands_.find(id);
        if (it == commands_.end()) return;
        auto late = std::move(it->second.reply);
        commands_.erase(it);
        late({"ui_command_timeout", "The window did not answer in time. Check it before retrying.", {{"outcomeUnknown", true}}}, {});
    }, false);
    commands_[id] = std::move(pending);
}
void UiProcess::failPending(const Error& error) {
    auto pending = std::move(pending_); pending_.clear();
    for (auto& p : pending) { if (p.timer) Loop::instance().stopTimer(p.timer); p.reply(error); }
}
void UiProcess::start(const std::string& view) {
    ++generation_; ready_ = false; closing_ = false;
    helper_ = std::make_unique<helpers::Supervised>("ui", std::vector<std::string>{"--ui", view});
    helper_->setSilenceWatchdog(false);
    json::Object bootstrap{{"session", session_}, {"residentEpoch", epoch_}, {"state", state_}, {"view", view}};
    helper_->start(bootstrap, protocol::startupMs, [this](const protocol::Frame& frame) { handle(frame); }, [this](int code, bool normal, const std::string& reason) {
        lastExit_ = {{"generation", double(generation_)}, {"exitCode", double(code)}, {"normal", normal}, {"reason", reason}};
        const bool incompatible = reason == "helper_incompatible";
        helper_.reset(); ready_ = false; closing_ = false;
        if (onProcessChanged) onProcessChanged(0);
        failCommands({"ui_view_closed", "The window closed before answering.", {{"outcomeUnknown", true}}});
        failPending(incompatible ? Error{"ui_incompatible", "The installed UI differs from the running resident; let pending uploads finish, then restart put.gd", {}} : Error{"ui_unavailable", "The put.gd window could not be opened", {{"exitCode", double(code)}, {"reason", reason}}});
        log::info("ui", "UI helper exited", {{"exitCode", double(code)}, {"normal", normal}, {"reason", reason}});
    }, [this] {
        ready_ = true;
        if (onProcessChanged) onProcessChanged(helper_ ? helper_->pid() : 0);
        for (const auto& p : pending_) helper_->send("activate", json::Object{{"view", p.view}});
    });
}
void UiProcess::show(const std::string& view, std::function<void(Error)> reply) {
    if (!knownView(view)) { reply({"invalid_request", "Unknown view", {}}); return; }
    if (helper_ && closing_) {
        // A helper in its close handshake is immutable; end it now so the replacement
        // can start. Its exit callback runs synchronously here, before the new request
        // joins the pending list, so the request is not failed with it.
        auto old = std::move(helper_);
        old->terminate("replaced");
    }
    Pending p; p.view = view; p.reply = std::move(reply);
    p.timer = Loop::instance().startTimer(viewDeadlineMs, [this, view] {
        for (auto it = pending_.begin(); it != pending_.end(); ++it) if (it->view == view) { auto r = it->reply; pending_.erase(it); r({"ui_timeout", "The put.gd window did not acknowledge the view in time", {}}); break; }
    }, false);
    pending_.push_back(std::move(p));
    if (!helper_) { start(view); return; }
    if (ready_) helper_->send("activate", json::Object{{"view", view}});
}
void UiProcess::invalidate(const std::string& kind) { if (helper_ && ready_ && !closing_) helper_->send("invalidate", json::Object{{"kind", kind}}); }
void UiProcess::stop() {
    if (!helper_) return;
    helper_->terminate("quit");
    helper_.reset();
    failPending({"ui_unavailable", "put.gd is quitting", {}});
    failCommands({"ui_view_closed", "The window closed before answering.", {{"outcomeUnknown", true}}});
}
void UiProcess::handle(const protocol::Frame& frame) {
    if (frame.type == "viewShown") {
        const auto view = frame.payload.value("view").toString();
        for (auto it = pending_.begin(); it != pending_.end();) {
            if (it->view == view) { if (it->timer) Loop::instance().stopTimer(it->timer); auto r = it->reply; it = pending_.erase(it); r({}); }
            else ++it;
        }
    } else if (frame.type == "shortcutRecording") {
        const bool active = frame.payload.value("active").toBool();
        const auto replyTo = frame.payload.value("replyTo").isNull() ? json::Value(std::to_string(frame.sequence)) : frame.payload.value("replyTo");
        auto answer = [this, replyTo, active](Error e) { if (helper_) helper_->send("shortcutRecordingChanged", json::Object{{"replyTo", replyTo}, {"active", e ? !active : active}, {"error", e.toJson()}}); };
        if (onShortcutRecording) onShortcutRecording(active, answer); else answer({"shortcuts_unavailable", "Shortcut recording is unavailable", {}});
    } else if (frame.type == "closeRequested") {
        const auto sequence = frame.payload.value("sequence");
        const auto state = frame.payload.value("state").toObject();
        if (const auto invalid = uistate::validateState(state)) { helper_->send("closeCancelled", json::Object{{"sequence", sequence}, {"error", invalid.toJson()}}); return; }
        if (!pending_.empty() || !commands_.empty()) { helper_->send("closeCancelled", json::Object{{"sequence", sequence}}); return; }
        state_ = state; closing_ = true;
        helper_->send("closeAccepted", json::Object{{"sequence", sequence}});
        // A confirmed helper that lingers is killed after two seconds.
        Loop::instance().singleShot(2000 + 5000, [this, generation = generation_] { if (helper_ && closing_ && generation == generation_) helper_->terminate("close_timeout"); });
    } else if (frame.type == "commandResult") {
        const auto it = commands_.find(frame.payload.value("requestId").toString());
        if (it == commands_.end()) return; // answered after its deadline
        auto command = std::move(it->second);
        commands_.erase(it);
        if (command.timer) Loop::instance().stopTimer(command.timer);
        command.reply(Error::fromJson(frame.payload.value("error").toObject()), frame.payload.value("result").toObject());
    } else if (frame.type == "checkpoint") {
        // Open windows checkpoint 250 ms after a change; ui.viewState reads the last accepted one.
        const auto state = frame.payload.value("state").toObject();
        if (const auto invalid = uistate::validateState(state)) { log::warning("ui", "Window checkpoint rejected", {{"code", invalid.code}}); return; }
        if (!closing_) state_ = state;
    } else if (frame.type == "closeConfirmed") {
        if (onProcessChanged) onProcessChanged(0);
    } else if (frame.type == "closeCancel") {
        closing_ = false;
    }
}
}
