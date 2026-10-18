#include "StorageLocations.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QLockFile>
#include <QStandardPaths>
#ifdef Q_OS_UNIX
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <sys/stat.h>
#endif
namespace putgd {
namespace {
bool present(const QString& path) {const QFileInfo info(path);return info.exists()||info.isSymLink();}
// One atomic rename that never replaces an existing entry and never copies.
bool move(const QString& from,const QString& to) {
#ifdef Q_OS_UNIX
 const auto source=QFile::encodeName(from),target=QFile::encodeName(to);
#if defined(Q_OS_LINUX)&&defined(RENAME_NOREPLACE)
 if(::renameat2(AT_FDCWD,source.constData(),AT_FDCWD,target.constData(),RENAME_NOREPLACE)==0)return true;
 if(errno!=EINVAL&&errno!=ENOSYS)return false;
#endif
 struct stat existing;
 return ::lstat(target.constData(),&existing)!=0&&errno==ENOENT&&::rename(source.constData(),target.constData())==0;
#else
 return !present(to)&&QDir().rename(from,to);
#endif
}
Error interrupted() {return {"storage_migration_interrupted","Moving put.gd files stopped; it resumes at the next start",{}};}
// A journal lock in a tree about to move must be stale: a live owner is an
// earlier put.gd still running from it. Releasing a stale lock removes the file.
Error releaseStaleJournal(const QString& root) {
 if(!present(root+"/resident.lock"))return {};
 QLockFile owner(root+"/resident.lock");owner.setStaleLockTime(0);
 if(!owner.tryLock(0))return {"storage_busy","An earlier put.gd version is still using its data folder; quit it and start put.gd again",{{"path",root}}};
 owner.unlock();return {};
}
// Renames each staged entry into the root. An entry the root already has stays
// staged and is reported; nothing is replaced or copied.
Error finish(const QString& root,QJsonObject& report,const StorageLocations::MoveHook& hook) {
 const auto staging=StorageLocations::stagingRoot(root);
 if(auto error=releaseStaleJournal(staging))return error;
 QJsonArray conflicts;
 for(const auto& name:QDir(staging).entryList(QDir::AllEntries|QDir::Hidden|QDir::System|QDir::NoDotAndDotDot,QDir::Name)) {
  const auto from=staging+"/"+name,to=root+"/"+name;
  if(present(to)){conflicts.append(name);continue;}
  if(hook&&!hook(from,to))return interrupted();
  if(!move(from,to))return {"storage_migration_failed","Cannot move put.gd files into their new folder; the rest stay staged and moving resumes at the next start",{{"from",from},{"to",to}}};
 }
 report["state"]="migrated";
 QFile::setPermissions(root,QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner);
 if(!conflicts.isEmpty()) {report["state"]="conflict";report["stagingPath"]=staging;report["conflicts"]=conflicts;return {};}
 if(hook&&!hook(staging,{}))return interrupted();
 if(!QDir().rmdir(staging))return {"storage_migration_failed","Cannot remove the emptied staging folder",{{"path",staging}}};
 return {};
}
// Returns the root to use. Markers identify a root already used by this layout.
QString prepareRoot(const QString& root,const QStringList& markers,const StorageLocations::MoveHook& hook,QJsonObject& report,Error& error) {
 const auto legacy=StorageLocations::legacyRoot(root),staging=StorageLocations::stagingRoot(root);
 const QFileInfo old(legacy);
 if(present(staging)) {
  // An interrupted move always completes before the root is opened. A legacy
  // tree recreated since then by an earlier version is left unchanged.
  error=finish(root,report,hook);
  if(!error&&old.isDir())report["legacyPath"]=legacy;
  return root;
 }
 if(!old.isDir())return root;
 for(const auto& marker:markers)if(present(root+"/"+marker)) {
  report["state"]="legacy_retained";report["legacyPath"]=legacy;
  qInfo("put.gd uses %s and left the earlier folder %s unchanged",qUtf8Printable(root),qUtf8Printable(legacy));
  return root;
 }
 // A linked or mounted legacy tree cannot move by rename; keep using it.
 if(old.isSymLink()) {report={{"path",legacy},{"state","legacy_active"},{"reason","symlink"},{"legacyPath",legacy}};return legacy;}
 if((error=releaseStaleJournal(legacy)))return root;
 if(hook&&!hook(legacy,staging)){error=interrupted();return root;}
 if(!move(legacy,staging)) {report={{"path",legacy},{"state","legacy_active"},{"reason","rename_failed"},{"legacyPath",legacy}};return legacy;}
 if(!(error=finish(root,report,hook)))qInfo("put.gd moved %s to %s",qUtf8Printable(legacy),qUtf8Printable(root));
 return root;
}
}
StorageLocations StorageLocations::prepare() {
 return prepare(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation),QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation));
}
StorageLocations StorageLocations::prepare(const QString& genericConfig,const QString& genericData,const MoveHook& hook) {
 StorageLocations result;
 if(genericConfig.isEmpty()||genericData.isEmpty()) {result.error={"storage_unavailable","Cannot resolve the put.gd configuration and data folders",{}};return result;}
 const auto config=QDir::cleanPath(genericConfig+"/putgd"),data=QDir::cleanPath(genericData+"/putgd");
 result.configRoot=config;result.dataRoot=data;
 QJsonObject configReport{{"path",config},{"state","current"}},dataReport{{"path",data},{"state","current"}};
 const auto pending=[](const QString& root){return present(legacyRoot(root))||present(stagingRoot(root));};
 if(pending(config)||pending(data)) {
  // The resident's own journal lock serializes concurrent starts and refuses a
  // running resident of this layout; it is released before the journal opens.
  if(!QDir().mkpath(data)) {result.error={"storage_unavailable","Cannot create the put.gd data folder",{{"path",data}}};return result;}
  QLockFile owner(data+"/resident.lock");owner.setStaleLockTime(0);
  if(!owner.tryLock(5000)) {result.error={"storage_busy","Another put.gd process is using the data folder; quit it and start put.gd again",{{"path",data}}};return result;}
  // Windows resolves both generic locations to one folder: it is one root.
  const bool shared=config==data;
  result.dataRoot=prepareRoot(data,shared?QStringList{"history.sqlite","config.json"}:QStringList{"history.sqlite"},hook,dataReport,result.error);
  if(shared) {result.configRoot=result.dataRoot;configReport=dataReport;}
  else if(!result.error)result.configRoot=prepareRoot(config,{"config.json"},hook,configReport,result.error);
 }
 result.report={{"schemaVersion",1},{"config",configReport},{"data",dataReport}};
 return result;
}
}
