// putgd.exe --record: the screen recording helper (role "record" in
// common/Protocol.h). It owns the region selector, the GDI capture, the Media
// Foundation encoder and the exports; the resident only sees control frames and
// files it created itself, checked by byte count and SHA-256.
//
// Threads: the main thread runs the handshake, the selector and the command loop
// (a Win32 message loop, so the overlay and later windows work); a reader thread
// owns stdin after activate and posts every parent frame to it; a heartbeat
// thread sends `heartbeat` every second; the capture thread belongs to Recorder;
// stop and export run on worker threads and answer from there (sends are
// serialized). A closed parent pipe ends the process with exit code 1.
#include "capture/Grab.h"
#include "capture/Overlay.h"
#include "common/Files.h"
#include "common/Frames.h"
#include "common/Log.h"
#include "common/Modes.h"
#include "common/Paths.h"
#include "common/Protocol.h"
#include "core/Recording.h"
#include "common/Text.h"
#include "record/Export.h"
#include "record/Media.h"
#include "record/Recorder.h"
#include <atomic>
#include <memory>
#include <mutex>
#include <thread>

namespace putgd {
namespace {
constexpr UINT frameMessage = WM_APP + 2;
constexpr UINT_PTR progressTimer = 1;

class Link {
public:
    explicit Link(frames::Channel& channel) : channel_(channel) {}
    bool send(const std::string& type, const json::Object& payload = {}) { std::lock_guard lock(mutex_); return channel_.send(type, payload); }
    void reply(const std::string& requestId, const Error& e, const json::Object& result = {}) {
        if (e) send("commandResult", {{"requestId", requestId}, {"ok", false}, {"code", e.code}, {"message", e.message}, {"details", e.details}});
        else send("commandResult", {{"requestId", requestId}, {"ok", true}, {"result", result}});
    }
    void event(const std::string& name, json::Object fields = {}) { fields["event"] = name; send("event", fields); }
    void startThreads() {
        mainThread_ = GetCurrentThreadId();
        MSG msg;
        PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE); // the queue exists before the first frame
        std::thread([this] {
            while (true) {
                auto frame = std::make_unique<protocol::Frame>();
                std::string error;
                if (!channel_.receive(*frame, -1, &error)) { log::warning("record", "Parent channel ended", {{"reason", error}}); TerminateProcess(GetCurrentProcess(), 1); }
                PostThreadMessageW(mainThread_, frameMessage, 0, reinterpret_cast<LPARAM>(frame.release()));
            }
        }).detach();
        std::thread([this] { while (true) { Sleep(protocol::heartbeatMs); if (!send("heartbeat")) TerminateProcess(GetCurrentProcess(), 1); } }).detach();
    }
private:
    frames::Channel& channel_;
    std::mutex mutex_;
    DWORD mainThread_ = 0;
};

void becomeDpiAware() {
    using SetContext = BOOL(WINAPI*)(HANDLE);
    const auto user32 = GetModuleHandleW(L"user32.dll");
    const auto set = user32 ? reinterpret_cast<SetContext>(reinterpret_cast<void*>(GetProcAddress(user32, "SetProcessDpiAwarenessContext"))) : nullptr;
    if (!set || (!set(reinterpret_cast<HANDLE>(intptr_t(-4))) && GetLastError() != ERROR_ACCESS_DENIED)) log::warning("record", "Per-monitor DPI awareness unavailable");
}

grab::Rect primaryMonitor() {
    MONITORINFO info{sizeof info};
    GetMonitorInfoW(MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY), &info);
    return grab::fromRECT(info.rcMonitor);
}
json::Array monitorList() {
    json::Array list;
    const auto all = grab::monitors();
    const auto primary = primaryMonitor();
    for (size_t i = 0; i < all.size(); ++i) list.push_back(json::Object{{"identity", std::to_string(i + 1)}, {"rect", json::Array{all[i].x, all[i].y, all[i].w, all[i].h}}, {"primary", all[i] == primary}});
    return list;
}
// The capture area in virtual-screen pixels, shrunk to even dimensions.
Error resolveArea(const RecordingTarget& target, grab::Rect& area) {
    if (target.kind == "desktop") area = grab::virtualScreen();
    else if (target.kind == "monitor") {
        if (target.identity.empty()) area = primaryMonitor();
        else {
            const auto all = grab::monitors();
            size_t index = 0;
            for (char c : target.identity) { if (c < '0' || c > '9' || index > 64) return {"recording_target", "Monitor identities are 1 to the number of monitors", {}}; index = index * 10 + size_t(c - '0'); }
            if (index < 1 || index > all.size()) return {"recording_target", "No monitor has this identity", {{"monitors", monitorList()}}};
            area = all[index - 1];
        }
    } else if (target.kind == "region" && target.rect) {
        const grab::Rect wanted{int(target.rect->x), int(target.rect->y), int(target.rect->w), int(target.rect->h)};
        area = grab::intersect(wanted, grab::virtualScreen());
        if (!(area == wanted)) return {"recording_target", "The region is not on the desktop", {{"desktop", json::Array{grab::virtualScreen().x, grab::virtualScreen().y, grab::virtualScreen().w, grab::virtualScreen().h}}}};
    } else return {"recording_unsupported", "This target kind is not available on Windows yet", {{"kind", target.kind}}};
    const auto even = recording::evenArea({area.x, area.y, area.w, area.h});
    area = {int(even.x), int(even.y), int(even.w), int(even.h)};
    if (area.w < 16 || area.h < 16) return {"recording_target", "The area to record must be at least 16 by 16 pixels", {}};
    return {};
}

