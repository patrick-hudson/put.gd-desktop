#include "record/Recorder.h"
#include "common/Log.h"
#include "common/Paths.h"
#include "core/Recording.h"
#include <cmath>

namespace putgd::record {
namespace {
constexpr int budgetCheckFrames = 15;
long long now100ns(const LARGE_INTEGER& start, const LARGE_INTEGER& frequency) {
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    return (long long)((double(t.QuadPart - start.QuadPart) * 10000000.0) / double(frequency.QuadPart));
}
}
Recorder::~Recorder() { cancel(); if (wake_) CloseHandle(wake_); if (ready_) CloseHandle(ready_); }
long long Recorder::fileBytes() const {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (output_.empty() || !GetFileAttributesExW(paths::wide(output_).c_str(), GetFileExInfoStandard, &data)) return 0;
    return (long long)(((unsigned long long)data.nFileSizeHigh << 32) | data.nFileSizeLow);
}
Error Recorder::prepare(const grab::Rect& area, const std::string& output, int fps, long long budgetBytes) {
    output_ = output; budget_ = budgetBytes; period_ = 10000000LL / std::max(1, fps);
    wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    ready_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    thread_ = std::thread([this, area, output, fps] { run(area, output, fps); });
    WaitForSingleObject(ready_, INFINITE);
    if (openError_) { thread_.join(); return openError_; }
    return {};
}
void Recorder::start() { command_ = int(Command::Start); SetEvent(wake_); }
void Recorder::pause() { paused_ = true; SetEvent(wake_); }
void Recorder::resume() { paused_ = false; SetEvent(wake_); }
Error Recorder::stop(long long& durationUs) {
    if (thread_.joinable()) { command_ = int(Command::Stop); SetEvent(wake_); thread_.join(); }
    durationUs = this->durationUs();
    if (auto f = failure()) return f;
    if (stopError_) return stopError_;
    if (lastFrame_ < 0) return {"recording_failed", "The recording ended without a frame", {}};
    return {};
}
void Recorder::cancel() {
    if (!thread_.joinable()) return;
    command_ = int(Command::Cancel); SetEvent(wake_); thread_.join();
}
void Recorder::finishOwn(const std::string& reason, Error e) {
    std::lock_guard l(mutex_);
    if (stopped_.empty()) stopped_ = reason;
    if (e && !failure_) failure_ = std::move(e);
}
void Recorder::run(grab::Rect area, std::string output, int fps) {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    HDC screen = GetDC(nullptr);
    grab::Image image;
    media::Mp4Writer writer;
    if (!screen || !image.create(area.w, area.h)) openError_ = {"recording_failed", "Cannot open the screen for recording", {}};
    else openError_ = writer.open(output, {area.w, area.h, fps, recording::liveBitrate(area.w, area.h, fps)});
    encoder_ = writer.encoder();
    SetEvent(ready_);
    auto cleanup = [&] { if (screen) ReleaseDC(nullptr, screen); image.release(); if (SUCCEEDED(com)) CoUninitialize(); };
    if (openError_) { cleanup(); return; }
    while (command_ == int(Command::None)) WaitForSingleObject(wake_, INFINITE);
    if (command_ != int(Command::Start)) { writer.abandon(); cleanup(); return; }

    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!timer) timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
    LARGE_INTEGER frequency, origin;
    QueryPerformanceFrequency(&frequency); QueryPerformanceCounter(&origin);
    long long pausedTotal = 0, pauseStarted = -1, frames = 0;
    bool ownStop = false;
    while (command_ == int(Command::Start)) {
        const long long raw = now100ns(origin, frequency);
        if (paused_) {
            if (pauseStarted < 0) pauseStarted = raw;
            WaitForSingleObject(wake_, 100);
            continue;
        }
        if (pauseStarted >= 0) {
            // Remove a whole number of frames, so later frames stay on the frame grid.
            pausedTotal += std::llround(double(raw - pauseStarted) / double(period_)) * period_;
            pauseStarted = -1;
        }
        const long long clip = raw - pausedTotal;
        const long long index = std::max(0LL, std::llround(double(clip) / double(period_)));
        if (index > lastFrame_) {
            if (!BitBlt(image.dc(), 0, 0, area.w, area.h, screen, area.x, area.y, SRCCOPY | CAPTUREBLT)) { finishOwn("error", {"recording_failed", "The screen could not be read", {}}); break; }
            GdiFlush();
            if (auto e = writer.write(image.bits(), image.stride(), index * period_, period_)) { finishOwn("error", e); break; }
            lastFrame_ = index;
            if (budget_ > 0 && ++frames % budgetCheckFrames == 0 && fileBytes() >= budget_) { ownStop = true; break; }
        }
        // Sleep until the next frame boundary, or a command.
        const long long due = (lastFrame_ + 1) * period_ - (now100ns(origin, frequency) - pausedTotal);
        if (due > 0 && timer) {
            LARGE_INTEGER relative; relative.QuadPart = -due;
            SetWaitableTimer(timer, &relative, 0, nullptr, nullptr, FALSE);
            HANDLE handles[2] = {timer, wake_};
            WaitForMultipleObjects(2, handles, FALSE, INFINITE);
        }
    }
    if (timer) CloseHandle(timer);
    if (command_ == int(Command::Cancel) || failure()) writer.abandon();
    else {
        stopError_ = writer.finish();
        if (ownStop) finishOwn("budget", stopError_);
    }
    log::info("record", "Capture ended", {{"frames", double(lastFrame_ + 1)}, {"encoder", encoder_}, {"reason", ownStop ? "budget" : command_ == int(Command::Cancel) ? "cancel" : "stop"}});
    cleanup();
}
}
