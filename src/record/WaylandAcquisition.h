#pragma once
#include <QProcessEnvironment>
#include <QString>
#include <QtGlobal>

namespace putgd::record {
// The session PipeWire daemon's socket as `pipewiresrc` would find it (the
// `PIPEWIRE_REMOTE` name under `PIPEWIRE_RUNTIME_DIR`, `XDG_RUNTIME_DIR` or
// `USERPROFILE`), or an empty string. The compositor-native Wayland paths hand
// over a node id and need it; the portal passes its own connection instead.
QString pipewireSocket(const QProcessEnvironment& env);

// A screen-cast frame's PipeWire header PTS is the compositor's own frame time
// in CLOCK_MONOTONIC nanoseconds (Mutter: when it painted the frame; KWin: the
// output's last presentation). Converts it to pipeline running time from a
// reading of both clocks taken together: running = runningNow - (monotonicNow
// - framePts). A missing time, one from the future, or one older than
// kMaxFrameAgeNs (KWin's presentation time goes stale on an idle output)
// falls back to runningNow, the arrival time. The result never goes back past
// `previous` (-1 for the first frame), so videorate sees increasing times.
inline constexpr qint64 kMaxFrameAgeNs = 100'000'000;
inline constexpr qint64 kFutureSlackNs = 5'000'000;
struct FrameTime { qint64 running = 0; qint64 ageNs = -1; };  // ageNs -1: arrival time used
FrameTime pipewireFrameTime(qint64 framePts, qint64 monotonicNow, qint64 runningNow, qint64 previous);
}
