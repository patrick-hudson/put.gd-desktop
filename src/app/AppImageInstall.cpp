#include "AppImageInstall.h"
#include "LoginStartup.h"
#include "core/HostEnvironment.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStorageInfo>
#ifdef Q_OS_UNIX
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#endif
namespace putgd::appimage {
namespace {
constexpr qint64 maximumState=64*1024;
// Room for the new image and its staging copy's slack.
constexpr qint64 freeMargin=16LL*1024*1024;
Error notWritable(const QString& message,const QString& directory) {return {"update_image_not_writable",message,{{"directory",directory}}};}
#ifdef Q_OS_UNIX
bool syncPath(const QString& path,bool directory) {
 const auto fd=::open(QFile::encodeName(path).constData(),O_RDONLY|O_CLOEXEC|(directory?O_DIRECTORY:0));
 if(fd<0)return false;
 const bool synced=::fsync(fd)==0;::close(fd);
 return synced;
}
#endif
// The file at path is the one record {device, inode, size, mtime} describes.
bool isRecordedFile(const QString& path,const QJsonObject& record) {
#ifdef Q_OS_UNIX
 struct stat info{};
 return ::lstat(QFile::encodeName(path).constData(),&info)==0&&S_ISREG(info.st_mode)&&
  QString::number(quint64(info.st_dev))==record["device"].toString()&&QString::number(quint64(info.st_ino))==record["inode"].toString()&&
  qint64(info.st_size)==record["size"].toInteger()&&qint64(info.st_mtim.tv_sec)==record["mtime"].toInteger();
#else
 Q_UNUSED(path);Q_UNUSED(record);return false;
#endif
}
}
bool isAppImage(const QString& path) {
 QFile file(path);
 if(!file.open(QIODevice::ReadOnly))return false;
 const auto head=file.read(11);
 return head.size()==11&&head.startsWith("\x7f""ELF")&&head.mid(8,3)==QByteArray("AI\x02",3);
}
Access hostAccess() {
#ifdef Q_OS_UNIX
 return {
  [](const QString& file){return QFileInfo(file).ownerId()==::getuid();},
  [](const QString& directory){struct statvfs volume{};return ::statvfs(QFile::encodeName(directory).constData(),&volume)==0&&(volume.f_flag&ST_RDONLY);},
  [](const QString& directory){return ::access(QFile::encodeName(directory).constData(),W_OK|X_OK)==0;},
 };
#else
 return {[](const QString&){return false;},[](const QString&){return true;},[](const QString&){return false;}};
#endif
}
Image inspect(const QString& path,qint64 requiredBytes,Error& error,const Access& access) {
 Image image;
 const QFileInfo given(path);
 if(path.isEmpty()||!given.isAbsolute()){error={"update_install_unavailable","put.gd is not running from an AppImage file",{}};return {};}
 // A link to the image is followed: the file itself is replaced, and the link keeps working.
 const auto canonical=given.canonicalFilePath();const QFileInfo file(canonical);
 if(canonical.isEmpty()||!file.isFile()||!isAppImage(canonical)){error={"update_install_unavailable","The AppImage put.gd runs from is missing or is not an AppImage",{{"image",path}}};return {};}
 image.path=canonical;image.directory=file.absolutePath();image.size=file.size();
 image.updateInformation=LoginStartup::appImageUpdateInformation(canonical);
#ifdef Q_OS_UNIX
 if(!access.ownedByUser(canonical)){error=notWritable("The AppImage belongs to another user, so put.gd won't replace it",image.directory);return {};}
 if(access.readOnly(image.directory)){error=notWritable("The AppImage's folder is on a read-only file system",image.directory);return {};}
 if(!access.writable(image.directory)){error=notWritable("put.gd can't write to the AppImage's folder",image.directory);return {};}
#else
 Q_UNUSED(access);
 error={"update_install_unavailable","In-app updates replace AppImages on Linux only",{}};return {};
#endif
 const QStorageInfo storage(image.directory);
 if(storage.isValid()&&storage.bytesAvailable()>=0&&storage.bytesAvailable()<requiredBytes+freeMargin){
  error={"update_disk_full","There isn't enough free space beside the AppImage for the new version",{{"directory",image.directory},{"availableBytes",storage.bytesAvailable()},{"requiredBytes",requiredBytes+freeMargin}}};return {};
 }
 return image;
}
QString sha256(const QString& path) {
 QFile file(path);QCryptographicHash hash(QCryptographicHash::Sha256);
 if(!file.open(QIODevice::ReadOnly)||!hash.addData(&file))return {};
 return QString::fromLatin1(hash.result().toHex());
}
QString sumsLine(const QByteArray& sums,const QString& name) {
 static const QRegularExpression line(R"(^([0-9a-f]{64}) [ *](.+)$)");
 QString found;
 for(const auto& raw:QString::fromUtf8(sums).split('\n')) {
  const auto match=line.match(raw.trimmed());
  if(!match.hasMatch()||match.captured(2)!=name)continue;
  if(!found.isEmpty())return {};
  found=match.captured(1);
 }
 return found;
}
Error verify(const QString& candidate,const QString& expectedSha256,const QString& updateInformation) {
 const auto actual=sha256(candidate);
 if(actual.isEmpty()||actual!=expectedSha256)
  return {"update_verification_failed","The new version doesn't match the release's SHA256SUMS; nothing was changed",{{"check","sha256"},{"expected",expectedSha256},{"actual",actual.isEmpty()?QJsonValue():QJsonValue(actual)}}};
 if(!isAppImage(candidate))return {"update_verification_failed","The new version is not an AppImage; nothing was changed",{{"check","appimage"}}};
 // The same channel's permalink: an image for another channel or origin is not this update.
 const auto information=LoginStartup::appImageUpdateInformation(candidate);
 if(information!=updateInformation)
  return {"update_verification_failed","The new version follows other update information; nothing was changed",{{"check","updateInformation"},{"expected",updateInformation},{"actual",information}}};
 return {};
}
bool linkOrCopy(const QString& source,const QString& destination,bool* copied) {
 if(copied)*copied=false;
#ifdef Q_OS_UNIX
 if(::link(QFile::encodeName(source).constData(),QFile::encodeName(destination).constData())==0)return true;
#endif
 if(!QFile::copy(source,destination))return false;
 if(copied)*copied=true;
 return true;
}
QJsonObject swap(const QString& candidate,const Image& image,const QString& previousVersion,Error& error) {
#ifdef Q_OS_UNIX
 const auto failed=[&](const QString& message){error={"update_install_failed",message,{{"image",image.path}}};return QJsonObject{};};
 struct stat old{};
 if(::stat(QFile::encodeName(image.path).constData(),&old)!=0)return failed("The AppImage is no longer there");
 if(::chmod(QFile::encodeName(candidate).constData(),old.st_mode&07777)!=0||!syncPath(candidate,false))return failed("Cannot prepare the new version beside the AppImage");
 // Hidden, so the login script never picks it, and named after what it holds.
 static const QRegularExpression unsafe("[^0-9A-Za-z.+~-]");
 auto version=previousVersion;version.replace(unsafe,"_");if(version.isEmpty())version="previous";
 QString backup;
 for(int attempt=0;attempt<10&&backup.isEmpty();++attempt) {
  const auto name=backupPrefix+version+(attempt?QString("-%1").arg(attempt+1):QString())+".AppImage";
  const auto path=QDir(image.directory).filePath(name);
  if(!QFileInfo::exists(path)&&!QFileInfo(path).isSymLink())backup=path;
 }
 if(backup.isEmpty())return failed("Too many earlier backups of the AppImage are in its folder");
 bool copied=false;
 if(!linkOrCopy(image.path,backup,&copied))return failed("Cannot keep a backup of the current AppImage");
 struct stat kept{};
 if(::stat(QFile::encodeName(backup).constData(),&kept)!=0){QFile::remove(backup);return failed("Cannot keep a backup of the current AppImage");}
 // The one step that changes what the path holds; atomic on one file system.
 if(::rename(QFile::encodeName(candidate).constData(),QFile::encodeName(image.path).constData())!=0){
  QFile::remove(backup);
  return failed("Cannot put the new version in place of the AppImage");
 }
 syncPath(image.directory,true);
 return {{"path",backup},{"device",QString::number(quint64(kept.st_dev))},{"inode",QString::number(quint64(kept.st_ino))},{"size",qint64(kept.st_size)},
  {"mtime",qint64(kept.st_mtim.tv_sec)},{"copied",copied}};
#else
 Q_UNUSED(candidate);Q_UNUSED(image);Q_UNUSED(previousVersion);
 error={"update_install_unavailable","In-app updates replace AppImages on Linux only",{}};return {};
#endif
}
QString statePath(const QString& stateRoot) {return QDir(stateRoot).filePath("update-install.json");}
QJsonObject readState(const QString& stateRoot) {
 QFile file(statePath(stateRoot));
 if(!file.open(QIODevice::ReadOnly)||file.size()>maximumState)return {};
 const auto state=QJsonDocument::fromJson(file.readAll()).object();
 return state["schemaVersion"]==1?state:QJsonObject{};
}
bool writeState(const QString& stateRoot,const QJsonObject& state) {
 auto document=state;document["schemaVersion"]=1;
 const auto bytes=QJsonDocument(document).toJson(QJsonDocument::Compact);
 QDir().mkpath(stateRoot);
 QSaveFile file(statePath(stateRoot));file.setDirectWriteFallback(false);
 return file.open(QIODevice::WriteOnly)&&file.setPermissions(QFile::ReadOwner|QFile::WriteOwner)&&file.write(bytes)==bytes.size()&&file.commit();
}
bool removeStaging(const QString& path) {
 const QFileInfo info(path);
 static const QRegularExpression name("^\\.putgd-update-[A-Za-z0-9]{6}$");
 if(!info.exists())return true;
 if(info.isSymLink()||!info.isDir()||!name.match(info.fileName()).hasMatch())return false;
 QDir directory(path);
 const auto entries=directory.entryInfoList(QDir::AllEntries|QDir::Hidden|QDir::System|QDir::NoDotAndDotDot);
 // Only what the install and its tool wrote: files, never a folder of someone else's.
 for(const auto& entry:entries)if(entry.isDir()&&!entry.isSymLink())return false;
 for(const auto& entry:entries)if(!QFile::remove(entry.filePath()))return false;
 return QDir().rmdir(path);
}
QJsonObject settle(const QString& stateRoot,const QString& runningImage,const QString& runningVersion) {
 auto state=readState(stateRoot);
 if(state.isEmpty())return state;
 bool changed=false;
 const auto staging=state.value("staging").toString();
 if(!staging.isEmpty()){removeStaging(staging);state["staging"]=QJsonValue();changed=true;}
 const auto installed=state.value("installed").toObject();const auto backup=state.value("backup").toObject();
 if(!backup.isEmpty()&&!runningImage.isEmpty()&&installed["image"].toString()==runningImage&&installed["version"].toString()==runningVersion) {
  // The new version has started from the path: the backup has done its job,
  // if it is still the file put.gd made. Anything else there is the user's.
  const auto path=backup["path"].toString();
  const bool remove=QFileInfo(path).fileName().startsWith(backupPrefix)&&isRecordedFile(path,backup);
  auto record=installed;
  record["backupRemoved"]=remove&&QFile::remove(path);
  state["installed"]=record;state["backup"]=QJsonValue();changed=true;
 } else if(!backup.isEmpty()&&!runningImage.isEmpty()&&installed["image"].toString()==runningImage&&isRecordedFile(runningImage,backup)) {
  // The backup was moved back onto the path by hand, because the new version
  // didn't start (#412): nothing waits for a restart any more. Files stay.
  state["restored"]=QJsonObject{{"image",runningImage},{"version",installed["version"]},{"runningVersion",runningVersion}};
  state["installed"]=QJsonValue();state["backup"]=QJsonValue();changed=true;
 }
 if(changed)writeState(stateRoot,state);
 return state;
}
ZsyncTool findZsyncTool(const QProcessEnvironment& host) {
 for(const auto& [kind,program]:{std::pair{"appimageupdatetool","appimageupdatetool"},std::pair{"appimageupdatetool","appimageupdatetool-x86_64.AppImage"},std::pair{"zsync2","zsync2"}}) {
  const auto path=host::findProgram(program,host);
  if(!path.isEmpty())return {kind,path};
 }
 return {};
}
QStringList zsyncArguments(const ZsyncTool& tool,const QString& seed,const QString& output,const QString& zsyncUrl) {
 // -u pins the .zsync of the release whose SHA256SUMS was read, whatever the
 // image's own update information names by now.
 if(tool.kind=="appimageupdatetool")return {"-u","zsync|"+zsyncUrl,seed};
 return {"-i",seed,"-o",output,zsyncUrl};
}
QProcessEnvironment zsyncEnvironment(const QProcessEnvironment& host,const QString& caBundle) {
 auto environment=host;
 if(!caBundle.isEmpty())environment.insert("SSL_CERT_FILE",caBundle);
 return environment;
}
QStringList terminalLines(const QByteArray& output) {
 QStringList lines;QString line;qsizetype column=0;
 const auto finish=[&]{const auto text=line.trimmed();if(!text.isEmpty())lines.append(text);line.clear();column=0;};
 const auto text=QString::fromUtf8(output);
 for(qsizetype i=0;i<text.size();++i) {
  const auto c=text.at(i).unicode();
  if(c==0x1b||c==0x9b) {
   // CSI (ESC [ or 0x9B): parameters and intermediates up to a final byte.
   // An erase-line (K) clears what it names; everything else just goes.
   if(c==0x9b||(i+1<text.size()&&text.at(i+1)==u'[')) {
    auto j=i+(c==0x9b?1:2);const auto start=j;
    while(j<text.size()&&text.at(j).unicode()>=0x20&&text.at(j).unicode()<=0x3f)++j;
    if(j<text.size()&&text.at(j)==u'K') {
     const auto mode=text.mid(start,j-start);
     if(mode=="1"||mode=="2"){for(qsizetype k=0;k<line.size()&&(mode=="2"||k<=column);++k)line[k]=u' ';}else line.truncate(column);
    }
    i=j;continue;
   }
   // OSC, DCS, SOS, PM and APC run to BEL or ST (ESC \).
   if(i+1<text.size()&&QStringView(u"]PX^_").contains(text.at(i+1))) {
    auto j=i+2;
    while(j<text.size()&&text.at(j)!=u'\a'&&!(text.at(j)==u'\x1b'&&j+1<text.size()&&text.at(j+1)==u'\\'))++j;
    i=j<text.size()&&text.at(j)==u'\x1b'?j+1:j;continue;
   }
   // Any other escape: intermediates, then one final character.
   auto j=i+1;while(j<text.size()&&text.at(j).unicode()>=0x20&&text.at(j).unicode()<=0x2f)++j;
   i=j;continue;
  }
  if(c=='\n'){finish();continue;}
  if(c=='\r'){column=0;continue;}
  if(c=='\b'){column=qMax<qsizetype>(0,column-1);continue;}
  if(c!='\t'&&(c<0x20||c==0x7f||(c>=0x80&&c<0xa0)))continue;
  const QChar shown=c=='\t'?QChar(u' '):text.at(i);
  if(column<line.size())line[column]=shown;else{line.append(QString(column-line.size(),u' '));line.append(shown);}
  ++column;
 }
 finish();
 return lines;
}
QString zsyncOutputTail(const QByteArray& output) {
 // The tool's last lines name what went wrong; only for the result's details.
 const auto lines=terminalLines(output);
 return lines.mid(qMax(0,int(lines.size())-4)).join(" | ").left(400);
}
}