struct Bootstrap {
    std::string nonce, output;
    bool probe = false;
    RecordingTarget target;
    RecordingConfig config;
    long long budget = 0;
    std::vector<DWORD> excludePids;
};
bool parseBootstrap(const json::Object& p, Bootstrap& b) {
    if (p.value("role") != "record" || !p.value("nonce").isString() || p.value("nonce").stringRef().empty()) return false;
    b.nonce = p.value("nonce").stringRef();
    b.probe = p.value("probe").toBool();
    if (b.probe) return true;
    if (recording::parseTarget(p.value("target"), b.target)) return false;
    auto config = p.value("config").toObject();
    config.remove("mime"); config.remove("budgetBytes");
    if (recording::parseConfig(config, b.config)) return false;
    b.output = p.value("output")["path"].toString();
    if (b.output.empty() || !paths::isAbsolute(b.output)) return false;
    b.budget = p.value("limits")["budgetBytes"].toInteger(0);
    for (const auto& pid : p.value("excludePids").toArray()) if (json::integer(pid, 0, 4294967295.0)) b.excludePids.push_back(DWORD(pid.toInteger()));
    return true;
}

int probe(Link& link, frames::Channel& channel) {
    json::Object result{{"acquisition", "gdi"}, {"targets", json::Array{"monitor", "desktop", "region"}}, {"audio", json::Object{{"system", false}, {"microphone", false}}},
        {"formats", json::Array{}}, {"codecs", json::Object{}}, {"monitors", monitorList()}};
    media::Platform platform;
    if (!platform.ok()) {
        result["available"] = false; result["reason"] = platform.failure();
        result["message"] = "Media Foundation is not available. On a Windows N edition, install the Media Feature Pack.";
    } else {
        std::string encoder;
        if (auto e = media::probeH264(encoder)) {
            result["available"] = false; result["reason"] = "h264_encoder_unusable"; result["message"] = e.message; result["details"] = e.details;
        } else {
            result["available"] = true; result["encoder"] = encoder;
            result["formats"] = json::Array{"mp4"}; result["codecs"] = json::Object{{"mp4", json::Array{"h264"}}};
        }
    }
    link.send("result", result);
    protocol::Frame frame;
    channel.receive(frame, 10000, nullptr); // resultAccepted
    return 0;
}

