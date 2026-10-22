#include "record/Recorder.h"
#include "common/Log.h"
#include "common/Paths.h"
#include "core/Audio.h"
#include "core/Recording.h"
#include <algorithm>
#include <audioclient.h>
#include <cmath>
#include <cstring>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <vector>

namespace putgd::record {
namespace {
constexpr int budgetCheckFrames = 15;
// KSDATAFORMAT_SUBTYPE_IEEE_FLOAT and _PCM, spelled out (ksmedia.h pulls in much more).
const GUID subtypeFloat{0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
const GUID subtypePcm{0x00000001, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

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

// How long the default output takes from the engine's mix to the speaker: a short
// silent shared-mode stream compares the frames the engine has taken with the
// endpoint clock's play position (the median of 15 readings, 100-ns units). A
// player times its picture to when its sound is heard, and loopback hands over the
// mix, so loopback samples are that much early.
long long outputLatency() {
    IMMDeviceEnumerator* enumerator = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator)))) return 0;
    IMMDevice* device = nullptr;
    const HRESULT found = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    enumerator->Release();
    if (FAILED(found)) return 0;
    IAudioClient* client = nullptr;
    const HRESULT activated = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client));
    device->Release();
    if (FAILED(activated)) return 0;
    std::vector<long long> readings;
    WAVEFORMATEX* mix = nullptr;
    IAudioRenderClient* render = nullptr;
    IAudioClock* clock = nullptr;
    UINT32 size = 0;
    UINT64 frequency = 0;
    if (SUCCEEDED(client->GetMixFormat(&mix)) && SUCCEEDED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 1000000, 0, mix, nullptr)) &&
        SUCCEEDED(client->GetBufferSize(&size)) && SUCCEEDED(client->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(&render))) &&
        SUCCEEDED(client->GetService(__uuidof(IAudioClock), reinterpret_cast<void**>(&clock))) && SUCCEEDED(clock->GetFrequency(&frequency)) && frequency > 0) {
        const long long rate = mix->nSamplesPerSec;
        unsigned long long written = 0;
        auto fill = [&] {
            UINT32 padding = 0; BYTE* data = nullptr;
            if (FAILED(client->GetCurrentPadding(&padding)) || padding >= size) return;
            if (SUCCEEDED(render->GetBuffer(size - padding, &data))) { render->ReleaseBuffer(size - padding, AUDCLNT_BUFFERFLAGS_SILENT); written += size - padding; }
        };
        fill();
        client->Start();
        for (int i = 0; i < 20; ++i) {
            Sleep(10);
            fill();
            UINT32 padding = 0; UINT64 position = 0, at = 0;
            if (i < 5 || FAILED(client->GetCurrentPadding(&padding)) || FAILED(clock->GetPosition(&position, &at))) continue;
            const long long taken = (long long)(written - padding) * 10000000 / rate;
            const long long played = (long long)(position * 10000000 / frequency) + (now100ns() - (long long)at);
            if (taken > played) readings.push_back(taken - played);
        }
        client->Stop();
    }
    if (clock) clock->Release();
    if (render) render->Release();
    if (mix) CoTaskMemFree(mix);
    client->Release();
    if (readings.empty()) return 0;
    std::nth_element(readings.begin(), readings.begin() + long(readings.size() / 2), readings.end());
    return std::min(readings[readings.size() / 2], 2000000LL); // at most 200 ms
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
long long Recorder::clock100ns() const { return now100ns() - origin_; }
Error Recorder::prepare(const CaptureTarget& target, const std::string& output, int fps, long long budgetBytes) {
    output_ = output; budget_ = budgetBytes; period_ = 10000000LL / std::max(1, fps);
    wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    ready_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    thread_ = std::thread([this, target, output, fps] { run(target, output, fps); });
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
    // Samples are placed where they are heard: the mix time plus the output's latency.
    const long long origin100 = origin_ - latency_;
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
void Recorder::run(CaptureTarget target, std::string output, int fps) {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    std::unique_ptr<Source> source;
    std::string note;
    if (target.window) openError_ = openWindowSource(target.window, target.width, target.height, source);
    else openError_ = openScreenSource(target.area, source, note);
    if (!openError_) {
        std::lock_guard l(mutex_);
        acquisition_ = source->name(); acquisitionNote_ = note;
    }
    const int width = target.window ? target.width : target.area.w, height = target.window ? target.height : target.area.h;
    media::Mp4Writer writer;
    SetEvent(ready_);
    auto cleanup = [&] { source.reset(); if (SUCCEEDED(com)) CoUninitialize(); };
    if (openError_) { cleanup(); return; }
    while (command_ == int(Command::None)) WaitForSingleObject(wake_, INFINITE);
    if (command_ != int(Command::Start)) { cleanup(); return; }

    // The encoder opens at Record, with the audio chosen by then.
    Loopback loopback;
    bool audio = audio_;
    if (audio) if (auto e = loopback.open()) { finishOwn("error", e); cleanup(); return; }
    if (audio) latency_ = outputLatency();
    const int audioRate = audio ? (audio::encoderRate(loopback.rate) ? loopback.rate : 48000) : 0;
    if (auto e = writer.open(output, {width, height, fps, recording::liveBitrate(width, height, fps), false, audioRate})) { finishOwn("error", e); cleanup(); return; }
    { std::lock_guard l(mutex_); encoder_ = writer.encoder(); audioEncoder_ = audio ? "Media Foundation AAC" : ""; }
    log::info("record", "Capture started", {{"acquisition", source->name()}, {"note", note}, {"size", json::Array{width, height}}, {"fps", fps}, {"outputLatencyUs", double(latency_ / 10)}});

    origin_ = now100ns();
    std::thread audioThread;
    if (audio) audioThread = std::thread([this, &loopback, &writer] { captureAudio(&loopback, writer); });
    std::vector<uint8_t> current(size_t(width) * height * 4);
    const size_t stride = size_t(width) * 4;
    bool have = false;
    long long frames = 0;
    std::string ownReason;
    auto emit = [&](long long index) {
        if (auto e = writer.write(current.data(), stride, index * period_, period_)) { finishOwn("error", e); return false; }
        lastFrame_ = index;
        if (budget_ > 0 && ++frames % budgetCheckFrames == 0 && fileBytes() >= budget_) { ownReason = "budget"; return false; }
        return true;
    };
    bool running = true;
    while (running && command_ == int(Command::Start) && stoppedReason().empty()) {
        if (paused_) { WaitForSingleObject(wake_, 100); continue; }
        const long long next = lastFrame_ + 1;
        const long long deadline = origin_ + pausedTotal_ + (2 * next + 1) * period_ / 2;
        if (have && now100ns() >= deadline) { running = emit(next); continue; }
        bool got = false;
        long long present = 0;
        if (auto e = source->next(deadline, wake_, got, present)) { finishOwn("error", e); break; }
        if (source->ended()) { ownReason = "target_closed"; break; }
        if (!got) continue;
        // Frames before this picture's slot keep the previous one.
        const long long slot = recording::frameSlot(present - origin_ - pausedTotal_, period_);
        while (running && have && !paused_ && lastFrame_ + 1 < slot) running = emit(lastFrame_ + 1);
        for (int y = 0; y < height; ++y) std::memcpy(current.data() + stride * size_t(y), source->bits() + source->stride() * size_t(y), stride);
        have = true;
    }
    // The audio thread finishes its track at the clip's end before the file is finalized.
    audioEnd_ = (lastFrame_ + 1) * period_;
    audioStop_ = true;
    if (audioThread.joinable()) audioThread.join();
    if (command_ == int(Command::Cancel) || (failure() && ownReason.empty())) writer.abandon();
    else {
        stopError_ = writer.finish();
        if (!ownReason.empty()) finishOwn(ownReason, stopError_);
    }
    log::info("record", "Capture ended", {{"frames", double(lastFrame_ + 1)}, {"encoder", encoder()}, {"acquisition", source->name()}, {"audio", audio},
        {"reason", !ownReason.empty() ? ownReason : command_ == int(Command::Cancel) ? "cancel" : "stop"}});
    cleanup();
}
}
