#include "LocalArchive.h"
#include "UploadNaming.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryFile>
#include <QUuid>
namespace putgd {
QJsonObject LocalCopy::toJson() const {return {{"status",status},{"path",path},{"error",error.toJson()}};}
LocalCopy LocalCopy::fromJson(const QJsonObject& o) {auto e=o["error"].toObject();return {o["status"].toString("disabled"),o["path"].toString(),{e["code"].toString(),e["message"].toString(),e["details"].toObject()}};}
QString LocalArchive::folder(const ArchiveSettings& s,const QString& category) {return s.folders.value(category).isEmpty()?QDir(s.root).filePath(category):s.folders.value(category);}
LocalCopy LocalArchive::save(const Artifact& a,const ArchiveSettings& s) {
 LocalCopy r; r.status="failed"; const auto dir=folder(s,a.category);
 auto fail=[&] {r.error={"archive_failed","Cannot create local copy; retry the retained artifact",{}};return r;};
 if(!QDir().mkpath(dir)) return fail();
 QFile in(a.path); if(!in.open(QIODevice::ReadOnly)) return fail();
 QTemporaryFile temp(QDir(dir).filePath(".putgd-XXXXXX")); if(!temp.open()) return fail();
 while(!in.atEnd()) {const auto chunk=in.read(1024*1024); if((chunk.isEmpty()&&in.error()!=QFile::NoError)||temp.write(chunk)!=chunk.size()) return fail();}
 if(!temp.flush()) return fail();
 temp.close();
 const auto name=UploadNaming::candidate(a,"original");
 r.path=QDir(dir).filePath(name);
 // QFile::rename refuses an existing destination. Try the requested basename
 // first and use a fresh suffix only after a local collision is confirmed.
 const auto temporaryPath=temp.fileName();
 bool saved=QFile::rename(temporaryPath,r.path);
 for(int attempt=0;!saved&&attempt<32;++attempt) {
  if(!QFileInfo::exists(r.path))return fail();
  r.path=QDir(dir).filePath(UploadNaming::renamedCandidate(name));
  saved=QFile::rename(temporaryPath,r.path);
 }
 if(!saved)return fail();
 temp.setAutoRemove(false); r.status="saved"; return r;
}
}
