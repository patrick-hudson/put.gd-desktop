#pragma once
// The shared operation engine (docs/architecture.md): acceptance, acquisition
// through the capture/image helpers or the clipboard, immutable artifacts,
// naming, local archive, publication through a provider, explicit retries,
// result copying and the live observations the CLI/UI read. Owner-thread API;
// provider and file work runs on background threads.
#include "resident/Artifacts.h"
#include "resident/Clipboard.h"
#include "resident/Helpers.h"
#include "resident/HistoryStore.h"
#include "resident/Provider.h"
#include "resident/SettingsService.h"
#include <atomic>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <set>

namespace putgd {
using JsonReply = std::function<void(Error, json::Object)>;
class OperationService {
public:
    OperationService(std::string dataRoot, SettingsService&, ProviderRegistry&, Clipboard&);
    ~OperationService();
    // Journal ownership, recovery and staging cleanup; `done` runs once ready.
    void initialize(std::function<void()> done);
    bool ready() const { return ready_; }
    void setUiProcessId(unsigned long pid) { uiPid_ = pid; }
    // `edit` accepts options {source: region|window|desktop|clipboard|file, path}.
    void invoke(const std::string& action, const json::Object& options, JsonReply);
    void editorCommand(const std::string& id, const std::string& command, const json::Object& params, JsonReply);
    std::string editingOperation() const { return editingId_; }
    void inspect(const std::string& id, JsonReply);
    void recent(JsonReply);
    json::Array activeObservations() const;
    void cancel(const std::string& id, const std::string& itemId, JsonReply);
    void retry(const std::string& id, JsonReply);
    void retryLocalSave(const std::string& id, JsonReply);
    void copyResult(const std::string& id, JsonReply);
    void copyHistoryImage(const std::string& historyId, JsonReply);
    void copyHistoryUrl(const std::string& url, JsonReply);
    void cleanup(bool automatic, JsonReply);
    json::Object capabilities() const;
    json::Object diagnostics() const;
    std::string databasePath() const { return paths_db_; }
    std::string dataRoot() const { return root_; }
    ProviderRegistry& providers() { return providers_; }
    Settings settingsSnapshot() const { return settings_.snapshot(); }
    bool storageActive() const { return storageRequests_ > 0; }
    bool idle() const;
    void shutdown();
    // Run work on a background thread and the reply on the owner thread.
    void background(std::function<json::Object(Error&)>, JsonReply);
    // Serializes provider work on one destination object across operations and deletions.
    std::shared_ptr<std::mutex> objectLock(const Destination&, const json::Object& objectReference, const std::string& candidate);
    // Observers (owner thread).
    std::function<void(const std::string& id)> onChanged;
    std::function<void(const Operation&)> onResultReady; // terminal or copy result durable
    std::function<void(const Error&)> onOperationError;
    // An automatic cleanup could not abort uploads it reported for the first time (#226).
    std::function<void(const json::Object& notice)> onIncompleteUploadsKept;
    std::function<void()> onObservationsChanged;
    std::function<void(bool active, bool uploading)> onActivityChanged;
    std::function<json::Object()> shortcutStatus; // Hotkeys::status for capabilities
    unsigned long long invalidateCopyEligibility();
    // Explicit allocator/working-set trim after two quiet seconds (diagnostics.memory).
    void scheduleHeapReclamation();
private:
    struct Job { Operation op; Payload payload; bool acquire = false; CancelFlag cancelled; };
    struct CaptureSession;
    struct EditorSession;
    void acceptOperation(Operation op, const std::string& source, const std::string& sourcePath, bool upload, JsonReply);
    void acquireSource(Operation op, const std::string& source, const std::string& sourcePath, CancelFlag);
    void startCapture(Operation op, const std::string& mode, CancelFlag);
    void startEditing(Operation op, Payload input, CancelFlag);
    void enqueue(Job);
    void pump();
    Operation run(Job);
    void finish(Operation, JsonReply = {});
    void releaseActive(const std::string& id);
    void observe(const json::Object& publicJson);
    void clearObservation(const std::string& id);
    json::Object currentObservation(json::Object document) const;
    void performCopy(Operation&, bool explicitCopy, JsonReply);
    void uploadLinkReady(const Operation&, const OperationItem&);
    void copyText(const std::string& text, unsigned long long expectedRevision, bool automatic, std::function<void(Error)>);
    std::string routeDestination(const Operation&) const;
    void expireArtifacts(bool automatic, JsonReply);
    // Cleanup (background thread): abort retired items' journaled uploads, then word the notice.
    void abandonUploads(HistoryStore&, Operation&, json::Object& outcome);
    static void describeIncompleteUploads(json::Object& outcome, const Settings&, bool automatic);
    void notifyActivity();
    std::string root_, paths_db_;
    SettingsService& settings_;
    ProviderRegistry& providers_;
    Clipboard& clipboard_;
    ArtifactStore artifacts_;
    std::deque<Job> queue_;
    std::map<std::string, CancelFlag> active_;
    std::map<std::string, std::weak_ptr<std::mutex>> locks_;
    std::mutex locksMutex_;
    mutable std::mutex observationsMutex_;
    std::map<std::string, json::Object> observations_;
    int running_ = 0;
    std::atomic<int> storageRequests_{0};
    unsigned long long invocation_ = 0, copyEpoch_ = 0;
    std::string selectingId_, editingId_;
    std::unique_ptr<CaptureSession> capture_;
    std::shared_ptr<EditorSession> editor_;
    bool ready_ = false, stopping_ = false, cleanupInProgress_ = false;
    Error startupError_;
    unsigned long uiPid_ = 0;
    UINT_PTR heapTimer_ = 0, cleanupTimer_ = 0;
    unsigned long long heapReclamations_ = 0; long long lastHeapReclamationUsec_ = 0; bool heapPending_ = false;
    json::Object lastCleanup_, lastIncompleteNotice_;
    // Per-operation upload-copy bookkeeping (docs/architecture.md, issue #32 policy).
    struct UploadCopy { std::vector<std::pair<int, std::string>> links; unsigned long long clipboardRevision = 0; unsigned long long epoch = 0; bool copied = false, suppressed = false, failed = false; std::string mode; int copiedCount = 0; };
    std::map<std::string, UploadCopy> uploadCopies_;
    friend struct CaptureSession;
    friend struct EditorSession;
};
}
