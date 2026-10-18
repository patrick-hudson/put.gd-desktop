#include "X11Targets.h"
#include "platform/x11/X11NativeSnapshot.h"
#include <QJsonArray>
#include <QSet>
#include <xcb/xcb.h>
#include <memory>

namespace putgd::record {
namespace {
QRect rectangle(const QJsonArray& a) { return a.size() == 4 ? QRect(a[0].toInt(), a[1].toInt(), a[2].toInt(), a[3].toInt()) : QRect(); }
// H.264 4:2:0 wants even dimensions; shrink by one pixel rather than grow into
// pixels the user did not select.
QRect evenRect(QRect r) {
    if (r.width() % 2) r.setWidth(r.width() - 1);
    if (r.height() % 2) r.setHeight(r.height() - 1);
    return r;
}
Error windowRect(unsigned long window, QRect& out) {
    int screen = 0;
    xcb_connection_t* connection = xcb_connect(nullptr, &screen);
    if (!connection || xcb_connection_has_error(connection)) { if (connection) xcb_disconnect(connection); return {"capture_unavailable", "Cannot connect to the X11 display", {}}; }
    const auto* setup = xcb_get_setup(connection);
    auto iterator = xcb_setup_roots_iterator(setup);
    for (int i = 0; i < screen && iterator.rem; ++i) xcb_screen_next(&iterator);
    const xcb_window_t root = iterator.data ? iterator.data->root : XCB_WINDOW_NONE;
    std::unique_ptr<xcb_get_window_attributes_reply_t, decltype(&free)> attributes(xcb_get_window_attributes_reply(connection, xcb_get_window_attributes(connection, xcb_window_t(window)), nullptr), free);
    std::unique_ptr<xcb_get_geometry_reply_t, decltype(&free)> geometry(xcb_get_geometry_reply(connection, xcb_get_geometry(connection, xcb_window_t(window)), nullptr), free);
    std::unique_ptr<xcb_translate_coordinates_reply_t, decltype(&free)> origin(geometry ? xcb_translate_coordinates_reply(connection, xcb_translate_coordinates(connection, xcb_window_t(window), root, 0, 0), nullptr) : nullptr, free);
    xcb_disconnect(connection);
    if (!attributes || attributes->map_state != XCB_MAP_STATE_VIEWABLE || !geometry || !origin || geometry->width < 2 || geometry->height < 2)
        return {"recording_target", "The window is not viewable on this display", {{"identity", QString::number(window)}}};
    out = QRect(origin->dst_x, origin->dst_y, geometry->width, geometry->height);
    return {};
}
}
Error resolveX11Target(const RecordingTarget& target, ResolvedTarget& out) {
    X11NativeSnapshot snapshot;
    if (!snapshot.available()) return {"capture_unavailable", "A native X11 session is unavailable", {}};
    Error error;
    const auto frozen = snapshot.freeze("desktop", QSet<qint64>{}, error);
    if (error) return error;
    const auto layout = frozen["layout"].toObject();
    const auto root = rectangle(layout["root"].toArray());
    ResolvedTarget resolved;
    resolved.layout = layout;
    if (target.kind == "desktop") resolved.rect = root;
    else if (target.kind == "monitor") {
        for (const auto& value : layout["outputs"].toArray()) {
            const auto output = value.toObject();
            if (output["identity"].toString() == target.identity || (target.identity.isEmpty() && (output["primary"].toBool() || resolved.rect.isEmpty()))) resolved.rect = rectangle(output["native"].toArray());
        }
        if (resolved.rect.isEmpty()) return {"recording_target", "No connected monitor has this identity", {{"identity", target.identity}}};
    } else if (target.kind == "region") {
        resolved.rect = target.rect;
        if (!root.contains(resolved.rect)) return {"recording_target", "The region must lie inside the X11 root window", {{"root", QJsonArray{root.x(), root.y(), root.width(), root.height()}}}};
    } else if (target.kind == "window") {
        bool ok = false;
        if (target.identity.isEmpty()) return {"recording_target", "Window targets need an X window id on X11", {}};
        const auto id = target.identity.startsWith("0x") ? target.identity.mid(2).toULong(&ok, 16) : target.identity.toULong(&ok, 10);
        if (!ok || id == 0) return {"recording_target", "Window identity must be a decimal or 0x-prefixed X window id", {{"identity", target.identity}}};
        if (auto e = windowRect(id, resolved.rect)) return e;
        resolved.window = id;
    } else return {"recording_unsupported", "Unknown target kind", {{"kind", target.kind}}};
    resolved.rect = evenRect(resolved.rect);
    if (resolved.rect.width() < 2 || resolved.rect.height() < 2) return {"recording_target", "The recorded area is too small", {}};
    resolved.canvas = resolved.rect.size();
    out = resolved;
    return {};
}
}
