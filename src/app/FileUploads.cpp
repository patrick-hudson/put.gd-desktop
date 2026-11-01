#include "FileUploads.h"
#include "core/ArtifactStore.h"
#include "core/FolderZip.h"
#include "core/UploadNaming.h"
#include <QDirIterator>
#include <QFileInfo>
#include <QJsonArray>
namespace putgd::FileUploads {
namespace {
QString size(qint64 bytes){return ArtifactStore::sizeText(bytes);}
// At most this many bad files are listed in the message; details carry them all, bounded.
constexpr int listedProblems=8,maximumProblems=200;
}
Route resolve(const Settings& settings,const QString& requested){
 Route route;
 const auto defaults=settings.document["captureDefaults"].toObject();
 route.publicationMode=defaults["publicationMode"].toString("default");
 if(!requested.isEmpty()){
  if(!settings.profiles.contains(requested))route.error={"profile_not_found","No destination has this ID",{{"profileId",requested}}};
  else route.profileId=requested;
  return route;
 }
 // captureDefaults.profileId may be null even when its destination is upload.
 if(defaults["destination"]=="upload"&&defaults["profileId"].isString()&&settings.profiles.contains(defaults["profileId"].toString())){route.profileId=defaults["profileId"].toString();return route;}
 if(settings.profiles.size()==1){route.profileId=settings.profiles.cbegin().key();return route;}
 route.error={"destination_required",settings.profiles.isEmpty()?QStringLiteral("No upload destination is set up. Add one in put.gd Settings → Destinations, then try again."):QStringLiteral("Choose a default destination in put.gd Settings → Capture, then try again."),{{"destinations",int(settings.profiles.size())}}};
 return route;
}
QJsonObject routedDocument(const Settings& settings,const Route& route){
 auto document=settings.document;
 document["captureDefaults"]=QJsonObject{{"destination","upload"},{"profileId",route.profileId},{"publicationMode",route.publicationMode}};
 return document;
}
Error matchInstallation(const QString& requested,const QString& resident){
 if(requested==resident)return {};
 const auto name=[](const QString& value){return value=="appimage"?QStringLiteral("the AppImage"):QStringLiteral("the installed package");};
 return {"installation_mismatch",QStringLiteral("put.gd from %1 is running. Quit it from its tray menu, or use the menu entry of that installation.").arg(name(resident)),{{"resident",resident},{"requested",requested}}};
}
Selection select(const QStringList& paths,const Settings& settings,const QString& action,qint64 objectLimit,qint64 diskAvailable){
 Selection result;result.zipFolders=settings.fileManagerFolders!="files";
 const auto& limits=settings.limits;
 QJsonArray problems;QStringList listed;
 const auto reject=[&](const QString& path,const QString& code,const QString& reason){
  const auto name=QFileInfo(path).fileName().isEmpty()?path:QFileInfo(path).fileName();
  if(problems.size()<maximumProblems)problems.append(QJsonObject{{"path",path},{"code",code},{"message",reason}});
  if(listed.size()<listedProblems)listed.append(QStringLiteral("%1: %2").arg(name.left(120),reason));
 };
 // The name an item would upload under: the naming policy decides, as for every upload.
 const auto named=[&](const QString& path,const QString& name)->bool{
  Artifact artifact;artifact.name=name;artifact.originalFile=true;artifact.mime=QStringLiteral("application/octet-stream");
  const auto chosen=UploadNaming::acquiredName(artifact,settings,action,QDateTime::currentDateTime());
  if(chosen.error){reject(path,chosen.error.code,QStringLiteral("this name can't be uploaded (it has a control character, newline or backslash)"));return false;}
  return true;
 };
 qint64 batch=0;bool counted=true;
 const auto addFile=[&](const QString& path,const QFileInfo& info,const QString& origin){
  if(!info.isReadable()){reject(path,"source_unavailable","can't be read");return;}
  if(info.size()==0){reject(path,"source_empty","is empty (0 bytes)");return;}
  if(!named(path,info.fileName()))return;
  if(objectLimit>0&&info.size()>objectLimit){reject(path,"destination_object_too_large",QStringLiteral("is %1; this destination accepts files of at most %2").arg(size(info.size()),size(objectLimit)));return;}
  if(info.size()>limits.maxFileBytes){reject(path,"input_excessive",QStringLiteral("is %1; the Largest file limit is %2").arg(size(info.size()),size(limits.maxFileBytes)));return;}
  result.files.append(path);result.origins.append(origin);batch+=info.size();result.diskBytes+=info.size();
 };
 for(const auto& path:paths){
  if(!QFileInfo(path).isAbsolute()){reject(path,"invalid_path","is not an absolute path");continue;}
  const QFileInfo info(path);
  if(!info.exists()){reject(path,"source_missing","no longer exists");continue;}
  if(info.isDir()){
   if(!info.isReadable()||!info.isExecutable()){reject(path,"source_unavailable","can't be read");continue;}
   if(result.zipFolders){
    const auto plan=FolderZip::plan(info.absoluteFilePath());
    if(plan.error){reject(path,plan.error.code,plan.error.message);continue;}
    if(!named(path,plan.archiveName))continue;
    // The folder's contents are summed before writing: a compressible folder whose
    // archive would fit can still be refused here, and the message says so.
    const auto bound=std::max(plan.sourceBytes,plan.finalBound);
    if(objectLimit>0&&bound>objectLimit){reject(path,"destination_object_too_large",QStringLiteral("folder totals %1; this destination accepts files of at most %2").arg(size(bound),size(objectLimit)));continue;}
    if(bound>limits.maxFileBytes){reject(path,"input_excessive",QStringLiteral("folder totals %1; the Largest file limit is %2").arg(size(bound),size(limits.maxFileBytes)));continue;}
    result.files.append(info.absoluteFilePath());result.origins.append(path);batch+=plan.sourceBytes;result.diskBytes+=plan.peakBytes;
    continue;
   }
   // Files mode: every regular file under the folder; symlinks, sockets and devices are skipped.
   QDirIterator walk(info.absoluteFilePath(),QDir::Files|QDir::Hidden|QDir::NoSymLinks,QDirIterator::Subdirectories);
   while(walk.hasNext()){
    const auto file=walk.next();const auto entry=walk.fileInfo();
    if(!entry.isFile()||entry.isSymLink())continue;
    if(result.files.size()>=limits.maxFiles){counted=false;break;}
    addFile(file,entry,path);
   }
   if(!counted){reject(path,"input_excessive",QStringLiteral("folder has more than %1 files, the file limit. Upload it as a .zip, or raise the limit in Settings → Advanced").arg(limits.maxFiles));break;}
   continue;
  }
  if(!info.isFile()){reject(path,"source_unsupported","is not a regular file or folder");continue;}
  if(result.files.size()>=limits.maxFiles){reject(path,"input_excessive",QStringLiteral("is past the file limit of %1 files").arg(limits.maxFiles));break;}
  addFile(info.absoluteFilePath(),info,path);
 }
 result.sourceBytes=batch;
 if(problems.isEmpty()&&result.files.isEmpty())reject(QStringLiteral("selection"),"input_excessive","contains no files to upload");
 if(problems.isEmpty()&&batch>limits.maxBatchBytes)reject(QStringLiteral("selection"),"input_excessive",QStringLiteral("totals %1; the Largest batch limit is %2").arg(size(batch),size(limits.maxBatchBytes)));
 if(problems.isEmpty()&&diskAvailable>=0&&result.diskBytes>diskAvailable)
  result.error={"disk_space_insufficient",QStringLiteral("Not enough free disk space to import: it needs %1, and %2 can be used in put.gd's data folder.").arg(size(result.diskBytes),size(diskAvailable)),{{"requiredBytes",double(result.diskBytes)},{"availableBytes",double(diskAvailable)}}};
 if(!problems.isEmpty()){
  const auto more=problems.size()>listed.size()?QStringLiteral("\n… and %1 more").arg(problems.size()-listed.size()):QString();
  // Size refusals keep their own code when every problem is one; anything else is files_rejected.
  QString code=problems.first().toObject()["code"].toString();
  for(const auto& value:problems)if(value.toObject()["code"].toString()!=code)code.clear();
  if(code!="input_excessive"&&code!="destination_object_too_large")code=QStringLiteral("files_rejected");
  result.error={code,(problems.size()==1?QStringLiteral("Nothing was uploaded. "):QStringLiteral("Nothing was uploaded:\n"))+listed.join('\n')+more,{{"files",problems}}};
  result.files.clear();result.origins.clear();
 }
 return result;
}
}
