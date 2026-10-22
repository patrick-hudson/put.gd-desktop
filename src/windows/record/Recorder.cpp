#include "record/Recorder.h"
#include "common/Log.h"
#include "common/Paths.h"
#include "core/Audio.h"
#include "core/Recording.h"
#include <audioclient.h>
#include <cmath>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <vector>

namespace putgd::record {
namespace {
constexpr int budgetCheckFrames = 15;
// KSDATAFORMAT_SUBTYPE_IEEE_FLOAT and _PCM, spelled out (ksmedia.h pulls in much more).
const GUID subtypeFloat{0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
const GUID subtypePcm{0x00000001, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
long long frequency() { static const long long f = [] { LARGE_INTEGER v; QueryPerformanceFrequency(&v); return v.QuadPart; }(); return f; }
long long qpc() { LARGE_INTEGER v; QueryPerformanceCounter(&v); return v.QuadPart; }
// QPC ticks to 100-ns units without overflowing.
long long hundredNs(long long ticks) { const long long f = frequency(); return ticks / f * 10000000 + ticks % f * 10000000 / f; }

// A loopback capture client on the default output device and how its samples look.
struct Loopback {
    IAudioClient* client = nullptr;
    IAudioCaptureClient* capture = nullptr;
    int rate = 0, channels = 0;
    bool isFloat = false;
    ~Loopback() { if (capture) capture->Release(); if (client) { client->Stop(); client->Release(); } }
    Error open() {
        IMMDeviceEnumerator* enumerator = nullptr;
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator)))) return {"recording_audio_unavailable", "Windows audio is not available", {}};
        IMMDevice* device = nullptr;
        HRESULT hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
        enumerator->Release();
        if (FAILED(hr)) return {"recording_audio_unavailable", "No audio output device is available", {}};
        hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client));
        device->Release();
        if (FAILED(hr)) return {"recording_audio_unavailable", "The audio output device cannot be opened", {}};
        WAVEFORMATEX* mix = nullptr;
        if (FAILED(client->GetMixFormat(&mix))) return {"recording_audio_unavailable", "The audio output device has no mix format", {}};
        rate = int(mix->nSamplesPerSec); channels = int(mix->nChannels);
        const bool extensible = mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE;
        const GUID sub = extensible ? reinterpret_cast<WAVEFORMATEXTENSIBLE*>(mix)->SubFormat : GUID{};
        isFloat = (mix->wFormatTag == WAVE_FORMAT_IEEE_FLOAT || (extensible && sub == subtypeFloat)) && mix->wBitsPerSample == 32;
        const bool pcm16 = (mix->wFormatTag == WAVE_FORMAT_PCM || (extensible && sub == subtypePcm)) && mix->wBitsPerSample == 16;
        if (!isFloat && !pcm16) { CoTaskMemFree(mix); return {"recording_audio_unavailable", "The audio output device's format is not supported", {{"bits", int(mix->wBitsPerSample)}}}; }
        // A 200 ms buffer, drained every 10 ms.
        hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, 2000000, 0, mix, nullptr);
        CoTaskMemFree(mix);
        if (FAILED(hr)) return {"recording_audio_unavailable", "Loopback capture is not available on the output device", {}};
        if (FAILED(client->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(&capture)))) return {"recording_audio_unavailable", "Loopback capture is not available on the output device", {}};
        return {};
    }
};
}

bool loopbackAvailable(std::string& reason) {
    Loopback loopback;
    if (auto e = loopback.open()) { reason = e.message; return false; }
    return true;
}

