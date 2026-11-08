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
// A Budgie session names Budgie in XDG_CURRENT_DESKTOP; forge test sessions use
// Budgie:GNOME. Budgie always wins over the GNOME token it may carry.
inline bool desktopNamesBudgie() {
 for(const auto& desktop:qEnvironmentVariable("XDG_CURRENT_DESKTOP").split(':',Qt::SkipEmptyParts))
  if(desktop.compare("Budgie",Qt::CaseInsensitive)==0)return true;
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
 if(desktopNamesBudgie())return false;
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
  // Only the resident's existing parser flags are safe before QCoreApplication.
  // Delegate every other token to QApplication, which consumes its own GUI
  // options before the app parser runs. Keep this guard GNOME-specific.
  if(argument!="--background"&&argument!="--history"&&argument!="--activity"&&
     argument!="-h"&&argument!="--help"&&argument!="--help-all"&&
     argument!="-v"&&argument!="--version")return false;
 }
 return true;
}
// KDE owns independent native Wayland connections in its Core route. Do not
// route mixed desktops or Qt GUI command-line overrides through that route.
inline bool kdeCoreSessionEnvironment() {
 if(qEnvironmentVariable("XDG_SESSION_TYPE")!="wayland"||qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY"))return false;
 const auto platform=qEnvironmentVariable("QT_QPA_PLATFORM");
 if(platform.contains(';'))return false;
 const auto plugin=platform.section(':',0,0);
 if(!platform.isEmpty()&&plugin!="wayland"&&plugin!="wayland-egl")return false;
 if(desktopNamesBudgie())return false;
 bool kde=false;
 for(const auto& desktop:qEnvironmentVariable("XDG_CURRENT_DESKTOP").split(':',Qt::SkipEmptyParts)){
  if(desktop.compare("GNOME",Qt::CaseInsensitive)==0||desktop.compare("Cinnamon",Qt::CaseInsensitive)==0||desktop.compare("X-Cinnamon",Qt::CaseInsensitive)==0)return false;
  if(desktop.compare("KDE",Qt::CaseInsensitive)==0)kde=true;
 }
 return kde;
}
inline bool kdeCoreSessionArguments(int argc,const char* const* argv) {
 for(int i=1;i<argc;++i){
  const QByteArray argument(argv[i]);
  if(argument!="--background"&&argument!="--history"&&argument!="--activity"&&
     argument!="-h"&&argument!="--help"&&argument!="--help-all"&&
     argument!="-v"&&argument!="--version")return false;
 }
 return true;
}

// Budgie 10.10 on labwc, and Budgie 11 on Magpie, own independent native Wayland
// connections in the Core route. startbudgielabwc sets QT_QPA_PLATFORM to
// "wayland;xcb" when it is unset, so a list whose first plugin is Wayland is
// accepted here. A KDE or Cinnamon token keeps the session off this route.
inline bool budgieCoreSessionEnvironment() {
 if(qEnvironmentVariable("XDG_SESSION_TYPE")!="wayland"||qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY"))return false;
 const auto platform=qEnvironmentVariable("QT_QPA_PLATFORM");
 const auto plugin=platform.section(';',0,0).section(':',0,0);
 if(!platform.isEmpty()&&plugin!="wayland"&&plugin!="wayland-egl")return false;
 if(!desktopNamesBudgie())return false;
 for(const auto& desktop:qEnvironmentVariable("XDG_CURRENT_DESKTOP").split(':',Qt::SkipEmptyParts))
  if(desktop.compare("KDE",Qt::CaseInsensitive)==0||desktop.compare("Cinnamon",Qt::CaseInsensitive)==0||desktop.compare("X-Cinnamon",Qt::CaseInsensitive)==0)return false;
 return true;
}
// The Core routes whose adapters open their own Wayland connections for
// outputs and data control. Add a new compositor's route here.
inline bool waylandCoreSessionEnvironment() {
 return kdeCoreSessionEnvironment()||budgieCoreSessionEnvironment();
}

}
