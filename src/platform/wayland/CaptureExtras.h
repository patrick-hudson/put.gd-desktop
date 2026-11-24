#pragma once
#include "platform/Geometry.h"
#include <QPointF>
namespace putgd {
// Shared by the layer-shell adapters (Plasma, Budgie, Hyprland) for Capture
// monitor and Repeat last region (#1363).
// The last region that was captured, kept in the adapter's memory only: the
// logical rectangle and the layout it was chosen on, without pixels.
struct RegionMemory {
 QRectF rectangle;
 QList<CaptureFrame> layout;
};
// The output a logical layout position lies on. Rectangles are half-open, so a
// position on a shared edge belongs to the output that starts there.
inline const CaptureFrame* outputAt(const QList<CaptureFrame>& layout,QPointF position) {
 for(const auto& frame:layout) {
  const auto& r=frame.logicalRect;
  if(position.x()>=r.left()&&position.x()<r.right()&&position.y()>=r.top()&&position.y()<r.bottom()) return &frame;
 }
 return nullptr;
}
}
