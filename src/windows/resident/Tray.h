#pragma once
// The notification-area icon, its menu and toast notifications, through
// Shell_NotifyIcon alone: Windows 10/11 render NIF_INFO balloons as toasts in
// the notification center, so no WinRT is needed.
#include <functional>
#include <string>
#include <windows.h>

namespace putgd {
class Tray {
public:
    enum class Tint { Idle, Busy, Uploading, Failed };
    // Menu commands; the resident maps them to actions and views.
    enum Command : UINT {
        CmdCaptureRegion = 1001, CmdCaptureEdit, CmdCaptureWindow, CmdCaptureDesktop, CmdRecord,
        CmdEditClipboard, CmdEditFile, CmdUploadClipboard, CmdSettings, CmdHistory, CmdActivity, CmdAbout, CmdQuit
    };
    explicit Tray(HWND owner);
    ~Tray();
    bool available() const { return added_; }
    void setTint(Tint);
    void setTooltip(const std::string& utf8);
    // Shows a toast; `link` (optional) opens when the user clicks it.
    void notify(const std::string& title, const std::string& message, bool warning, const std::string& link = {});
    void onCommand(std::function<void(UINT)> handler) { command_ = std::move(handler); }
    // Handles WM_APP_TRAY and the TaskbarCreated broadcast; true when consumed.
    bool handleMessage(UINT message, WPARAM wParam, LPARAM lParam);
    UINT taskbarCreatedMessage() const { return taskbarCreated_; }
    // The recording entry is disabled on Windows until a native recorder exists.
    void setRecordingAvailable(bool available) { recording_ = available; }
private:
    void add();
    void remove();
    void showMenu();
    HICON makeIcon(Tint, int size);
    HWND owner_;
    bool added_ = false, recording_ = false;
    Tint tint_ = Tint::Idle;
    HICON icon_ = nullptr;
    UINT taskbarCreated_ = 0;
    std::string pendingLink_, tooltip_ = "put.gd";
    std::function<void(UINT)> command_;
};
}
