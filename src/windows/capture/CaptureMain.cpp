// putgd.exe --capture: the ephemeral live region/window/desktop acquisition
// helper (role "capture" in common/Protocol.h). It owns the selection overlay,
// the GDI grab and the PNG encode; the resident only ever sees the finished file
// at output.path plus its byte count, SHA-256 and dimensions.
//
// Threads: the main (UI) thread runs the handshake, the overlay message loop
// and the capture; a reader thread owns stdin after activate and posts parent
// `cancel` to the UI thread as WM_APP + 1; a heartbeat thread sends `heartbeat`
// every second and enforces the 30-s selected-to-checkpoint deadline. Exactly
// one terminal frame (checkpoint or failed) is ever sent. A failed frame is a
// clean answer: the process then exits 0. A closed parent pipe or a protocol
// violation by the parent terminates the process with exit code 1.
#include "capture/Grab.h"
#include "capture/Overlay.h"
#include "capture/Png.h"
#include "common/Frames.h"
#include "common/Log.h"
#include "common/Modes.h"
#include "common/Paths.h"
#include "common/Protocol.h"
#include "common/Sha256.h"
#include <algorithm>
#include <atomic>
#include <mutex>
#include <tuple>

namespace putgd {
namespace {
constexpr UINT cancelMessage = WM_APP + 1;
constexpr int deadlineMarginMs = 2000;

struct Bootstrap {
    std::string nonce;
    std::string mode;
    std::string outputPath;
    grab::Limits limits;
    std::vector<DWORD> excludePids;
    bool includeCursor = false;
    std::vector<grab::Rect> monitors; // empty: no layout check
};

class Session {
public:
    explicit Session(frames::Channel& channel) : channel_(channel) {
        accepted_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        cancelled_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    }
    bool send(const std::string& type, const json::Object& payload = {}) {
        std::lock_guard lock(mutex_);
        return channel_.send(type, payload);
    }
    // The single checkpoint/failed frame; false when another one already went out.
    bool sendTerminal(const std::string& type, const json::Object& payload) {
        bool expected = false;
        if (!terminal_.compare_exchange_strong(expected, true)) return false;
        checkpointSent_ = type == "checkpoint";
        return send(type, payload);
    }
    bool fail(const Error& e) { return sendTerminal("failed", {{"code", e.code}, {"message", e.message}}); }
    bool cancelRequested() const { return cancelRequested_; }

    void start() {
        uiThread_ = GetCurrentThreadId();
        MSG msg;
        PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE); // the thread queue exists before any cancel
        CloseHandle(CreateThread(nullptr, 0, &Session::readerThread, this, 0, nullptr));
        CloseHandle(CreateThread(nullptr, 0, &Session::heartbeatThread, this, 0, nullptr));
    }
    void markSelected() { selectedTick_ = GetTickCount64(); }
    // After the checkpoint: wait for checkpointAccepted (or a late cancel).
    void awaitAcceptance() {
        HANDLE events[2] = {accepted_, cancelled_};
        WaitForMultipleObjects(2, events, FALSE, INFINITE);
    }

private:
    static DWORD WINAPI readerThread(void* data) {
        auto* self = static_cast<Session*>(data);
        while (true) {
            protocol::Frame frame;
            std::string error;
            if (!self->channel_.receive(frame, -1, &error)) {
                log::warning("capture", "Parent channel ended", {{"reason", error}});
                TerminateProcess(GetCurrentProcess(), 1);
            }
            if (frame.type == "cancel") {
                self->cancelRequested_ = true;
                SetEvent(self->cancelled_);
                PostThreadMessageW(self->uiThread_, cancelMessage, 0, 0);
            } else if (frame.type == "checkpointAccepted" && self->checkpointSent_) {
                SetEvent(self->accepted_);
            } else {
                log::warning("capture", "Unexpected parent frame", {{"type", frame.type}});
                TerminateProcess(GetCurrentProcess(), 1);
            }
        }
    }
    static DWORD WINAPI heartbeatThread(void* data) {
        auto* self = static_cast<Session*>(data);
        while (true) {
            Sleep(protocol::heartbeatMs);
            const auto selected = self->selectedTick_.load();
            if (selected && !self->terminal_ && GetTickCount64() - selected > ULONGLONG(protocol::captureCheckpointMs - deadlineMarginMs)) {
                if (self->fail({"capture_failed", "The capture did not finish in time"})) TerminateProcess(GetCurrentProcess(), 0);
            }
            if (!self->send("heartbeat")) TerminateProcess(GetCurrentProcess(), 1);
        }
    }