class Helper {
public:
    Helper(Link& link, Bootstrap b) : link_(link), b_(std::move(b)) {}
    int run() {
        grab::Rect area;
        if (b_.target.kind == "region" && !b_.target.rect) { if (!select(area)) return finishLoop(); }
        else if (auto e = resolveArea(b_.target, area)) { link_.send("failed", {{"code", e.code}, {"message", e.message}, {"details", e.details}}); return finishLoop(); }
        if (quitting_) return finishLoop();
        if (auto e = recorder_.prepare(area, b_.output, b_.config.fps, b_.budget)) { link_.send("failed", {{"code", e.code}, {"message", e.message}, {"details", e.details}}); return finishLoop(); }
        area_ = area; prepared_ = true;
        link_.send("prepared", {{"canvas", json::Array{area.w, area.h}}, {"rect", json::Array{area.x, area.y, area.w, area.h}}, {"acquisition", "gdi"}, {"encoder", recorder_.encoder()}});
        SetTimer(nullptr, progressTimer, 1000, nullptr);
        return finishLoop();
    }
private:
    // Region selection with the capture helper's overlay; cancel ends the session.
    bool select(grab::Rect& area) {
        overlay::Selector selector;
        if (auto e = selector.show(overlay::Mode::Region, b_.excludePids)) { link_.send("failed", {{"code", e.code}, {"message", e.message}}); return false; }
        selecting_ = &selector;
        link_.send("selecting", {{"selector", "win32"}});
        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            if (!msg.hwnd && msg.message == frameMessage) { handle(std::unique_ptr<protocol::Frame>(reinterpret_cast<protocol::Frame*>(msg.lParam))); continue; }
            TranslateMessage(&msg); DispatchMessageW(&msg);
        }
        selecting_ = nullptr;
        const auto result = selector.result();
        selector.hide(); // gone from the compositor before any frame is grabbed
        if (quitting_) return false;
        if (result.kind != overlay::Result::Kind::Selected || result.rect.empty()) {
            if (result.kind == overlay::Result::Kind::Failed) link_.send("failed", {{"code", result.error.code}, {"message", result.error.message}});
            else link_.send("failed", {{"code", "cancelled"}, {"message", "Region selection cancelled; nothing was recorded"}});
            return false;
        }
        RecordingTarget target; target.kind = "region"; target.rect = RecordingRect{result.rect.x, result.rect.y, result.rect.w, result.rect.h};
        if (auto e = resolveArea(target, area)) { link_.send("failed", {{"code", e.code}, {"message", e.message}}); return false; }
        return true;
    }
    int finishLoop() {
        MSG msg;
        while (!quitting_ && GetMessageW(&msg, nullptr, 0, 0) > 0) {
            if (!msg.hwnd && msg.message == frameMessage) { handle(std::unique_ptr<protocol::Frame>(reinterpret_cast<protocol::Frame*>(msg.lParam))); continue; }
            if (!msg.hwnd && msg.message == WM_TIMER && msg.wParam == progressTimer) { progress(); continue; }
            TranslateMessage(&msg); DispatchMessageW(&msg);
        }
        if (worker_.joinable()) worker_.join();
        recorder_.cancel();
        return 0;
    }
    void progress() {
        if (!started_ || stopped_) return;
        const auto reason = recorder_.stoppedReason();
        if (reason == "budget") {
            long long duration = 0;
            stopped_ = true;
            if (auto e = recorder_.stop(duration)) { link_.event("failed", {{"error", e.toJson()}}); return; }
            link_.event("stopped", {{"durationUs", double(duration)}, {"canvas", json::Array{area_.w, area_.h}}, {"reason", "budget"}});
            return;
        }
        if (reason == "error") { stopped_ = true; link_.event("failed", {{"error", recorder_.failure().toJson()}}); return; }
        link_.event("progress", {{"durationUs", double(recorder_.durationUs())}, {"bytes", double(recorder_.fileBytes())}, {"paused", recorder_.paused()}, {"encoder", recorder_.encoder()}});
    }
    void handle(std::unique_ptr<protocol::Frame> frame) {
        if (frame->type == "shutdown") { quitting_ = true; if (selecting_) selecting_->cancel(); PostQuitMessage(0); return; }
        if (frame->type != "command") { log::warning("record", "Unexpected parent frame", {{"type", frame->type}}); TerminateProcess(GetCurrentProcess(), 1); }
        const auto requestId = frame->payload.value("requestId").toString();
        const auto command = frame->payload.value("command").toString();
        const auto params = frame->payload.value("params").toObject();
        if (command == "cancel") {
            if (selecting_) selecting_->cancel();
            exportCancel_ = true;
            stopped_ = true;
            if (worker_.joinable()) worker_.join();
            recorder_.cancel();
            link_.reply(requestId, {});
            return;
        }
        if (!prepared_) { link_.reply(requestId, {"recording_state", "The helper has no prepared session", {}}); return; }
        if (busy()) { link_.reply(requestId, {"recording_busy", "The helper is still finishing a command", {}}); return; }
        if (command == "start") { if (started_) { link_.reply(requestId, {"recording_state", "Already started", {}}); return; } started_ = true; recorder_.start(); link_.reply(requestId, {}, status()); return; }
        if (command == "pause") { recorder_.pause(); link_.reply(requestId, {}, status()); return; }
        if (command == "resume") { recorder_.resume(); link_.reply(requestId, {}, status()); return; }
        if (command == "stop") {
            if (!started_ || stopped_) { link_.reply(requestId, {"recording_state", "Nothing is recording", {}}); return; }
            stopped_ = true;
            workerDone_ = false;
            worker_ = std::thread([this, requestId] {
                long long duration = 0;
                const auto e = recorder_.stop(duration);
                workerDone_ = true;
                if (e) link_.reply(requestId, e);
                else link_.reply(requestId, {}, {{"durationUs", double(duration)}, {"canvas", json::Array{area_.w, area_.h}}, {"bytes", double(recorder_.fileBytes())}, {"encoder", recorder_.encoder()}});
            });
            return;
        }
        if (command == "export" || command == "preview") {
            if (!stopped_) { link_.reply(requestId, {"recording_state", "Nothing to export before stop", {}}); return; }
            RecordingEdit edit;
            if (!recording::parseEdit(params.value("edit").toObject(), edit)) { link_.reply(requestId, {"invalid_request", "Invalid edit", {}}); return; }
            const RecordingSize canvas{area_.w, area_.h};
            workerDone_ = false;
            if (command == "preview") {
                const long long time = params.value("time").toInteger(0);
                const int maxEdge = params.value("maxEdge").toInt(1024);
                worker_ = std::thread([this, requestId, canvas, edit, time, maxEdge] {
                    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
                    record::PreviewResult preview;
                    const auto e = record::previewFrame(b_.output, canvas, edit, time, maxEdge, preview);
                    if (SUCCEEDED(com)) CoUninitialize();
                    workerDone_ = true;
                    if (e) link_.reply(requestId, e);
                    else link_.reply(requestId, {}, {{"png", text::base64(preview.png)}, {"frameTimeUs", double(preview.frameTimeUs)}, {"width", preview.width}, {"height", preview.height}});
                });
                return;
            }
            record::ExportRequest request;
            request.working = b_.output; request.output = params.value("output")["path"].toString();
            request.canvas = canvas; request.durationUs = recorder_.durationUs(); request.edit = edit;
            auto config = params.value("config").toObject(); config.remove("mime"); config.remove("budgetBytes");
            if (request.output.empty() || !paths::isAbsolute(request.output) || recording::parseConfig(config, request.config)) { workerDone_ = true; link_.reply(requestId, {"invalid_request", "Invalid export request", {}}); return; }
            exportCancel_ = false;
            worker_ = std::thread([this, requestId, request] {
                const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
                record::ExportResult result;
                ULONGLONG reported = 0;
                // At most ten progress events a second, so a fast export stays inside the control channel's rate.
                const auto e = record::exportClip(request, result, exportCancel_, [this, &reported](double fraction) {
                    const auto now = GetTickCount64();
                    if (fraction < 1.0 && now - reported < 100) return;
                    reported = now;
                    link_.event("progress", {{"exportFraction", fraction}});
                });
                if (SUCCEEDED(com)) CoUninitialize();
                workerDone_ = true;
                if (e) link_.reply(requestId, e);
                else link_.reply(requestId, {}, {{"bytes", double(result.bytes)}, {"sha256", result.sha256}, {"durationUs", double(result.durationUs)}, {"frames", result.frames},
                    {"videoEncoder", result.encoder}, {"size", json::Array{result.size.w, result.size.h}}});
            });
            return;
        }
        link_.reply(requestId, {"unknown_method", "Unknown recording helper command", {}});
    }
    // A worker that already answered is joined here; one still running makes the helper busy.
    bool busy() {
        if (!worker_.joinable()) return false;
        if (!workerDone_) return true;
        worker_.join();
        return false;
    }
    json::Object status() const { return {{"durationUs", double(recorder_.durationUs())}, {"bytes", double(recorder_.fileBytes())}, {"paused", recorder_.paused()}, {"encoder", recorder_.encoder()}}; }

    Link& link_;
    Bootstrap b_;
    record::Recorder recorder_;
    grab::Rect area_;
    overlay::Selector* selecting_ = nullptr;
    std::thread worker_;
    bool prepared_ = false, started_ = false, quitting_ = false;
    std::atomic<bool> stopped_{false}, workerDone_{true}, exportCancel_{false};
};
}

