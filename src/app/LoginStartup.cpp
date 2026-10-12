#include "LoginStartup.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
namespace putgd {
Error LoginStartup::apply(const QString& root,const QString& executable,bool enabled) {
 const auto directory=QDir(root).filePath("autostart"),path=QDir(directory).filePath("putgd.desktop");
 QFile existing(path);QByteArray current;
 if(existing.exists()) {
  if(!existing.open(QIODevice::ReadOnly)||existing.size()>64*1024)return {"autostart_io","Cannot read login startup entry",{}};
  current=existing.readAll();
  if(!current.contains("\nX-putgd-Managed=true\n"))return {"autostart_conflict","An existing login startup entry is not managed by put.gd",{}};
  existing.close();
 }
 if(!enabled)return existing.exists()&&!existing.remove()?Error{"autostart_io","Cannot remove login startup entry",{}}:Error{};
 auto startupExecutable=executable;
#ifdef Q_OS_LINUX
 // An AppImage mount is temporary; persist the user's original image instead.
 const auto appImage=qEnvironmentVariable("APPIMAGE"),appDirectory=qEnvironmentVariable("APPDIR");
 const auto mountedLauncher=QDir(appDirectory).filePath("usr/bin/putgd-launcher");
 if(!appImage.isEmpty()&&!appDirectory.isEmpty()&&QFileInfo(appDirectory).isAbsolute()&&
    QDir::cleanPath(executable)==QDir::cleanPath(mountedLauncher)) {
  const QFileInfo image(appImage);
  if(!image.isAbsolute()||!image.isFile()||!image.isExecutable())return {"autostart_io","Login startup requires an accessible AppImage file",{}};
  startupExecutable=appImage;
 }
#endif
 if(!QFileInfo(startupExecutable).isAbsolute()||startupExecutable.contains('\n')||startupExecutable.contains('\r'))return {"autostart_io","Login startup requires an absolute executable path",{}};
 if(!QDir().mkpath(directory))return {"autostart_io","Cannot create login startup directory",{}};
 auto escaped=startupExecutable;escaped.replace("\\","\\\\");escaped.replace("\"","\\\"");escaped.replace("$","\\$");escaped.replace("`","\\`");escaped.replace("%","%%");
 const auto bytes=("[Desktop Entry]\nType=Application\nName=put.gd\nExec=\""+escaped+"\" --background\nTerminal=false\nX-putgd-Managed=true\n").toUtf8();
 if(current==bytes)return {};
 QSaveFile file(path);file.setDirectWriteFallback(false);
 if(!file.open(QIODevice::WriteOnly)||!file.setPermissions(QFile::ReadOwner|QFile::WriteOwner)||file.write(bytes)!=bytes.size()||!file.commit())return {"autostart_io","Cannot commit login startup entry",{}};
 return {};
}
}
