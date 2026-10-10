#include "KdeRegionProtocol.h"
#include <QSet>
#include <cmath>

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
}
