// Screen recording in the resident (docs/recording.md): one session at a time,
// supervised through a transient `putgd.exe --record` helper (role "record" in
// common/Protocol.h). The resident keeps the state machine, the journal intent
// and the staging files; it never maps Media Foundation or touches a pixel.
// Stop always leads to the private review state; only the explicit upload
// admits the exported bytes into the ordinary operation pipeline.
#include "resident/OperationService.h"
#include "common/Files.h"
#include "common/Log.h"
#include "common/Paths.h"
#include "common/Text.h"
#include "common/Uuid.h"
#include "core/Recording.h"
#include "resident/Resident.h"
#include <algorithm>

namespace putgd {
namespace {
constexpr int commandMs = 15000, stopMs = 30000, exportMs = 600000, shutdownMs = 2000;
constexpr unsigned long long failedProbeCacheMs = 60000;
constexpr long long diskReserveBytes = 512LL * 1024 * 1024, minimumBudgetBytes = 64LL * 1024 * 1024;
Error noSession() { return {"recording_unavailable", "No recording is active", {}}; }
Error invalidRequest(const std::string& message) { return {"invalid_request", message, {}}; }
RecordingLimits limitsOf(const Settings& s) { return {s.limits.recordingAnimationSeconds * 1000000, s.limits.recordingAnimationEdge}; }
bool contains(const json::Array& list, const std::string& value) { for (const auto& v : list) if (v == value.c_str()) return true; return false; }
}

struct OperationService::RecordingSession : std::enable_shared_from_this<RecordingSession> {
    Operation op; CancelFlag cancel;
    std::unique_ptr<helpers::Supervised> helper;
    std::string state = "preparing", pending, stopReason, directory, workingPath;
    RecordingTarget target; RecordingConfig config; RecordingSize canvas; long long duration = 0;
    RecordingEdit edit; json::Object progress, lastExport; int exports = 0;
    bool selecting = false, ending = false;
    struct Waiter { JsonReply reply; UINT_PTR timer = 0; };
    std::map<std::string, Waiter> waiting;
    // One command to the helper; the reply arrives on the owner thread, once.
    void request(const std::string& command, const json::Object& params, int timeoutMs, JsonReply reply) {
        if (!helper || ending) { reply({"recording_helper_closed", "The recording helper is not running", {}}, {}); return; }
        const auto requestId = uuid::create();
        auto& waiter = waiting[requestId];
        waiter.reply = std::move(reply);
        std::weak_ptr<RecordingSession> weak = shared_from_this();
        waiter.timer = Loop::instance().startTimer(timeoutMs, [weak, requestId, command, timeoutMs] {
            auto s = weak.lock(); if (!s) return;
            const auto it = s->waiting.find(requestId); if (it == s->waiting.end()) return;
            auto late = std::move(it->second.reply); s->waiting.erase(it);
            log::warning("recording", "Recording helper command timed out", {{"operationId", s->op.id}, {"command", command}, {"timeoutMs", timeoutMs}});
            late({"recording_helper_timeout", "The recording helper did not answer " + command + " in time", {{"step", command}, {"timeoutMs", timeoutMs}}}, {});
        }, false);
        if (!helper->send("command", {{"requestId", requestId}, {"command", command}, {"params", params}})) {
            auto it = waiting.find(requestId);
            Loop::instance().stopTimer(it->second.timer);
            auto failed = std::move(it->second.reply); waiting.erase(it);
            failed({"recording_helper_closed", "The recording helper is not reachable", {}}, {});
        }
    }
    void answerAll(const Error& e) {
        auto all = std::move(waiting); waiting.clear();
        for (auto& pair : all) { Loop::instance().stopTimer(pair.second.timer); pair.second.reply(e, {}); }
    }
};

std::string OperationService::recordingOperation() const { return recording_ ? recording_->op.id : std::string(); }
// The resident is quitting: the helper goes, and the journal keeps the operation in
// state `recording`, which the next start turns into recording_interrupted.
void OperationService::terminateRecording() {
    if (!recording_) return;
    auto session = recording_;
    session->ending = true;
    session->answerAll({"recording_helper_closed", "put.gd is quitting", {}});
    if (session->helper) session->helper->terminate("shutdown");
}

json::Object OperationService::recordingJson() const {
    const auto& s = *recording_;
    const auto retained = s.state == "reviewing" || s.state == "exporting" ? s.edit.exportDuration(s.duration) : 0;
    json::Object o{{"recordingId", s.op.id}, {"operationId", s.op.id}, {"state", s.state}, {"pending", s.pending.empty() ? json::Value() : json::Value(s.pending)},
        {"target", s.target.toJson()}, {"config", s.config.toJson()}, {"canvas", s.canvas.empty() ? json::Value() : json::Value(json::Array{s.canvas.w, s.canvas.h})},
        {"durationUs", double(s.duration)}, {"retainedUs", double(retained)}, {"edit", s.edit.toJson()}, {"progress", s.progress}, {"exports", s.exports},
        {"lastExport", s.lastExport.empty() ? json::Value() : json::Value(s.lastExport)}, {"error", s.op.error.toJson()}, {"selecting", s.selecting}};
    if (!s.canvas.empty()) { const auto out = recording::outputSize(s.canvas, s.config.maxEdge); o["outputSize"] = json::Array{out.w, out.h}; }
    if (!s.stopReason.empty()) o["stopReason"] = s.stopReason;
    return o;
}

// ---- capabilities -------------------------------------------------------------
json::Object OperationService::recordingCapabilities() const {
    json::Object result = recordingProbe_;
    if (result.empty()) result = {{"available", false}, {"reason", recordingProbing_ ? "recording_probe_pending" : "recording_not_probed"}, {"targets", json::Array{}}, {"formats", json::Array{}}, {"codecs", json::Object{}},
        {"audio", json::Object{{"system", false}, {"microphone", false}}}};
    const auto settings = settingsSnapshot();
    const auto limits = limitsOf(settings);
    result["active"] = recording_ ? json::Value(recording_->op.id) : json::Value();
    result["states"] = json::stringArray(recording::states());
    result["timeUnit"] = "microseconds";
    result["maskKinds"] = json::Array{"blur", "solid"};
    result["editCommands"] = json::Array{"setTrim", "resetTrim", "addMask", "updateMask", "removeMask"};
    result["limits"] = json::Object{{"animationMaxUs", double(limits.animationMaxUs)}, {"animationMaxEdge", limits.animationMaxEdge}, {"maxFileBytes", double(settings.limits.maxFileBytes)}, {"maxMasks", recording::maximumMasks}};
    // What this build's resident serves; the rest of docs/recording.md answers recording_unsupported.
    result["commands"] = json::Array{"prepare", "start", "pause", "resume", "stop", "cancel", "inspect", "saveCopy", "upload"};
    return result;
}
void OperationService::recordingProbe(std::function<void()> done) {
    const auto now = GetTickCount64();
    if (!recordingProbe_.empty() && (recordingProbe_.value("available").toBool() || now - recordingProbeAt_ < failedProbeCacheMs)) { done(); return; }
    recordingProbeWaiters_.push_back(std::move(done));
    if (recordingProbing_) return;
    recordingProbing_ = true;
    background([](Error& e) {
        // A separate transient run: it opens no capture source, only encodes two synthetic frames to memory.
        const auto run = helpers::runSynchronous("record", {"--record"}, json::Object{{"probe", true}}, protocol::startupMs, 60000, {});
        if (run.error) e = run.error;
        return run.result;
    }, [this](Error e, json::Object result) {
        recordingProbing_ = false;
        if (e || !result.value("available").isBool()) {
            result = {{"available", false}, {"reason", e ? e.code : "recording_probe_invalid"}, {"message", "The recording helper could not be checked"}, {"targets", json::Array{}}, {"formats", json::Array{}},
                {"codecs", json::Object{}}, {"audio", json::Object{{"system", false}, {"microphone", false}}}};
            log::warning("recording", "Recording probe failed", {{"code", e.code}});
        }
        recordingProbe_ = result; recordingProbeAt_ = GetTickCount64();
        auto waiters = std::move(recordingProbeWaiters_); recordingProbeWaiters_.clear();
        for (auto& w : waiters) w();
    });
}

// ---- dispatch -------------------------------------------------------------------
void OperationService::recordingCommand(const std::string& command, const json::Object& params, JsonReply reply) {
    if (command == "capabilities") { recordingProbe([this, reply] { reply({}, recordingCapabilities()); }); return; }
    if (!ready_) { reply(startupError_ ? startupError_ : Error{"initializing", "Journal recovery is in progress", {}}, {}); return; }
    if (command == "prepare") { prepareRecording(params, reply); return; }
    if (!recording_ || recording_->op.id != params.value("id").toString()) { reply(noSession(), {}); return; }
    if (command == "inspect") { reply({}, recordingJson()); return; }
    if (auto e = recording::checkTransition(recording_->state, command)) { reply(e, {}); return; }
    if (command == "cancel") { cancelRecording(reply); return; }
    if (command == "start" || command == "pause" || command == "resume" || command == "stop") { recordingControl(command, reply); return; }
    if (command == "saveCopy" || command == "upload") { exportRecording(command, params, reply); return; }
    reply({"recording_unsupported", "recording." + command + " is not available on Windows yet", {{"command", command}}}, {});
}
void OperationService::recordInvoke(const std::string& mode, JsonReply reply) {
    if (!recording_) {
        prepareRecording(json::Object{{"target", json::Object{{"kind", "region"}}}, {"format", "mp4"}}, [reply](Error e, json::Object result) {
            if (e) { reply(e, {}); return; }
            result["step"] = "prepare"; reply({}, result);
        });
        return;
    }
    const auto& state = recording_->state;
    std::string step = "raise";
    if (mode == "toggle" && state == "ready") step = "start";
    else if (mode == "toggle" && (state == "recording" || state == "paused")) step = "stop";
    if (step == "raise") { auto result = recordingJson(); result["step"] = step; reply({}, result); return; }
    recordingControl(step, [reply, step](Error e, json::Object result) { if (e) { reply(e, {}); return; } result["step"] = step; reply({}, result); });
}

void OperationService::endRecording(Operation op, bool removeStaging) {
    auto session = recording_;
    if (!session) return;
    recording_.reset();
    session->ending = true;
    session->answerAll({"recording_helper_closed", "The recording session ended", {}});
    if (removeStaging) files::removeDirectoryRecursively(session->directory);
    if (session->helper && session->helper->running()) {
        // A normal exit within 2 s; the helper answers what it was handling, which is no fault.
        session->helper->send("shutdown");
        Loop::instance().singleShot(shutdownMs, [session] { if (session->helper && session->helper->running()) session->helper->terminate("shutdown_timeout"); });
    }
    (void)op;
}

void OperationService::prepareRecording(const json::Object& params, JsonReply reply) {
    recordingProbe([this, params, reply] {
        const auto& caps = recordingProbe_;
        if (!caps.value("available").toBool()) { reply({"recording_unavailable", caps.value("message").toString("Screen recording is not available on this computer"), {{"reason", caps.value("reason")}}}, {}); return; }
        if (recording_) { reply({"recording_active", "A recording is already active", {{"operationId", recording_->op.id}}}, {}); return; }
        if (editor_) { reply({"editor_active", "An edit is open; finish or cancel it before recording", {{"operationId", editingId_}}}, {}); return; }
        RecordingTarget target; RecordingConfig config;
        if (auto e = recording::parseTarget(params.value("target"), target)) { reply(e, {}); return; }
        auto configParams = params; configParams.remove("target");
        if (auto e = recording::parseConfig(configParams, config)) { reply(e, {}); return; }
        if (!contains(caps.value("targets").toArray(), target.kind)) { reply({"recording_unsupported", "Recording a " + target.kind + " is not available on Windows yet", {{"kind", target.kind}}}, {}); return; }
        if (!contains(caps.value("formats").toArray(), config.format) || (!config.codec.empty() && !contains(caps.value("codecs")[config.format].toArray(), config.codec)))
            { reply({"recording_unsupported", "This format is not available: no built-in Windows encoder produces it", {{"format", config.format}, {"codec", config.codec}}}, {}); return; }
        if (config.audioSystem) { reply({"recording_unsupported", "System audio is not available on Windows yet", {{"field", "audio.system"}}}, {}); return; }
        const bool interactive = target.kind == "region" && !target.rect;
        if (interactive && !selectingId_.empty()) { reply({"busy_selecting", "Another interactive acquisition is active", {}}, {}); return; }
        const auto snapshot = settings_.snapshot();
        const auto& route = snapshot.action(Settings::recordingActionName());
        const bool upload = route.destination == "upload" && !route.profileId.empty();
        if (!upload && !snapshot.archive.keepUploads) { reply({"output_required", "No output is enabled. Choose a destination in Settings → Capture or enable Keep a local copy in Settings → General.", {}}, {}); return; }
        // Room for the working clip, its export and the upload copy: a third of the free disk above the reserve.
        const auto directory = paths::join(paths::join(root_, "recording"), "rec-" + uuid::create());
        const auto free = files::freeBytes(root_);
        long long budget = snapshot.limits.maxFileBytes - 1024 * 1024;
        if (free >= 0) budget = std::min(budget, (free - diskReserveBytes) / 3);
        if (budget < minimumBudgetBytes) { reply({"disk_space_insufficient", "Not enough free disk space to record", {{"freeBytes", double(free)}, {"reserveBytes", double(diskReserveBytes)}, {"requiredBytes", double(minimumBudgetBytes * 3 + diskReserveBytes)}}}, {}); return; }
        config.budgetBytes = budget;

        auto session = std::make_shared<RecordingSession>();
        session->target = target; session->config = config; session->directory = directory;
        session->workingPath = paths::join(directory, "working.mp4");
        session->cancel = std::make_shared<std::atomic_bool>(false);
        Operation op;
        op.id = uuid::create(); op.action = Settings::recordingActionName(); op.settingsDocument = snapshot.document; op.settingsRevision = settings_.revision(); op.created = files::nowIsoUtc();
        op.invocation = upload ? ++invocation_ : invalidateCopyEligibility(); op.clipboardRevision = clipboard_.revision();
        if (upload) { const auto& profile = snapshot.profiles.at(route.profileId); op.destination = {profile.value("provider").toString(), route.profileId, settings_.revision(), profile.value("settings").toObject(), profile.value("credentials").toObject()}; }
        op.state = "recording"; op.phase = "preparing";
        session->op = op;
        if (!files::makePrivateDirectory(directory) || !helpers::prepareOutputFile(session->workingPath)) { files::removeDirectoryRecursively(directory); reply({"artifact_io", "Cannot stage the recording", {}}, {}); return; }
        recording_ = session;
        active_[op.id] = session->cancel;
        if (interactive) selectingId_ = op.id;
        if (upload) { UploadCopy copy; copy.mode = snapshot.uploadClipboardBehavior; copy.clipboardRevision = op.clipboardRevision; copy.epoch = copyEpoch_; uploadCopies_[op.id] = copy; }
        notifyActivity();
        // The durable intent first: a restart turns it into recording_interrupted.
        background([this, op](Error& e) { HistoryStore store(root_, paths_db_); e = store.saveOperation(op); return json::Object{}; }, [this, session, reply](Error e, json::Object) {
            if (recording_ != session) { reply({"recording_unavailable", "The recording ended", {}}, {}); return; }
            if (e) { auto op = session->op; endRecording(op); op.error = e; op.state = "failed"; finish(op); reply(e, {}); return; }
            observe(session->op.publicJson());
            log::info("operations", "Operation accepted", {{"operationId", session->op.id}, {"action", session->op.action}, {"output", session->op.destination.providerId.empty() ? "local" : "upload"}, {"profileId", session->op.destination.profileId}});
            reply({}, recordingJson());
            if (onChanged) onChanged(session->op.id);
            json::Array exclude; if (uiPid_) exclude.push_back(double(uiPid_)); exclude.push_back(double(GetCurrentProcessId()));
            json::Object bootstrap{{"target", session->target.toJson()}, {"config", session->config.toJson()}, {"output", json::Object{{"path", session->workingPath}}},
                {"limits", json::Object{{"budgetBytes", double(session->config.budgetBytes)}}}, {"excludePids", exclude}};
            session->helper = std::make_unique<helpers::Supervised>("record", std::vector<std::string>{"--record"});
            auto phase = [this, session](const std::string& state) { session->state = state; auto doc = session->op.publicJson(); doc["phase"] = state; observe(doc); if (onChanged) onChanged(session->op.id); };
            auto fail = [this, session](Error e) {
                if (recording_ != session) return;
                auto op = session->op; endRecording(op);
                op.cancelled = e.code == "cancelled" || session->cancel->load();
                op.state = op.cancelled ? "cancelled" : "failed"; op.error = e;
                if (selectingId_ == op.id) selectingId_.clear();
                log::info("recording", op.cancelled ? "Recording cancelled" : "Recording failed", {{"operationId", op.id}, {"code", e.code}});
                finish(op);
            };
            session->helper->start(bootstrap, protocol::startupMs, [this, session, phase, fail](const protocol::Frame& frame) {
                if (recording_ != session || session->ending) return;
                const auto& p = frame.payload;
                if (frame.type == "selecting") { session->selecting = true; phase("preparing"); }
                else if (frame.type == "prepared") {
                    session->selecting = false;
                    if (selectingId_ == session->op.id) selectingId_.clear();
                    const auto& canvas = p.value("canvas");
                    session->canvas = {canvas[size_t(0)].toInt(), canvas[size_t(1)].toInt()};
                    if (session->target.kind == "region") { const auto& r = p.value("rect"); session->target.rect = RecordingRect{r[size_t(0)].toInteger(), r[size_t(1)].toInteger(), r[size_t(2)].toInteger(), r[size_t(3)].toInteger()}; }
                    recording::clampAnimationSize(session->config, session->canvas, limitsOf(settingsSnapshot()));
                    session->progress = {{"durationUs", 0}, {"bytes", 0}, {"paused", false}, {"encoder", p.value("encoder")}, {"acquisition", p.value("acquisition")}};
                    log::info("recording", "Recording step", {{"operationId", session->op.id}, {"state", "ready"}, {"encoder", p.value("encoder")}});
                    phase("ready");
                } else if (frame.type == "failed") fail({p.value("code").toString("recording_failed"), p.value("message").toString("The recording could not be prepared"), p.value("details").toObject()});
                else if (frame.type == "commandResult") {
                    // A result nobody waits for (its caller timed out, or the session ended) is dropped.
                    const auto it = session->waiting.find(p.value("requestId").toString());
                    if (it == session->waiting.end()) return;
                    auto waiter = std::move(it->second); session->waiting.erase(it);
                    Loop::instance().stopTimer(waiter.timer);
                    if (p.value("ok").toBool()) waiter.reply({}, p.value("result").toObject());
                    else waiter.reply({p.value("code").toString("recording_failed"), p.value("message").toString("The recording helper refused the command"), p.value("details").toObject()}, {});
                } else if (frame.type == "event") {
                    const auto event = p.value("event").toString();
                    if (event == "progress") { for (const auto& pair : p) if (pair.first != "event") session->progress[pair.first] = pair.second; }
                    else if (event == "stopped" && (session->state == "recording" || session->state == "paused" || session->state == "stopping")) {
                        session->duration = p.value("durationUs").toInteger(0); session->stopReason = p.value("reason").toString("user"); session->edit = {};
                        log::warning("recording", "Recording stopped on its budget", {{"operationId", session->op.id}, {"durationUs", double(session->duration)}, {"budgetBytes", double(session->config.budgetBytes)}});
                        session->pending.clear();
                        phase("reviewing");
                    } else if (event == "failed") fail(Error::fromJson(p.value("error").toObject()));
                }
            }, [this, session, fail](int code, bool normal, const std::string& reason) {
                log::info("recording", session->ending ? "Recording helper exited" : "Recording helper ended unexpectedly", {{"operationId", session->op.id}, {"exitCode", double(code)}, {"normal", normal}, {"reason", reason}});
                if (session->ending) { Loop::instance().post([session] { session->helper.reset(); }); return; }
                session->answerAll({"recording_helper_closed", "The recording helper ended", {}});
                fail({normal ? "recording_helper_exited" : "recording_helper_crashed", "The recording helper ended unexpectedly; nothing was uploaded", {{"exitCode", double(code)}, {"reason", reason}}});
                Loop::instance().post([session] { session->helper.reset(); });
            });
        });
    });
}

void OperationService::recordingControl(const std::string& command, JsonReply reply) {
    auto session = recording_;
    auto& s = *session;
    if (auto e = recording::checkTransition(s.state, command)) { reply(e, {}); return; }
    if (s.pending == command) { reply({}, recordingJson()); return; }
    if (!s.pending.empty()) { reply({"recording_busy", "Another recording command is still being acknowledged", {{"pending", s.pending}}}, {}); return; }
    s.pending = command;
    const auto previous = s.state;
    if (command == "start") s.state = "starting";
    if (command == "stop") s.state = "stopping";
    session->request(command, {}, command == "stop" ? stopMs : commandMs, [this, session, command, previous, reply](Error e, json::Object result) {
        if (recording_ != session) { reply(e ? e : noSession(), {}); return; }
        auto& s = *session;
        s.pending.clear();
        if (e) {
            if (command == "pause" || command == "resume") { s.state = previous; reply(e, {}); return; }
            auto op = s.op; endRecording(op); op.state = "failed"; op.error = e; finish(op);
            reply(e, {}); return;
        }
        for (const auto& pair : result) if (pair.first != "canvas") s.progress[pair.first] = pair.second;
        std::string next = command == "pause" ? "paused" : "recording";
        if (command == "stop") {
            s.duration = result.value("durationUs").toInteger(0);
            if (s.duration < 1) { auto op = s.op; endRecording(op); op.state = "failed"; op.error = {"recording_failed", "The recording ended without a usable clip", {}}; finish(op); reply(op.error, {}); return; }
            s.edit = {}; s.stopReason = "user"; next = "reviewing";
        }
        s.state = next;
        auto doc = s.op.publicJson(); doc["phase"] = next; observe(doc);
        log::info("recording", "Recording step", {{"operationId", s.op.id}, {"state", next}});
        if (onChanged) onChanged(s.op.id);
        reply({}, recordingJson());
    });
    auto doc = s.op.publicJson(); doc["phase"] = s.state; observe(doc);
}

void OperationService::cancelRecording(JsonReply reply) {
    auto session = recording_;
    session->cancel->store(true);
    // The helper discards its clip; its answer may arrive after the session ended and is dropped.
    session->request("cancel", {}, commandMs, [](Error, json::Object) {});
    auto result = recordingJson(); result["state"] = "cancelled"; result["pending"] = json::Value();
    auto op = session->op;
    endRecording(op);
    op.cancelled = true; op.state = "cancelled"; op.error = {};
    if (selectingId_ == op.id) selectingId_.clear();
    finish(op);
    reply({}, result);
}

void OperationService::exportRecording(const std::string& command, const json::Object& params, JsonReply reply) {
    auto session = recording_;
    auto& s = *session;
    const auto settings = settingsSnapshot();
    std::string destination;
    bool overwrite = false;
    const auto extension = "." + s.config.extension();
    if (command == "saveCopy") {
        destination = params.value("path").toString();
        overwrite = params.value("overwrite").toBool();
        if (!paths::isAbsolute(destination) || destination.size() <= extension.size() || text::lower(destination.substr(destination.size() - extension.size())) != extension) { reply(invalidRequest("saveCopy needs an absolute path ending in " + extension), {}); return; }
        if (files::exists(destination) && !overwrite) { reply({"file_exists", "The file already exists; pass overwrite to replace it", {}}, {}); return; }
    } else {
        if (!json::integer(params.value("revision"), 1, 4503599627370496.0)) { reply(invalidRequest("upload takes the edit revision it was reviewed at"), {}); return; }
        if ((unsigned long long)params.value("revision").toInteger() != s.edit.revision) { reply({"recording_stale", "The edit revision moved; inspect and retry", {{"currentRevision", double(s.edit.revision)}}}, {}); return; }
    }
    if (auto e = recording::checkExportBudget(s.config, s.edit, s.duration, s.canvas, limitsOf(settings))) { reply(e, {}); return; }
    if (!s.edit.unedited(s.duration)) { reply({"recording_unsupported", "Trimmed or masked exports are not available on Windows yet", {}}, {}); return; }
    const auto output = paths::join(s.directory, "export-" + std::to_string(++s.exports) + extension);
    if (!helpers::prepareOutputFile(output)) { reply({"recording_export_failed", "Cannot stage the export", {}}, {}); return; }
    const auto edit = s.edit;
    s.lastExport = {{"command", command}, {"status", "exporting"}, {"revision", double(edit.revision)}};
    if (command == "saveCopy") s.lastExport["path"] = destination;
    s.state = "exporting"; s.pending = command;
    { auto doc = s.op.publicJson(); doc["phase"] = command == "upload" ? "encoding" : "exporting"; observe(doc); }
    // An export may take minutes, longer than a client waits: answer now; inspect carries the outcome.
    reply({}, recordingJson());
    const json::Object exportParams{{"output", json::Object{{"path", output}}}, {"format", s.config.format}, {"codec", s.config.codec}, {"edit", edit.toJson()}, {"unedited", true}};
    session->request("export", exportParams, exportMs, [this, session, command, destination, overwrite, output, edit](Error e, json::Object result) {
        if (recording_ != session) { files::remove(output); return; }
        auto& s = *session;
        const auto maximum = settingsSnapshot().limits.maxFileBytes;
        auto failed = [&](const Error& cause) {
            files::remove(output);
            s.lastExport["status"] = "failed"; s.lastExport["error"] = cause.toJson();
            s.state = "reviewing"; s.pending.clear();
            log::warning("recording", "Recording export failed", {{"operationId", s.op.id}, {"command", command}, {"code", cause.code}});
            auto doc = s.op.publicJson(); doc["phase"] = "reviewing"; observe(doc);
        };
        if (e) { failed(e); return; }
        long long bytes = 0;
        const auto digest = files::sha256(output, &bytes);
        if (digest.empty() || bytes < 1 || bytes != result.value("bytes").toInteger(-1) || digest != result.value("sha256").toString()) { failed({"recording_export_failed", "The export did not pass the handoff checks", {}}); return; }
        if (bytes > maximum) { failed({"input_excessive", "The export exceeds the file size limit", {{"bytes", double(bytes)}, {"limit", double(maximum)}}}); return; }
        if (command == "saveCopy") {
            if (overwrite && files::exists(destination)) files::remove(destination);
            if (files::copyFile(output, destination)) { failed({"local_save_failed", "Cannot write the copy", {{"path", destination}}}); return; }
            files::remove(output);
            s.lastExport["status"] = "saved"; s.lastExport["bytes"] = double(bytes); s.lastExport["durationUs"] = result.value("durationUs");
            s.state = "reviewing"; s.pending.clear();
            log::info("recording", "Recording copy saved", {{"operationId", s.op.id}, {"bytes", double(bytes)}});
            auto doc = s.op.publicJson(); doc["phase"] = "reviewing"; observe(doc);
            return;
        }
        // Explicit Upload: these exact checked bytes become the operation's only artifact.
        auto op = s.op;
        endRecording(op, false);
        Payload payload; payload.kind = "recording"; payload.imagePath = output; payload.stagingDirectory = s.directory; payload.mime = s.config.mime();
        op.state = "acquiring"; op.phase = "queued";
        log::info("recording", "Recording submitted", {{"operationId", op.id}, {"bytes", double(bytes)}, {"revision", double(edit.revision)}});
        enqueue({std::move(op), std::move(payload), true, s.cancel});
    });
}
}
