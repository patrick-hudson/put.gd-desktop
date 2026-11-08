#pragma once
#include "core/Error.h"
#include <QList>
#include <QString>
#include <optional>

namespace putgd {
// A window as the toplevel protocols describe it. ext-foreign-toplevel-list-v1
// gives the identifier capture uses; wlr-foreign-toplevel-management gives the
// activated state. Neither gives a PID or geometry.
struct WaylandToplevel {
 QString identifier,appId,title;
 bool activated=false;
};
// The active window: the one ext handle whose app ID and title match the one
// activated wlr handle. Anything but exactly one match is capture_target.
inline std::optional<WaylandToplevel> matchActiveToplevel(const QList<WaylandToplevel>& ext,const QList<WaylandToplevel>& wlr,Error& error){
 error={};
 QList<WaylandToplevel> active;for(const auto& window:wlr)if(window.activated)active.append(window);
 if(active.size()!=1){error={"capture_target",active.isEmpty()?"No window is active":"More than one window reports itself active",{{"active",int(active.size())}}};return {};}
 QList<WaylandToplevel> matches;
 for(const auto& window:ext)if(window.appId==active.first().appId&&window.title==active.first().title&&!window.identifier.isEmpty())matches.append(window);
 if(matches.size()!=1){error={"capture_target",matches.isEmpty()?"The active window has no capturable match":"More than one window matches the active window's app ID and title",{{"matches",int(matches.size())}}};return {};}
 auto match=matches.first();match.activated=true;return match;
}
// put.gd's own windows, by app ID (Wayland gives no PID for a toplevel).
inline bool putgdAppId(const QString& appId){
 return appId=="putgd"||appId=="putgd-ui"||appId=="putgd-record"||appId=="putgd-region";
}
}
