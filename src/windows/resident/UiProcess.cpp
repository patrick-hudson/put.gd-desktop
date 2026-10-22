#include "resident/UiProcess.h"
#include "common/Log.h"
#include "resident/Resident.h"

namespace putgd {
namespace {
constexpr int viewDeadlineMs = 12000;
bool knownView(const std::string& v) { return v == "settings" || v == "history" || v == "activity" || v == "about" || v == "editFile"; }
}
UiProcess::UiProcess(std::string session, std::string epoch) : session_(std::move(session)), epoch_(std::move(epoch)) {}
UiProcess::~UiProcess() { stop(); }
json::Object UiProcess::diagnostics() const { return {{"running", running()}, {"pid", double(pid())}, {"generation", double(generation_)}, {"pendingViews", double(pending_.size())}, {"lastExit", lastExit_}}; }
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
    Pending p; p.view = view; p.reply = std::move(reply);
    p.timer = Loop::instance().startTimer(viewDeadlineMs, [this, view] {
        for (auto it = pending_.begin(); it != pending_.end(); ++it) if (it->view == view) { auto r = it->reply; pending_.erase(it); r({"ui_timeout", "The put.gd window did not acknowledge the view in time", {}}); break; }
    }, false);
    pending_.push_back(std::move(p));
    if (!helper_ || closing_) { start(view); return; }
    if (ready_) helper_->send("activate", json::Object{{"view", view}});
}
void UiProcess::invalidate(const std::string& kind) { if (helper_ && ready_ && !closing_) helper_->send("invalidate", json::Object{{"kind", kind}}); }
void UiProcess::stop() {
    if (!helper_) return;
    helper_->terminate("quit");
    helper_.reset();
    failPending({"ui_unavailable", "put.gd is quitting", {}});
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
        if (json::serialize(state).size() > protocol::maximumUiStateBytes) { helper_->send("closeCancelled", json::Object{{"sequence", sequence}, {"error", Error{"state_too_large", "The window state exceeds 8 MiB", {}}.toJson()}}); return; }
        if (!pending_.empty()) { helper_->send("closeCancelled", json::Object{{"sequence", sequence}}); return; }
        state_ = state; closing_ = true; closeSequence_ = sequence.toInteger();
        helper_->send("closeAccepted", json::Object{{"sequence", sequence}});
        // A confirmed helper that lingers is killed after two seconds.
        Loop::instance().singleShot(2000 + 5000, [this, generation = generation_] { if (helper_ && closing_ && generation == generation_) helper_->terminate("close_timeout"); });
    } else if (frame.type == "closeConfirmed") {
        if (onProcessChanged) onProcessChanged(0);
    } else if (frame.type == "closeCancel") {
        closing_ = false;
    }
}
}
