#include "LoginStartup.h"
#include "LoginStartScript.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QtEndian>
namespace putgd {
namespace {
QString desktopQuoted(QString value) {
 value.replace("\\","\\\\");value.replace("\"","\\\"");value.replace("$","\\$");value.replace("`","\\`");value.replace("%","%%");
 return "\""+value+"\"";
}
QString shellQuoted(QString value) {return "'"+value.replace("'","'\\''")+"'";}
Error commit(const QString& path,const QByteArray& bytes,QFile::Permissions permissions,const QString& what) {
 constexpr auto shared=QFile::ReadGroup|QFile::WriteGroup|QFile::ExeGroup|QFile::ReadOther|QFile::WriteOther|QFile::ExeOther;
 QFile current(path);
 if(current.open(QIODevice::ReadOnly)&&current.size()<=64*1024&&current.readAll()==bytes&&
    current.permissions().testFlags(permissions)&&!(current.permissions()&shared))return {};
 current.close();
 QSaveFile file(path);file.setDirectWriteFallback(false);
 if(!file.open(QIODevice::WriteOnly)||!file.setPermissions(permissions)||file.write(bytes)!=bytes.size()||!file.commit())return {"autostart_io","Cannot commit "+what,{}};
 return {};
}
// Remove put.gd's own login script; never a file it did not write.
Error removeScript(const QString& path) {
 QFile script(path);
 if(!script.exists())return {};
 if(!script.open(QIODevice::ReadOnly)||script.size()>64*1024)return {"autostart_io","Cannot read login startup script",{}};
 const auto managed=script.readAll().contains("\n# X-putgd-Managed=true\n");script.close();
 if(managed&&!script.remove())return {"autostart_io","Cannot remove login startup script",{}};
 return {};
}
}
QString LoginStartup::appImageScriptPath(const QString& dataRoot) {return QDir(dataRoot).filePath("login/appimage-start.sh");}
QString LoginStartup::appImageUpdateInformation(const QString& path) {
 QFile file(path);
 if(!file.open(QIODevice::ReadOnly))return {};
 const auto header=file.read(64);
 if(header.size()!=64||!header.startsWith("\x7f""ELF")||header[4]!=2||header[5]!=1)return {};
 const auto bytes=reinterpret_cast<const uchar*>(header.constData());
 const auto tableOffset=qFromLittleEndian<quint64>(bytes+0x28);
 const auto entrySize=qFromLittleEndian<quint16>(bytes+0x3A),count=qFromLittleEndian<quint16>(bytes+0x3C),names=qFromLittleEndian<quint16>(bytes+0x3E);
 if(entrySize!=64||count>256||names==0||names>=count||!file.seek(qint64(tableOffset)))return {};
 const auto table=file.read(qint64(entrySize)*count);
 if(table.size()!=qint64(entrySize)*count)return {};
 const auto section=[&](int index,quint64 limit)->QByteArray {
  const auto entry=reinterpret_cast<const uchar*>(table.constData())+index*entrySize;
  const auto offset=qFromLittleEndian<quint64>(entry+24),size=qFromLittleEndian<quint64>(entry+32);
  if(size>limit||!file.seek(qint64(offset)))return {};
  const auto data=file.read(qint64(size));
  return data.size()==qint64(size)?data:QByteArray{};
 };
 const auto strings=section(names,64*1024);
 for(int index=0;index<count;++index) {
  const qsizetype name=qFromLittleEndian<quint32>(reinterpret_cast<const uchar*>(table.constData())+index*entrySize);
  const auto end=name<strings.size()?strings.indexOf('\0',name):-1;
  if(end<0||strings.mid(name,end-name)!=".upd_info")continue;
  const auto data=section(index,64*1024);
  const auto text=data.left(data.indexOf('\0')<0?data.size():data.indexOf('\0'));
  for(const auto c:text)if(c<0x20||c>0x7e)return {};
  return QString::fromLatin1(text);
 }
 return {};
}
Error LoginStartup::apply(const QString& root,const QString& dataRoot,const QString& executable,bool enabled) {
 const auto directory=QDir(root).filePath("autostart"),path=QDir(directory).filePath("putgd.desktop");
 const auto scriptPath=appImageScriptPath(dataRoot);
 QFile existing(path);
 if(existing.exists()) {
  if(!existing.open(QIODevice::ReadOnly)||existing.size()>64*1024)return {"autostart_io","Cannot read login startup entry",{}};
  if(!existing.readAll().contains("\nX-putgd-Managed=true\n"))return {"autostart_conflict","An existing login startup entry is not managed by put.gd",{}};
  existing.close();
 }
 if(!enabled) {
  if(existing.exists()&&!existing.remove())return {"autostart_io","Cannot remove login startup entry",{}};
  return removeScript(scriptPath);
 }
 QString exec;
#ifdef Q_OS_LINUX
 // An AppImage mount is temporary, and an update may write the next version
 // under a new name beside the image or remove it (#307). Persist a script that
 // starts the newest image beside the user's original one instead.
 const auto appImage=qEnvironmentVariable("APPIMAGE"),appDirectory=qEnvironmentVariable("APPDIR");
 const auto mountedLauncher=QDir(appDirectory).filePath("usr/bin/putgd-launcher");
 if(!appImage.isEmpty()&&!appDirectory.isEmpty()&&QFileInfo(appDirectory).isAbsolute()&&
    QDir::cleanPath(executable)==QDir::cleanPath(mountedLauncher)) {
  const QFileInfo image(appImage);
  if(!image.isAbsolute()||!image.isFile()||!image.isExecutable())return {"autostart_io","Login startup requires an accessible AppImage file",{}};
  const auto imagePath=QDir::cleanPath(image.absoluteFilePath());
  if(imagePath.contains('\n')||imagePath.contains('\r')||!QFileInfo(scriptPath).isAbsolute()||scriptPath.contains('\n')||scriptPath.contains('\r'))
   return {"autostart_io","Login startup requires an absolute executable path",{}};
  auto script=QString::fromUtf8(putgdLoginStartScript);
  script.replace("__PUTGD_CONFIGURED__",shellQuoted(imagePath));
  script.replace("__PUTGD_UPDATE_INFORMATION__",shellQuoted(appImageUpdateInformation(imagePath)));
  if(!QDir().mkpath(QFileInfo(scriptPath).path()))return {"autostart_io","Cannot create login startup script directory",{}};
  if(auto error=commit(scriptPath,script.toUtf8(),QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner,"login startup script"))return error;
  exec="/bin/sh "+desktopQuoted(scriptPath);
 }
#endif
 if(exec.isEmpty()) {
  if(!QFileInfo(executable).isAbsolute()||executable.contains('\n')||executable.contains('\r'))return {"autostart_io","Login startup requires an absolute executable path",{}};
  exec=desktopQuoted(executable);
  if(auto error=removeScript(scriptPath))return error;
 }
 if(!QDir().mkpath(directory))return {"autostart_io","Cannot create login startup directory",{}};
 const auto bytes=("[Desktop Entry]\nType=Application\nName=put.gd\nExec="+exec+" --background\nTerminal=false\nX-putgd-Managed=true\n").toUtf8();
 return commit(path,bytes,QFile::ReadOwner|QFile::WriteOwner,"login startup entry");
}
}
