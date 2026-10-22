#include "record/FrameSource.h"
#include "common/Log.h"
#include "core/RecordingFrames.h"
#include "record/Media.h"
#include <chrono>

namespace putgd::record {
FrameSource::FrameSource(HWND window, UINT message, std::string clip, RecordingSize canvas)
    : window_(window), message_(message), clip_(std::move(clip)), canvas_(canvas) { thread_ = std::thread([this] { run(); }); }
FrameSource::~FrameSource() {
    { std::lock_guard lock(mutex_); quit_ = true; }
    wake_.notify_all();
    if (thread_.joinable()) thread_.join();
}
unsigned long long FrameSource::seek(long long us, const RecordingEdit& edit) {
    std::lock_guard lock(mutex_);
    pending_ = {Request::Seek, us, 0, 1, edit, ++next_};
    wake_.notify_all();
    return next_;
}
unsigned long long FrameSource::play(long long from, long long end, double speed, const RecordingEdit& edit) {
    std::lock_guard lock(mutex_);
    pending_ = {Request::Play, from, end, speed, edit, ++next_};
    wake_.notify_all();
    return next_;
}
void FrameSource::stop() {
    std::lock_guard lock(mutex_);
    ++next_;
    pending_ = {};
    wake_.notify_all();
}
void FrameSource::post(ReviewFrame* frame) {
    if (!PostMessageW(window_, message_, 0, reinterpret_cast<LPARAM>(frame))) delete frame;
}
void FrameSource::run() {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    media::ClipReader reader;
    const Error opened = reader.open(clip_, canvas_.w, canvas_.h);
    if (opened) log::warning("record", "Review cannot decode the working clip", {{"code", opened.code}, {"message", opened.message}});
    std::vector<uint8_t> bits, previous;
    while (true) {
        Request request;
        {
            std::unique_lock lock(mutex_);
            wake_.wait(lock, [this] { return quit_ || pending_.kind != Request::None; });
            if (quit_) break;
            request = pending_;
            pending_ = {};
        }
        if (opened) continue;
        auto current = [&] { std::lock_guard lock(mutex_); return next_ == request.id && !quit_; };
        auto deliver = [&](std::vector<uint8_t>& frame, long long us, bool playing, bool ended) {
            const recording::Frame view{frame.data(), canvas_.w, canvas_.h, size_t(canvas_.w) * 4};
            recording::applyMasks(view, request.edit, us);
            post(new ReviewFrame{std::move(frame), us, playing, ended, request.id});
        };
        if (reader.seek(request.from)) continue;
        long long time = 0, previousTime = -1;
        Error error;
        if (request.kind == Request::Seek) {
            // The frame at or after the time; past the clip's end, its last frame.
            bool found = false;
            while (current() && reader.next(bits, time, error)) {
                if (time + 5 >= request.from * 10) { found = true; break; }
                previous.swap(bits); previousTime = time;
            }
            if (!current()) continue;
            if (found) deliver(bits, time / 10, false, false);
            else if (previousTime >= 0) deliver(previous, previousTime / 10, false, false);
            continue;
        }
        // Playback: frames are shown at their clip time; those behind the clock are
        // decoded but not drawn, unless nothing was drawn for a quarter second.
        const auto start = std::chrono::steady_clock::now();
        long long shownAt = -1;
        auto due = [&] { return request.from + (long long)(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count() * request.speed); };
        while (current() && reader.next(bits, time, error)) {
            const long long us = time / 10;
            if (us + 5 < request.from) continue;
            if (us >= request.end) break;
            if (us + 40000 < due() && shownAt >= 0 && us - shownAt < 250000) continue;
            while (current() && due() < us) {
                std::unique_lock lock(mutex_);
                wake_.wait_for(lock, std::chrono::milliseconds(4));
            }
            if (!current()) break;
            shownAt = us;
            deliver(bits, us, true, false);
        }
        // The end of the kept part (or of the clip): the window stops playing there.
        if (current()) post(new ReviewFrame{{}, std::max(request.from, request.end - 1), false, true, request.id});
    }
    if (SUCCEEDED(com)) CoUninitialize();
}
}
