#pragma once
// The versioned JSON session (docs/cli.md) served over the named pipe: the
// request envelope, method dispatch and every error code CLI/MCP clients rely on.
#include "common/Pipe.h"
#include "resident/HistoryService.h"
#include "resident/OperationService.h"
#include "resident/SetupService.h"
#include "resident/SettingsService.h"
#include "resident/UiProcess.h"
#include "resident/UpdateService.h"

namespace putgd {
class Session {
public:
    Session(SettingsService&, OperationService&, HistoryService&, SetupService&, UpdateService&, UiProcess&, std::string logDirectory);
    Error listen(const std::string& pipePath);
    void stop();
    const std::string& path() const { return server_.path(); }
    const std::string& residentEpoch() const { return epoch_; }
    std::function<void()> onQuit;
    static json::Object response(const std::string& requestId, const Error&, const json::Object& result);
private:
    void dispatch(const json::Object& request, JsonReply);
    json::Object snapshot() const;
    void diagnosticsBundle(const json::Object& params, JsonReply);
    pipe::Server server_;
    std::string epoch_, logs_;
    SettingsService& settings_;
    OperationService& operations_;
    HistoryService& history_;
    SetupService& setup_;
    UpdateService& updates_;
    UiProcess& ui_;
};
}
