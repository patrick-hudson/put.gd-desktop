#pragma once
#include <QPainter>
#include <QRectF>
#include <algorithm>
namespace putgd {
// Paint entirely inside the selected bounds; selection/capture geometry is independent.
inline void paintSelectionOutline(QPainter& painter,const QRectF& bounds){
 auto ring=[&](QRectF rect,qreal width,QColor color){
  if(rect.isEmpty())return;
  const qreal x=std::min(width,rect.width()),y=std::min(width,rect.height());
  painter.fillRect(QRectF(rect.left(),rect.top(),rect.width(),y),color);
  painter.fillRect(QRectF(rect.left(),rect.bottom()-y,rect.width(),y),color);
  painter.fillRect(QRectF(rect.left(),rect.top(),x,rect.height()),color);
  painter.fillRect(QRectF(rect.right()-x,rect.top(),x,rect.height()),color);
 };
 ring(bounds,3,Qt::black);
 ring(bounds.adjusted(1,1,-1,-1),1,Qt::white);
}
}
