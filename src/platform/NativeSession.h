#pragma once
#include <QtCore/QString>
#include <QtCore/QByteArray>
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
// GNOME's resident bridge uses the session bus without a Qt GUI connection.
// Keep ambiguous sessions and explicit graphical overrides on QApplication.
inline bool gnomeCoreSessionEnvironment() {
 if(qEnvironmentVariable("XDG_SESSION_TYPE")!="wayland"||qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY"))return false;
 const auto platform=qEnvironmentVariable("QT_QPA_PLATFORM");
 if(platform.contains(';'))return false;
 const auto plugin=platform.section(':',0,0);
 if(!platform.isEmpty()&&plugin!="wayland"&&plugin!="wayland-egl")return false;
 bool gnome=false;
 for(const auto& desktop:qEnvironmentVariable("XDG_CURRENT_DESKTOP").split(':',Qt::SkipEmptyParts)){
  if(desktop.compare("KDE",Qt::CaseInsensitive)==0||desktop.compare("Cinnamon",Qt::CaseInsensitive)==0||desktop.compare("X-Cinnamon",Qt::CaseInsensitive)==0)return false;
  if(desktop.compare("GNOME",Qt::CaseInsensitive)==0)gnome=true;
 }
 return gnome;
}
inline bool gnomeCoreSessionArguments(int argc,const char* const* argv) {
 for(int i=1;i<argc;++i){
  const QByteArray argument(argv[i]);
  if(argument=="-platform"||argument=="--platform"||argument.startsWith("-platform=")||argument.startsWith("--platform="))return false;
 }
 return true;
}
}
