#pragma once
// Supervision of the transient helpers (common/Protocol.h): the handshake, the
// heartbeat watchdog, frame delivery on the owner thread and clean exit.
// Synchronous runs (image job, S3 worker) happen on the calling worker thread.
#include "common/Error.h"
#include "common/Process.h"
#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace putgd::helpers {
std::string helperExecutable();
struct SyncResult { Error error; json::Object checkpoint; json::Object result; bool normalExit = false; int exitCode = -1; };
// Starts `args`, sends bootstrap (with role/nonce/generation/build added), waits
// for ready and sends activate, then feeds every frame to `onFrame` until the
// child sends a terminal frame (checkpoint/failed/result) or the budget ends.
// `onFrame` returns true for frames it fully handled; the terminal frame ends the
// run and is returned. `send` lets the handler answer (checkpointAccepted ...).
using FrameHandler = std::function<bool(const protocol::Frame&, frames::Channel& send)>;
// `firstType`/`firstPayload` (optional) is sent right after activate (the S3 worker's request).
// `cancelGraceMs` (> 0) bounds how long a cancelled helper may take to drain.
SyncResult runSynchronous(const std::string& role, const std::vector<std::string>& args, json::Object bootstrap, int startupMs, int totalMs,
                          FrameHandler onFrame, std::shared_ptr<std::atomic_bool> cancel = {}, int silenceMs = 15000,
                          const std::string& firstType = {}, const json::Object& firstPayload = {}, int cancelGraceMs = 0);
// Creates an empty owner-only file a helper will fill (helpers never create files).
bool prepareOutputFile(const std::string& path);
// An asynchronous helper whose frames arrive on the owner thread.
class Supervised {
public:
    using Frame = std::function<void(const protocol::Frame&)>;
    using Exit = std::function<void(int code, bool normal, const std::string& reason)>;
    Supervised(std::string role, std::vector<std::string> args);
    ~Supervised();
    // Spawns and runs the handshake; `ready` is called on the owner thread once
    // activate was sent, or `exit` with the failure reason.
    void start(json::Object bootstrap, int startupMs, Frame onFrame, Exit onExit, std::function<void()> ready = {});
    bool send(const std::string& type, const json::Object& payload = {});
    bool running() const;
    void terminate(const std::string& reason);
    unsigned long pid() const;
    void setSilenceWatchdog(bool enabled) { watchdog_ = enabled; }
private:
    struct State;
    std::shared_ptr<State> state_;
    bool watchdog_ = true;
};
}
