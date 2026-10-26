#pragma once
// The UI helper's client of the resident session pipe (docs/ui-process-protocol.md,
// "Command transport"): one pipe::request per call on a worker thread, the reply
// marshalled to the UI thread with PostMessage, a per-method deadline and no
// automatic retries. It also caches the authoritative settings document,
// metadata (observedRevision), build identity and diagnostics from ui.snapshot.
#include "common/Error.h"
#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <windows.h>

namespace putgd::ui {
inline constexpr UINT sessionReplyMessage = WM_APP + 10;
inline constexpr int defaultDeadlineMs = 15000;

struct Reply {
    bool ok = false;
    json::Object result;
    Error error;
    // The request may have reached the resident: inspect before repeating it.
    bool outcomeUnknown = false;
};
using Callback = std::function<void(const Reply&)>;

class UiSession {
public:
    UiSession() = default;
    UiSession(const UiSession&) = delete;
    ~UiSession();
    void setPipe(const std::string& path) { pipe_ = path; }
    const std::string& pipe() const { return pipe_; }
    // Replies are posted to `window` as sessionReplyMessage; call deliver() from its procedure.
    void attach(HWND window);
    // Stops delivering replies (final close or exit); accepted work continues in the resident.
    void detach();
    // Asynchronous call; the callback runs on the UI thread at most once.
    void call(const std::string& method, const json::Object& params, Callback callback, int deadlineMs = 0);
    // Synchronous call for bootstrap before the window exists.
    Reply callNow(const std::string& method, const json::Object& params, int deadlineMs = 0);
    void deliver(LPARAM);
    static int deadlineFor(const std::string& method);
    // Fixed English guidance for a stable error code; the resident's message may follow it.
    static std::wstring guidance(const Error&);
    static std::wstring describe(const Reply&);

    // Settings cache (settings.get / ui.snapshot / settings.patch results).
    const json::Object& document() const { return document_; }
    const json::Object& metadata() const { return metadata_; }
    std::string revision() const { return metadata_.value("observedRevision").toString(); }
    void storeSettings(const json::Object& document, const json::Object& metadata);
    void storeMetadata(const json::Object& metadata);
    void storeSnapshot(const json::Object& snapshot);
    const json::Object& build() const { return build_; }
    const json::Object& diagnostics() const { return diagnostics_; }
    void storeDiagnostics(const json::Object& diagnostics) { diagnostics_ = diagnostics; }
    std::string profileLabel(const std::string& profileId) const;
private:
    struct Pending;
    std::string pipe_;
    struct Target { std::atomic<HWND> window{nullptr}; };
    std::shared_ptr<Target> target_ = std::make_shared<Target>();
    json::Object document_, metadata_, build_, diagnostics_;
};
}
