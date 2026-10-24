#include "LoginStartup.h"
#include "LoginStartScript.h"
#include <QDir>
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QProcess>
#include <QRegularExpression>
#include <QSaveFile>
#include <QtEndian>
namespace putgd {
namespace {
QString desktopQuoted(QString value) {
 value.replace("\\","\\\\");value.replace("\"","\\\"");value.replace("$","\\$");value.replace("`","\\`");value.replace("%","%%");
 // Desktop-string decoding happens before Exec command-line decoding.
 value.replace("\\","\\\\");value.replace("\t","\\t");
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
QString LoginStartup::scriptPath(const QString& dataRoot) {return QDir(dataRoot).filePath("login/start.sh");}
Error LoginStartup::monitor(const QString& dataRoot,bool enabled) {
#ifdef Q_OS_LINUX
 const auto path=scriptPath(dataRoot);QFile script(path);
 if(!script.exists())return {};
 if(!script.open(QIODevice::ReadOnly)||script.size()>64*1024||!script.readAll().contains("\n# X-putgd-Managed=true\n"))
  return {"autostart_conflict","Login startup script is not managed by put.gd",{}};
 QProcess process;process.start("/bin/sh",{path,enabled?"--install-watch":"--stop-watch"});
 if(!process.waitForStarted(3000)||!process.waitForFinished(10000)) {
  process.kill();process.waitForFinished();return {"autostart_monitor","Cannot configure the user login startup monitor",{}};
 }
 if(process.exitStatus()!=QProcess::NormalExit||process.exitCode()!=0)
  return {"autostart_monitor","Cannot configure the user login startup monitor",{}};
#else
 Q_UNUSED(dataRoot);Q_UNUSED(enabled);
#endif
 return {};
}
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
 const auto scriptPath=LoginStartup::scriptPath(dataRoot),legacyScript=appImageScriptPath(dataRoot);
 QFile existing(path);
 if(existing.exists()) {
  if(!existing.open(QIODevice::ReadOnly)||existing.size()>64*1024)return {"autostart_io","Cannot read login startup entry",{}};
  if(!existing.readAll().contains("\nX-putgd-Managed=true\n"))return {"autostart_conflict","An existing login startup entry is not managed by put.gd",{}};
  existing.close();
 }
 if(!enabled) {
  if(existing.exists()&&!existing.remove())return {"autostart_io","Cannot remove login startup entry",{}};
  if(auto error=removeScript(scriptPath))return error;
  return removeScript(legacyScript);
 }
#ifndef Q_OS_LINUX
 if(!QFileInfo(executable).isAbsolute()||executable.contains('\n')||executable.contains('\r'))return {"autostart_io","Login startup requires an absolute executable path",{}};
 if(!QDir().mkpath(directory))return {"autostart_io","Cannot create login startup directory",{}};
 const auto bytes=("[Desktop Entry]\nType=Application\nName=put.gd\nExec="+desktopQuoted(executable)+" --background\nTerminal=false\nX-putgd-Managed=true\n").toUtf8();
 return commit(path,bytes,QFile::ReadOwner|QFile::WriteOwner,"login startup entry");
#else
 QString configured=executable,information,kind="native";
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
  configured=imagePath;information=appImageUpdateInformation(imagePath);kind="appimage";
 }
 if(kind=="native") {
  if(!QFileInfo(executable).isAbsolute()||executable.contains('\n')||executable.contains('\r'))return {"autostart_io","Login startup requires an absolute executable path",{}};
  if(!QFileInfo(executable).isFile()||!QFileInfo(executable).isExecutable())return {"autostart_io","Login startup requires an accessible executable",{}};
 }
 if(!QFileInfo(scriptPath).isAbsolute()||scriptPath.contains('\n')||scriptPath.contains('\r'))return {"autostart_io","Login startup requires an absolute script path",{}};
 const auto id=QString::fromLatin1(QCryptographicHash::hash((path+'\n'+configured).toUtf8(),QCryptographicHash::Sha256).toHex());
 const QHash<QString,QString> values{{"CONFIGURED",shellQuoted(configured)},{"UPDATE_INFORMATION",shellQuoted(information)},
  {"KIND",shellQuoted(kind)},{"ENTRY",shellQuoted(path)},{"SCRIPT",shellQuoted(scriptPath)},
  {"ID",id},{"WATCH_UNIT",shellQuoted("putgd-login-startup-"+id)}};
 const auto source=QString::fromUtf8(putgdLoginStartScript);
 QString script;qsizetype position=0;
 auto matches=QRegularExpression("__PUTGD_([A-Z_]+)__").globalMatch(source);
 while(matches.hasNext()) {
  const auto match=matches.next();script+=source.mid(position,match.capturedStart()-position)+values.value(match.captured(1));
  position=match.capturedEnd();
 }
 script+=source.mid(position);
 QFile previous(scriptPath);
 if(previous.exists()&&(!previous.open(QIODevice::ReadOnly)||previous.size()>64*1024||!previous.readAll().contains("\n# X-putgd-Managed=true\n")))
  return {"autostart_conflict","An existing login startup script is not managed by put.gd",{}};
 previous.close();
 if(!QDir().mkpath(QFileInfo(scriptPath).path()))return {"autostart_io","Cannot create login startup script directory",{}};
 if(auto error=commit(scriptPath,script.toUtf8(),QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner,"login startup script"))return error;
 if(auto error=removeScript(legacyScript))return error;
 if(!QDir().mkpath(directory))return {"autostart_io","Cannot create login startup directory",{}};
 // No TryExec: an absent installation must still run the user-scoped cleanup
 // at the next login. The shell and this script survive package removal.
 const auto entry="[Desktop Entry]\nType=Application\nName=put.gd\nExec=/bin/sh "+desktopQuoted(scriptPath)+" --background\n";
 const auto bytes=(entry+"Terminal=false\nX-putgd-Managed=true\nX-putgd-StartupId="+id+"\n").toUtf8();
 return commit(path,bytes,QFile::ReadOwner|QFile::WriteOwner,"login startup entry");
#endif
}
}
