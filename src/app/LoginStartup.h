#pragma once
#include "core/Error.h"
namespace putgd {
class LoginStartup {
public:
 // Writes or removes <configurationRoot>/autostart/putgd.desktop. The Linux
 // entry runs a user-owned script. Missing installations remove their managed
 // startup entry without changing the saved preference.
 static Error apply(const QString& configurationRoot,const QString& dataRoot,const QString& executable,bool enabled);
 // Call after enabling to arm a user-manager path unit, or to stop it explicitly.
 // File-only apply tests do not contact the user manager.
 static Error monitor(const QString& dataRoot,bool enabled);
 // Call before apply; stop only when disabling or changing installation identity.
 static Error prepareMonitor(const QString& root,const QString& dataRoot,const QString& executable,bool enabled);
 static QString scriptPath(const QString& dataRoot);
 static QString appImageScriptPath(const QString& dataRoot);
 // The NUL-terminated text in an AppImage runtime's .upd_info ELF section;
 // empty when there is none or the file is not a little-endian ELF64 file.
 static QString appImageUpdateInformation(const QString& path);
};
}
