#include "KdeRegionProtocol.h"
#include <QSet>
#include <cmath>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <cerrno>

namespace putgd {
namespace {
Error invalid() { return {"capture_geometry", "The region helper returned invalid geometry", {}}; }
bool finite(qreal value) { return std::isfinite(value) && std::abs(value)<=1000000; }
bool validRect(const QRectF& r) {
 return finite(r.x()) && finite(r.y()) && finite(r.width()) && finite(r.height()) &&
  r.width()>=1 && r.height()>=1 && r.width()<=131072 && r.height()<=131072 &&
  finite(r.right()) && finite(r.bottom());
}
QRectF rectangle(const QJsonObject& o, Error& error) {
 if(o.size()!=4) { error=invalid(); return {}; }
 for(const auto* key:{"x","y","width","height"})
  if(!o[key].isDouble() || !std::isfinite(o[key].toDouble())) { error=invalid(); return {}; }
 const QRectF r(o["x"].toDouble(),o["y"].toDouble(),o["width"].toDouble(),o["height"].toDouble());
 if(!validRect(r)) { error=invalid(); return {}; }
 return r;
}
}
QJsonObject kdeRegionRectangle(const QRectF& r) {
 return {{"x",r.x()},{"y",r.y()},{"width",r.width()},{"height",r.height()}};
}
QJsonArray kdeRegionLayout(const QList<CaptureFrame>& frames, Error& error) {
 error={}; QJsonArray result; QSet<QString> names;
 if(frames.isEmpty() || frames.size()>32) { error=invalid(); return {}; }
 for(const auto& frame:frames) {
  if(frame.identity.isEmpty() || frame.identity.toUtf8().size()>256 || frame.identity.contains(QChar::Null) ||
     names.contains(frame.identity) || !validRect(frame.logicalRect) || !frame.pixels.isNull() ||
     (frame.rotation!=0 && frame.rotation!=90 && frame.rotation!=180 && frame.rotation!=270)) {
   error=invalid(); return {};
  }
  names.insert(frame.identity);
  result.append(QJsonObject{{"identity",frame.identity},{"logicalRect",kdeRegionRectangle(frame.logicalRect)},
                            {"rotation",frame.rotation}});
 }
 return result;
}
QList<CaptureFrame> kdeRegionLayout(const QJsonArray& array, Error& error) {
 error={}; QList<CaptureFrame> frames; QSet<QString> names;
 if(array.isEmpty() || array.size()>32) { error=invalid(); return {}; }
 for(const auto& value:array) {
  if(!value.isObject()) { error=invalid(); return {}; }
  const auto row=value.toObject();
  if(row.size()!=3 || !row["identity"].isString() || !row["logicalRect"].isObject() ||
     !row["rotation"].isDouble() || row["rotation"].toDouble()!=row["rotation"].toInteger()) {
   error=invalid(); return {};
  }
  CaptureFrame frame; frame.identity=row["identity"].toString();
  frame.logicalRect=rectangle(row["logicalRect"].toObject(),error); if(error) return {};
  const auto rotation=row["rotation"].toInteger();
  if(rotation!=0 && rotation!=90 && rotation!=180 && rotation!=270) { error=invalid(); return {}; }
  frame.rotation=int(rotation);
  if(frame.identity.isEmpty() || frame.identity.toUtf8().size()>256 || frame.identity.contains(QChar::Null) ||
     names.contains(frame.identity)) { error=invalid(); return {}; }
  names.insert(frame.identity); frames.append(std::move(frame));
 }
 return frames;
}
bool kdeRegionLayoutsMatch(const QList<CaptureFrame>& frozen,const QList<CaptureFrame>& current) {
 Error error; kdeRegionLayout(frozen,error); if(error) return false;
 kdeRegionLayout(current,error); if(error || current.size()!=frozen.size()) return false;
 for(const auto& frame:frozen) {
  bool found=false;
  for(const auto& now:current)
   if(frame.identity==now.identity && frame.logicalRect==now.logicalRect) found=true;
  if(!found) return false;
 }
 return true;
}
QRectF kdeRegionRectangle(const QJsonObject& object,const QList<CaptureFrame>& frames,Error& error) {
 error={}; kdeRegionLayout(frames,error); if(error) return {};
 const auto r=rectangle(object,error); if(error) return {};
 QRectF bounds; bool intersects=false;
 for(const auto& frame:frames) { bounds=bounds.united(frame.logicalRect); intersects|=r.intersects(frame.logicalRect); }
 if(!intersects || !bounds.contains(r)) { error=invalid(); return {}; }
 return r;
}
namespace {
Error frozenInvalid() { return {"capture_geometry", "The frozen region frames are invalid", {{"stage","select"},{"check","frozen_frames"}}}; }
constexpr qint64 frozenBytesLimit=qint64(4)<<30;
constexpr int frozenSeals=F_SEAL_SEAL|F_SEAL_SHRINK|F_SEAL_GROW|F_SEAL_WRITE;
}
KdeFrozenFrames kdeFrozenFrames(const QList<CaptureFrame>& layout,const QList<CaptureFrame>& pixels,Error& error) {
 error={}; KdeFrozenFrames result;
 if(layout.isEmpty() || layout.size()!=pixels.size()) { error=frozenInvalid(); return {}; }
 qint64 total=0; QList<QImage> images;
 for(int i=0;i<layout.size();++i) {
  const auto& frame=pixels[i];
  if(frame.identity!=layout[i].identity || frame.pixels.isNull() || frame.pixels.width()>32768 || frame.pixels.height()>32768 ||
     !std::isfinite(frame.density) || frame.density<=0) { error=frozenInvalid(); return {}; }
  auto image=frame.pixels.convertToFormat(QImage::Format_ARGB32_Premultiplied);
  total+=qint64(image.width())*image.height()*4;
  if(total>frozenBytesLimit) { error=frozenInvalid(); return {}; }
  result.frames.append(QJsonObject{{"width",image.width()},{"height",image.height()},{"density",frame.density},
   {"native",QJsonArray{frame.nativeRect.x(),frame.nativeRect.y(),frame.nativeRect.width(),frame.nativeRect.height()}}});
  images.append(std::move(image));
 }
 const int fd=int(::syscall(SYS_memfd_create,"putgd-region-frames",MFD_CLOEXEC|MFD_ALLOW_SEALING));
 if(fd<0) { error={"capture_unavailable","Cannot share the frozen frames with the region helper",{{"stage","freeze"},{"check","memfd"}}}; return {}; }
 bool ok=::ftruncate(fd,total)==0;
 for(const auto& image:images) {
  for(int y=0;ok && y<image.height();++y) {
   const char* row=reinterpret_cast<const char*>(image.constScanLine(y)); qint64 left=qint64(image.width())*4;
   while(ok && left>0) { const auto written=::write(fd,row,size_t(left)); if(written<0 && errno==EINTR) continue; ok=written>0; row+=written; left-=written; }
  }
 }
 ok=ok && ::fcntl(fd,F_ADD_SEALS,frozenSeals)==0;
 if(!ok) { ::close(fd); error={"capture_unavailable","Cannot share the frozen frames with the region helper",{{"stage","freeze"},{"check","memfd_write"}}}; return {}; }
 result.fd=fd; return result;
}
bool kdeFrozenPixels(int fd,const QJsonArray& frames,QList<CaptureFrame>& layout,Error& error) {
 error={};
 struct Close { int fd; ~Close() { if(fd>=0) ::close(fd); } } owner{fd};
 if(fd<3 || frames.size()!=layout.size() || ::fcntl(fd,F_SETFD,FD_CLOEXEC)!=0) { error=frozenInvalid(); return false; }
 // Only a sealed memfd of exactly the announced size is read.
 const int seals=::fcntl(fd,F_GET_SEALS);
 if(seals<0 || (seals&frozenSeals)!=frozenSeals) { error=frozenInvalid(); return false; }
 qint64 total=0; QList<QSize> sizes;
 for(const auto& value:frames) {
  const auto row=value.toObject(); const auto native=row["native"].toArray();
  if(row.size()!=4 || !row["width"].isDouble() || !row["height"].isDouble() || !row["density"].isDouble() || native.size()!=4) { error=frozenInvalid(); return false; }
  const auto width=row["width"].toInteger(), height=row["height"].toInteger(); const auto density=row["density"].toDouble();
  if(width<1 || height<1 || width>32768 || height>32768 || !std::isfinite(density) || density<=0 || density>32) { error=frozenInvalid(); return false; }
  for(const auto& part:native) if(!part.isDouble() || part.toDouble()!=part.toInteger() || std::abs(part.toInteger())>1000000) { error=frozenInvalid(); return false; }
  total+=width*height*4; if(total>frozenBytesLimit) { error=frozenInvalid(); return false; }
  sizes.append(QSize(int(width),int(height)));
 }
 struct stat info{};
 if(::fstat(fd,&info)!=0 || !S_ISREG(info.st_mode) || info.st_size!=total) { error=frozenInvalid(); return false; }
 void* mapped=::mmap(nullptr,size_t(total),PROT_READ,MAP_PRIVATE,fd,0);
 if(mapped==MAP_FAILED) { error=frozenInvalid(); return false; }
 qint64 offset=0;
 for(int i=0;i<layout.size();++i) {
  const auto row=frames[i].toObject(); const auto native=row["native"].toArray();
  const QImage view(static_cast<const uchar*>(mapped)+offset,sizes[i].width(),sizes[i].height(),sizes[i].width()*4,QImage::Format_ARGB32_Premultiplied);
  layout[i].pixels=view.copy(); layout[i].density=row["density"].toDouble();
  layout[i].nativeRect=QRect(native[0].toInt(),native[1].toInt(),native[2].toInt(),native[3].toInt());
  offset+=qint64(sizes[i].width())*sizes[i].height()*4;
 }
 ::munmap(mapped,size_t(total));
 for(const auto& frame:layout) if(frame.pixels.isNull()) { error=frozenInvalid(); return false; }
 return true;
}
}