int recordMain(const std::vector<std::string>&) {
    becomeDpiAware();
    auto& channel = frames::helperChannel();
    protocol::Frame frame;
    std::string error;
    if (!channel.receive(frame, protocol::startupMs, &error) || frame.type != "bootstrap") { log::error("record", "No bootstrap frame", {{"reason", error}}); return 1; }
    Bootstrap b;
    // Leaked on purpose: the reader and heartbeat threads use it until the process exits.
    Link& link = *new Link(channel);
    if (!parseBootstrap(frame.payload, b)) { log::error("record", "Invalid bootstrap"); return 1; }
    if (!link.send("ready", {{"nonce", b.nonce}, {"build", protocol::buildIdentity()}})) return 1;
    if (!channel.receive(frame, -1, &error) || frame.type != "activate") return 1;
    if (b.probe) return probe(link, channel);
    wic::Apartment com;
    media::Platform platform;
    link.startThreads();
    if (!com.ok() || !platform.ok()) {
        link.send("failed", {{"code", "recording_unavailable"}, {"message", "Media Foundation is not available"}, {"details", json::Object{{"reason", platform.failure()}}}});
        MSG msg; while (GetMessageW(&msg, nullptr, 0, 0) > 0) { if (!msg.hwnd && msg.message == frameMessage) { std::unique_ptr<protocol::Frame> f(reinterpret_cast<protocol::Frame*>(msg.lParam)); if (f->type == "shutdown") break; } }
        return 0;
    }
    Helper helper(link, std::move(b));
    return helper.run();
}
}
