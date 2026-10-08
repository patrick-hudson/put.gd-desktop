#include "LocalArchive.h"
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
 const auto fi=QFileInfo(a.name); auto ext=fi.suffix();
 auto base=ext.isEmpty()?fi.fileName():fi.fileName().left(fi.fileName().size()-ext.size()-1);
 const auto unique=QUuid::createUuid().toString(QUuid::WithoutBraces);
 while(ext.toUtf8().size()>210)ext.chop(1);
 while((base+"-"+unique+(ext.isEmpty()?QString{}:"."+ext)).toUtf8().size()>255&&!base.isEmpty())base.chop(1);
 r.path=QDir(dir).filePath(base+"-"+unique+(ext.isEmpty()?QString{}:"."+ext));
 if(!temp.rename(r.path)) return fail();
 temp.setAutoRemove(false); r.status="saved"; return r;
}
}
