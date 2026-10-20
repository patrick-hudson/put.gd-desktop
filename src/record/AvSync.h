#pragma once
#include "core/Error.h"
#include <QJsonObject>
#include <QString>

namespace putgd::record {
// Lab diagnostic for system audio: a recording of a synthetic marker clip
// (white flashes with simultaneous tone bursts, one per period) is decoded in
// one demux pass. Flash onsets are rising edges of the mean luma, burst onsets
// rising edges of the 2 ms RMS envelope, both in container time. Each flash is
// paired with the nearest burst within half a period; the offsets (audio minus
// video, positive when the sound is late) are the clip's A/V sync.
// Result: {flashes, bursts, pairs, offsetsMs[], medianMs, minMs, maxMs,
// spreadMs, videoFrames, audioSeconds, peak}.
Error measureAvSync(const QString& clip, QJsonObject& result);
// Writes the marker clip (Matroska, MJPEG 1280x720 at 30 fps and 48 kHz PCM):
// black frames with a white flash and a 1 kHz tone burst, both 100 ms long,
// at the start of every second. Only encoders every FFmpeg build has.
// audioDelayMs starts the audio stream that much later, as a capture's
// working clip does, with the bursts still on the whole seconds (tests).
// audioGapMs leaves that much audio out at audioGapAtMs, a hole in the audio
// timestamps such as a pause can leave, the bursts still on the whole seconds.
Error writeAvMarker(const QString& path, int seconds, int audioDelayMs = 0, int audioGapAtMs = -1, int audioGapMs = 0);
}
