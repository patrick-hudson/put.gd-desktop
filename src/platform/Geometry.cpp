#include "Geometry.h"
#include <QPainter>
#include <cmath>
#include <limits>
namespace putgd {
QImage composeFrames(const QList<CaptureFrame>& frames,const QRectF& selection,qint64 maxPixels,Error& error) {
 auto finite=[](const QRectF& r){return std::isfinite(r.x())&&std::isfinite(r.y())&&std::isfinite(r.width())&&std::isfinite(r.height())&&r.width()>0&&r.height()>0;};
 if(!finite(selection)||frames.isEmpty()) {error={"capture_geometry","No valid capture selection",{}};return {};}
 qreal density=0;QList<const CaptureFrame*> participating;
 for(const auto& f:frames) {
  if(!finite(f.logicalRect)||f.nativeRect.isEmpty()||f.identity.isEmpty()||!std::isfinite(f.density)||f.density<=0||f.pixels.isNull()||f.pixels.width()!=f.nativeRect.width()||f.pixels.height()!=f.nativeRect.height()) {error={"capture_geometry","Invalid monitor frame metadata",{}};return {};}
  if(f.logicalRect.intersects(selection)){participating.append(&f);density=std::max(density,f.density);}
 }
 if(participating.isEmpty()){error={"capture_geometry","Selection does not intersect an enabled monitor",{}};return {};}
 if(participating.size()==1){
  const auto& f=*participating.first();const auto part=f.logicalRect.intersected(selection);
  const qreal sx=f.pixels.width()/f.logicalRect.width(),sy=f.pixels.height()/f.logicalRect.height();
  const int left=int(std::floor((part.left()-f.logicalRect.left())*sx)),top=int(std::floor((part.top()-f.logicalRect.top())*sy));
  const int right=int(std::ceil((part.right()-f.logicalRect.left())*sx)),bottom=int(std::ceil((part.bottom()-f.logicalRect.top())*sy));
  if(qint64(right-left)*(bottom-top)>maxPixels){error={"image_limit","Capture exceeds image pixel limit",{}};return {};}
  // An entirely single-monitor selection preserves its original pixel grid.
  // Selections extending into layout gaps still need the transparent canvas.
  if(f.logicalRect.contains(selection))return f.pixels.copy(QRect(left,top,right-left,bottom-top));
 }

 // Round output boundaries outwards once, then derive every destination rectangle
 // from the same origin so adjacent monitor edges cannot acquire rounding cracks.
 const qreal originX=std::floor(selection.left()*density),originY=std::floor(selection.top()*density);
 const auto width=std::ceil(selection.right()*density)-originX,height=std::ceil(selection.bottom()*density)-originY;
 if(!std::isfinite(width)||!std::isfinite(height)||width>std::numeric_limits<int>::max()||height>std::numeric_limits<int>::max()||width*height>maxPixels) {error={"image_limit","Capture exceeds image pixel limit",{}};return {};}
 QImage result(int(width),int(height),QImage::Format_ARGB32);if(result.isNull()){error={"image_limit","Cannot allocate capture image",{}};return {};}
 result.fill(Qt::transparent);QPainter painter(&result);painter.setRenderHint(QPainter::SmoothPixmapTransform,false);
 for(const auto* f:participating) {
  const auto intersection=f->logicalRect.intersected(selection);
  const auto left=std::floor(intersection.left()*density-originX),top=std::floor(intersection.top()*density-originY);
  const auto right=std::ceil(intersection.right()*density-originX),bottom=std::ceil(intersection.bottom()*density-originY);
  const qreal sx=f->pixels.width()/f->logicalRect.width(),sy=f->pixels.height()/f->logicalRect.height();
  const QRectF source((intersection.x()-f->logicalRect.x())*sx,(intersection.y()-f->logicalRect.y())*sy,intersection.width()*sx,intersection.height()*sy);
  painter.drawImage(QRectF(left,top,right-left,bottom-top),f->pixels,source);
 }
 return result;
}
}
