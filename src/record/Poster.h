#pragma once
#include "core/Error.h"
#include <QImage>
#include <QString>

namespace putgd::record {
// History's poster frame for a finished video (#112): one still, decoded from
// the file as stored, so blur and cover masks baked into an export stay
// applied. The input is untrusted (a user-shared file is as likely as one of
// our recordings): only the local file protocol and the MP4/QuickTime,
// Matroska/WebM, AVI and Ogg demuxers are allowed, so a crafted file can never
// make FFmpeg open a playlist, a URL or another file.
struct Poster {
    QImage image;           // long edge at most maxEdge, rotation applied
    qint64 durationUs = 0;  // 0 when the container does not say
    qint64 frameUs = 0;     // time of the chosen frame
    QSize size;             // coded frame size
};
// Picks a frame about 1 s in (half-way into shorter clips), then 25% and 50%
// in when that frame is flat (black, a fade or a solid fill), then the first
// decodable frame. Frames larger than maxPixels are refused before decoding.
Error renderPoster(const QString& input, int maxEdge, qint64 maxPixels, Poster& out);
}
