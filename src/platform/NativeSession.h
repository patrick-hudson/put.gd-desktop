#pragma once
#include <QtCore/QString>
#include <QtCore/QProcessEnvironment>

namespace putgd {
// Environment is a routing guard, not proof of a working display. Each native
// adapter must independently verify its connection and required extensions.
inline bool nativeX11SessionEnvironment() {
 const auto type=qEnvironmentVariable("XDG_SESSION_TYPE");
 const auto platform=qEnvironmentVariable("QT_QPA_PLATFORM").section(':',0,0);
 return !qEnvironmentVariableIsEmpty("DISPLAY") && qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY") &&
  (type.isEmpty()||type=="x11") && (platform.isEmpty()||platform=="xcb");
}
inline bool cinnamonCoreSessionEnvironment() {
 if(!nativeX11SessionEnvironment())return false;
 for(const auto& desktop:qEnvironmentVariable("XDG_CURRENT_DESKTOP").split(':',Qt::SkipEmptyParts))
  if(desktop.compare("Cinnamon",Qt::CaseInsensitive)==0||desktop.compare("X-Cinnamon",Qt::CaseInsensitive)==0)return true;
 return false;
}
}
