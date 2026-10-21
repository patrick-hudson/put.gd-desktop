#pragma once
#include "core/Error.h"
namespace putgd {
class LoginStartup {
public:
 // Writes or removes <configurationRoot>/autostart/putgd.desktop. From an
 // AppImage, the entry runs <dataRoot>/login/appimage-start.sh, which starts the
 // newest put.gd AppImage beside the running one (#307).
 static Error apply(const QString& configurationRoot,const QString& dataRoot,const QString& executable,bool enabled);
 static QString appImageScriptPath(const QString& dataRoot);
 // The NUL-terminated text in an AppImage runtime's .upd_info ELF section;
 // empty when there is none or the file is not a little-endian ELF64 file.
 static QString appImageUpdateInformation(const QString& path);
};
}
