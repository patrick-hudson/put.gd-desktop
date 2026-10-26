#pragma once
// The resident's single owner thread: a message-only window whose loop runs
// posted callbacks and timers, like the Qt event loop the Linux resident uses.
// Everything that touches services runs here; workers hand results back with post().
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <windows.h>

namespace putgd {
inline constexpr UINT WM_APP_POST = WM_APP + 1;
inline constexpr UINT WM_APP_TRAY = WM_APP + 2;
class Loop {
public:
    static Loop& instance();
    // Thread-safe: run the callback on the owner thread.
    void post(std::function<void()>);
    // Owner thread only. Returns a timer id; callbacks run on the owner thread.
    UINT_PTR startTimer(int milliseconds, std::function<void()>, bool repeat = true);
    void stopTimer(UINT_PTR);
    void singleShot(int milliseconds, std::function<void()>);
    HWND hwnd() const { return hwnd_; }
    bool ownerThread() const { return GetCurrentThreadId() == threadId_; }
    int run();
    void quit(int code = 0);
    // Raw window messages the resident wants (hotkeys, clipboard, tray, session end).
    using MessageHook = std::function<bool(UINT, WPARAM, LPARAM, LRESULT&)>;
    void setMessageHook(MessageHook hook) { hook_ = std::move(hook); }
    // Background work: runs fn on a new thread, then reply on the owner thread.
    void background(std::function<void()> fn, std::function<void()> reply = {});
    // Waits for every background thread started through this loop.
    void joinBackground();
    int backgroundCount();
private:
    Loop();
    static LRESULT CALLBACK procedure(HWND, UINT, WPARAM, LPARAM);
    void drain();
    HWND hwnd_ = nullptr;
    DWORD threadId_ = 0;
    std::mutex mutex_;
    std::vector<std::function<void()>> queue_;
    struct Timer { std::function<void()> callback; bool repeat; };
    std::map<UINT_PTR, std::shared_ptr<Timer>> timers_;
    UINT_PTR nextTimer_ = 1;
    MessageHook hook_;
    std::mutex threadsMutex_;
    std::vector<std::thread> threads_;
    int active_ = 0;
};
}
