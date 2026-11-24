#include "platform/gnome/GnomeExtensionSetup.h"
#include "platform/gnome/GnomeProtocol.h"
#include "core/HostEnvironment.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDBusConnection>
#include <QDBusArgument>
#include <QDBusMessage>
#include <QDBusVariant>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QRegularExpression>
#include <QSaveFile>
#include <QThread>
#include <QHash>
#include <QUuid>
#include <QVariantMap>
#include <algorithm>
#include <utility>

namespace putgd::gnome {
namespace {
constexpr auto shellService="org.gnome.Shell";
constexpr auto shellPath="/org/gnome/Shell";
constexpr auto extensionsInterface="org.gnome.Shell.Extensions";
constexpr auto propertiesInterface="org.freedesktop.DBus.Properties";
constexpr char settingsSchema[]="org.gnome.shell";
// Shell's ExtensionState values (js/misc/extensionUtils.js, GNOME 50).
constexpr int extensionActive=1,extensionError=3,extensionOutOfDate=4,extensionActivating=8;
const QString loginMessage=QStringLiteral("Log out and back in once to finish setting up put.gd.");

SetupStatus status(const QString& state,const QString& code,const QString& message,QJsonObject details={}){
 return {state,code,message,std::move(details)};
}
SetupStatus ready(QJsonObject details={}){return {"ready",{},{},std::move(details)};}
SetupStatus loginRequired(QJsonObject details={}){return status("login_required","gnome_login_required",loginMessage,std::move(details));}

struct UserCopy {
 bool exists=false;
 bool managed=false;
 int version=0;
 QStringList files;
};
QString fileSha256(const QString& path){
 QFile file(path);if(!file.open(QIODevice::ReadOnly))return {};
 QCryptographicHash hash(QCryptographicHash::Sha256);if(!hash.addData(&file))return {};
 return QString::fromLatin1(hash.result().toHex());
}
int metadataVersion(const QString& directory){
 QFile file(QDir(directory).filePath("metadata.json"));if(!file.open(QIODevice::ReadOnly))return 0;
 return QJsonDocument::fromJson(file.readAll()).object()["version"].toInt();
}
// An owned copy has our marker and every file listed there unchanged. A directory
// without one (or with an edited file) is treated as the user's.
UserCopy inspectUserCopy(const QString& path){
 UserCopy copy;
 const QFileInfo info(path);
 if(!info.exists()&&!info.isSymLink())return copy;
 copy.exists=true;
 if(info.isSymLink()||!info.isDir())return copy;
 const auto markerPath=QDir(path).filePath(copyMarkerName);
 const QFileInfo marker(markerPath);
 if(!marker.isFile()||marker.isSymLink())return copy;
 QFile file(markerPath);if(!file.open(QIODevice::ReadOnly))return copy;
 const auto document=QJsonDocument::fromJson(file.readAll()).object();
 if(document["managedBy"].toString()!="put.gd")return copy;
 const auto files=document["files"].toObject();if(files.isEmpty())return copy;
 // Only plain names: the marker decides which files later removal touches.
 for(const auto& name:files.keys())if(name.contains('/')||name=="."||name==".."||name==copyMarkerName)return copy;
 // A directory holding anything else has been added to since put.gd wrote it.
 QStringList expected=files.keys();expected.append(copyMarkerName);expected.sort();
 auto present=QDir(path).entryList(QDir::AllEntries|QDir::Hidden|QDir::NoDotAndDotDot);present.sort();
 if(present!=expected)return copy;
 for(auto it=files.begin();it!=files.end();++it){
  if(fileSha256(QDir(path).filePath(it.key()))!=it.value().toString())return copy;
  copy.files.append(it.key());
 }
 copy.managed=true;
 copy.version=document["extensionVersion"].toInt();
 return copy;
}
bool hasSystemCopy(const QStringList& dataDirs,int* version){
 for(const auto& dir:dataDirs){
  const QString path=QDir(dir).filePath(QStringLiteral("gnome-shell/extensions/")+extensionUuid);
  if(QFileInfo::exists(QDir(path).filePath("metadata.json"))){*version=metadataVersion(path);return true;}
 }
 return false;
}
bool writeFile(const QString& path,const QByteArray& bytes){
 QSaveFile file(path);if(!file.open(QIODevice::WriteOnly))return false;
 if(file.write(bytes)!=bytes.size())return false;
 return file.commit();
}
bool removeCopyFiles(const QString& userDir,const QStringList& files){
 const QDir dir(userDir);
 for(const auto& name:files)QFile::remove(dir.filePath(name));
 QFile::remove(dir.filePath(copyMarkerName));
 return QDir().rmdir(userDir);
}
// The files a stale directory's own marker lists and still match: a retired copy
// can be an older version with files this AppImage doesn't carry.
QStringList verifiedMarkerFiles(const QFileInfoList& contents){
 const auto plain=[](const QFileInfo& entry){return entry.isFile()&&!entry.isSymLink();};
 const auto markerEntry=std::find_if(contents.begin(),contents.end(),[](const QFileInfo& entry){return entry.fileName()==QLatin1String(copyMarkerName);});
 if(markerEntry==contents.end()||!plain(*markerEntry))return {};
 QFile file(markerEntry->filePath());if(!file.open(QIODevice::ReadOnly))return {};
 const auto marker=QJsonDocument::fromJson(file.read(1024*1024)).object();
 if(marker["managedBy"].toString()!="put.gd")return {};
 const auto listed=marker["files"].toObject();
 QStringList verified;
 // Only what is really in this directory is hashed, whatever the marker names.
 for(const auto& entry:contents)if(plain(entry)&&listed.contains(entry.fileName())&&fileSha256(entry.filePath())==listed[entry.fileName()].toString())verified.append(entry.fileName());
 return verified;
}
// A crash can leave a staging directory behind. Only what installCopy itself would
// have left goes: a real directory with its exact name, holding nothing but plain
// files named like the extension's or in its own marker. Anything else stays.
void removeStaleStaging(const QString& parent,QStringList bundled){
 bundled.append(copyMarkerName);
 static const QRegularExpression ownName(QRegularExpression::anchoredPattern(QStringLiteral("\\.putgd-extension-(new|old)-[0-9a-f]{32}")));
 for(const auto& directory:QDir(parent).entryInfoList(QDir::Dirs|QDir::NoSymLinks|QDir::Hidden|QDir::NoDotAndDotDot)){
  if(!ownName.match(directory.fileName()).hasMatch())continue;
  const QDir stale(directory.filePath());
  const auto contents=stale.entryInfoList(QDir::AllEntries|QDir::Hidden|QDir::System|QDir::NoDotAndDotDot);
  const auto expected=bundled+verifiedMarkerFiles(contents);
  const bool ours=std::all_of(contents.begin(),contents.end(),[&](const QFileInfo& entry){
   return entry.isFile()&&!entry.isSymLink()&&expected.contains(entry.fileName());
  });
  if(!ours)continue;
  for(const auto& entry:contents)QFile::remove(entry.filePath());
  QDir().rmdir(stale.path());
 }
}
// A copy fails partway only in the staging directory; the live copy is replaced
// by rename. `replaced` is the owned copy being updated, if there is one.
std::optional<QString> installCopy(const QString& bundledDir,const QString& userDir,int version,const UserCopy& replaced){
 const QDir source(bundledDir);
 const auto names=source.entryList(QDir::Files);
 if(names.isEmpty())return QStringLiteral("the AppImage's extension files are missing");
 const QFileInfo target(userDir);
 const auto base=target.path();
 if(!QDir().mkpath(base))return QStringLiteral("cannot create %1").arg(base);
 const auto id=QUuid::createUuid().toString(QUuid::Id128);
 const auto parentOfExtensions=QFileInfo(base).path();
 removeStaleStaging(parentOfExtensions,names);
 const bool replacing=replaced.managed;
 const auto staging=QDir(parentOfExtensions).filePath(".putgd-extension-new-"+id);
 const auto retired=QDir(parentOfExtensions).filePath(".putgd-extension-old-"+id);
 if(!QDir().mkpath(staging))return QStringLiteral("cannot create %1").arg(staging);
 QJsonObject files;
 const auto abandon=[&](const QString& reason){
  for(const auto& name:names)QFile::remove(QDir(staging).filePath(name));
  QFile::remove(QDir(staging).filePath(copyMarkerName));
  QDir().rmdir(staging);
  return std::optional<QString>(reason);
 };
 for(const auto& name:names){
  const auto copied=QDir(staging).filePath(name);
  if(!QFile::copy(source.filePath(name),copied))return abandon(QStringLiteral("cannot copy %1").arg(name));
  QFile::setPermissions(copied,QFile::ReadOwner|QFile::WriteOwner|QFile::ReadGroup|QFile::ReadOther);
  files[name]=fileSha256(copied);
 }
 const QJsonObject marker{{"managedBy","put.gd"},{"schemaVersion",1},{"extensionVersion",version},{"files",files}};
 if(!writeFile(QDir(staging).filePath(copyMarkerName),QJsonDocument(marker).toJson()))return abandon(QStringLiteral("cannot write %1").arg(copyMarkerName));
 if(replacing&&!QDir().rename(userDir,retired))return abandon(QStringLiteral("cannot replace %1").arg(userDir));
 if(!QDir().rename(staging,userDir)){
  if(replacing)QDir().rename(retired,userDir);
  return abandon(QStringLiteral("cannot write %1").arg(userDir));
 }
 // The files inspectUserCopy verified, and nothing else that may have appeared since.
 if(replacing)removeCopyFiles(retired,replaced.files);
 return std::nullopt;
}
QString settingsReason(const ProcessResult& result){
 if(!result.started)return QStringLiteral("gsettings is not installed");
 const auto error=result.standardError.trimmed();
 return error.isEmpty()?QStringLiteral("gsettings exited with %1").arg(result.exitCode):error;
}
SetupStatus settingsUnavailable(const ProcessResult& result){
 return status("settings_unavailable","gnome_settings_unavailable",
  QStringLiteral("put.gd could not change GNOME's extension list (%1).").arg(settingsReason(result)));
}
SetupStatus installFailed(const QString& reason){
 return status("install_failed","gnome_extension_install_failed",QStringLiteral("put.gd could not install its GNOME extension: %1.").arg(reason));
}
ProcessResult runGet(const SetupInputs& in,const QString& key){return in.gsettings({"get",settingsSchema,key});}
int shellMajor(const QString& version){return version.split('.').value(0).toInt();}
// Copies XDG_CONFIG_HOME and XDG_DATA_HOME from the systemd user session. Without
// systemd (no `systemctl --user`) the resident's own values stay.
void applySessionXdg(QProcessEnvironment& environment){
 const auto program=host::findProgram("systemctl",environment);
 if(program.isEmpty())return;
 QProcess process;process.setProcessEnvironment(environment);process.setProgram(program);
 process.setArguments({"--user","show-environment"});process.setStandardInputFile(QProcess::nullDevice());
 process.start();
 if(!process.waitForFinished(3000)||process.exitStatus()!=QProcess::NormalExit||process.exitCode()!=0){process.kill();process.waitForFinished(500);return;}
 QHash<QString,QString> session;
 for(const auto& line:QString::fromUtf8(process.readAllStandardOutput()).split('\n')){
  const auto separator=line.indexOf('=');
  if(separator>0)session.insert(line.left(separator),line.mid(separator+1));
 }
 for(const auto* name:{"XDG_CONFIG_HOME","XDG_DATA_HOME"}){
  const QString key=QString::fromLatin1(name);
  if(session.contains(key))environment.insert(key,session.value(key));
  else environment.remove(key);
 }
}
}

std::optional<QStringList> parseStringList(const QString& raw){
 auto text=raw.trimmed();
 if(text.startsWith("@as "))text=text.mid(4).trimmed();
 if(text.size()<2||text.front()!='['||text.back()!=']')return std::nullopt;
 QStringList values;
 qsizetype position=1;
 const auto end=text.size()-1;
 const auto skipBlank=[&]{while(position<end&&text[position].isSpace())++position;};
 skipBlank();
 if(position==end)return values;
 while(position<end){
  const auto quote=text[position];
  if(quote!='\''&&quote!='"')return std::nullopt;
  ++position;
  QString value;
  bool closed=false;
  for(;position<end;++position){
   const auto c=text[position];
   if(c=='\\'){if(++position==end)return std::nullopt;value+=text[position];continue;}
   if(c==quote){closed=true;++position;break;}
   value+=c;
  }
  if(!closed)return std::nullopt;
  values.append(value);
  skipBlank();
  if(position==end)break;
  if(text[position]!=',')return std::nullopt;
  ++position;
  skipBlank();
  if(position==end)return std::nullopt;
 }
 return values;
}
// Inserts before the closing bracket, so every existing entry keeps its exact text.
std::optional<QString> appendToStringList(const QString& raw,const QString& value){
 const auto parsed=parseStringList(raw);
 if(!parsed)return std::nullopt;
 const auto trimmed=raw.trimmed();
 if(parsed->isEmpty())return QStringLiteral("['%1']").arg(value);
 auto text=trimmed;
 if(text.startsWith("@as "))text=text.mid(4).trimmed();
 text.chop(1);
 return QStringLiteral("%1, '%2']").arg(text,value);
}

SetupStatus runExtensionSetup(const SetupInputs& in){
 auto shell=in.shell();
 if(shell.version.isEmpty())return status("checking",{},QStringLiteral("Waiting for GNOME Shell."));
 const auto major=shellMajor(shell.version);
 QJsonObject details{{"shellVersion",shell.version}};
 if(major!=50)return status("unsupported_shell","gnome_shell_unsupported",
  QStringLiteral("put.gd's GNOME extension needs GNOME 50; this session runs GNOME %1.").arg(shell.version),details);

 const auto userDir=QDir(in.dataHome).filePath(QStringLiteral("gnome-shell/extensions/")+extensionUuid);
 auto userCopy=inspectUserCopy(userDir);
 int systemVersion=0;
 const bool systemCopy=hasSystemCopy(in.dataDirs,&systemVersion);
 const auto loadedPath=shell.extension["path"].toString();
 bool loginNeededForFiles=false;

 // A copy we installed earlier would shadow a system package, including a newer one.
 if(userCopy.managed&&systemCopy){
  if(!removeCopyFiles(userDir,userCopy.files))return installFailed(QStringLiteral("cannot remove the old copy at %1").arg(userDir));
  loginNeededForFiles=loadedPath==userDir;
  userCopy={};
 }
 if(userCopy.exists&&!userCopy.managed){
  details["path"]=userDir;
  return status("foreign_user_copy","gnome_foreign_extension",
   QStringLiteral("A copy of the put.gd extension that put.gd did not install is in %1. Remove it and start put.gd again, or turn that copy on yourself in the Extensions app.").arg(userDir),details);
 }
 if(systemCopy)details["installedVersion"]=systemVersion;

 if(!systemCopy){
  if(in.bundledDir.isEmpty())return status("not_installed","gnome_extension_missing",QStringLiteral("put.gd's GNOME extension is not installed. Reinstall the put.gd package."),details);
  // Before anything is written: a blocked system must not gain a copy or a setting.
  const auto allowReply=runGet(in,"allow-extension-installation");
  if(allowReply.started&&allowReply.exitCode==0&&allowReply.standardOutput.trimmed()=="false")
   return status("extension_blocked","gnome_extension_blocked",QStringLiteral("This system does not allow per-user GNOME extensions, so an AppImage can't add put.gd's. Ask your administrator to install the put.gd package."),details);
  const auto bundledVersion=metadataVersion(in.bundledDir);
  const bool replacing=userCopy.managed;
  // Keep a copy in step with this AppImage; an older image never downgrades a newer copy.
  const bool newer=!replacing||bundledVersion>userCopy.version;
  const bool changed=!newer&&bundledVersion==userCopy.version&&
   (fileSha256(QDir(in.bundledDir).filePath("extension.js"))!=fileSha256(QDir(userDir).filePath("extension.js")));
  if(newer||changed){
   if(auto error=installCopy(in.bundledDir,userDir,bundledVersion,userCopy))return installFailed(*error);
   // A replaced copy still loaded from memory; the next login picks up the new one.
   if(replacing&&(newer||changed))loginNeededForFiles=loginNeededForFiles||loadedPath==userDir;
  }
  details["installedVersion"]=bundledVersion;
 }else if(!in.bundledDir.isEmpty()&&systemVersion<in.packagedVersion){
  // Never a user copy beside a system one, so an AppImage can't fix this itself.
  details["path"]=userDir;
  details["packagedVersion"]=in.packagedVersion;
  return status("system_copy_outdated","gnome_system_extension_outdated",
   QStringLiteral("The put.gd extension installed on this system (version %1) is older than this AppImage's (version %2). Update the put.gd package to match.").arg(systemVersion).arg(in.packagedVersion),details);
 }

 // The settings: a user copy needs the Shell to allow per-user extensions; a
 // deliberate disable (the Extensions app, gnome-extensions disable) is respected.
 const auto disabledReply=runGet(in,"disabled-extensions");
 if(!disabledReply.started||disabledReply.exitCode!=0)return settingsUnavailable(disabledReply);
 const auto disabled=parseStringList(disabledReply.standardOutput);
 if(disabled&&disabled->contains(extensionUuid))
  return status("disabled_by_user","gnome_extension_disabled",QStringLiteral("The put.gd extension is turned off. Turn it back on in the Extensions app to use capture."),details);

 const auto disableUserReply=runGet(in,"disable-user-extensions");
 const bool userExtensionsOff=disableUserReply.started&&disableUserReply.exitCode==0&&disableUserReply.standardOutput.trimmed()=="true";

 const auto enabledReply=runGet(in,"enabled-extensions");
 if(!enabledReply.started||enabledReply.exitCode!=0)return settingsUnavailable(enabledReply);
 const auto enabled=parseStringList(enabledReply.standardOutput);
 if(!enabled)return settingsUnavailable({true,enabledReply.exitCode,{},QStringLiteral("unexpected reply: %1").arg(enabledReply.standardOutput.trimmed())});
 bool enabledChanged=false;
 if(!enabled->contains(extensionUuid)){
  const auto appended=appendToStringList(enabledReply.standardOutput,extensionUuid);
  if(!appended)return settingsUnavailable({true,0,{},QStringLiteral("unexpected reply")});
  const auto writable=in.gsettings({"writable",settingsSchema,"enabled-extensions"});
  if(writable.started&&writable.exitCode==0&&writable.standardOutput.trimmed()=="false")
   return status("settings_locked","gnome_settings_locked",QStringLiteral("GNOME's extension list is locked on this system, so put.gd can't turn its extension on."),details);
  const auto write=in.gsettings({"set",settingsSchema,"enabled-extensions",*appended});
  if(!write.started||write.exitCode!=0)return settingsUnavailable(write);
  enabledChanged=true;
 }

 if(userExtensionsOff)
  return status("user_extensions_disabled","gnome_user_extensions_off",QStringLiteral("GNOME extensions are turned off for this account. Turn on Extensions in the Extensions app, and put.gd's extension will start too."),details);
 shell=in.shell();
 for(int waited=0;enabledChanged&&waited<30;++waited){
  const auto current=shell.extension.value("state").toInt();
  if(current==extensionActive||current==extensionError||current==extensionOutOfDate)break;
  if(shell.extension.isEmpty())break;
  QThread::msleep(100);
  shell=in.shell();
 }
 const auto extension=shell.extension;
 details["loaded"]=extension["version"].toInt();
 details["installedVersion"]=systemCopy?systemVersion:details.value("installedVersion");
 if(extension.isEmpty())return loginRequired(details);
 const auto current=extension["state"].toInt();
 const auto error=extension["error"].toString();
 if(current==extensionError||current==extensionOutOfDate)
  return status("extension_error","gnome_extension_error",
   QStringLiteral("GNOME Shell could not start the put.gd extension%1").arg(error.isEmpty()?QStringLiteral("."):QStringLiteral(": %1").arg(error)),details);
 if(loginNeededForFiles)return loginRequired(details);
 if(current==extensionActive||current==extensionActivating)return extension["version"].toInt()<in.packagedVersion?loginRequired(details):ready(details);
 // The Shell knows the extension but hasn't activated it (screen locked, or still starting).
 return status("checking",{},QStringLiteral("Waiting for GNOME Shell to start the put.gd extension."),details);
}

SetupStatus removeOwnedUserCopy(const SetupInputs& in){
 const auto userDir=QDir(in.dataHome).filePath(QStringLiteral("gnome-shell/extensions/")+extensionUuid);
 const auto copy=inspectUserCopy(userDir);
 if(copy.exists&&!copy.managed)return status("foreign_user_copy","gnome_foreign_extension",
  QStringLiteral("The copy in %1 was not installed by put.gd, so it was left alone.").arg(userDir));
 // With a system package installed the entry is the package's now; keep it.
 // Without one, a stale entry is cleared even if the copy is already gone.
 int systemVersion=0;
 if(!hasSystemCopy(in.dataDirs,&systemVersion)){
  // The setting goes first, so a failure here leaves a working install, not a stale entry.
  const auto enabledReply=runGet(in,"enabled-extensions");
  if(!enabledReply.started||enabledReply.exitCode!=0)return settingsUnavailable(enabledReply);
  const auto enabled=parseStringList(enabledReply.standardOutput);
  if(!enabled)return settingsUnavailable({true,enabledReply.exitCode,{},QStringLiteral("unexpected reply: %1").arg(enabledReply.standardOutput.trimmed())});
  if(enabled->contains(extensionUuid)){
   QStringList quoted;
   for(const auto& entry:*enabled)if(entry!=extensionUuid)quoted.append(QStringLiteral("'%1'").arg(QString(entry).replace('\\',QStringLiteral("\\\\")).replace('\'',QStringLiteral("\\'"))));
   const auto write=in.gsettings({"set",settingsSchema,"enabled-extensions","["+quoted.join(", ")+"]"});
   if(!write.started||write.exitCode!=0)return settingsUnavailable(write);
  }
 }
 if(copy.managed&&!removeCopyFiles(userDir,copy.files))return installFailed(QStringLiteral("cannot remove the files in %1").arg(userDir));
 return ready({{"removed",copy.managed}});
}

SetupStatus reportedSetup(const SetupStatus& last,bool helperReady,int loadedVersion,int packagedVersion){
 if(!helperReady)return last;
 // The write failed, so the disk doesn't hold what a connected helper implies.
 if(last.state=="install_failed")return last;
 if(loadedVersion>=packagedVersion)return {"ready",{},{},last.details};
 // A login loads what is on disk. Most states that need the user mean that won't
 // bring the packaged version, so asking for one would send them the wrong way.
 // A failed gsettings read is the exception: the files are in place and the loaded
 // extension proves it is already enabled.
 if(last.needsUser()&&last.state!="login_required"&&last.state!="settings_unavailable")return last;
 return {"login_required","gnome_extension_updated",QStringLiteral("The put.gd GNOME Shell extension was updated; log out and back in to load the new version"),
  {{"loaded",loadedVersion},{"packaged",packagedVersion}}};
}

QJsonObject SetupStatus::toJson()const{
 QJsonObject result{{"state",state}};
 if(!code.isEmpty())result["code"]=code;
 if(!message.isEmpty())result["message"]=message;
 if(!details.isEmpty())result["details"]=details;
 return result;
}

bool ownedCopyMarkerExists(const SetupInputs& in){
 return QFileInfo::exists(QDir(in.dataHome).filePath(QStringLiteral("gnome-shell/extensions/%1/%2").arg(extensionUuid,copyMarkerName)));
}

SetupInputs defaultSetupInputs(){
 SetupInputs inputs;
 auto environment=host::environment();
 const auto home=environment.value("HOME");
 // GNOME's own dconf reader and writer run with the login session's environment.
 // A resident started with a private XDG_CONFIG_HOME/XDG_DATA_HOME (the lab harness)
 // must still see the session's values, or `gsettings get` would read a different
 // database than the `gsettings set` it follows and could drop real entries.
 applySessionXdg(environment);
 inputs.dataHome=environment.contains("XDG_DATA_HOME")?environment.value("XDG_DATA_HOME"):QDir(home).filePath(".local/share");
 inputs.dataDirs=environment.value("XDG_DATA_DIRS","/usr/local/share:/usr/share").split(':',Qt::SkipEmptyParts);
 inputs.packagedVersion=gnome::extensionVersion;
 // Only inside an AppImage is the extension next to this binary.
 const auto current=QProcessEnvironment::systemEnvironment();
 if(host::insideAppDir(current,QCoreApplication::applicationFilePath()))
  inputs.bundledDir=QDir(current.value("APPDIR")).filePath(QStringLiteral("usr/share/gnome-shell/extensions/")+extensionUuid);
 inputs.gsettings=[environment](const QStringList& arguments){
  ProcessResult result;
  const auto program=host::findProgram("gsettings",environment);
  if(program.isEmpty())return result;
  QProcess process;process.setProcessEnvironment(environment);process.setProgram(program);process.setArguments(arguments);
  process.setStandardInputFile(QProcess::nullDevice());
  process.start();
  if(!process.waitForStarted(5000))return result;
  result.started=true;
  if(!process.waitForFinished(10000)){process.kill();process.waitForFinished(1000);result.exitCode=-1;result.standardError=QStringLiteral("gsettings timed out");return result;}
  result.exitCode=process.exitStatus()==QProcess::NormalExit?process.exitCode():-1;
  result.standardOutput=QString::fromUtf8(process.readAllStandardOutput());
  result.standardError=QString::fromUtf8(process.readAllStandardError());
  return result;
 };
 inputs.shell=[]{
  ShellFacts facts;
  auto bus=QDBusConnection::sessionBus();
  auto property=QDBusMessage::createMethodCall(shellService,shellPath,propertiesInterface,"Get");
  property.setArguments({QString::fromLatin1(extensionsInterface),QStringLiteral("ShellVersion")});
  const auto versionReply=bus.call(property,QDBus::Block,2000);
  if(versionReply.type()!=QDBusMessage::ReplyMessage||versionReply.arguments().isEmpty())return facts;
  facts.version=versionReply.arguments().first().value<QDBusVariant>().variant().toString();
  auto info=QDBusMessage::createMethodCall(shellService,shellPath,extensionsInterface,"GetExtensionInfo");
  info.setArguments({QString::fromLatin1(extensionUuid)});
  const auto infoReply=bus.call(info,QDBus::Block,2000);
  if(infoReply.type()==QDBusMessage::ReplyMessage&&!infoReply.arguments().isEmpty())
   facts.extension=QJsonObject::fromVariantMap(qdbus_cast<QVariantMap>(infoReply.arguments().first()));
  return facts;
 };
 return inputs;
}
}