Recorder::~Recorder() { cancel(); if (wake_) CloseHandle(wake_); if (ready_) CloseHandle(ready_); }
long long Recorder::fileBytes() const {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (output_.empty() || !GetFileAttributesExW(paths::wide(output_).c_str(), GetFileExInfoStandard, &data)) return 0;
    return (long long)(((unsigned long long)data.nFileSizeHigh << 32) | data.nFileSizeLow);
}
long long Recorder::clock100ns() const { return hundredNs(qpc() - origin_); }
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
void Recorder::pause() {
    if (paused_) return;
    pauseStarted_ = clock100ns();
    paused_ = true;
    SetEvent(wake_);
}
void Recorder::resume() {
    if (!paused_) return;
    // Remove a whole number of frames, so later frames stay on the frame grid and
    // the audio moves by exactly as much.
    const long long paused = clock100ns() - pauseStarted_;
    pausedTotal_ += std::llround(double(paused) / double(period_)) * period_;
    pauseStarted_ = -1;
    paused_ = false;
    SetEvent(wake_);
}
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
void Recorder::captureAudio(void* source, media::Mp4Writer& writer) {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    auto& loopback = *static_cast<Loopback*>(source);
    const int outRate = audio::encoderRate(loopback.rate) ? loopback.rate : 48000;
    audio::Resampler resampler(loopback.rate, outRate);
    audio::Timeline timeline(outRate);
    std::vector<int16_t> stereo, resampled;
    log::info("record", "System audio capture", {{"rate", loopback.rate}, {"channels", loopback.channels}, {"float", loopback.isFloat}});
    auto write = [&](const audio::Timeline::Piece& piece) {
        if (piece.samples.empty()) return true;
        if (auto e = writer.writeAudio(piece.samples.data(), piece.samples.size() / 2, piece.start * 10000000 / outRate)) { finishOwn("error", e); return false; }
        return true;
    };
    const long long origin100 = hundredNs(origin_);
    loopback.client->Start();
    bool ok = true;
    while (ok && !audioStop_) {
        Sleep(10);
        UINT32 packet = 0;
        bool got = false;
        while (ok && SUCCEEDED(loopback.capture->GetNextPacketSize(&packet)) && packet > 0) {
            BYTE* data = nullptr; UINT32 frames = 0; DWORD flags = 0; UINT64 position = 0, qpcPosition = 0;
            if (FAILED(loopback.capture->GetBuffer(&data, &frames, &flags, &position, &qpcPosition))) break;
            const long long time = (long long)qpcPosition - origin100; // u64QPCPosition is in 100-ns units
            const long long pauseStart = pauseStarted_;
            const bool drop = time < 0 || (paused_ && pauseStart >= 0 && time >= pauseStart);
            if (!drop && frames > 0) {
                if (flags & AUDCLNT_BUFFERFLAGS_SILENT) stereo.assign(size_t(frames) * 2, 0);
                else if (loopback.isFloat) audio::toStereo16(reinterpret_cast<const float*>(data), frames, loopback.channels, stereo);
                else audio::toStereo16(reinterpret_cast<const int16_t*>(data), frames, loopback.channels, stereo);
                const auto& samples = outRate == loopback.rate ? stereo : (resampler.process(stereo, resampled), resampled);
                ok = write(timeline.place(audio::Timeline::frameAt(time - pausedTotal_, outRate), samples));
                got = true;
            }
            loopback.capture->ReleaseBuffer(frames);
        }
        // Nothing plays: loopback sends nothing, so the clip gets silence, 100 ms behind
        // the clock so a late packet still finds its place.
        if (!got && !paused_) {
            const long long now = clock100ns() - pausedTotal_;
            if (now - timeline.written() * 10000000 / outRate > 2000000) ok = write(timeline.fillTo(audio::Timeline::frameAt(now - 1000000, outRate)));
        }
    }
    // The audio track ends where the video does.
    const long long end = audioEnd_;
    if (ok && end > 0) write(timeline.fillTo(audio::Timeline::frameAt(end, outRate)));
    if (SUCCEEDED(com)) CoUninitialize();
}
void Recorder::run(grab::Rect area, std::string output, int fps) {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    HDC screen = GetDC(nullptr);
    grab::Image image;
    media::Mp4Writer writer;
    if (!screen || !image.create(area.w, area.h)) openError_ = {"recording_failed", "Cannot open the screen for recording", {}};
    SetEvent(ready_);
    auto cleanup = [&] { if (screen) ReleaseDC(nullptr, screen); image.release(); if (SUCCEEDED(com)) CoUninitialize(); };
    if (openError_) { cleanup(); return; }
    while (command_ == int(Command::None)) WaitForSingleObject(wake_, INFINITE);
    if (command_ != int(Command::Start)) { cleanup(); return; }

    // The encoder opens at Record, with the audio chosen by then.
    Loopback loopback;
    bool audio = audio_;
    if (audio) if (auto e = loopback.open()) { finishOwn("error", e); cleanup(); return; }
    const int audioRate = audio ? (audio::encoderRate(loopback.rate) ? loopback.rate : 48000) : 0;
    if (auto e = writer.open(output, {area.w, area.h, fps, recording::liveBitrate(area.w, area.h, fps), false, audioRate})) { finishOwn("error", e); cleanup(); return; }
    { std::lock_guard l(mutex_); encoder_ = writer.encoder(); audioEncoder_ = audio ? "Media Foundation AAC" : ""; }

    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!timer) timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
    origin_ = qpc();
    std::thread audioThread;
    if (audio) audioThread = std::thread([this, &loopback, &writer] { captureAudio(&loopback, writer); });
    long long frames = 0;
    bool ownStop = false;
    while (command_ == int(Command::Start) && stoppedReason().empty()) {
        if (paused_) { WaitForSingleObject(wake_, 100); continue; }
        const long long clip = clock100ns() - pausedTotal_;
        const long long index = std::max(0LL, std::llround(double(clip) / double(period_)));
        if (index > lastFrame_) {
            if (!BitBlt(image.dc(), 0, 0, area.w, area.h, screen, area.x, area.y, SRCCOPY | CAPTUREBLT)) { finishOwn("error", {"recording_failed", "The screen could not be read", {}}); break; }
            GdiFlush();
            if (auto e = writer.write(image.bits(), image.stride(), index * period_, period_)) { finishOwn("error", e); break; }
            lastFrame_ = index;
            if (budget_ > 0 && ++frames % budgetCheckFrames == 0 && fileBytes() >= budget_) { ownStop = true; break; }
        }
        // Sleep until the next frame boundary, or a command.
        const long long due = (lastFrame_ + 1) * period_ - (clock100ns() - pausedTotal_);
        if (due > 0 && timer) {
            LARGE_INTEGER relative; relative.QuadPart = -due;
            SetWaitableTimer(timer, &relative, 0, nullptr, nullptr, FALSE);
            HANDLE handles[2] = {timer, wake_};
            WaitForMultipleObjects(2, handles, FALSE, INFINITE);
        }
    }
    if (timer) CloseHandle(timer);
    // The audio thread finishes its track at the clip's end before the file is finalized.
    audioEnd_ = (lastFrame_ + 1) * period_;
    audioStop_ = true;
    if (audioThread.joinable()) audioThread.join();
    if (command_ == int(Command::Cancel) || (failure() && !ownStop)) writer.abandon();
    else {
        stopError_ = writer.finish();
        if (ownStop) finishOwn("budget", stopError_);
    }
    log::info("record", "Capture ended", {{"frames", double(lastFrame_ + 1)}, {"encoder", encoder()}, {"audio", audio}, {"reason", ownStop ? "budget" : command_ == int(Command::Cancel) ? "cancel" : "stop"}});
    cleanup();
}
}
