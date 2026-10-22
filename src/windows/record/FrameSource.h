#pragma once
// Frames for the review window, decoded on a worker thread (a COM multithreaded
// apartment with its own Media Foundation reader) so the window never waits on
// the decoder. A seek decodes the frame at or after a time (the newest request
// wins); playback decodes in order from a time to an end at a speed, drawing
// frames when the clock reaches them and skipping those already behind it.
// Masks are applied with the export's rules before a frame is posted.
#include "core/Recording.h"
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <windows.h>

namespace putgd::record {
struct ReviewFrame {
    std::vector<uint8_t> bits; // top-down BGRX at the canvas size
    long long timeUs = 0;
    bool playing = false, ended = false;
    unsigned long long request = 0;
};
class FrameSource {
public:
    // Frames arrive as PostMessage(window, message, 0, ReviewFrame*); the receiver deletes them.
    FrameSource(HWND window, UINT message, std::string clip, RecordingSize canvas);
    ~FrameSource();
    FrameSource(const FrameSource&) = delete;
    FrameSource& operator=(const FrameSource&) = delete;
    unsigned long long seek(long long us, const RecordingEdit&);
    unsigned long long play(long long fromUs, long long endUs, double speed, const RecordingEdit&);
    void stop();
private:
    struct Request { enum Kind { None, Seek, Play } kind = None; long long from = 0, end = 0; double speed = 1; RecordingEdit edit; unsigned long long id = 0; };
    void run();
    void post(ReviewFrame*);
    HWND window_; UINT message_;
    std::string clip_; RecordingSize canvas_;
    std::mutex mutex_;
    std::condition_variable wake_;
    Request pending_;
    unsigned long long next_ = 0;
    bool quit_ = false;
    std::thread thread_;
};
}
