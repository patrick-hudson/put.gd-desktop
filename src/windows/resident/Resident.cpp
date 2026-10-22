#include "resident/Resident.h"
#include "common/Log.h"

namespace putgd {
Loop& Loop::instance() { static Loop loop; return loop; }
Loop::Loop() {
    threadId_ = GetCurrentThreadId();
    WNDCLASSW wc{}; wc.lpfnWndProc = &Loop::procedure; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"putgd.resident.loop";
    RegisterClassW(&wc);
    hwnd_ = CreateWindowExW(0, wc.lpszClassName, L"put.gd", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
    SetWindowLongPtrW(hwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
}
void Loop::post(std::function<void()> fn) {
    { std::lock_guard lock(mutex_); queue_.push_back(std::move(fn)); }
    PostMessageW(hwnd_, WM_APP_POST, 0, 0);
}
void Loop::drain() {
    std::vector<std::function<void()>> batch;
    { std::lock_guard lock(mutex_); batch.swap(queue_); }
    for (auto& fn : batch) fn();
}
UINT_PTR Loop::startTimer(int ms, std::function<void()> callback, bool repeat) {
    const auto id = nextTimer_++;
    timers_[id] = std::make_shared<Timer>(Timer{std::move(callback), repeat});
    SetTimer(hwnd_, id, UINT(ms < 1 ? 1 : ms), nullptr);
    return id;
}
void Loop::stopTimer(UINT_PTR id) { if (timers_.erase(id)) KillTimer(hwnd_, id); }
void Loop::singleShot(int ms, std::function<void()> callback) { startTimer(ms, std::move(callback), false); }
LRESULT CALLBACK Loop::procedure(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* self = reinterpret_cast<Loop*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!self) return DefWindowProcW(hwnd, message, wParam, lParam);
    if (message == WM_APP_POST) { self->drain(); return 0; }
    if (message == WM_TIMER) {
        const auto it = self->timers_.find(wParam);
        if (it == self->timers_.end()) { KillTimer(hwnd, wParam); return 0; }
        auto timer = it->second;
        if (!timer->repeat) { KillTimer(hwnd, wParam); self->timers_.erase(it); }
        timer->callback();
        return 0;
    }
    LRESULT result = 0;
    if (self->hook_ && self->hook_(message, wParam, lParam, result)) return result;
    return DefWindowProcW(hwnd, message, wParam, lParam);
}
int Loop::run() {
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    return int(msg.wParam);
}
void Loop::quit(int code) { PostQuitMessage(code); }
void Loop::background(std::function<void()> fn, std::function<void()> reply) {
    std::lock_guard lock(threadsMutex_);
    for (auto it = threads_.begin(); it != threads_.end();) {
        if (WaitForSingleObject(reinterpret_cast<HANDLE>(it->native_handle()), 0) == WAIT_OBJECT_0) { it->join(); it = threads_.erase(it); } else ++it;
    }
    ++active_;
    threads_.emplace_back([this, fn = std::move(fn), reply = std::move(reply)] {
        fn();
        post([this, reply] { { std::lock_guard lock(threadsMutex_); --active_; } if (reply) reply(); });
    });
}
void Loop::joinBackground() {
    std::vector<std::thread> threads;
    { std::lock_guard lock(threadsMutex_); threads.swap(threads_); }
    for (auto& t : threads) if (t.joinable()) t.join();
}
int Loop::backgroundCount() { std::lock_guard lock(threadsMutex_); return active_; }
}
