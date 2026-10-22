#pragma once
// The recording helper's capture engine: one thread (a COM multithreaded
// apartment) that grabs the target area with GDI BitBlt at the configured rate
// and feeds the H.264 MP4 writer. Frame times are on the clip timeline: each
// frame is stamped with the nearest frame boundary of the time since Record
// minus the paused time, so a pause removes exactly its own length and frames
// stay on the export's frame grid. The DIB section is reused for every frame.
#include "capture/Grab.h"
#include "record/Media.h"
#include <atomic>
#include <mutex>
#include <string>
#include <thread>

namespace putgd::record {
class Recorder {
public:
    Recorder() = default;
    ~Recorder();
    Recorder(const Recorder&) = delete;
    Recorder& operator=(const Recorder&) = delete;
    // Opens the source and the encoder on the capture thread and waits until
    // both are ready; nothing is captured before start().
    Error prepare(const grab::Rect& area, const std::string& output, int fps, long long budgetBytes);
    void start();
    void pause();
    void resume();
    // Ends the clip now, finalizes the file and joins the thread.
    Error stop(long long& durationUs);
    // Discards the clip without finalizing; joins the thread.
    void cancel();
    long long durationUs() const { return lastFrame_ < 0 ? 0 : (lastFrame_ + 1) * period_ / 10; }
    bool paused() const { return paused_; }
    // Set once the capture stopped on its own: "budget" or "error".
    std::string stoppedReason() const { std::lock_guard l(mutex_); return stopped_; }
    Error failure() const { std::lock_guard l(mutex_); return failure_; }
    const std::string& encoder() const { return encoder_; }
    long long fileBytes() const;
private:
    enum class Command { None, Start, Stop, Cancel };
    void run(grab::Rect area, std::string output, int fps);
    void finishOwn(const std::string& reason, Error e = {});
    std::thread thread_;
    std::string output_, encoder_;
    long long budget_ = 0, period_ = 333333;
    std::atomic<long long> lastFrame_{-1};
    std::atomic<bool> paused_{false};
    std::atomic<int> command_{0};
    HANDLE wake_ = nullptr, ready_ = nullptr;
    Error openError_;
    mutable std::mutex mutex_;
    std::string stopped_;
    Error failure_;
    Error stopError_;
    bool finalized_ = false;
};
}
