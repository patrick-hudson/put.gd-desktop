#include "ArtifactStore.h"
#include "UploadNaming.h"
#include <QBuffer>
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QMimeDatabase>
#include <QImageReader>
#include "ImageFormat.h"
#include "ImageJob.h"
#include <QRegularExpression>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>
#include <QSaveFile>
#include <QStorageInfo>
#include <QUuid>
#include <atomic>
#include <cmath>
#include <mutex>
#include <utility>
namespace putgd {
namespace {
QString category(const QString& mime) { if(mime.startsWith("image/")) return "images"; if(mime.startsWith("video/")) return "videos"; if(mime.startsWith("text/")) return "text"; return "other"; }
std::atomic<qint64> freeOverride{-1},totalOverride{-1};
// Bytes of imports in progress per filesystem, so concurrent imports (#28) and a
// recording's budget never both count the same free space.
std::mutex reservationsMutex;QHash<QString,qint64> reservations;
QString filesystem(const QString& path){QStorageInfo volume(path);return volume.isValid()?volume.rootPath():QStringLiteral("/");}
struct Reservation {
 QString key;qint64 bytes=0;
 ~Reservation(){if(bytes<=0)return;std::lock_guard lock(reservationsMutex);if((reservations[key]-=bytes)<=0)reservations.remove(key);}
};
QString size(qint64 bytes) {
 constexpr double mib=1024.0*1024,gib=mib*1024,tib=gib*1024;
 if(bytes>=tib)return QString::number(bytes/tib,'f',1)+" TiB";
 if(bytes>=gib)return QString::number(bytes/gib,'f',1)+" GiB";
 return QString::number(qMax(1.0,std::ceil(bytes/mib)),'f',0)+" MiB";
}
}
QJsonObject ArtifactStore::DiskBudget::toJson() const {return {{"freeBytes",double(freeBytes)},{"totalBytes",double(totalBytes)},{"reserveBytes",double(reserveBytes)},{"reservedBytes",double(reservedBytes)},{"availableBytes",double(available())}};}
qint64 ArtifactStore::reserveFor(qint64 total) {constexpr qint64 gib=1LL<<30;return qMax(gib,qMin(total>0?total*15/100:0,4*gib));}
ArtifactStore::DiskBudget ArtifactStore::diskBudget(const QString& path) {
 DiskBudget budget;QStorageInfo volume(path);volume.refresh();
 budget.freeBytes=freeOverride.load()>=0?freeOverride.load():volume.isValid()?volume.bytesAvailable():-1;
 budget.totalBytes=totalOverride.load()>=0?totalOverride.load():volume.isValid()?volume.bytesTotal():-1;
 budget.reserveBytes=reserveFor(budget.totalBytes);
 std::lock_guard lock(reservationsMutex);budget.reservedBytes=reservations.value(filesystem(path));
 return budget;
}
std::shared_ptr<void> ArtifactStore::reserveDisk(const QString& path,qint64 bytes,DiskBudget* seen) {
 const auto key=filesystem(path);auto budget=diskBudget(path);
 std::lock_guard lock(reservationsMutex);budget.reservedBytes=reservations.value(key);
 if(seen)*seen=budget;
 if(budget.freeBytes>=0&&bytes>budget.available())return {};
 reservations[key]+=bytes;
 auto hold=std::make_shared<Reservation>();hold->key=key;hold->bytes=bytes;return hold;
}
void ArtifactStore::setDiskForTesting(qint64 freeBytes,qint64 totalBytes) {freeOverride.store(freeBytes);totalOverride.store(totalBytes);}
QJsonObject Artifact::toJson() const {return {{"id",id},{"path",path},{"name",name},{"mime",mime},{"category",category},{"bytes",double(bytes)},{"sha256",sha256},{"originalFile",originalFile},{"previewPath",previewPath},{"namingPrepared",namingPrepared}};}
Artifact Artifact::fromJson(const QJsonObject& o) {return {o["id"].toString(),o["path"].toString(),o["name"].toString(),o["mime"].toString(),o["category"].toString(),o["sha256"].toString(),qint64(o["bytes"].toDouble()),o["originalFile"].toBool(),o["previewPath"].toString(),o["namingPrepared"].toBool()};}
ArtifactStore::ArtifactStore(QString root):root_(std::move(root)) {QDir().mkpath(root_); QFile::setPermissions(root_,QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner);}
qint64 ArtifactStore::bytes() const {qint64 n=0; QDirIterator it(root_,QDir::Files,QDirIterator::Subdirectories); while(it.hasNext()) {const QFileInfo file(it.next());if(!QUuid(file.fileName()).isNull())n+=file.size();} return n;}
Error ArtifactStore::remove(const QString& id) {
 if(QUuid(id).isNull())return {"invalid_id","Invalid operation ID",{}};
 QDir directory(root_+"/"+id);
 // Remove only spool filenames created by acquisition. User-owned archives or
 // unrelated files remain intact even if a user chose a folder inside this tree.
 for(const auto& name:directory.entryList(QDir::Files|QDir::Hidden))if(!QUuid(name).isNull()&&!QFile::remove(directory.filePath(name)))return {"artifact_io","Cannot remove artifacts",{}};
 QFile::remove(directory.filePath("_manifest.json"));QDir().rmdir(directory.path());return {};
}
void ArtifactStore::clearPreviews() {
 QDir cache(root_+"/../previews");
 for(const auto& name:cache.entryList(QDir::Files))if(name.endsWith(".png")&&!QUuid(name.chopped(4)).isNull())QFile::remove(cache.filePath(name));
}
QString ArtifactStore::preview(const Artifact& artifact,qint64 budget,qint64 maxPixels) {
 if(budget<=0||artifact.category!="images")return {};
 // UUID spool files have no suffix. Hint only a signature-proven PNG; keep
 // content detection for other formats, including inaccurate MIME metadata.
 QFile input(artifact.path);if(!input.open(QIODevice::ReadOnly))return {};
 QByteArray png;
#ifdef Q_OS_UNIX
 const bool useWorker=imageFormatHint(input.peek(8))!="png";
#else
 // Non-Unix residents retain GUI initialization and their existing codec path.
 const bool useWorker=false;
#endif
 if(!useWorker){
  const auto hint=imageFormatHint(input.peek(8));QImageReader reader(&input,hint);
  // Qt 6.4.2 and 6.8.2 instantiate every installed image plugin for a named
  // format unless detection is off. Only a signature-proven PNG is named.
  if(!hint.isEmpty())reader.setAutoDetectImageFormat(false);
  const auto size=reader.size();
  if(!size.isValid()||qint64(size.width())*size.height()>maxPixels)return {};
  reader.setScaledSize(size.scaled(256,256,Qt::KeepAspectRatio));auto image=reader.read();if(image.isNull())return {};
  image=image.scaled(256,256,Qt::KeepAspectRatio,Qt::SmoothTransformation);QBuffer buffer(&png);buffer.open(QIODevice::WriteOnly);if(!image.save(&buffer,"PNG")||png.size()>budget)return {};
 }else{
  // Generic codecs may render text (SVG), which requires GUI font services.
  // The transient job preserves content sniffing without adding those services
  // or plugin caches to a Core resident. Retained upload bytes are unchanged.
  input.close();Error error;png=ImageJob().preview(artifact.path,maxPixels,budget,error);if(error||png.isEmpty())return {};
 }
 QDir cache(root_+"/../previews");if(!QDir().mkpath(cache.path()))return {};QFile::setPermissions(cache.path(),QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner);
 qint64 used=0;const auto files=cache.entryInfoList(QDir::Files,QDir::Time|QDir::Reversed);for(const auto& file:files)if(file.suffix()=="png"&&!QUuid(file.completeBaseName()).isNull())used+=file.size();
 for(const auto& file:files)if(used+png.size()>budget&&file.suffix()=="png"&&!QUuid(file.completeBaseName()).isNull()) {if(QFile::remove(file.absoluteFilePath()))used-=file.size();}
 const auto path=cache.filePath(artifact.id+".png");QSaveFile output(path);output.setDirectWriteFallback(false);if(!output.open(QIODevice::WriteOnly)||!output.setPermissions(QFile::ReadOwner|QFile::WriteOwner)||output.write(png)!=png.size()||!output.commit())return {};return path;
}

Acquisition ArtifactStore::recover(const QString& operationId) const {
 Acquisition result;if(QUuid(operationId).isNull())return result;
 QFile manifest(root_+"/"+operationId+"/_manifest.json");if(!manifest.exists())return result;
 if(!manifest.open(QIODevice::ReadOnly)||manifest.size()>16*1024*1024) {result.error={"artifact_recovery_failed","Cannot read retained batch metadata",{}};return result;}
 const auto document=QJsonDocument::fromJson(manifest.read(16*1024*1024+1)).object();const auto entries=document["artifacts"].toArray();
 if(document["schemaVersion"]!=1||document["operationId"]!=operationId||!document["artifacts"].isArray()||entries.isEmpty()||entries.size()>10000) {result.error={"artifact_recovery_failed","Retained batch metadata is invalid",{}};return result;}
 QSet<QString> ids;
 for(const auto& entry:entries) {auto artifact=Artifact::fromJson(entry.toObject());if(QUuid(artifact.id).isNull()||ids.contains(artifact.id)) {result.error={"artifact_recovery_failed","Invalid retained artifact identity",{}};result.artifacts.clear();return result;}ids.insert(artifact.id);artifact.path=root_+"/"+operationId+"/"+artifact.id;if(auto e=verify(artifact)) {result.error=e;result.artifacts.clear();return result;}result.artifacts.append(artifact);}
 return result;
}
void ArtifactStore::discardIncomplete() {
 QDir root(root_);
 for(const auto& name:root.entryList(QDir::Dirs|QDir::Hidden|QDir::NoDotAndDotDot)) {
  if(!name.startsWith('.')||name.size()!=74||QUuid(name.mid(1,36)).isNull()||QUuid(name.right(36)).isNull())continue;
  QDir staging(root.filePath(name));for(const auto& file:staging.entryList(QDir::Files|QDir::Hidden))if(!QUuid(file).isNull()||file=="_manifest.json")QFile::remove(staging.filePath(file));QDir().rmdir(staging.path());
 }
}

Error ArtifactStore::verify(const Artifact& a) const {QFile f(a.path); if(!f.open(QIODevice::ReadOnly)) return {"artifact_missing","Retained artifact unavailable",{}}; QCryptographicHash hash(QCryptographicHash::Sha256); if(!hash.addData(&f)||f.size()!=a.bytes||QString::fromLatin1(hash.result().toHex())!=a.sha256) return {"artifact_changed","Retained artifact identity changed",{}}; return {};}
Acquisition ArtifactStore::acquire(const QString& op,const ClipboardPayload& p,const ResourceLimits& limits,
                                   const QString& action,const Settings& settings,const QDateTime& actionTime,qint64 maximumObjectBytes) {
 Acquisition result; const QString staging=root_+"/."+op+"-"+QUuid::createUuid().toString(QUuid::WithoutBraces), target=root_+"/"+op;
 if(!QDir().mkpath(staging)) {result.error={"artifact_io","Cannot create private artifact storage",{}}; return result;}
 QFile::setPermissions(staging,QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner);
 auto fail=[&](QString code,QString message,int index) {result.error={code,message,{{"itemIndex",index}}}; result.artifacts.clear(); QDir(staging).removeRecursively();};
 int count=p.kind=="files"||p.kind=="recording"?p.files.size():1;
 if(count<1 || count>limits.maxFiles) {fail("input_excessive","Invalid batch size",0); return result;}
 qint64 total=0,existing=bytes();
 const auto acquiredAt=actionTime.isValid()?actionTime.toLocalTime():QDateTime::currentDateTime();
 // Reject what the destination can never take, or what the disk cannot hold,
 // before copying anything. Sizes are rechecked while streaming below.
 qint64 required=p.kind=="text"||p.kind=="png"?p.bytes.size():p.kind=="image"?qMin<qint64>(p.image.sizeInBytes(),limits.imageBytes):0;
 if(p.kind=="files"||p.kind=="recording")for(int i=0;i<count;++i){
  const QFileInfo source(p.files[i]);if(!source.isFile())continue;
  if(maximumObjectBytes>0&&source.size()>maximumObjectBytes){
   result.error={"destination_object_too_large",QStringLiteral("%1 is %2; this destination accepts files of at most %3.").arg(source.fileName().left(255),size(source.size()),size(maximumObjectBytes)),{{"itemIndex",i},{"bytes",double(source.size())},{"maximumObjectBytes",double(maximumObjectBytes)}}};
   QDir(staging).removeRecursively();return result;
  }
  required+=source.size();
 }
 // Check and reserve in one step, so a concurrent import sees these bytes as taken.
 DiskBudget budget;const auto hold=reserveDisk(root_,required,&budget);
 {
  if(!hold){
   result.error={"disk_space_insufficient",QStringLiteral("Not enough free disk space to import: it needs %1, and %2 can be used in put.gd's data folder (%3 free, with %4 kept free for the system).").arg(size(required),size(budget.available()),size(budget.freeBytes),size(budget.reserveBytes)),
    {{"requiredBytes",double(required)},{"availableBytes",double(budget.available())},{"freeBytes",double(budget.freeBytes)},{"reserveBytes",double(budget.reserveBytes)}}};
   QDir(staging).removeRecursively();return result;
  }
 }
 for(int i=0;i<count;++i) {
  Artifact a; a.id=QUuid::createUuid().toString(QUuid::WithoutBraces); a.path=staging+"/"+a.id;
  QFile input; QByteArray encoded;
  if(p.kind=="files"||p.kind=="recording") {
   QFileInfo fi(p.files[i]); if(!fi.isFile()) {fail("source_unavailable","Source is not an accessible regular file",i); return result;}
   input.setFileName(fi.absoluteFilePath()); if(!input.open(QIODevice::ReadOnly)) {fail("source_unavailable","Cannot read source file",i); return result;}
   // A finalized recording export is streamed exactly as validated, with its declared
   // type; it is a generated artifact, never an imported original.
   if(p.kind=="recording") {if(p.mime.isEmpty()||!(p.mime.startsWith("video/")||p.mime=="image/gif"||p.mime=="image/webp")) {fail("source_unsupported","Unsupported recording type",i); return result;} a.mime=p.mime; a.name="recording."+fi.suffix();}
   else {a.originalFile=true; a.name=fi.fileName();}
  } else if(p.kind=="text") { encoded=p.bytes; a.mime="text/plain"; a.name="text.txt"; if(encoded.isEmpty()||encoded.size()>limits.clipboardTextBytes) {fail("input_excessive","Invalid text size",i); return result;} }
  else if(p.kind=="image") {
   if(p.image.isNull()||qint64(p.image.width())*p.image.height()>limits.imagePixels||p.image.sizeInBytes()>limits.imageBytes) {fail("input_excessive","Invalid image size",i); return result;}
   QBuffer buffer(&encoded); buffer.open(QIODevice::WriteOnly); if(!p.image.save(&buffer,"PNG")) {fail("encoding_failed","Cannot encode image",i); return result;} a.mime="image/png"; a.name="image.png";
  } else if(p.kind=="png") {
   // Finalized editor bytes are stored exactly as validated; never re-encoded.
   if(p.bytes.isEmpty()) {fail("source_empty","Empty source",i); return result;}
   encoded=p.bytes; a.mime="image/png"; a.name="image.png";
  } else {fail("source_unsupported","Unsupported source",i); return result;}
  const auto named=UploadNaming::acquiredName(a,settings,action,acquiredAt);
  if(named.error) {fail(named.error.code,named.error.message,i);return result;}
  a.name=named.name;a.namingPrepared=true;
  const qint64 expected=input.isOpen()?input.size():encoded.size();
  if(maximumObjectBytes>0&&expected>maximumObjectBytes) {fail("destination_object_too_large",QStringLiteral("This item is %1; this destination accepts files of at most %2.").arg(size(expected),size(maximumObjectBytes)),i); return result;}
  if(expected>limits.maxFileBytes) {fail("input_excessive",QStringLiteral("Artifact budget exceeded: this file is %1 and the Largest file limit is %2").arg(size(expected),size(limits.maxFileBytes)),i); return result;}
  if(expected>limits.maxBatchBytes-total) {fail("input_excessive",QStringLiteral("Artifact budget exceeded: this batch is over the Largest batch limit of %1").arg(size(limits.maxBatchBytes)),i); return result;}
  if(expected>limits.pendingArtifactBytes-existing-total) {fail("input_excessive",QStringLiteral("Artifact budget exceeded: waiting uploads would pass the Waiting uploads limit of %1").arg(size(limits.pendingArtifactBytes)),i); return result;}
  QSaveFile output(a.path); output.setDirectWriteFallback(false); if(!output.open(QIODevice::WriteOnly)) {fail("artifact_io","Cannot store artifact",i); return result;}
  output.setPermissions(QFile::ReadOwner|QFile::WriteOwner); QCryptographicHash hash(QCryptographicHash::Sha256);
  while(input.isOpen()?!input.atEnd():!encoded.isEmpty()) {
   QByteArray chunk=input.isOpen()?input.read(1024*1024):std::exchange(encoded,{});
   if(chunk.isEmpty()&&input.error()!=QFile::NoError) {fail("source_unavailable","Source read failed",i); return result;}
   a.bytes+=chunk.size(); if(a.bytes>limits.maxFileBytes||a.bytes+total>limits.maxBatchBytes||a.bytes+total+existing>limits.pendingArtifactBytes) {fail("input_excessive","Artifact budget exceeded",i); return result;}
   if(output.write(chunk)!=chunk.size()) {fail("artifact_io","Artifact write failed",i); return result;} hash.addData(chunk);
  }
  if(a.bytes==0) {fail("source_empty","Empty source",i); return result;}
  if(!output.commit()) {fail("artifact_io","Artifact commit failed",i); return result;}
  QFile::setPermissions(a.path,QFile::ReadOwner); a.sha256=QString::fromLatin1(hash.result().toHex()); if(p.kind=="files")a.mime=QMimeDatabase().mimeTypeForFile(a.path,QMimeDatabase::MatchContent).name();a.category=category(a.mime); total+=a.bytes; result.artifacts.append(a);
 }
 QJsonArray entries;for(const auto& artifact:result.artifacts) {auto stored=artifact;stored.path=target+"/"+artifact.id;entries.append(stored.toJson());}
 const auto metadata=QJsonDocument(QJsonObject{{"schemaVersion",1},{"operationId",op},{"artifacts",entries}}).toJson(QJsonDocument::Compact);
 QSaveFile manifest(staging+"/_manifest.json");manifest.setDirectWriteFallback(false);
 if(metadata.size()>16*1024*1024||!manifest.open(QIODevice::WriteOnly)||!manifest.setPermissions(QFile::ReadOwner)||manifest.write(metadata)!=metadata.size()||!manifest.commit()) {fail("artifact_io","Cannot commit batch recovery metadata",0);return result;}
 if(!QDir().rename(staging,target)) {fail("artifact_io","Cannot commit acquired batch",0); return result;}
 for(auto& a:result.artifacts) a.path=target+"/"+a.id;
 return result;
}
}
