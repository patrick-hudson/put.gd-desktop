#include "ui/UiSession.h"
#include "common/Pipe.h"
#include "common/Paths.h"
#include "common/Uuid.h"
#include <map>
#include <thread>

namespace putgd::ui {
struct UiSession::Pending {
    Callback callback;
    Reply reply;
};

namespace {
Reply exchange(const std::string& pipePath, const std::string& method, const json::Object& params, int deadlineMs) {
    Reply reply;
    const auto requestId = uuid::create();
    std::string line = json::serialize(json::Object{{"schemaVersion", 1}, {"requestId", requestId}, {"session", pipePath}, {"method", method}, {"params", params}});
    std::string transport;
    auto answer = pipe::request(pipePath, line, deadlineMs, &transport);
    // Credential input must not linger in this buffer longer than the exchange.
    SecureZeroMemory(line.data(), line.size());
    if (!answer) {
        const auto code = transport.empty() ? std::string("transport_failed") : transport;
        reply.outcomeUnknown = code != "session_unavailable";
        reply.error = Error(code, code == "session_unavailable" ? "put.gd is not running" : "No complete reply from put.gd",
                            reply.outcomeUnknown ? json::Object{{"outcomeUnknown", true}} : json::Object{});
        return reply;
    }
    const auto object = json::parseObject(*answer);
    if (!object || !json::integer(object->value("schemaVersion"), 1, 1) || object->value("requestId") != requestId.c_str() || !object->value("ok").isBool()) {
        reply.outcomeUnknown = true;
        reply.error = Error("invalid_response", "The reply from put.gd was not understood", json::Object{{"outcomeUnknown", true}});
        return reply;
    }
    reply.ok = object->value("ok").toBool();
    if (reply.ok) reply.result = object->value("result").toObject();
    else {
        reply.error = Error::fromJson(object->value("error").toObject());
        if (reply.error.code.empty()) reply.error.code = "request_failed";
    }
    return reply;
}
}

UiSession::~UiSession() { detach(); }
void UiSession::attach(HWND window) { target_->window.store(window); }
void UiSession::detach() { target_->window.store(nullptr); }

int UiSession::deadlineFor(const std::string& method) {
    if (method == "update.check") return 60000;
    if (method == "diagnostics.bundle" || method == "diagnostics.uploadTarget") return 610000;
    if (method == "history.preview") return 50000;
    return defaultDeadlineMs;
}

void UiSession::call(const std::string& method, const json::Object& params, Callback callback, int deadlineMs) {
    if (!deadlineMs) deadlineMs = deadlineFor(method);
    auto* pending = new Pending{std::move(callback), {}};
    std::thread([pending, target = target_, pipePath = pipe_, method, params, deadlineMs] {
        pending->reply = exchange(pipePath, method, params, deadlineMs);
        HWND window = target->window.load();
        if (!window || !PostMessageW(window, sessionReplyMessage, 0, reinterpret_cast<LPARAM>(pending))) delete pending;
    }).detach();
}

Reply UiSession::callNow(const std::string& method, const json::Object& params, int deadlineMs) {
    return exchange(pipe_, method, params, deadlineMs ? deadlineMs : deadlineFor(method));
}

void UiSession::deliver(LPARAM data) {
    std::unique_ptr<Pending> pending(reinterpret_cast<Pending*>(data));
    if (!pending || !target_->window.load() || !pending->callback) return;
    pending->callback(pending->reply);
}

std::wstring UiSession::guidance(const Error& error) {
    static const std::map<std::string, std::wstring> fixed{
        {"session_unavailable", L"put.gd is not running. Start put.gd and try again."},
        {"transport_failed", L"put.gd did not answer in time. The request may still finish; check History before trying again."},
        {"invalid_response", L"put.gd sent a reply this window does not understand. Restart put.gd after pending uploads finish."},
        {"response_too_large", L"The reply from put.gd was too large."},
        {"config_conflict", L"Settings changed elsewhere. Reload to continue."},
        {"config_invalid", L"A setting is not valid. Correct the highlighted field."},
        {"config_busy", L"Settings are being saved by another program. Try again."},
        {"config_unwritable", L"The settings file could not be written. Check the configuration folder."},
        {"settings_not_saved", L"Settings were not saved. Try again."},
        {"s3_settings_invalid", L"Check the bucket, region, endpoint, link domain and timeouts."},
        {"output_required", L"No output is enabled. Choose a destination in Settings > Capture or turn on Keep a local copy."},
        {"credential_setup_required", L"Store or unlock credentials for this destination first."},
        {"credentials_not_stored", L"No saved credentials were found for this destination. Store them first."},
        {"credential_store_unavailable", L"Windows Credential Manager is not available."},
        {"credential_store_locked", L"Windows Credential Manager is locked."},
        {"profile_order_invalid", L"The destination order changed. Reload and try again."},
        {"queue_full", L"Too many actions are waiting. Wait for current work to finish."},
        {"busy_selecting", L"A capture is already selecting. Finish or cancel it first."},
        {"source_unavailable", L"Nothing usable was found to capture or upload."},
        {"source_unsupported", L"This content cannot be uploaded."},
        {"input_excessive", L"The input is larger than the configured limits."},
        {"disk_space_insufficient", L"Not enough free disk space."},
        {"storage_unavailable", L"The history store is not available yet."},
        {"delete_confirmation_changed", L"The deletion targets changed. Review them again."},
        {"delete_target_changed", L"A deletion target changed. Review it again."},
        {"link_unavailable", L"This link was removed or has expired."},
        {"link_not_opened", L"No browser could open the link."},
        {"unsupported_capability", L"This is not available on Windows."},
        {"provider_unavailable", L"This destination type is not available."},
        {"update_feed_unavailable", L"The update feed could not be reached."},
        {"update_feed_invalid", L"The update feed was not understood."},
        {"update_check_failed", L"The update check failed."},
        {"update_check_unavailable", L"This build does not check for updates."},
        {"setup_not_found", L"This setup result is no longer available; the action was not repeated."},
        {"invalid_request", L"The request was not accepted."},
        {"wrong_session", L"This window belongs to another put.gd session."},
    };
    if (auto it = fixed.find(error.code); it != fixed.end()) return it->second;
    return L"The request failed.";
}
std::wstring UiSession::describe(const Reply& reply) {
    if (reply.ok) return {};
    auto text = guidance(reply.error);
    if (!reply.error.message.empty()) text += L" (" + paths::wide(reply.error.code) + L": " + paths::wide(reply.error.message) + L")";
    else text += L" (" + paths::wide(reply.error.code) + L")";
    return text;
}

void UiSession::storeSettings(const json::Object& document, const json::Object& metadata) {
    document_ = document;
    metadata_ = metadata;
}
void UiSession::storeMetadata(const json::Object& metadata) { if (!metadata.empty()) metadata_ = metadata; }
void UiSession::storeSnapshot(const json::Object& snapshot) {
    build_ = snapshot.value("build").toObject();
    diagnostics_ = snapshot.value("diagnostics").toObject();
    const auto settings = snapshot.value("settings").toObject();
    storeSettings(settings.value("document").toObject(), settings.value("metadata").toObject());
}
std::string UiSession::profileLabel(const std::string& profileId) const {
    for (const auto& entry : document_.value("profiles").arrayRef())
        if (entry.objectRef().value("id") == profileId.c_str()) return entry.objectRef().value("label").toString(profileId);
    return profileId;
}
}
