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
// output's last presentation). The recorder stamps frames on arrival and uses
// this for the frame age in its lab log and for the `compositor` lab mode.
// Converts it to pipeline running time from a
// reading of both clocks taken together: running = runningNow - (monotonicNow
// - framePts). A missing time, one from the future, or one older than
// kMaxFrameAgeNs (KWin's presentation time goes stale on an idle output)
// falls back to runningNow, the arrival time. The result never goes back past
// `previous` (-1 for the first frame), so videorate sees increasing times.
// The pipeline's source for a screen-cast node: `element` (pipewiresrc or the
// recorder's subclass) on the portal's remote `fd` (-1: the session daemon).
// min-buffers=1 lets the compositor choose the buffer count. Without it the
// PipeWire 1.0 plugin in the AppImage (Ubuntu 24.04's) asks for 8 or more and
// KWin offers 2-4, which PipeWire 1.5.81 and later refuse when linking the two
// ("error alloc buffers: Invalid argument", #137); newer plugins default to 1.
QString pipewireSourceDescription(const QString& element, quint32 node, int fd, bool doTimestamp);

inline constexpr qint64 kMaxFrameAgeNs = 100'000'000;
inline constexpr qint64 kFutureSlackNs = 5'000'000;
struct FrameTime { qint64 running = 0; qint64 ageNs = -1; };  // ageNs -1: arrival time used
FrameTime pipewireFrameTime(qint64 framePts, qint64 monotonicNow, qint64 runningNow, qint64 previous);

// KWin 6.7 defers a screen-cast frame that arrives within an integer
// millisecond interval of its previous send and stamps that send after its
// synchronous readback, so a 60 Hz stream yields about 48 frames per second
// and the arrivals jitter by a frame. The throttle only runs when the
// negotiated max_framerate is non-zero, and KWin offers 0/1 as the minimum:
// this capsfilter asks for it. A compositor whose range excludes 0/1 refuses
// the link, and the pipeline retries without the filter.
inline QString pipewireUnthrottledCaps() { return QStringLiteral(" ! video/x-raw,max-framerate=0/1"); }

// Arrival times carry the delivery jitter (readback, deferral, scheduling):
// about +-8 ms on KWin for a 16.7 ms frame, enough for videorate to put two
// frames in one slot and none in the next (a repeat and a skip, 5 of 60 per
// second). The cadence lock stamps each frame with new content on the slot
// after the previous stamp as long as it arrived within `tolerance` of that
// slot; a frame arriving earlier than that is an extra (a cursor move, a
// second render before one presentation) and is dropped (-1); one arriving
// later resynchronises to its arrival. Stamps therefore follow arrivals on
// average (the audio offset #86 measured stays) and sit on the grid between
// resyncs. `previous` -1 takes the arrival.
struct FrameLock { qint64 periodNs = 0; qint64 toleranceNs = 0; };
inline FrameLock frameLock(int fps) { const qint64 period = fps > 0 ? 1'000'000'000 / fps : 0; return {period, period * 7 / 10}; }
qint64 lockedFrameTime(qint64 arrivalRunning, qint64 previous, const FrameLock& lock);
}