    frames::Channel& channel_;
    std::mutex mutex_;
    std::atomic<bool> terminal_{false}, checkpointSent_{false}, cancelRequested_{false};
    std::atomic<ULONGLONG> selectedTick_{0};
    HANDLE accepted_ = nullptr, cancelled_ = nullptr;
    DWORD uiThread_ = 0;
};

void becomeDpiAware() {
    using SetContext = BOOL(WINAPI*)(HANDLE);
    const auto user32 = GetModuleHandleW(L"user32.dll");
    const auto set = user32 ? reinterpret_cast<SetContext>(reinterpret_cast<void*>(GetProcAddress(user32, "SetProcessDpiAwarenessContext"))) : nullptr;
    // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 == (HANDLE)-4. Failure with
    // ERROR_ACCESS_DENIED means a manifest already chose the awareness.
    if (!set || (!set(reinterpret_cast<HANDLE>(intptr_t(-4))) && GetLastError() != ERROR_ACCESS_DENIED))
        log::warning("capture", "Per-monitor DPI awareness unavailable");
}

bool limitValue(const json::Value& v, long long& out) {
    if (!json::integer(v, 1, 9007199254740991.0)) return false;
    out = v.toInteger();
    return true;
}

bool parseRect(const json::Value& v, grab::Rect& out) {
    if (!v.isArray() || v.arrayRef().size() != 4) return false;
    for (const auto& n : v.arrayRef()) if (!json::integer(n, -2147483647.0, 2147483647.0)) return false;
    out = {v[0].toInt(), v[1].toInt(), v[2].toInt(), v[3].toInt()};
    return !out.empty();
}

bool parseBootstrap(const json::Object& p, Bootstrap& b) {
    if (p.value("role") != "capture" || !p.value("nonce").isString() || p.value("nonce").stringRef().empty()) return false;
    b.nonce = p.value("nonce").stringRef();
    b.mode = p.value("mode").toString();
    if (b.mode != "region" && b.mode != "window" && b.mode != "desktop") return false;
    b.outputPath = p.value("output")["path"].toString();
    if (b.outputPath.empty() || !paths::isAbsolute(b.outputPath)) return false;
    const auto& limits = p.value("limits");
    if (!limitValue(limits["imagePixels"], b.limits.pixels) || !limitValue(limits["imageBytes"], b.limits.bytes)) return false;
    const auto& exclude = p.value("excludePids");
    if (!exclude.isNull()) {
        if (!exclude.isArray()) return false;
        for (const auto& pid : exclude.arrayRef()) {
            if (!json::integer(pid, 0, 4294967295.0)) return false;
            b.excludePids.push_back(DWORD(pid.toInteger()));
        }
    }
    const auto& cursor = p.value("includeCursor");
    if (!cursor.isNull() && !cursor.isBool()) return false;
    b.includeCursor = cursor.toBool();
    const auto& monitors = p.value("monitors");
    if (!monitors.isNull()) {
        if (!monitors.isArray()) return false;
        for (const auto& m : monitors.arrayRef()) {
            grab::Rect r;
            if (!parseRect(m["rect"], r)) return false;
            b.monitors.push_back(r);
        }
    }
    return true;
}

// The resident froze the monitor layout before launch; any change fails closed.
bool layoutMatches(const std::vector<grab::Rect>& expected) {
    if (expected.empty()) return true;
    auto key = [](const grab::Rect& r) { return std::make_tuple(r.x, r.y, r.w, r.h); };
    auto less = [&](const grab::Rect& a, const grab::Rect& b) { return key(a) < key(b); };
    auto want = expected, have = grab::monitors();
    std::sort(want.begin(), want.end(), less);
    std::sort(have.begin(), have.end(), less);
    return want == have;
}

json::Array rectJson(const grab::Rect& r) { return {r.x, r.y, r.w, r.h}; }

// Grab, encode, write, checkpoint. Returns the process exit code.
int acquire(Session& session, const Bootstrap& b, const overlay::Result& target, bool window) {
    if (session.cancelRequested()) { session.fail({"capture_cancelled", "Capture cancelled"}); return 0; }
    if (!layoutMatches(b.monitors)) { session.fail({"capture_geometry", "The display layout changed"}); return 0; }
    std::string png;
    int width = 0, height = 0;
    {
        grab::Image image;
        grab::View view;
        Error e;
        if (window) {
            grab::Rect now;
            if (!IsWindow(target.window) || !grab::frameBounds(target.window, now) || !(now == target.rect))
                e = {"capture_geometry", "The selected window moved or closed"};
            else
                e = grab::captureWindow(target.window, target.rect, b.includeCursor, b.limits, image, view);
        } else {
            e = grab::captureScreen(target.rect, b.includeCursor, b.limits, image, view);
        }
        if (e) { session.fail(e); return 0; }
        width = view.width;
        height = view.height;
        if (auto encoded = wic::encodePixels(view.bits, view.width, view.height, view.stride, false, png)) { session.fail(encoded); return 0; }
    } // pixels released before the file write
    if (session.cancelRequested()) { session.fail({"capture_cancelled", "Capture cancelled"}); return 0; }
    if (auto written = wic::writeExisting(b.outputPath, png)) { session.fail(written); return 0; }
    const json::Object checkpoint{{"bytes", (long long)png.size()}, {"sha256", crypto::Sha256::hexDigest(png)}, {"width", width}, {"height", height}};
    png.clear();
    png.shrink_to_fit();
    if (!session.sendTerminal("checkpoint", checkpoint)) return 0;
    session.awaitAcceptance();
    return 0;
}
}

