#pragma once
// The recording helper's capture engine: one thread (a COM multithreaded
// apartment) takes pictures from a source (record/Sources.h) and feeds the MP4
// writer at the configured rate; with system audio, a second thread drains a
// WASAPI loopback client on the default output device into the AAC track.
// Frame and sample times are on the clip timeline: the time since Record minus
// the paused time, which pause() and resume() keep as a whole number of frames,
// so a pause removes the same interval from both tracks and frames stay on the
// export's frame grid. Frame k shows the newest picture that reached the screen
// by (k + 1/2) frame periods, by the source's present time: centred on its
// instant, so the video is on average neither late nor early against the sound.
#include "capture/Grab.h"
#include "record/Media.h"
#include "record/Sources.h"
#include <atomic>
#include <mutex>
#include <string>
#include <thread>

namespace putgd::record {
// Whether system audio can be recorded here: a default output device whose mix
// format loopback capture takes. Fills a reason when not.
bool loopbackAvailable(std::string& reason);

// What to record: a screen area, or a window into a width x height canvas.
struct CaptureTarget { grab::Rect area; HWND window = nullptr; int width = 0, height = 0; };

class Recorder {
public:
    Recorder() = default;
    ~Recorder();
    Recorder(const Recorder&) = delete;
    Recorder& operator=(const Recorder&) = delete;
    // Opens the source on the capture thread and waits until it is ready; the
    // encoder opens at start(), with the audio choice made by then.
    Error prepare(const CaptureTarget&, const std::string& output, int fps, long long budgetBytes);
    void setAudio(bool audio) { audio_ = audio; }
    void start();
    void pause();
    void resume();
    // Ends the clip now, finalizes the file and joins the threads.
    Error stop(long long& durationUs);
    // Discards the clip without finalizing; joins the threads.
    void cancel();
    long long durationUs() const { return lastFrame_ < 0 ? 0 : (lastFrame_ + 1) * period_ / 10; }
    bool paused() const { return paused_; }
    // The source in use (gdi, dxgi, wgc) and why the preferred one was not.
    std::string acquisition() const { std::lock_guard l(mutex_); return acquisition_; }
    std::string acquisitionNote() const { std::lock_guard l(mutex_); return acquisitionNote_; }
    // Set once the capture stopped on its own: "budget", "target_closed" or "error".
    std::string stoppedReason() const { std::lock_guard l(mutex_); return stopped_; }
    Error failure() const { std::lock_guard l(mutex_); return failure_; }
    std::string encoder() const { std::lock_guard l(mutex_); return encoder_; }
    std::string audioEncoder() const { std::lock_guard l(mutex_); return audioEncoder_; }
    long long outputLatencyUs() const { return latency_ / 10; }
    long long fileBytes() const;
private:
    enum class Command { None, Start, Stop, Cancel };
    void run(CaptureTarget target, std::string output, int fps);
    void captureAudio(void* client, media::Mp4Writer& writer);
    void finishOwn(const std::string& reason, Error e = {});
    long long clock100ns() const; // time since Record, before pauses are removed
    std::thread thread_;
    std::string output_, encoder_, audioEncoder_, acquisition_, acquisitionNote_;
    long long budget_ = 0, period_ = 333333;
    std::atomic<long long> latency_{0}; // the output's, added to loopback times
    std::atomic<long long> lastFrame_{-1}, pausedTotal_{0}, pauseStarted_{-1}, origin_{0}, audioEnd_{-1};
    std::atomic<bool> paused_{false}, audio_{false}, audioStop_{false};
    std::atomic<int> command_{0};
    HANDLE wake_ = nullptr, ready_ = nullptr;
    Error openError_;
    mutable std::mutex mutex_;
    std::string stopped_;
    Error failure_;
    Error stopError_;
};
}
