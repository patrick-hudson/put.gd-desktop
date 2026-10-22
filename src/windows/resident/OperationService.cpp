#include "resident/OperationService.h"
#include "common/BuildInfo.h"
#include "common/Files.h"
#include "common/Log.h"
#include "common/Paths.h"
#include "common/Text.h"
#include "common/Uuid.h"
#include "core/Naming.h"
#include "core/SanitizedPng.h"
#include "resident/Resident.h"
#include <algorithm>
#include <chrono>

namespace putgd {
namespace {
constexpr long long editorAdmissionPixels = 24000000;
bool in(const std::vector<std::string>& list, const std::string& v) { return std::find(list.begin(), list.end(), v) != list.end(); }
Error notReady() { return {"initializing", "Journal recovery is in progress", {}}; }
// Local copies: unique atomic user-owned files under the configured folders (docs/configuration.md).
std::string archiveFolder(const ArchiveSettings& s, const std::string& category) {
    const auto it = s.folders.find(category);
    return it == s.folders.end() || it->second.empty() ? paths::join(s.root, category) : it->second;
}
LocalCopy saveLocalCopy(const Artifact& a, const ArchiveSettings& s) {
    LocalCopy r; r.status = "failed";
    auto fail = [&] { r.error = {"archive_failed", "Cannot create local copy; retry the retained artifact", {}}; return r; };
    const auto dir = archiveFolder(s, a.category);
    if (!files::makeDirectories(dir)) return fail();
    const auto temporary = files::temporaryName(dir, ".putgd-");
    if (!CopyFileW(paths::wide(a.path).c_str(), paths::wide(temporary).c_str(), TRUE)) return fail();
    const auto name = naming::candidate(a, "original", a.id);
    r.path = paths::join(dir, name);
    bool saved = files::rename(temporary, r.path, false);
    for (int attempt = 0; !saved && attempt < 32; ++attempt) {
        if (!files::exists(r.path)) { files::remove(temporary); return fail(); }
        r.path = paths::join(dir, naming::renamedCandidate(name, uuid::create()));
        saved = files::rename(temporary, r.path, false);
    }
    if (!saved) { files::remove(temporary); return fail(); }
    r.status = "saved";
    return r;
}
}

struct OperationService::CaptureSession {
    std::unique_ptr<helpers::Supervised> helper;
    Operation op; CancelFlag cancel; std::string directory, outputPath; json::Object checkpoint; Error error; bool selected = false;
};
struct OperationService::EditorSession {
    std::unique_ptr<helpers::Supervised> helper;
    Operation op; CancelFlag cancel; std::string directory, inputPath, outputPath; long long outputLimit = 0;
    bool editing = false, finalizing = false, ready = false; json::Object checkpoint; Error error;
    std::map<std::string, JsonReply> pending; std::map<std::string, UINT_PTR> timers;
    UINT_PTR deadline = 0;
};

OperationService::OperationService(std::string root, SettingsService& settings, ProviderRegistry& providers, Clipboard& clipboard)
    : root_(std::move(root)), paths_db_(paths::join(root_, "history.sqlite")), settings_(settings), providers_(providers), clipboard_(clipboard), artifacts_(root_) {}
OperationService::~OperationService() { shutdown(); }

void OperationService::background(std::function<json::Object(Error&)> work, JsonReply reply) {
    auto error = std::make_shared<Error>(); auto result = std::make_shared<json::Object>();
    Loop::instance().background([work, error, result] { *result = work(*error); }, [reply, error, result] { if (reply) reply(*error, *result); });
}
std::shared_ptr<std::mutex> OperationService::objectLock(const Destination& d, const json::Object& reference, const std::string& candidate) {
    std::string key = d.providerId;
    if (auto provider = providers_.find(d.providerId)) key += ":" + provider->serializationKey(d, reference, candidate);
    if (key.empty() || key == d.providerId) key = d.providerId + ":" + json::serialize(reference) + ":" + candidate;
    std::lock_guard lock(locksMutex_);
    if (auto existing = locks_[key].lock()) return existing;
    auto fresh = std::make_shared<std::mutex>();
    locks_[key] = fresh;
    return fresh;
}
void OperationService::initialize(std::function<void()> done) {
    files::makePrivateDirectory(paths::join(root_, "editing"));
    files::makePrivateDirectory(paths::join(root_, "capture"));
    background([this](Error& e) {
        HistoryStore store(root_, paths_db_);
        if (store.error()) { e = store.error(); return json::Object{}; }
        for (auto& op : store.recover()) {
            // Interrupted work never republishes on its own (docs/architecture.md).
            bool changed = false;
            for (auto& item : op.items) if (item.uploadStatus == "uploading") { item.uploadStatus = "unknown"; item.phase = "unknown"; item.error = {"publication_unknown", "The resident stopped while this upload was in flight; retry reconciles it", {}}; changed = true; }
            if (op.state == "editing") op.error = {"edit_interrupted", "The editor was open when put.gd stopped; start a new edit", {}};
            else if (op.state == "accepted" || op.state == "acquiring") op.error = {"interrupted", "The resident stopped before this capture finished", {}};
            else op.error = {"interrupted", "The resident stopped while this upload was running; use Retry", {}};
            op.state = "interrupted"; op.phase = "interrupted"; if (op.copyStatus == "pending") op.copyStatus = "no_result";
            (void)changed;
            store.saveOperation(op, &providers_);
        }
        for (const auto& name : files::list(paths::join(root_, "editing"))) files::removeDirectoryRecursively(paths::join(paths::join(root_, "editing"), name));
        for (const auto& name : files::list(paths::join(root_, "capture"))) files::removeDirectoryRecursively(paths::join(paths::join(root_, "capture"), name));
        ArtifactStore(root_).discardIncomplete();
        return json::Object{};
    }, [this, done](Error e, json::Object) {
        startupError_ = e; ready_ = !e;
        if (e) log::error("operations", "Journal unavailable", {{"code", e.code}});
        cleanupTimer_ = Loop::instance().startTimer(60 * 60 * 1000, [this] { cleanup(true, [](Error, json::Object) {}); });
        scheduleHeapReclamation();
        if (done) done();
    });
}
bool OperationService::idle() const { return !stopping_ && queue_.empty() && running_ == 0 && active_.empty() && !capture_ && !editor_; }
void OperationService::scheduleHeapReclamation() {
    heapPending_ = true;
    if (heapTimer_) Loop::instance().stopTimer(heapTimer_);
    heapTimer_ = Loop::instance().startTimer(2000, [this] {
        heapTimer_ = 0;
        if (!idle() || Loop::instance().backgroundCount() > 0) { heapPending_ = true; return; }
        const auto start = std::chrono::steady_clock::now();
        HeapCompact(GetProcessHeap(), 0);
        SetProcessWorkingSetSizeEx(GetCurrentProcess(), SIZE_T(-1), SIZE_T(-1), 0);
        lastHeapReclamationUsec_ = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();
        ++heapReclamations_; heapPending_ = false;
    }, false);
}
void OperationService::notifyActivity() { if (onActivityChanged) onActivityChanged(!active_.empty() || running_ > 0, storageRequests_ > 0); }
void OperationService::observe(const json::Object& document) {
    { std::lock_guard lock(observationsMutex_); observations_[document.value("id").toString()] = document; }
    Loop::instance().post([this] { if (!stopping_ && onObservationsChanged) onObservationsChanged(); });
}
void OperationService::clearObservation(const std::string& id) { std::lock_guard lock(observationsMutex_); observations_.erase(id); }
json::Object OperationService::currentObservation(json::Object document) const {
    if (in({"completed", "partial", "failed", "cancelled", "interrupted"}, document.value("state").toString())) return document;
    json::Object live;
    { std::lock_guard lock(observationsMutex_); const auto it = observations_.find(document.value("id").toString()); if (it != observations_.end()) live = it->second; }
    if (live.empty()) return document;
    document["phase"] = live.value("phase");
    document["state"] = live.value("state");
    std::map<std::string, json::Object> observed;
    for (const auto& v : live.value("items").toArray()) observed[v.objectRef().value("artifact").objectRef().value("id").toString()] = v.toObject();
    auto items = document.value("items").toArray();
    for (auto& v : items) {
        auto item = v.toObject();
        const auto it = observed.find(item.value("artifact").objectRef().value("id").toString());
        if (it != observed.end()) { item["phase"] = it->second.value("phase"); item["uploadProgress"] = it->second.value("uploadProgress"); item["uploadStatus"] = it->second.value("uploadStatus"); }
        v = item;
    }
    if (items.empty() && !live.value("items").toArray().empty()) items = live.value("items").toArray();
    document["items"] = items;
    return document;
}
json::Array OperationService::activeObservations() const { std::lock_guard lock(observationsMutex_); json::Array a; for (const auto& pair : observations_) a.push_back(pair.second); return a; }
std::string OperationService::routeDestination(const Operation& op) const { return Settings::fromValidated(op.settingsDocument).action(op.action).destination; }

// ---- acceptance -------------------------------------------------------------
void OperationService::invoke(const std::string& action, const json::Object& options, JsonReply reply) {
    if (!ready_) { reply(startupError_ ? startupError_ : notReady(), {}); return; }
    if (action == Settings::recordingActionName()) { reply({"recording_unavailable", "Screen recording is not available in the Windows build yet", {}}, {}); return; }
    if (!in(Settings::actionNames(), action)) { reply({"invalid_action", "Unknown sharing action", {}}, {}); return; }
    const auto source = action == "edit" ? options.value("source").toString("region") : action;
    const auto sourcePath = options.value("path").toString();
    if ((action != "edit" && !options.empty()) || (action == "edit" && (!in({"region", "window", "desktop", "clipboard", "file"}, source) || (source == "file") != options.contains("path") || (source == "file" && !paths::isAbsolute(sourcePath)))))
        { reply({"invalid_request", "Edit sources are region, window, desktop, clipboard, or file with an absolute path", {}}, {}); return; }
    if (action == "edit" && editor_) { editorCommand(editingId_, "raise", {}, [](Error, json::Object) {}); reply({"editor_active", "An edit is already open; finish or cancel it first", {{"operationId", editingId_}}}, {}); return; }
    const bool interactive = source != "clipboard" && source != "file";
    const auto snapshot = settings_.snapshot();
    if (int(active_.size()) >= snapshot.limits.pendingOperations + snapshot.limits.concurrentUploads) { reply({"queue_full", "Pending operation limit reached", {}}, {}); return; }
    if (interactive && !selectingId_.empty()) { reply({"busy_selecting", "Another interactive acquisition is active", {}}, {}); return; }
    const auto& route = snapshot.action(action);
    const bool upload = route.destination == "upload" && !route.profileId.empty();
    if (!upload && !snapshot.archive.keepUploads) { reply({"output_required", "No output is enabled. Choose a destination in Settings → Capture or enable Keep a local copy in Settings → General.", {}}, {}); return; }
    Operation op;
    op.id = uuid::create(); op.action = action; op.settingsDocument = snapshot.document; op.settingsRevision = settings_.revision(); op.created = files::nowIsoUtc();
    op.invocation = upload ? ++invocation_ : invalidateCopyEligibility(); op.clipboardRevision = clipboard_.revision();
    if (upload) { const auto& profile = snapshot.profiles.at(route.profileId); op.destination = {profile.value("provider").toString(), route.profileId, settings_.revision(), profile.value("settings").toObject(), profile.value("credentials").toObject()}; }
    acceptOperation(std::move(op), source, sourcePath, upload, std::move(reply));
}
void OperationService::acceptOperation(Operation op, const std::string& source, const std::string& sourcePath, bool upload, JsonReply reply) {
    auto flag = std::make_shared<std::atomic_bool>(false);
    active_[op.id] = flag;
    const bool interactive = source != "clipboard" && source != "file";
    if (interactive) selectingId_ = op.id;
    if (op.action == "edit") editingId_ = op.id;
    if (upload) { UploadCopy copy; copy.mode = settings_.snapshot().uploadClipboardBehavior; copy.clipboardRevision = op.clipboardRevision; copy.epoch = copyEpoch_; uploadCopies_[op.id] = copy; }
    notifyActivity();
    const auto id = op.id;
    background([this, op](Error& e) { HistoryStore store(root_, paths_db_); e = store.saveOperation(op); return op.publicJson(); }, [this, op, flag, reply, source, sourcePath](Error e, json::Object result) mutable {
        if (e) { uploadCopies_.erase(op.id); releaseActive(op.id); if (selectingId_ == op.id) selectingId_.clear(); if (editingId_ == op.id) editingId_.clear(); reply(e, {}); return; }
        observe(op.publicJson());
        reply({}, {{"operationId", op.id}, {"operation", result}});
        if (onChanged) onChanged(op.id);
        log::info("operations", "Operation accepted", {{"operationId", op.id}, {"action", op.action}, {"output", op.destination.providerId.empty() ? "local" : "upload"}, {"profileId", op.destination.profileId}});
        if (flag->load()) { op.state = "cancelled"; op.cancelled = true; finish(op); return; }
        op.state = "acquiring"; op.phase = "acquiring";
        background([this, op](Error& err) { HistoryStore store(root_, paths_db_); err = store.saveOperation(op); return json::Object{}; }, [this, op, flag, source, sourcePath](Error err, json::Object) mutable {
            if (err) { op.error = err; op.state = "failed"; finish(op); return; }
            observe(op.publicJson());
            if (flag->load()) { op.state = "cancelled"; op.cancelled = true; finish(op); return; }
            acquireSource(std::move(op), source, sourcePath, flag);
        });
    });
    (void)id;
}
void OperationService::acquireSource(Operation op, const std::string& source, const std::string& sourcePath, CancelFlag flag) {
    if (source == "clipboard") {
        const auto limits = settingsSnapshot().limits;
        Error error;
        const auto offer = clipboard_.read(limits.clipboardTextBytes, limits.imageBytes, limits.maxFiles, error);
        if (selectingId_ == op.id) selectingId_.clear();
        if (error) { op.error = error; op.state = "failed"; finish(op); return; }
        Payload payload;
        if (offer.kind == "files") { payload.kind = "files"; payload.files = offer.files; }
        else if (offer.kind == "image") { payload.kind = "image"; payload.bytes = offer.bytes; payload.imageFormat = offer.imageFormat; }
        else { payload.kind = "text"; payload.bytes = offer.bytes; }
        if (op.action == "edit") startEditing(std::move(op), std::move(payload), flag); else enqueue({std::move(op), std::move(payload), true, flag});
        return;
    }
    if (source == "file") {
        Payload payload; payload.kind = "files"; payload.files = {sourcePath};
        startEditing(std::move(op), std::move(payload), flag);
        return;
    }
    startCapture(std::move(op), source, flag);
}
void OperationService::startCapture(Operation op, const std::string& mode, CancelFlag flag) {
    auto session = std::make_unique<CaptureSession>();
    session->op = op; session->cancel = flag;
    session->directory = paths::join(paths::join(root_, "capture"), "cap-" + uuid::create());
    session->outputPath = paths::join(session->directory, "capture.png");
    if (!files::makePrivateDirectory(session->directory) || !helpers::prepareOutputFile(session->outputPath)) { op.error = {"artifact_io", "Cannot stage the capture", {}}; op.state = "failed"; if (selectingId_ == op.id) selectingId_.clear(); finish(op); return; }
    const auto limits = settingsSnapshot().limits;
    json::Array exclude; if (uiPid_) exclude.push_back(double(uiPid_)); exclude.push_back(double(GetCurrentProcessId()));
    json::Object bootstrap{{"mode", mode}, {"output", json::Object{{"path", session->outputPath}}}, {"limits", json::Object{{"imagePixels", double(limits.imagePixels)}, {"imageBytes", double(limits.imageBytes)}}}, {"excludePids", exclude}, {"includeCursor", false}};
    session->helper = std::make_unique<helpers::Supervised>("capture", std::vector<std::string>{"--capture"});
    capture_ = std::move(session);
    auto* raw = capture_.get();
    auto observeState = [this, raw](const std::string& phase) { auto doc = raw->op.publicJson(); doc["phase"] = phase; observe(doc); };
    raw->helper->setSilenceWatchdog(false); // no deadline while the selector is open
    raw->helper->start(bootstrap, protocol::startupMs, [this, raw, observeState](const protocol::Frame& frame) {
        if (frame.type == "selecting") observeState("selecting");
        else if (frame.type == "selected") { raw->selected = true; if (selectingId_ == raw->op.id) selectingId_.clear(); observeState("capturing"); }
        else if (frame.type == "checkpoint") {
            long long bytes = 0; const auto digest = files::sha256(raw->outputPath, &bytes);
            if (bytes != raw->checkpoint.value("bytes").toInteger(-1) && (bytes != frame.payload.value("bytes").toInteger(-1) || digest != frame.payload.value("sha256").toString())) raw->error = {"capture_failed", "The capture helper's checkpoint does not match the written file", {}};
            else raw->checkpoint = frame.payload;
            raw->helper->send("checkpointAccepted");
        } else if (frame.type == "failed") raw->error = {frame.payload.value("code").toString("capture_failed"), frame.payload.value("message").toString("Capture failed"), {}};
    }, [this, raw](int code, bool normal, const std::string& reason) {
        auto session = std::move(capture_);
        if (!session) return;
        auto op = session->op;
        if (selectingId_ == op.id) selectingId_.clear();
        if (session->error || session->checkpoint.empty() || !normal) {
            Error e = session->error ? session->error : Error{"capture_failed", "The capture helper exited without a result", {{"exitCode", double(code)}, {"reason", reason}}};
            op.error = e; op.cancelled = session->cancel->load() || e.code == "capture_cancelled"; op.state = op.cancelled ? "cancelled" : "failed";
            if (op.cancelled && !op.error) op.error = {};
            files::removeDirectoryRecursively(session->directory);
            finish(op); return;
        }
        Payload payload; payload.kind = "capture"; payload.imagePath = session->outputPath;
        if (op.action == "edit") startEditing(std::move(op), std::move(payload), session->cancel); else enqueue({std::move(op), std::move(payload), true, session->cancel});
        // The staging directory is removed once the artifact store moved the file.
        const auto directory = session->directory;
        Loop::instance().singleShot(30000, [directory] { files::removeDirectoryRecursively(directory); });
    });
}

// ---- editing ---------------------------------------------------------------
void OperationService::startEditing(Operation op, Payload input, CancelFlag flag) {
    auto session = std::make_shared<EditorSession>();
    session->op = op; session->cancel = flag;
    session->directory = paths::join(paths::join(root_, "editing"), "pge-" + uuid::create());
    session->inputPath = paths::join(session->directory, "input.img"); session->outputPath = paths::join(session->directory, "output.png");
    const auto limits = settingsSnapshot().limits;
    auto fail = [this, session](Error e) { files::removeDirectoryRecursively(session->directory); auto failed = session->op; failed.error = e; failed.state = "failed"; if (editingId_ == failed.id) editingId_.clear(); editor_.reset(); finish(failed); };
    if (!files::makePrivateDirectory(session->directory)) { fail({"edit_staging_failed", "Cannot create private editor staging", {}}); return; }
    Error stage;
    if (input.kind == "capture") { if (!files::rename(input.imagePath, session->inputPath, false)) stage = {"edit_staging_failed", "Cannot stage the capture", {}}; }
    else if (input.kind == "png") { stage = files::writeAtomically(session->inputPath, input.bytes); }
    else if (input.kind == "files") {
        if (input.files.size() != 1 || !files::isRegularFile(input.files[0])) stage = {"source_unsupported", "Edit needs exactly one PNG or JPEG file", {}};
        else { const auto mime = ArtifactStore::mimeFor(input.files[0]); if (mime != "image/png" && mime != "image/jpeg") stage = {"source_unsupported", "Edit accepts a static PNG or JPEG", {}};
            else if (files::size(input.files[0]) > limits.imageBytes) stage = {"input_excessive", "The image file exceeds the configured size", {}};
            else stage = files::copyFile(input.files[0], session->inputPath); }
    } else if (input.kind == "image") {
        if (input.imageFormat == "png" && !png::inspect(input.bytes)) stage = files::writeAtomically(session->inputPath, input.bytes);
        else {
            const auto raw = paths::join(session->directory, "clipboard.src");
            stage = files::writeAtomically(raw, input.bytes);
            if (!stage) { helpers::prepareOutputFile(session->inputPath);
                const auto run = helpers::runSynchronous("image-job", {"--image-job"}, json::Object{{"input", json::Object{{"path", raw}, {"kind", input.imageFormat}}}, {"output", json::Object{{"path", session->inputPath}, {"kind", "png"}}},
                    {"limits", json::Object{{"imagePixels", double(editorAdmissionPixels)}, {"imageBytes", double(limits.imageBytes)}}}, {"reencode", true}}, 10000, 120000, {});
                files::remove(raw); if (run.error) stage = run.error; }
        }
    } else stage = {"source_unsupported", "Unsupported edit source", {}};
    if (stage) { fail(stage.code == "file_io" ? Error{"edit_staging_failed", "Cannot stage the editor input", {}} : stage); return; }
    long long inputBytes = 0; const auto digest = files::sha256(session->inputPath, &inputBytes);
    if (digest.empty() || !helpers::prepareOutputFile(session->outputPath)) { fail({"edit_staging_failed", "Cannot stage the editor files", {}}); return; }
    files::restrictToOwner(session->inputPath);
    const auto snapshot = settingsSnapshot();
    std::string label = "local copy";
    if (!op.destination.profileId.empty()) { const auto it = snapshot.profiles.find(op.destination.profileId); label = it != snapshot.profiles.end() ? it->second.value("label").toString(op.destination.profileId) : op.destination.profileId; }
    session->outputLimit = limits.imageBytes;
    json::Object bootstrap{{"input", json::Object{{"path", session->inputPath}, {"bytes", double(inputBytes)}, {"sha256", digest}}}, {"output", json::Object{{"path", session->outputPath}}},
        {"limits", json::Object{{"imagePixels", double(editorAdmissionPixels)}, {"outputBytes", double(limits.imageBytes)}}}, {"destinationLabel", label}, {"recentColors", json::stringArray(snapshot.editorRecentColors)}};
    session->helper = std::make_unique<helpers::Supervised>("image-edit", std::vector<std::string>{"--edit-image"});
    editor_ = session;
    session->deadline = Loop::instance().startTimer(protocol::editorStartupMs, [this, session] { session->deadline = 0; if (!session->editing && editor_ == session) { session->error = {"editor_unavailable", "The editor did not open in time", {}}; session->helper->terminate("startup_timeout"); } }, false);
    session->helper->start(bootstrap, protocol::editorStartupMs, [this, session](const protocol::Frame& frame) {
        if (frame.type == "editing") {
            session->editing = true;
            if (session->deadline) { Loop::instance().stopTimer(session->deadline); session->deadline = 0; }
            auto op = session->op; op.state = "editing"; op.phase = "editing"; session->op = op;
            background([this, op](Error& e) { HistoryStore store(root_, paths_db_); e = store.saveOperation(op); return json::Object{}; }, [this, op](Error, json::Object) { observe(op.publicJson()); if (onChanged) onChanged(op.id); });
        } else if (frame.type == "commandResult") {
            const auto id = frame.payload.value("requestId").toString();
            const auto it = session->pending.find(id);
            if (it == session->pending.end()) return;
            auto reply = it->second; session->pending.erase(it);
            if (auto timer = session->timers.find(id); timer != session->timers.end()) { Loop::instance().stopTimer(timer->second); session->timers.erase(timer); }
            if (frame.payload.value("ok").toBool()) reply({}, frame.payload.value("result").toObject());
            else reply({frame.payload.value("code").toString("editor_error"), frame.payload.value("message").toString("The editor refused the command"), {}}, {});
        } else if (frame.type == "finalizing") {
            session->finalizing = true;
            session->deadline = Loop::instance().startTimer(protocol::editorFinalizeMs, [session] { session->deadline = 0; if (session->checkpoint.empty()) { session->error = {"edit_export_failed", "The editor did not finish exporting in time", {}}; session->helper->terminate("finalize_timeout"); } }, false);
            auto doc = session->op.publicJson(); doc["phase"] = "finalizing"; observe(doc);
        } else if (frame.type == "checkpoint") {
            session->finalizing = true;
            if (frame.payload.contains("code")) session->error = {frame.payload.value("code").toString(), frame.payload.value("message").toString("The edit was not completed"), {}};
            else {
                long long bytes = 0; const auto hash = files::sha256(session->outputPath, &bytes);
                const auto content = files::read(session->outputPath, size_t(session->outputLimit));
                if (bytes != frame.payload.value("bytes").toInteger(-1) || hash != frame.payload.value("sha256").toString() || !content || png::inspect(*content)) session->error = {"edit_export_failed", "The editor's export did not pass the handoff checks", {}};
                else session->checkpoint = frame.payload;
            }
            auto colors = json::toStringVector(frame.payload.value("recentColors").toArray());
            if (!colors.empty() && colors != settingsSnapshot().editorRecentColors && colors.size() <= size_t(Settings::maximumRecentColors)) {
                bool valid = true; for (const auto& c : colors) valid = valid && text::matchesColor(c);
                if (valid) settings_.patch(json::Object{{"preferences", json::Object{{"editorRecentColors", json::stringArray(colors)}}}}, settings_.observedRevision());
            }
            session->helper->send("checkpointAccepted");
        }
    }, [this, session](int code, bool normal, const std::string& reason) {
        if (session->deadline) { Loop::instance().stopTimer(session->deadline); session->deadline = 0; }
        for (auto& pair : session->timers) Loop::instance().stopTimer(pair.second);
        for (auto& pair : session->pending) pair.second({"editor_closed", "The editor closed before answering", {}}, {});
        session->pending.clear(); session->timers.clear();
        if (editor_ == session) editor_.reset();
        if (editingId_ == session->op.id) editingId_.clear();
        auto op = session->op;
        if (session->error || session->checkpoint.empty() || !normal) {
            const Error e = session->error ? session->error : Error{"editor_unavailable", "The editor exited without a result", {{"exitCode", double(code)}, {"reason", reason}}};
            op.cancelled = e.code == "edit_cancelled" || session->cancel->load(); op.state = op.cancelled ? "cancelled" : "failed"; op.error = op.cancelled && e.code == "edit_cancelled" ? Error{} : e;
            files::removeDirectoryRecursively(session->directory);
            finish(op); return;
        }
        Payload payload; payload.kind = "png"; payload.bytes = files::read(session->outputPath, size_t(session->outputLimit)).value_or(std::string());
        files::removeDirectoryRecursively(session->directory);
        if (payload.bytes.empty()) { op.error = {"edit_export_failed", "The exported image could not be read", {}}; op.state = "failed"; finish(op); return; }
        op.state = "acquiring"; op.phase = "acquiring";
        enqueue({std::move(op), std::move(payload), true, session->cancel});
    });
}
void OperationService::editorCommand(const std::string& id, const std::string& command, const json::Object& params, JsonReply reply) {
    if (!editor_ || editingId_ != id) { reply({"editor_not_open", "No editor is open for this operation", {}}, {}); return; }
    auto session = editor_;
    if (!session->editing) { reply({"editor_starting", "The editor is still opening; retry", {}}, {}); return; }
    if (session->finalizing) { reply({"editor_finalizing", "The editor is finishing; no further commands are accepted", {}}, {}); return; }
    if (session->pending.size() >= 16) { reply({"editor_busy", "Too many editor commands are waiting", {}}, {}); return; }
    const auto requestId = uuid::create();
    session->pending[requestId] = reply;
    session->timers[requestId] = Loop::instance().startTimer(protocol::editorCommandMs, [session, requestId] {
        session->timers.erase(requestId);
        const auto it = session->pending.find(requestId);
        if (it == session->pending.end()) return;
        auto late = it->second; session->pending.erase(it);
        late({"editor_timeout", "The editor did not answer in time", {}}, {});
    }, false);
    if (!session->helper->send("command", json::Object{{"requestId", requestId}, {"command", command}, {"params", params}})) {
        Loop::instance().stopTimer(session->timers[requestId]); session->timers.erase(requestId); session->pending.erase(requestId);
        reply({"editor_unavailable", "The editor is not reachable", {}}, {});
    }
}

// ---- the pipeline ----------------------------------------------------------
void OperationService::enqueue(Job job) { queue_.push_back(std::move(job)); pump(); }
void OperationService::pump() {
    const int slots = std::max(1, settingsSnapshot().limits.concurrentUploads);
    while (!queue_.empty() && running_ < slots && !stopping_) {
        auto job = std::make_shared<Job>(std::move(queue_.front())); queue_.pop_front();
        ++running_; notifyActivity();
        auto result = std::make_shared<Operation>();
        Loop::instance().background([this, job, result] { *result = run(*job); }, [this, result] { --running_; notifyActivity(); finish(*result); pump(); });
    }
}
Operation OperationService::run(Job job) {
    auto op = job.op;
    HistoryStore store(root_, paths_db_);
    ArtifactStore artifacts(root_);
    auto save = [&] { if (auto e = store.saveOperation(op, &providers_)) { op.error = e; op.state = "failed"; return false; } observe(op.publicJson()); return true; };
    auto saveItem = [&](const OperationItem& item) {
        if (auto e = store.checkpointOperation(op, {item.artifact.id}, &providers_)) { op.error = e; op.state = "failed"; return false; }
        observe(op.publicJson());
        if (item.linkAvailable()) { auto snapshot = op; auto copy = item; Loop::instance().post([this, snapshot, copy] { uploadLinkReady(snapshot, copy); }); }
        return true;
    };
    if (job.cancelled->load()) { op.cancelled = true; op.state = "cancelled"; save(); return op; }
    const auto settings = Settings::fromValidated(op.settingsDocument);
    const auto route = settings.action(op.action);
    const bool local = route.destination == "local" || route.profileId.empty();
    auto provider = local ? nullptr : providers_.find(op.destination.providerId);
    if (job.acquire) {
        op.phase = "preparing"; if (!save()) return op;
        long long objectLimit = 0; if (provider) objectLimit = provider->capabilities(op.destination).maximumObjectBytes;
        auto acquired = artifacts.acquire(op.id, job.payload, settings, op.action, op.created, objectLimit);
        job.payload = {};
        if (acquired.error) { op.error = acquired.error; op.state = "failed"; save(); log::warning("operations", "Acquisition failed", {{"operationId", op.id}, {"code", op.error.code}}); return op; }
        for (const auto& a : acquired.artifacts) { OperationItem item; item.phase = "stored"; item.artifact = a; item.artifact.previewPath = artifacts.preview(a, settings.previewCacheBytes, settings.limits.imagePixels); item.publicationId = uuid::create(); op.items.push_back(item); }
        op.state = "stored"; if (!save()) return op;
        log::info("operations", "Artifacts acquired", {{"operationId", op.id}, {"items", double(op.items.size())}});
    }
    op.phase = "saving_local";
    for (auto& item : op.items) if ((local || settings.archive.keepUploads) && item.local.status != "saved") {
        item.phase = "saving_local"; observe(op.publicJson());
        item.local = saveLocalCopy(item.artifact, settings.archive); item.phase = "stored";
        if (!saveItem(item)) return op;
    }
    if (local) {
        for (auto& item : op.items) item.uploadStatus = "local_only";
        op.state = "completed"; for (const auto& item : op.items) if (item.local.status != "saved") op.state = "failed";
        save(); return op;
    }
    if (!provider) { op.error = {"provider_unavailable", "Restore access to the original destination", {}}; op.state = "failed"; save(); return op; }
    if (auto e = provider->readiness(op.destination)) { op.error = e; op.state = "failed"; save(); return op; }
    const auto capabilities = provider->capabilities(op.destination);
    auto publicationMode = route.publicationMode; if (publicationMode == "default") publicationMode = provider->defaultPublicationMode(op.destination);
    if (publicationMode == "private" && !capabilities.privateUploads) { op.error = {"unsupported_capability", "This provider cannot perform a private upload", {}}; op.state = "failed"; save(); return op; }
    if (!capabilities.conditionalCreate || (settings.collisionPolicy == "overwrite" && !capabilities.conditionalReplace)) { op.error = {"unsupported_capability", "Provider cannot enforce this collision policy", {}}; op.state = "failed"; save(); return op; }
    bool needsNames = job.acquire; for (const auto& item : op.items) if (!Operation::terminalUpload(item.uploadStatus) && item.candidate.empty()) needsNames = true;
    if (needsNames) {
        std::vector<Artifact> list; for (const auto& item : op.items) list.push_back(item.artifact);
        std::vector<std::string> uuids; for (size_t i = 0; i < list.size() * 2 + 2; ++i) uuids.push_back(uuid::create());
        auto names = naming::batch(list, settings.namingMode, settings.collisionPolicy, uuids, capabilities.maximumNameBytes);
        for (size_t i = 0; i < op.items.size(); ++i) { if (!op.items[i].candidate.empty()) continue; op.items[i].candidate = names[i].name; if (names[i].status != "pending") op.items[i].uploadStatus = names[i].status; }
        if (!save()) return op;
    }
    op.state = "running"; op.phase = "preflight"; if (!save()) return op;
    std::map<std::string, std::vector<size_t>> targets;
    for (size_t index = 0; index < op.items.size(); ++index) {
        auto& item = op.items[index];
        if (Operation::terminalUpload(item.uploadStatus) || item.uploadStatus == "unknown") continue;
        if (job.cancelled->load()) break;
        item.phase = "preflight"; observe(op.publicJson());
        if (auto e = artifacts.verify(item.artifact)) { item.error = e; item.uploadStatus = "failed"; if (!saveItem(item)) return op; continue; }
        if (!item.objectReference.empty() && !item.condition.empty() && item.uploadStatus != "failed") { targets[json::serialize(item.objectReference)].push_back(index); continue; }
        bool checked = false;
        for (int attempts = 0; attempts < 32; ++attempts) {
            ++storageRequests_; auto check = provider->checkName(op.destination, item.candidate, job.cancelled); --storageRequests_;
            if (check.error || check.status == "inconclusive" || check.objectReference.empty()) { item.error = check.error ? check.error : Error{"existence_unknown", "Name availability is inconclusive", {}}; item.uploadStatus = "failed"; break; }
            if (check.status == "present" && settings.collisionPolicy == "skip") { item.uploadStatus = "skipped"; break; }
            if (check.status == "present" && settings.collisionPolicy == "rename") { item.candidate = naming::renamedCandidate(item.candidate, uuid::create(), capabilities.maximumNameBytes); if (!saveItem(item)) return op; continue; }
            item.objectReference = check.objectReference; item.concurrencyToken = check.concurrencyToken; item.condition = check.status == "present" ? "match" : "absent";
            if (item.condition == "match" && item.concurrencyToken.empty()) { item.error = {"precondition_missing", "Provider returned no replacement precondition", {}}; item.uploadStatus = "failed"; break; }
            item.uploadStatus = "stored"; targets[json::serialize(item.objectReference)].push_back(index); checked = true; break;
        }
        if (!checked && item.uploadStatus == "stored") { item.uploadStatus = "failed"; item.error = {"collision_exhausted", "No available candidate after 32 checks", {}}; }
        if (!saveItem(item)) return op;
    }
    for (const auto& group : targets) if (group.second.size() > 1) {
        if (settings.collisionPolicy == "overwrite") for (auto i : group.second) op.items[i].uploadStatus = "conflict";
        else if (settings.collisionPolicy == "skip") for (size_t n = 1; n < group.second.size(); ++n) op.items[group.second[n]].uploadStatus = "skipped";
        else for (size_t n = 1; n < group.second.size(); ++n) { auto& item = op.items[group.second[n]]; item.uploadStatus = "failed"; item.error = {"name_alias_conflict", "Provider candidates resolve to one target", {}}; }
    }
    if (!save()) return op;
    for (size_t index = 0; index < op.items.size(); ++index) {
        auto& item = op.items[index];
        if (Operation::terminalUpload(item.uploadStatus) || item.uploadStatus == "failed") continue;
        if (job.cancelled->load()) { item.uploadStatus = "cancelled"; item.phase = "cancelled"; if (!saveItem(item)) return op; continue; }
        auto lock = objectLock(op.destination, item.objectReference, item.candidate); std::lock_guard guard(*lock);
        UploadRequest request{op.destination, item.artifact, item.candidate, item.publicationId, item.condition, item.objectReference, item.concurrencyToken, op.requestTimeoutMs};
        if (item.uploadStatus == "unknown") {
            item.phase = "reconciling"; observe(op.publicJson());
            ++storageRequests_; auto stat = provider->reconcile(request, job.cancelled); --storageRequests_;
            if (stat.status == "own" && !stat.receipt.validate() && stat.receipt.publicationId == item.publicationId) { item.receipt = stat.receipt; item.uploadStatus = "uploaded"; item.phase = "uploaded"; item.error = {}; if (!saveItem(item)) return op; continue; }
            if (stat.status != "missing") { item.phase = "unknown"; item.error = stat.error ? stat.error : Error{"publication_unknown", "Resolve ambiguous publication before uploading again", {}}; if (!saveItem(item)) return op; continue; }
            item.uploadStatus = "stored";
            if (auto e = artifacts.verify(item.artifact)) { item.uploadStatus = "failed"; item.error = e; saveItem(item); continue; }
        }
        for (int attempts = 0; attempts < 32; ++attempts) {
            item.uploadStatus = "uploading"; item.phase = "uploading"; item.uploadProgress.reset();
            if (!saveItem(item)) return op;
            auto progress = [this, &op, index](const UploadProgress& p) { op.items[index].uploadProgress = p; op.items[index].phase = p.phase; observe(op.publicJson()); };
            ++storageRequests_; notifyActivity();
            request.candidate = item.candidate; request.condition = item.condition; request.objectReference = item.objectReference; request.concurrencyToken = item.concurrencyToken;
            auto result = provider->upload(request, progress, job.cancelled);
            --storageRequests_; notifyActivity();
            item.uploadProgress.reset();
            if (result.status == "uploaded") {
                if (result.receipt.validate() || result.receipt.publicationId != item.publicationId) { item.uploadStatus = "unknown"; item.phase = "unknown"; item.error = {"receipt_invalid", "Provider returned an unusable receipt", {}}; break; }
                item.receipt = result.receipt; item.uploadStatus = "uploaded"; item.phase = "uploaded"; item.error = {}; break;
            }
            if (result.status == "collision") {
                if (settings.collisionPolicy == "skip") { item.uploadStatus = "skipped"; item.phase = "skipped"; break; }
                if (settings.collisionPolicy == "rename") {
                    item.candidate = naming::renamedCandidate(item.candidate, uuid::create(), capabilities.maximumNameBytes);
                    ++storageRequests_; auto check = provider->checkName(op.destination, item.candidate, job.cancelled); --storageRequests_;
                    if (check.error || check.status != "absent") { item.uploadStatus = "failed"; item.phase = "failed"; item.error = check.error ? check.error : Error{"collision_exhausted", "No available candidate after the collision", {}}; break; }
                    item.objectReference = check.objectReference; item.concurrencyToken = {}; item.condition = "absent"; continue;
                }
                item.uploadStatus = "failed"; item.phase = "failed"; item.error = result.error ? result.error : Error{"collision", "The object changed before the overwrite", {}}; break;
            }
            item.uploadStatus = result.status == "unknown" ? "unknown" : "failed"; item.phase = item.uploadStatus; item.error = result.error; break;
        }
        if (!saveItem(item)) return op;
    }
    int uploaded = 0, pending = 0, failed = 0;
    for (const auto& item : op.items) { if (item.uploadStatus == "uploaded" || item.uploadStatus == "skipped" || item.uploadStatus == "local_only") ++uploaded; else if (item.uploadStatus == "unknown" || item.uploadStatus == "stored" || item.uploadStatus == "uploading") ++pending; else ++failed; }
    if (job.cancelled->load() && pending + failed > 0) { op.cancelled = true; op.state = "cancelled"; }
    else if (failed == 0 && pending == 0) op.state = "completed";
    else if (uploaded > 0) { op.state = "partial"; op.error = {"partial_upload", "Some items did not upload; use Retry", {}}; }
    else { op.state = "failed"; if (!op.error) { for (const auto& item : op.items) if (item.error) { op.error = item.error; break; } } }
    save();
    return op;
}
void OperationService::releaseActive(const std::string& id) { active_.erase(id); notifyActivity(); if (active_.empty()) scheduleHeapReclamation(); }
void OperationService::finish(Operation op, JsonReply reply) {
    if (editingId_ == op.id) editingId_.clear();
    if (selectingId_ == op.id) selectingId_.clear();
    op.phase = op.state;
    if ((op.cancelled || op.state == "failed" || op.state == "interrupted") && op.copyStatus == "pending") op.copyStatus = "no_result";
    performCopy(op, false, {});
    background([this, op](Error& e) { HistoryStore store(root_, paths_db_); e = store.saveOperation(op, &providers_); return e ? json::Object{} : store.operation(op.id, &e).toJson(); }, [this, id = op.id, reply](Error e, json::Object document) {
        clearObservation(id);
        if (onChanged) onChanged(id);
        if (onObservationsChanged) onObservationsChanged();
        uploadCopies_.erase(id);
        if (e) { log::error("history", "Cannot record the operation result", {{"operationId", id}, {"code", e.code}}); releaseActive(id); if (reply) reply(e, {}); return; }
        const auto finished = Operation::fromJson(document);
        log::info("operations", "Operation finished", {{"operationId", finished.id}, {"action", finished.action}, {"state", finished.state}, {"items", double(finished.items.size())}, {"error", finished.error.code}});
        if (onResultReady) onResultReady(finished);
        if (finished.error && onOperationError) onOperationError(finished.error);
        releaseActive(id);
        if (reply) reply({}, finished.publicJson());
    });
}

// ---- copying results --------------------------------------------------------
unsigned long long OperationService::invalidateCopyEligibility() { return ++copyEpoch_; }
void OperationService::clipboardChangedByUser(unsigned long long) { /* eligibility compares live revisions; nothing to record */ }
void OperationService::copyText(const std::string& value, unsigned long long expected, bool automatic, std::function<void(Error)> done) {
    if (automatic && clipboard_.revision() != expected && clipboard_.lastCopyRevision() != clipboard_.revision()) { done({"copy_suppressed", "A user clipboard change, cancellation or explicit copy superseded automatic copying", {}}); return; }
    unsigned long long revision = 0;
    done(clipboard_.writeText(value, revision));
}
void OperationService::uploadLinkReady(const Operation& op, const OperationItem& item) {
    auto it = uploadCopies_.find(op.id);
    if (it == uploadCopies_.end()) return;
    auto& copy = it->second;
    int index = 0; for (const auto& i : op.items) { if (i.artifact.id == item.artifact.id) break; ++index; }
    copy.links.emplace_back(index, item.receipt.directUrl);
    if (copy.mode != "each_link") return;
    if (copy.epoch != copyEpoch_) { copy.suppressed = true; return; }
    copyText(item.receipt.directUrl, copy.clipboardRevision, true, [&copy, this](Error e) {
        if (!e) { copy.copied = true; ++copy.copiedCount; copy.clipboardRevision = clipboard_.revision(); }
        else if (e.code == "copy_suppressed") copy.suppressed = true; else copy.failed = true;
    });
}
void OperationService::performCopy(Operation& op, bool explicitCopy, JsonReply reply) {
    auto it = uploadCopies_.find(op.id);
    if (it != uploadCopies_.end()) {
        auto& copy = it->second;
        std::sort(copy.links.begin(), copy.links.end());
        if (copy.mode == "list" && !copy.links.empty()) {
            std::string joined; for (const auto& link : copy.links) { if (!joined.empty()) joined += "\n"; joined += link.second; }
            copyText(joined, copy.clipboardRevision, !explicitCopy, [&copy](Error e) { if (!e) copy.copied = true; else if (e.code == "copy_suppressed") copy.suppressed = true; else copy.failed = true; });
        }
        if (copy.copied && (copy.failed || copy.suppressed || (copy.mode == "each_link" && copy.copiedCount < int(copy.links.size())))) op.copyStatus = "partial";
        else if (copy.copied) op.copyStatus = "copied";
        else if (copy.links.empty()) op.copyStatus = "no_result";
        else if (copy.failed) op.copyStatus = "failed";
        else if (copy.suppressed) op.copyStatus = "suppressed";
        else op.copyStatus = "no_result";
        if (reply) reply({}, {{"operationId", op.id}, {"copyStatus", op.copyStatus}});
        return;
    }
    // Local-only results: offer the file itself (image bytes, text or the saved files).
    if (op.state != "completed" || op.items.empty()) { if (reply) reply({}, {{"operationId", op.id}, {"copyStatus", op.copyStatus}}); return; }
    std::vector<std::string> saved; for (const auto& item : op.items) if (item.local.status == "saved") saved.push_back(item.local.path);
    const auto& first = op.items.front();
    Error e;
    unsigned long long revision = 0;
    if (op.items.size() == 1 && first.artifact.mime == "text/plain" && first.artifact.bytes <= 16 * 1024 * 1024) { const auto bytes = files::read(first.artifact.path, 16 * 1024 * 1024); if (bytes) e = clipboard_.writeText(*bytes, revision); else e = {"copy_failed", "The retained text is unavailable", {}}; }
    else if (op.items.size() == 1 && first.artifact.mime == "image/png" && first.artifact.bytes <= 32 * 1024 * 1024) {
        const auto png = files::read(first.artifact.path, 32 * 1024 * 1024);
        const auto dibPath = paths::join(paths::join(root_, "capture"), "dib-" + uuid::create() + ".bin");
        std::string dib;
        if (png && helpers::prepareOutputFile(dibPath)) {
            const auto run = helpers::runSynchronous("image-job", {"--image-job"}, json::Object{{"input", json::Object{{"path", first.artifact.path}, {"kind", "png"}}}, {"output", json::Object{{"path", dibPath}, {"kind", "dib"}}},
                {"limits", json::Object{{"imagePixels", 100000000.0}, {"imageBytes", 536870912.0}}}, {"reencode", true}}, 10000, 60000, {});
            if (!run.error) dib = files::read(dibPath, 512 * 1024 * 1024).value_or(std::string());
            files::remove(dibPath);
        }
        e = png ? clipboard_.writeImage(*png, dib, revision) : Error{"copy_failed", "The retained image is unavailable", {}};
    } else if (!saved.empty()) e = clipboard_.writeFiles(saved, revision);
    else e = {"copy_unavailable", "Nothing from this operation can be copied", {}};
    op.copyStatus = e ? (e.code == "copy_unavailable" ? "no_result" : "failed") : "copied";
    if (reply) reply(e, {{"operationId", op.id}, {"copyStatus", op.copyStatus}});
}
void OperationService::copyResult(const std::string& id, JsonReply reply) {
    if (!ready_) { reply(notReady(), {}); return; }
    invalidateCopyEligibility();
    background([this, id](Error& e) { HistoryStore store(root_, paths_db_); return store.operation(id, &e).toJson(); }, [this, reply](Error e, json::Object doc) {
        if (e) { reply(e, {}); return; }
        auto op = Operation::fromJson(doc);
        std::vector<std::string> links; for (const auto& item : op.items) if (item.linkAvailable()) links.push_back(item.receipt.directUrl);
        if (!links.empty()) {
            unsigned long long revision = 0;
            const auto error = clipboard_.writeText(text::join(links, "\n"), revision);
            op.copyStatus = error ? "failed" : "copied";
            background([this, op](Error& err) { HistoryStore store(root_, paths_db_); err = store.saveOperation(op, &providers_); return json::Object{}; }, [reply, error, op](Error, json::Object) { reply(error, {{"operationId", op.id}, {"copyStatus", op.copyStatus}, {"copied", !error}}); });
            return;
        }
        performCopy(op, true, [this, reply, op](Error error, json::Object result) mutable {
            background([this, op](Error& err) { HistoryStore store(root_, paths_db_); err = store.saveOperation(op, &providers_); return json::Object{}; }, [reply, error, result](Error, json::Object) { auto r = result; r["copied"] = !error; reply(error, r); });
        });
    });
}
void OperationService::copyHistoryUrl(const std::string& url, JsonReply reply) {
    unsigned long long revision = 0;
    const auto e = clipboard_.writeText(url, revision);
    reply(e, {{"copied", !e}});
}
void OperationService::copyHistoryImage(const std::string& historyId, JsonReply reply) {
    background([this, historyId](Error& e) {
        HistoryStore store(root_, paths_db_);
        auto data = store.detail(historyId, &e);
        if (e) return json::Object{};
        if (data.value("mime") != "image/png" && data.value("mime") != "image/jpeg") { e = {"copy_unavailable", "Only retained images can be copied", {}}; return json::Object{}; }
        std::string path = paths::join(paths::join(paths::join(root_, "artifacts"), data.value("operationId").toString()), historyId);
        if (!files::isRegularFile(path) || files::sha256(path) != data.value("sha256").toString()) {
            const auto local = data.value("local").toObject();
            if (local.value("status") == "saved" && files::isRegularFile(local.value("path").toString()) && files::sha256(local.value("path").toString()) == data.value("sha256").toString()) path = local.value("path").toString();
            else { e = {"artifact_missing", "The original image is no longer available unchanged", {}}; return json::Object{}; }
        }
        if (files::size(path) > 32 * 1024 * 1024) { e = {"input_excessive", "The image exceeds the 32 MiB copy limit", {}}; return json::Object{}; }
        const auto pngPath = paths::join(paths::join(root_, "capture"), "png-" + uuid::create() + ".bin"), dibPath = paths::join(paths::join(root_, "capture"), "dib-" + uuid::create() + ".bin");
        helpers::prepareOutputFile(pngPath); helpers::prepareOutputFile(dibPath);
        auto run = helpers::runSynchronous("image-job", {"--image-job"}, json::Object{{"input", json::Object{{"path", path}, {"kind", data.value("mime") == "image/png" ? "png" : "file"}}}, {"output", json::Object{{"path", pngPath}, {"kind", "png"}}},
            {"limits", json::Object{{"imagePixels", 16000000.0}, {"imageBytes", 33554432.0}}}, {"reencode", data.value("mime") != "image/png"}}, 10000, 60000, {});
        json::Object out;
        if (!run.error) { run = helpers::runSynchronous("image-job", {"--image-job"}, json::Object{{"input", json::Object{{"path", pngPath}, {"kind", "png"}}}, {"output", json::Object{{"path", dibPath}, {"kind", "dib"}}},
            {"limits", json::Object{{"imagePixels", 16000000.0}, {"imageBytes", 268435456.0}}}, {"reencode", true}}, 10000, 60000, {}); }
        if (run.error) e = run.error; else { out["png"] = files::read(pngPath, 32 * 1024 * 1024).value_or(std::string()); out["dib"] = files::read(dibPath, 256 * 1024 * 1024).value_or(std::string()); }
        files::remove(pngPath); files::remove(dibPath);
        return out;
    }, [this, reply](Error e, json::Object out) {
        if (e) { reply(e, {}); return; }
        invalidateCopyEligibility();
        unsigned long long revision = 0;
        const auto error = clipboard_.writeImage(out.value("png").toString(), out.value("dib").toString(), revision);
        reply(error, {{"copied", !error}});
    });
}

// ---- inspection, cancellation, retries ------------------------------------
void OperationService::recent(JsonReply reply) {
    const int limit = settingsSnapshot().recentActivityLimit;
    background([this, limit](Error& e) { HistoryStore store(root_, paths_db_); return json::Object{{"operations", store.recent(limit, &e)}}; }, [this, reply](Error e, json::Object data) {
        auto values = data.value("operations").toArray();
        for (auto& v : values) v = currentObservation(v.toObject());
        data["operations"] = values;
        reply(e, data);
    });
}
void OperationService::inspect(const std::string& id, JsonReply reply) {
    background([this, id](Error& e) { HistoryStore store(root_, paths_db_); return store.operation(id, &e).publicJson(); }, [this, reply](Error e, json::Object data) { reply(e, currentObservation(data)); });
}
void OperationService::cancel(const std::string& id, const std::string& itemId, JsonReply reply) {
    const auto it = active_.find(id);
    if (it == active_.end()) { reply({"operation_not_active", "Operation is not active", {}}, {}); return; }
    if (!itemId.empty()) { reply({"invalid_request", "Per-item cancellation is not available in the Windows build; cancel the operation", {}}, {}); return; }
    it->second->store(true);
    if (capture_ && capture_->op.id == id) capture_->helper->send("cancel");
    if (editor_ && editingId_ == id) editorCommand(id, "cancel", {}, [](Error, json::Object) {});
    reply({}, {{"operationId", id}, {"cancellationRequested", true}});
}
void OperationService::retry(const std::string& id, JsonReply reply) {
    if (cleanupInProgress_) { reply({"cleanup_busy", "Wait for artifact cleanup to finish", {}}, {}); return; }
    if (!ready_) { reply(startupError_ ? startupError_ : notReady(), {}); return; }
    const auto limits = settingsSnapshot().limits;
    if (int(active_.size()) >= limits.pendingOperations + limits.concurrentUploads) { reply({"queue_full", "Pending operation limit reached", {}}, {}); return; }
    if (active_.count(id)) { reply({"operation_busy", "Operation is already active", {}}, {}); return; }
    auto reserved = std::make_shared<std::atomic_bool>(false);
    active_[id] = reserved;
    background([this, id](Error& e) { HistoryStore store(root_, paths_db_); return store.operation(id, &e).toJson(); }, [this, reply, id, reserved](Error e, json::Object doc) {
        if (e) { active_.erase(id); reply(e, {}); return; }
        auto op = Operation::fromJson(doc);
        if (op.items.empty()) { active_.erase(id); reply({"artifact_missing", "No acquired artifacts; invoke a new action", {}}, {}); return; }
        bool pending = false; for (const auto& i : op.items) if (!Operation::terminalUpload(i.uploadStatus)) pending = true;
        if (!pending) { active_.erase(id); performCopy(op, true, [this, reply, op](Error error, json::Object result) mutable { background([this, op](Error& err) { HistoryStore store(root_, paths_db_); err = store.saveOperation(op, &providers_); return json::Object{}; }, [reply, error, result](Error, json::Object) { reply(error, result); }); }); return; }
        op.cancelled = false; op.error = {}; op.state = "stored"; op.phase = "queued"; op.invocation = ++invocation_; op.clipboardRevision = clipboard_.revision(); op.copyStatus = "pending";
        for (auto& item : op.items) if (item.uploadStatus == "failed" || item.uploadStatus == "cancelled") { item.uploadStatus = "stored"; item.error = {}; }
        if (!op.destination.profileId.empty()) { UploadCopy copy; copy.mode = settingsSnapshot().uploadClipboardBehavior; copy.clipboardRevision = op.clipboardRevision; copy.epoch = copyEpoch_; uploadCopies_[op.id] = copy; }
        background([this, op](Error& err) { HistoryStore store(root_, paths_db_); err = store.saveOperation(op, &providers_); return json::Object{}; }, [this, reply, op, reserved](Error err, json::Object) mutable {
            if (err) { uploadCopies_.erase(op.id); releaseActive(op.id); reply(err, {}); return; }
            enqueue({op, {}, false, reserved});
            reply({}, {{"operationId", op.id}, {"retryAccepted", true}});
        });
    });
}
void OperationService::retryLocalSave(const std::string& id, JsonReply reply) {
    if (active_.count(id)) { reply({"operation_busy", "Operation is already active", {}}, {}); return; }
    background([this, id](Error& e) {
        HistoryStore store(root_, paths_db_);
        auto op = store.operation(id, &e); if (e) return json::Object{};
        const auto settings = Settings::fromValidated(op.settingsDocument);
        ArtifactStore artifacts(root_);
        int saved = 0, failed = 0;
        for (auto& item : op.items) {
            if (item.local.status == "saved") continue;
            if (auto verify = artifacts.verify(item.artifact)) { item.local.status = "failed"; item.local.error = verify; ++failed; continue; }
            item.local = saveLocalCopy(item.artifact, settings.archive);
            if (item.local.status == "saved") ++saved; else ++failed;
        }
        if (op.state == "failed" && failed == 0) { bool allLocal = true; for (const auto& item : op.items) if (item.uploadStatus != "local_only") allLocal = false; if (allLocal) op.state = "completed"; }
        e = store.saveOperation(op, &providers_);
        return json::Object{{"operationId", id}, {"saved", saved}, {"failed", failed}, {"state", op.state}};
    }, [this, reply, id](Error e, json::Object result) { if (onChanged) onChanged(id); reply(e, result); });
}
void OperationService::cleanup(bool automatic, JsonReply reply) {
    if (cleanupInProgress_) { reply({"cleanup_busy", "Wait for artifact cleanup to finish", {}}, {}); return; }
    cleanupInProgress_ = true;
    const auto snapshot = settingsSnapshot();
    std::set<std::string> activeIds; for (const auto& pair : active_) activeIds.insert(pair.first);
    if (capture_) activeIds.insert(capture_->op.id);
    if (editor_) activeIds.insert(editor_->op.id);
    const auto start = GetTickCount64();
    background([this, snapshot, activeIds, start, automatic](Error& e) {
        HistoryStore store(root_, paths_db_);
        ArtifactStore artifacts(root_);
        int removed = 0;
        const long long now = files::parseIsoUtcMs(files::nowIsoUtc());
        for (const auto& name : files::list(artifacts.root())) {
            if (name.empty() || name[0] == '.' || activeIds.count(name)) continue;
            Error lookup; const auto op = store.operation(name, &lookup);
            bool eligible = false;
            if (lookup) eligible = true; // no journal record: orphaned retained artifacts
            else if (Operation::activeState(op.state)) continue;
            else if (op.state == "completed") eligible = true;
            else { const auto age = now - files::parseIsoUtcMs(op.created); eligible = snapshot.failedArtifactDays > 0 && age > (long long)snapshot.failedArtifactDays * 86400000LL; }
            if (eligible && !artifacts.remove(name)) ++removed;
        }
        if (snapshot.previewCacheBytes <= 0) artifacts.clearPreviews();
        (void)e;
        return json::Object{{"removedOperations", removed}, {"abortedUploads", 0}, {"incompleteUploads", json::Array{}}, {"trigger", automatic ? "automatic" : "manual"}, {"elapsedMs", double(GetTickCount64() - start)}, {"finished", true}};
    }, [this, reply](Error e, json::Object result) { cleanupInProgress_ = false; lastCleanup_ = result; reply(e, result); });
}

// ---- capabilities and diagnostics ------------------------------------------
json::Object OperationService::capabilities() const {
    json::Object source{{"adapter", "win32"}, {"capture", json::Object{{"region", true}, {"window", true}, {"desktop", true}}},
        {"clipboard", json::Object{{"available", true}, {"import", json::Array{"text/uri-list", "image/png", "text/plain"}}, {"result", json::Array{"text/plain", "image/png", "text/uri-list"}}}},
        {"shortcuts", json::Object{{"available", true}}}};
    json::Object editor{{"available", true}, {"admissionPixels", double(editorAdmissionPixels)}, {"open", !editingId_.empty()}, {"sources", json::Array{"region", "window", "desktop", "clipboard", "file"}},
        {"fileFormats", json::Array{"png", "jpeg"}}, {"output", "png"}, {"shapes", json::Array{"redact", "blur", "arrow", "rectangle", "text", "highlight"}},
        {"commands", json::Array{"inspect", "apply", "preview", "saveCopy", "submit", "cancel", "raise"}}};
    return {{"source", source}, {"providers", providers_.capabilities()}, {"editor", editor}, {"recording", json::Object{{"available", false}, {"reason", "not_available_on_windows"}}}};
}
json::Object OperationService::diagnostics() const {
    return {{"schemaVersion", 1}, {"build", protocol::buildIdentity()}, {"backend", "win32"}, {"platform", "windows"}, {"activeOperations", double(active_.size())}, {"journalReady", ready_},
        {"capabilities", capabilities()}, {"storage", providers_.diagnostics()}, {"artifactCleanup", json::Object{{"running", cleanupInProgress_}, {"automatic", true}, {"intervalMs", 3600000}, {"last", lastCleanup_}}},
        {"memory", json::Object{{"allocatorReclamationSupported", true}, {"allocatorReclamationPending", heapPending_}, {"allocatorReclamationCount", double(heapReclamations_)}, {"lastAllocatorReclamationUsec", double(lastHeapReclamationUsec_)}}}};
}
void OperationService::shutdown() {
    if (stopping_) return;
    stopping_ = true;
    for (auto& pair : active_) pair.second->store(true);
    if (capture_) capture_->helper->terminate("shutdown");
    if (editor_) editor_->helper->terminate("shutdown");
    if (cleanupTimer_) Loop::instance().stopTimer(cleanupTimer_);
    if (heapTimer_) Loop::instance().stopTimer(heapTimer_);
}
}