int captureMain(const std::vector<std::string>&) {
    becomeDpiAware();
    auto& channel = frames::helperChannel();
    protocol::Frame frame;
    std::string error;
    if (!channel.receive(frame, protocol::startupMs, &error) || frame.type != "bootstrap") {
        log::error("capture", "No bootstrap frame", {{"reason", error}});
        return 1;
    }
    Bootstrap b;
    // Deliberately leaked: the reader and heartbeat threads use it until the process exits.
    Session& session = *new Session(channel);
    if (!parseBootstrap(frame.payload, b)) {
        log::error("capture", "Invalid bootstrap");
        session.fail({"capture_failed", "Invalid capture request"});
        return 1;
    }
    if (!session.send("ready", {{"nonce", b.nonce}, {"build", protocol::buildIdentity()}})) return 1;
    if (!channel.receive(frame, -1, &error)) return 1;
    if (frame.type == "cancel") { session.fail({"capture_cancelled", "Capture cancelled"}); return 0; }
    if (frame.type != "activate") return 1;
    session.start();

    wic::Apartment com;
    if (!com.ok() || !wic::factory()) { session.fail({"capture_failed", "Windows Imaging Component is unavailable"}); return 0; }
    if (!layoutMatches(b.monitors)) { session.fail({"capture_geometry", "The display layout changed"}); return 0; }

    if (b.mode == "desktop") {
        overlay::Result target;
        target.rect = grab::virtualScreen();
        session.markSelected();
        session.send("selected", {{"rect", rectJson(target.rect)}, {"target", json::Object{{"kind", "desktop"}}}});
        return acquire(session, b, target, false);
    }

    const bool window = b.mode == "window";
    overlay::Selector selector;
    if (auto e = selector.show(window ? overlay::Mode::Window : overlay::Mode::Region, b.excludePids)) { session.fail(e); return 0; }
    session.send("selecting");
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!msg.hwnd && msg.message == cancelMessage) { selector.cancel(); continue; }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    overlay::Result target = selector.result();
    selector.hide(); // hidden and flushed from the compositor before any grab
    if (session.cancelRequested() || target.kind == overlay::Result::Kind::Cancelled || target.kind == overlay::Result::Kind::None) {
        session.fail({"capture_cancelled", "Capture cancelled"});
        return 0;
    }
    if (target.kind == overlay::Result::Kind::Failed) { session.fail(target.error); return 0; }
    if (target.rect.empty()) { session.fail({"capture_geometry", "The selection is empty"}); return 0; }
    session.markSelected();
    json::Object kind{{"kind", window ? "window" : "region"}};
    if (window) {
        kind["title"] = target.title;
        kind["pid"] = (long long)target.pid;
    }
    session.send("selected", {{"rect", rectJson(target.rect)}, {"target", kind}});
    return acquire(session, b, target, window);
}
}
