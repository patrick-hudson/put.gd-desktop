#pragma once
#include "core/Error.h"
#include <QJsonObject>
#include <functional>

namespace putgd {
// The desktop's external windows where the compositor names windows itself
// (KWin's window UUIDs, Mutter's window ids): `recording.windows` lists them
// and a window recording resolves its target from them before the helper
// starts, so the stream follows that window whatever takes focus later.
// put.gd's own windows never appear (the desktop's capture rules). Unset on
// X11, where the helper resolves a window itself, and on other desktops.
class RecordingWindows {
public:
    using Reply = std::function<void(Error, QJsonObject)>;
    virtual ~RecordingWindows() = default;
    // {windows: [{identity, title, appId, rect: [x, y, w, h], active, minimized}],
    //  activeExcluded: the focused window is put.gd's own}. helper_outdated when
    // the desktop's integration loaded at login predates window recording.
    virtual void listWindows(Reply) = 0;
};
}
