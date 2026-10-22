#pragma once
// Supervision of the on-demand UI helper (`putgd --ui VIEW`, common/Protocol.h
// role "ui"): one helper at a time, view acknowledgements with a 12 s deadline,
// the bounded state checkpoint across close/reopen, shortcut-recording pauses and
// the view commands (ui.moveDestination, ui.diagnostics, ui.historySaveDiagnostics,
// ui.fileDialog) answered by the open window within 10 s.
#include "common/Error.h"
#include "resident/Helpers.h"
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace putgd {
class UiProcess {
public:
    UiProcess(std::string sessionPath, std::string residentEpoch);
    ~UiProcess();
    void show(const std::string& view, std::function<void(Error)> reply);
    void invalidate(const std::string& kind);
    // A command for the open window; it never starts a helper (ui_view_closed without one).
    using CommandReply = std::function<void(Error, json::Object)>;
    void command(const std::string& name, const json::Object& params, CommandReply reply);
    // ui.viewState: the derived projections of the last accepted checkpoint.
    json::Object viewState() const;
    // Explicit quit: close the helper without a checkpoint handshake.
    void stop();
    bool running() const { return helper_ && helper_->running() && !closing_; }
    unsigned long pid() const { return helper_ ? helper_->pid() : 0; }
    std::function<void(bool active, std::function<void(Error)>)> onShortcutRecording;
    std::function<void(unsigned long pid)> onProcessChanged;
    json::Object diagnostics() const;
private:
    void start(const std::string& view);
    void failPending(const Error&);
    void handle(const protocol::Frame&);
    void failCommands(const Error&);
    std::string phase() const;
    std::string session_, epoch_;
    std::unique_ptr<helpers::Supervised> helper_;
    json::Object state_;
    struct Pending { std::string view; std::function<void(Error)> reply; UINT_PTR timer = 0; };
    std::vector<Pending> pending_;
    bool ready_ = false, closing_ = false;
    unsigned long long generation_ = 0;
    json::Object lastExit_;
    struct Command { CommandReply reply; UINT_PTR timer = 0; };
    std::map<std::string, Command> commands_;
    unsigned long long commandSerial_ = 0;
};
}
