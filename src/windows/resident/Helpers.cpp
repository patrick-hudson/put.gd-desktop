#include "resident/Helpers.h"
#include "common/Files.h"
#include "common/Log.h"
#include "common/Paths.h"
#include "common/Uuid.h"
#include "resident/Resident.h"
#include <mutex>
#include <thread>

namespace putgd::helpers {
std::string helperExecutable() { return paths::executablePath(); }
namespace {
bool handshake(process::Child& child, const std::string& role, json::Object bootstrap, const std::string& nonce, int startupMs, std::string& failure) {
    bootstrap["role"] = role; bootstrap["nonce"] = nonce; bootstrap["generation"] = 1; bootstrap["build"] = protocol::buildIdentity();
    if (!child.channel->send("bootstrap", bootstrap)) { failure = "helper_pipe"; return false; }
    protocol::Frame frame; std::string error;
    while (true) {
        if (!child.channel->receive(frame, startupMs, &error)) { failure = error == "timeout" ? "helper_startup_timeout" : "helper_" + error; return false; }
        if (frame.type == "log" || frame.type == "heartbeat") continue;
        if (frame.type != "ready") { failure = "helper_protocol"; return false; }
        break;
    }
    if (frame.payload.value("nonce") != nonce || !protocol::buildMatches(frame.payload.value("build").toObject())) { failure = "helper_incompatible"; return false; }
    // The capture overlay and editor need the foreground; harmless for other roles.
    AllowSetForegroundWindow(child.pid);
    if (!child.channel->send("activate")) { failure = "helper_pipe"; return false; }
    return true;
}
}
bool prepareOutputFile(const std::string& path) {
    HANDLE h = CreateFileW(paths::wide(path).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    CloseHandle(h);
    return files::restrictToOwner(path) || true;
}
SyncResult runSynchronous(const std::string& role, const std::vector<std::string>& args, json::Object bootstrap, int startupMs, int totalMs, FrameHandler onFrame,
                          std::shared_ptr<std::atomic_bool> cancel, int silenceMs, const std::string& firstType, const json::Object& firstPayload, int cancelGraceMs) {
    SyncResult result;
    auto child = process::start(helperExecutable(), args, true);
    if (!child) { result.error = {"helper_unavailable", "Cannot start the " + role + " helper", {}}; return result; }
    const auto nonce = uuid::create();
    std::string failure;
    if (!handshake(*child, role, std::move(bootstrap), nonce, startupMs, failure)) {
        child->terminate();
        result.error = {failure, "The " + role + " helper did not start correctly", {}};
        return result;
    }
    if (!firstType.empty() && !child->channel->send(firstType, firstPayload)) { child->terminate(); result.error = {"helper_pipe", "Cannot send the request to the " + role + " helper", {}}; return result; }
    const ULONGLONG start = GetTickCount64();
    bool cancelSent = false;
    ULONGLONG cancelledAt = 0;
    while (true) {
        if (cancel && cancel->load() && !cancelSent) { child->channel->send("cancel"); cancelSent = true; cancelledAt = GetTickCount64(); }
        const auto elapsed = GetTickCount64() - start;
        if (elapsed >= ULONGLONG(totalMs)) { child->terminate(); result.error = {"helper_timeout", "The " + role + " helper exceeded its time budget", {}}; return result; }
        if (cancelSent && cancelGraceMs > 0 && GetTickCount64() - cancelledAt >= ULONGLONG(cancelGraceMs)) {
            child->terminate(); result.error = {"helper_timeout", "The cancelled " + role + " helper did not finish in time", {}}; return result;
        }
        protocol::Frame frame; std::string error;
        const int wait = int(std::min<ULONGLONG>(ULONGLONG(silenceMs), ULONGLONG(totalMs) - elapsed));
        if (!child->channel->receive(frame, wait, &error)) {
            if (error == "timeout") {
                if (cancel && cancel->load() && !cancelSent) continue;
                child->terminate(); result.error = {"helper_silent", "The " + role + " helper stopped responding", {}}; return result;
            }
            child->terminate(); result.error = {"helper_" + error, "The " + role + " helper failed its control channel", {}}; return result;
        }
        if (frame.type == "heartbeat" || frame.type == "log") { if (frame.type == "log") log::debug(role, frame.payload.value("message").toString().substr(0, 512)); continue; }
        if (frame.type == "checkpoint" && role != "s3") {
            result.checkpoint = frame.payload;
            child->channel->send("checkpointAccepted");
            break;
        }
        if (frame.type == "failed") {
            result.error = {frame.payload.value("code").toString("helper_failed"), frame.payload.value("message").toString("The helper reported a failure"), {}};
            break;
        }
        if (frame.type == "result") { result.result = frame.payload; child->channel->send("resultAccepted"); break; }
        if (onFrame && onFrame(frame, *child->channel)) continue;
        child->terminate(); result.error = {"helper_protocol", "The " + role + " helper sent an unexpected frame", {{"type", frame.type}}}; return result;
    }
    child->channel->close();
    result.normalExit = child->waitExit(5000) && child->exitCode() == 0;
    result.exitCode = child->exitCode();
    if (!result.normalExit) { child->terminate(); if (!result.error) result.error = {"helper_exit", "The " + role + " helper did not exit normally", {{"exitCode", double(result.exitCode)}}}; }
    return result;
}

struct Supervised::State {
    std::string role; std::vector<std::string> args;
    std::unique_ptr<process::Child> child;
    std::thread reader;
    std::atomic_bool stopping{false};
    std::mutex sendMutex;
    Frame onFrame; Exit onExit;
    std::atomic<ULONGLONG> lastFrame{0};
    UINT_PTR watchdog = 0;
    bool exited = false;
    void finish(int code, bool normal, const std::string& reason) {
        if (exited) return;
        exited = true;
        if (watchdog) { Loop::instance().stopTimer(watchdog); watchdog = 0; }
        if (onExit) onExit(code, normal, reason);
    }
};
Supervised::Supervised(std::string role, std::vector<std::string> args) : state_(std::make_shared<State>()) { state_->role = std::move(role); state_->args = std::move(args); }
Supervised::~Supervised() { terminate("destroyed"); }
void Supervised::start(json::Object bootstrap, int startupMs, Frame onFrame, Exit onExit, std::function<void()> ready) {
    auto state = state_;
    state->onFrame = std::move(onFrame); state->onExit = std::move(onExit);
    state->child = process::start(helperExecutable(), state->args, true);
    if (!state->child) { Loop::instance().post([state] { state->finish(-1, false, "helper_unavailable"); }); return; }
    const bool watchdog = watchdog_;
    state->reader = std::thread([state, bootstrap = std::move(bootstrap), startupMs, ready, watchdog] {
        const auto nonce = uuid::create();
        std::string failure;
        if (!handshake(*state->child, state->role, bootstrap, nonce, startupMs, failure)) {
            state->child->terminate();
            Loop::instance().post([state, failure] { state->finish(-1, false, failure); });
            return;
        }
        state->lastFrame = GetTickCount64();
        Loop::instance().post([state, ready, watchdog] {
            if (state->exited) return;
            if (watchdog) state->watchdog = Loop::instance().startTimer(5000, [state] {
                if (state->exited || !state->child) return;
                if (GetTickCount64() - state->lastFrame.load() > ULONGLONG(protocol::silenceMs) && state->child->running()) {
                    log::warning(state->role, "Helper silent; terminating", {{"pid", double(state->child->pid)}});
                    state->child->terminate();
                }
            });
            if (ready) ready();
        });
        while (!state->stopping) {
            protocol::Frame frame; std::string error;
            if (!state->child->channel->receive(frame, -1, &error)) {
                const bool normal = state->child->waitExit(5000) && state->child->exitCode() == 0;
                const int code = state->child->exitCode();
                Loop::instance().post([state, code, normal, error] { state->finish(code, normal, error == "pipe_closed" ? (normal ? "exited" : "crashed") : error); });
                return;
            }
            state->lastFrame = GetTickCount64();
            if (frame.type == "heartbeat") continue;
            if (frame.type == "log") { log::debug(state->role, frame.payload.value("message").toString().substr(0, 512)); continue; }
            Loop::instance().post([state, frame] { if (!state->exited && state->onFrame) state->onFrame(frame); });
        }
    });
}
bool Supervised::send(const std::string& type, const json::Object& payload) {
    if (!state_->child || state_->exited) return false;
    std::lock_guard lock(state_->sendMutex);
    return state_->child->channel->send(type, payload);
}
bool Supervised::running() const { return state_->child && state_->child->running() && !state_->exited; }
unsigned long Supervised::pid() const { return state_->child ? state_->child->pid : 0; }
void Supervised::terminate(const std::string& reason) {
    auto state = state_;
    state->stopping = true;
    if (state->child) { state->child->terminate(); if (state->child->channel) state->child->channel->close(); }
    if (state->reader.joinable()) { if (state->reader.get_id() == std::this_thread::get_id()) state->reader.detach(); else state->reader.join(); }
    if (Loop::instance().ownerThread()) state->finish(-1, false, reason);
    else Loop::instance().post([state, reason] { state->finish(-1, false, reason); });
}
}
