#pragma once
#include "core/Error.h"
#include "core/RecordingModel.h"
#include <QJsonObject>
#include <QRect>

namespace putgd::record {
// What ximagesrc needs for one target on the X11 root: a root-relative
// rectangle (even-sized, inside the root) or a viewable window id. Resolved
// from the current RandR layout; no pixels are read.
struct ResolvedTarget {
    QRect rect;               // root coordinates of the recorded area
    unsigned long window = 0; // XID for window targets, else 0
    QSize canvas;             // encoded frame size (even dimensions)
    QJsonObject layout;       // frozen RandR layout for diagnostics
};
Error resolveX11Target(const RecordingTarget&, ResolvedTarget&);
}
