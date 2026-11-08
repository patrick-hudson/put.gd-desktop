#pragma once
#include <QtCore/QString>
#include <QtCore/QByteArray>
#include <QtCore/QProcessEnvironment>
#include <QtCore/QFile>
#include <cstring>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

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
// Hyprland names itself in XDG_CURRENT_DESKTOP and sets HYPRLAND_INSTANCE_SIGNATURE.
inline bool desktopNamesHyprland() {
 for(const auto& desktop:qEnvironmentVariable("XDG_CURRENT_DESKTOP").split(':',Qt::SkipEmptyParts))
  if(desktop.compare("Hyprland",Qt::CaseInsensitive)==0)return true;
 return false;
}
// $XDG_RUNTIME_DIR/hypr/<signature>/.socket.sock, or empty when either is unset
// or the signature could name another directory.
inline QString hyprlandSocketPath(const char* name=".socket.sock") {
 const auto runtime=qEnvironmentVariable("XDG_RUNTIME_DIR");const auto signature=qEnvironmentVariable("HYPRLAND_INSTANCE_SIGNATURE");
 if(runtime.isEmpty()||signature.isEmpty()||signature.contains('/')||signature=="."||signature=="..")return {};
 return runtime+"/hypr/"+signature+'/'+QString::fromLatin1(name);
}
// Whether Hyprland's request socket accepts a connection now. Nothing is sent.
inline bool hyprlandSocketAnswers() {
 const QByteArray path=QFile::encodeName(hyprlandSocketPath());
 sockaddr_un address{};address.sun_family=AF_UNIX;
 if(path.isEmpty()||size_t(path.size())>=sizeof address.sun_path)return false;
 std::memcpy(address.sun_path,path.constData(),size_t(path.size()));
 const int fd=::socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0);if(fd<0)return false;
 const bool answered=::connect(fd,reinterpret_cast<const sockaddr*>(&address),sizeof address)==0;
 ::close(fd);return answered;
}
// Hyprland (Omarchy and any other Hyprland session) owns independent native
// Wayland connections in the Core route. Omarchy sets QT_QPA_PLATFORM to
// "wayland;xcb", so a list whose first plugin is Wayland is accepted, as for
// Budgie. A KDE, GNOME, Cinnamon or Budgie token keeps the session off this
// route, and so does a signature whose request socket does not answer.
inline bool hyprlandCoreSessionEnvironment() {
 if(qEnvironmentVariable("XDG_SESSION_TYPE")!="wayland"||qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY"))return false;
 const auto platform=qEnvironmentVariable("QT_QPA_PLATFORM");
 const auto plugin=platform.section(';',0,0).section(':',0,0);
 if(!platform.isEmpty()&&plugin!="wayland"&&plugin!="wayland-egl")return false;
 if(!desktopNamesHyprland()||desktopNamesBudgie())return false;
 for(const auto& desktop:qEnvironmentVariable("XDG_CURRENT_DESKTOP").split(':',Qt::SkipEmptyParts))
  if(desktop.compare("KDE",Qt::CaseInsensitive)==0||desktop.compare("GNOME",Qt::CaseInsensitive)==0||
     desktop.compare("Cinnamon",Qt::CaseInsensitive)==0||desktop.compare("X-Cinnamon",Qt::CaseInsensitive)==0)return false;
 return hyprlandSocketAnswers();
}
// The Core routes whose adapters open their own Wayland connections for
// outputs and data control. Add a new compositor's route here.
inline bool waylandCoreSessionEnvironment() {
 return kdeCoreSessionEnvironment()||budgieCoreSessionEnvironment()||hyprlandCoreSessionEnvironment();
}

}
