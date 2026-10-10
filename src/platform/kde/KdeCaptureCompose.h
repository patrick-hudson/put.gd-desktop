#pragma once
#include "platform/Geometry.h"
#include <cmath>
namespace putgd {
// A validated complete single-output snapshot already is the composed image.
// QImage shares immutable pixels until a writer detaches; partial captures and
// every rejected metadata/budget case retain the shared geometry implementation.
inline QImage composeKdeFrames(const QList<CaptureFrame>& frames,const QRectF& selection,
                              qint64 maxPixels,Error& error) {
 if(frames.size()==1){
  const auto& frame=frames.constFirst();const auto rect=frame.logicalRect;
  if(rect==selection&&std::isfinite(rect.x())&&std::isfinite(rect.y())&&
     std::isfinite(rect.width())&&std::isfinite(rect.height())&&rect.width()>0&&rect.height()>0&&
     !frame.identity.isEmpty()&&!frame.nativeRect.isEmpty()&&std::isfinite(frame.density)&&frame.density>0&&
     !frame.pixels.isNull()&&frame.pixels.size()==frame.nativeRect.size()&&
     qint64(frame.pixels.width())*frame.pixels.height()<=maxPixels){
   // Use exactly the existing outward rounding, including fractional origins.
   // Only skip the deep copy if it would copy the complete pixel rectangle.
   const auto part=rect.intersected(selection);
   const qreal sx=frame.pixels.width()/rect.width(),sy=frame.pixels.height()/rect.height();
   const auto left=std::floor((part.left()-rect.left())*sx),top=std::floor((part.top()-rect.top())*sy);
   const auto right=std::ceil((part.right()-rect.left())*sx),bottom=std::ceil((part.bottom()-rect.top())*sy);
   if(left==0&&top==0&&right==frame.pixels.width()&&bottom==frame.pixels.height())return frame.pixels;
  }
 }
 return composeFrames(frames,selection,maxPixels,error);
}
}
