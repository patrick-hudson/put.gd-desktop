#pragma once
#include "WaylandAcquisition.h"
#include <QRect>
#include <QSize>
#include <QString>
#include <algorithm>

namespace putgd::record {
// The GStreamer source descriptions the recording pipeline and the scroll
// capture's frame tap (#1376) share, so both read a desktop the same way. The
// PipeWire source for a screen-cast node is pipewireSourceDescription
// (WaylandAcquisition.h). Each names its element `src`.

// ximagesrc on a root rectangle, or on one window (`window` != 0), at `fps`.
// It reads every visible pixel of the area; `showPointer` draws the cursor in.
inline QString x11SourceDescription(const QRect& rect, unsigned long window, int fps, bool showPointer) {
    const QString pointer = showPointer ? QStringLiteral("true") : QStringLiteral("false");
    if (window) return QStringLiteral("ximagesrc name=src xid=%1 use-damage=false show-pointer=%2 ! video/x-raw,framerate=%3/1").arg(window).arg(pointer).arg(fps);
    return QStringLiteral("ximagesrc name=src use-damage=false show-pointer=%1 startx=%2 starty=%3 endx=%4 endy=%5 ! video/x-raw,framerate=%6/1")
        .arg(pointer).arg(rect.x()).arg(rect.y()).arg(rect.x() + rect.width() - 1).arg(rect.y() + rect.height() - 1).arg(fps);
}
// appsrc fed by ImageCopySource (ext-image-copy-capture frames), with its caps.
inline QString imageCopySourceDescription(const QString& caps) {
    return QStringLiteral("appsrc name=src is-live=true format=time do-timestamp=false leaky-type=downstream max-buffers=2 caps=\"%1\"").arg(caps);
}
// videocrop cutting `crop` (stream pixels) out of frames of `stream` size.
inline QString cropDescription(const QSize& stream, const QRect& crop) {
    return QStringLiteral(" ! videocrop left=%1 top=%2 right=%3 bottom=%4").arg(crop.x()).arg(crop.y())
        .arg(std::max(0, stream.width() - crop.x() - crop.width())).arg(std::max(0, stream.height() - crop.y() - crop.height()));
}
}
