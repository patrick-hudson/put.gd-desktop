#pragma once
// The state every driven file chooser reports (docs/file-dialogs.md, "Driving the
// dialogs"): open, mode, title, backend, parentWindow, fallbackReason and the last
// outcome. On Windows the chooser is the shell's own IFileDialog, backend "native".
#include "common/Json.h"
#include <string>

namespace putgd {
struct FileDialogState {
    bool open = false;
    std::string mode, title; // mode: open | save | folder
    json::Object last;       // status: chosen | cancelled | failed
    void begin(std::string chooserMode, std::string chooserTitle);
    void chosen(const std::string& path);
    void cancelled();
    void failed(const std::string& code, const std::string& message);
    json::Object toJson() const;
private:
    void finish(const char* status, const std::string& path, const json::Value& error);
};
}
