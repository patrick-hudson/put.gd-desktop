// putgd.exe --ui VIEW: the on-demand Settings/Destinations/History/Activity/About
// window (Protocol.h, role "ui"). Lifecycle frames travel on stdin/stdout; every
// command goes over the session pipe like the CLI.
#include "common/Frames.h"
#include "common/Log.h"
#include "common/Modes.h"
#include "common/Paths.h"
#include "ui/MainWindow.h"
#include <objbase.h>
#include <thread>

namespace putgd {
namespace {
const std::vector<std::string> views{"settings", "history", "activity", "about", "editFile", "updates", "destinations"};
bool knownView(const std::string& view) { for (const auto& v : views) if (v == view) return true; return false; }
}

int uiMain(const std::vector<std::string>& args) {
    auto& channel = frames::helperChannel();
    log::initialize(paths::join(paths::dataRoot(), "logs"), "putgd-ui");
    ui::enablePerMonitorDpi();
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE))) return 1;
    INITCOMMONCONTROLSEX controls{sizeof controls, ICC_STANDARD_CLASSES | ICC_WIN95_CLASSES | ICC_LISTVIEW_CLASSES | ICC_TAB_CLASSES | ICC_BAR_CLASSES};
    InitCommonControlsEx(&controls);

    protocol::Frame bootstrap;
    std::string error;
    if (!channel.receive(bootstrap, protocol::startupMs, &error) || bootstrap.type != "bootstrap" || bootstrap.payload.value("role") != "ui") {
        log::error("ui", "No valid bootstrap", {{"reason", error.empty() ? std::string("unexpected_frame") : error}});
        return 1;
    }
    const auto& payload = bootstrap.payload;
    if (payload.value("build").isObject() && !protocol::buildMatches(payload.value("build").toObject())) {
        log::error("ui", "Resident build differs from this helper");
        return 2;
    }
    std::string view = payload.value("view").toString(args.size() > 1 ? args[1] : std::string("history"));
    if (!knownView(view)) view = "history";

    ui::UiSession session;
    session.setPipe(payload.value("session").toString(paths::sessionPipePath()));
    // Verify the live resident before acknowledging readiness (docs/ui-process-protocol.md).
    // Bounded well inside the 10 s ready deadline.
    const auto snapshot = session.callNow("ui.snapshot", {}, 7000);
    if (!snapshot.ok) {
        log::error("ui", "Resident snapshot unavailable", {{"code", snapshot.error.code}});
        return 1;
    }
    if (snapshot.result.value("build").isObject() && !protocol::buildMatches(snapshot.result.value("build").toObject())) {
        log::error("ui", "Resident snapshot build differs from this helper");
        return 2;
    }
    session.storeSnapshot(snapshot.result);

    auto window = std::make_unique<ui::MainWindow>(session, channel);
    if (!window->create(view, payload.value("state").toObject())) {
        log::error("ui", "Main window could not be created");
        return 1;
    }
    if (!channel.send("ready", {{"nonce", payload.value("nonce")}, {"build", protocol::buildIdentity()}})) return 1;

    // Control frames arrive on a reader thread and are handled on the UI thread.
    HWND target = window->hwnd();
    std::thread([&channel, target] {
        while (true) {
            auto frame = std::make_unique<protocol::Frame>();
            std::string reason;
            if (!channel.receive(*frame, -1, &reason)) {
                if (reason != "pipe_closed") log::warning("ui", "Control channel failed", {{"reason", reason}});
                PostMessageW(target, ui::controlClosedMessage, 0, 0);
                return;
            }
            if (PostMessageW(target, ui::controlFrameMessage, 0, reinterpret_cast<LPARAM>(frame.get()))) frame.release();
            else return;
        }
    }).detach();

    MSG message{};
    int code = 0;
    while (true) {
        const BOOL got = GetMessageW(&message, nullptr, 0, 0);
        if (got == 0) { code = int(message.wParam); break; }
        if (got == -1) { code = 1; break; }
        if (IsWindow(window->hwnd()) && window->preTranslate(message)) continue;
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    session.detach();
    CoUninitialize();
    return code;
}
}
