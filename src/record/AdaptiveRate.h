#pragma once
#include <QString>
#include <QtGlobal>

namespace putgd::record {
// The adaptive frame rate (#984): a 1080p60 capture with its colour conversion
// and encoding can take the CPU the desktop being recorded needs, which then
// drops to a few frames per second while the clip still holds 60. The recorder
// feeds one window a second and lowers its rate (60 to 30, 30 to 15, never
// below 15) when the window shows the machine cannot keep up:
//   - encoder: the capture queue leaked frames because conversion or encoding
//     fell behind;
//   - capture: a fixed-rate source (ximagesrc) grabbed fewer frames than asked;
//   - cpu: the whole machine was busy while the recorder used half a core or more.
// Lateness at the encoder is no signal: videorate sends a static screen's
// repeats when the next frame arrives, up to a second late. A rate is lowered
// after `badWindows` such windows in a row, never in the first
// `warmupWindows` after the start or a change, and never raised again for
// that recording. Paused windows are not fed.
class AdaptiveRate {
public:
    struct Window {
        qint64 durationNs = 0;
        qint64 sourceFrames = 0;     // frames the source produced in the window
        qint64 leaked = 0;           // frames the capture queue leaked
        double systemBusy = -1;      // share of the machine's CPU time in use, 0..1, -1 unknown
        double recorderCores = -1;   // the recorder's own CPU in cores, -1 unknown
    };
    static constexpr int floorFps = 15;
    static constexpr int warmupWindows = 2;
    static constexpr int badWindows = 3;
    static constexpr double leakedShare = 0.05;
    static constexpr double captureShare = 0.8;
    static constexpr double busyShare = 0.9;
    static constexpr double recorderCoresBusy = 0.5;

    AdaptiveRate() = default;
    // fixedRateSource: the source produces every frame at the configured rate
    // (ximagesrc), so a shortfall means its grabs ran late; a compositor stream
    // only sends what changed and is never judged by its count.
    AdaptiveRate(int configuredFps, bool enabled, bool fixedRateSource);
    int configured() const { return configured_; }
    int current() const { return current_; }
    bool enabled() const { return enabled_; }
    bool reduced() const { return current_ != configured_; }
    // What the last reduction responded to: "encoder", "capture" or "cpu".
    QString reason() const { return reason_; }
    // The next lower rate, or fps itself at or below the floor.
    static int lower(int fps);
    // Why a window counts against the rate, or an empty string.
    QString judge(const Window&) const;
    // Feeds one window at the current rate; true when it lowered the rate.
    bool observe(const Window&);

private:
    int configured_ = 30, current_ = 30;
    bool enabled_ = false, fixedRateSource_ = false;
    int windows_ = 0, bad_ = 0;
    QString reason_;
};

// The whole machine's and this process's CPU time from /proc: busy is the
// share of the machine in use between two samples, process the cores this
// process used. Either is -1 when /proc cannot be read.
struct CpuSample { qint64 totalTicks = -1, idleTicks = -1, processTicks = -1; };
CpuSample readCpu();
void cpuShares(const CpuSample& before, const CpuSample& after, qint64 elapsedNs, double& systemBusy, double& processCores);
}
