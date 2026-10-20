#include "IconArt.h"
#include "Theme.h"
#include <QPainter>
#include <QPainterPath>

namespace putgd::IconArt {
namespace {
QPainterPath arrow() {
 QPainterPath p;
 p.moveTo(12,2);p.lineTo(20,10);p.lineTo(15,10);p.lineTo(15,16);
 p.lineTo(9,16);p.lineTo(9,10);p.lineTo(4,10);p.closeSubpath();
 return p;
}
QPainterPath tray() {
 QPainterPath p;
 p.moveTo(2,13);p.lineTo(6,13);p.lineTo(6,18);p.quadTo(6,19,7,19);
 p.lineTo(17,19);p.quadTo(18,19,18,18);p.lineTo(18,13);p.lineTo(22,13);
 p.lineTo(22,18);p.quadTo(22,23,17,23);p.lineTo(7,23);p.quadTo(2,23,2,18);p.closeSubpath();
 return p;
}
QImage canvas(int scale) {
 QImage result(24*scale,24*scale,QImage::Format_ARGB32);
 result.setDevicePixelRatio(scale);result.fill(Qt::transparent);return result;
}
}
QImage liftTray(const QColor& color,int percent,int scale) {
 auto image=canvas(scale);QPainter p(&image);p.setRenderHint(QPainter::Antialiasing);
 const auto shape=arrow().united(tray());
 if(percent<0||percent>=100) p.fillPath(shape,color);
 else {
  p.fillPath(shape,color.darker(230));
  p.save();const auto fill=qBound(0,percent,100)/100.0;
  p.setClipRect(QRectF(0,23-21*fill,24,21*fill));p.fillPath(shape,color);p.restore();
 }
 p.setPen(QPen(Theme::trayOutline(),0.65,Qt::SolidLine,Qt::RoundCap,Qt::RoundJoin));
 p.setBrush(Qt::NoBrush);p.drawPath(shape);
 return image;
}
QImage liftApp(const QColor& color,int scale) {
 auto image=canvas(scale);QPainter p(&image);p.setRenderHint(QPainter::Antialiasing);
 p.setPen(Qt::NoPen);p.setBrush(Theme::brandTile());p.drawRoundedRect(QRectF(0,0,24,24),5,5);
 p.translate(2.4,2);p.scale(0.8,0.8);
 p.fillPath(tray(),color.darker(190));p.fillPath(arrow(),color);
 return image;
}
QImage symbol(const QString& name,const QColor& color,int scale,const QColor& secondaryColor) {
 if(name=="brand")return liftApp(color,scale);
 if(name=="tray")return liftTray(color,-1,scale);
 auto image=canvas(scale);QPainter p(&image);p.setRenderHint(QPainter::Antialiasing);
 const auto secondary=secondaryColor.isValid()?secondaryColor:color.darker(190);
 auto fill=[&](const QColor& c){p.setPen(Qt::NoPen);p.setBrush(c);};
 auto line=[&](const QColor& c,qreal width=2){p.setBrush(Qt::NoBrush);p.setPen(QPen(c,width,Qt::SolidLine,Qt::RoundCap,Qt::RoundJoin));};
 auto rounded=[&](qreal x,qreal y,qreal w,qreal h,qreal radius=2){p.drawRoundedRect(QRectF(x,y,w,h),radius,radius);};
 if(name=="upload") {
  p.fillPath(tray(),secondary);p.fillPath(arrow(),color);
 } else if(name=="region"||name=="capture") {
  line(secondary,4);QPainterPath corners;
  corners.moveTo(8,3);corners.lineTo(3,3);corners.lineTo(3,8);
  corners.moveTo(16,3);corners.lineTo(21,3);corners.lineTo(21,8);
  corners.moveTo(3,16);corners.lineTo(3,21);corners.lineTo(8,21);
  corners.moveTo(16,21);corners.lineTo(21,21);corners.lineTo(21,16);p.drawPath(corners);
  line(color);p.drawLine(8,12,16,12);p.drawLine(12,8,12,16);
 } else if(name=="window") {
  fill(secondary);rounded(2,4,20,17,3);
  p.save();QPainterPath clip;clip.addRoundedRect(QRectF(2,4,20,17),3,3);p.setClipPath(clip);
  p.fillRect(QRectF(2,4,20,6),color);p.restore();
  fill(secondary);for(int x:{6,10,14})p.drawEllipse(QPointF(x,7),0.8,0.8);
 } else if(name=="desktop") {
  fill(secondary);rounded(2,3,20,15);rounded(10,17,4,5,1);
  fill(color);rounded(4,5,16,10,1);rounded(6,21,12,2,1);
 } else if(name=="clipboard") {
  fill(secondary);rounded(4,4,16,18,3);
  fill(color);rounded(8,2,8,5);line(color);p.drawLine(8,11,16,11);p.drawLine(8,15,14,15);
 } else if(name=="history") {
  fill(secondary);p.drawEllipse(QRectF(3,3,18,18));
  line(color);p.drawLine(12,7,12,12);p.drawLine(12,12,16,14);
  p.drawArc(QRectF(2,2,20,20),10*16,245*16);
  fill(color);QPainterPath tip;tip.moveTo(1,5);tip.lineTo(7,7);tip.lineTo(3,11);tip.closeSubpath();p.drawPath(tip);
 } else if(name=="destinations") {
  fill(secondary);QPainterPath folder;folder.moveTo(2,7);folder.quadTo(2,4,5,4);folder.lineTo(9,4);
  folder.lineTo(12,7);folder.lineTo(19,7);folder.quadTo(22,7,22,10);folder.lineTo(22,18);
  folder.quadTo(22,21,19,21);folder.lineTo(5,21);folder.quadTo(2,21,2,18);folder.closeSubpath();p.drawPath(folder);
  line(color,2.5);p.drawLine(13,18,13,10);p.drawLine(9,14,13,10);p.drawLine(13,10,17,14);
 } else if(name=="preferences"||name=="settings") {
  line(secondary,3);for(int y:{6,12,18})p.drawLine(3,y,21,y);
  fill(color);p.drawEllipse(QPointF(8,6),3,3);p.drawEllipse(QPointF(16,12),3,3);p.drawEllipse(QPointF(10,18),3,3);
 } else if(name=="info") {
  fill(secondary);p.drawEllipse(QRectF(2,2,20,20));fill(color);p.drawEllipse(QPointF(12,7.5),1.25,1.25);
  rounded(11,11,2,6,1);
 } else if(name=="shortcuts") {
  fill(secondary);rounded(1,4,22,16,3);fill(color);
  for(int y:{8,12})for(int x:{5,10,15,19})rounded(x-1,y-1,2,2,0.5);
  rounded(7,16,10,2,1);
 } else if(name=="images"||name=="image") {
  fill(secondary);rounded(2,2,20,20,3);fill(color);p.drawEllipse(QPointF(8,7),2,2);
  QPainterPath mountains;mountains.moveTo(3,19);mountains.lineTo(10,12);mountains.lineTo(14,16);mountains.lineTo(18,11);mountains.lineTo(21,15);mountains.lineTo(21,19);mountains.closeSubpath();p.drawPath(mountains);
 } else if(name=="edit") {
  line(secondary,4);QPainterPath corners;
  corners.moveTo(8,3);corners.lineTo(3,3);corners.lineTo(3,8);
  corners.moveTo(3,16);corners.lineTo(3,21);corners.lineTo(8,21);
  corners.moveTo(16,3);corners.lineTo(21,3);corners.lineTo(21,8);p.drawPath(corners);
  fill(color);QPainterPath pencil;pencil.moveTo(19,10);pencil.lineTo(22,13);pencil.lineTo(13,22);pencil.lineTo(9,23);pencil.lineTo(10,19);pencil.closeSubpath();p.drawPath(pencil);
 } else if(name=="record") {
  line(secondary,2.2);p.drawEllipse(QRectF(3.5,3.5,17,17));
  fill(color);p.drawEllipse(QPointF(12,12),5,5);
 } else if(name=="select") {
  fill(color);QPainterPath pointer;pointer.moveTo(5,2);pointer.lineTo(19,13);pointer.lineTo(12.5,14);pointer.lineTo(16,21);pointer.lineTo(13,22.5);pointer.lineTo(9.5,15.5);pointer.lineTo(5,20);pointer.closeSubpath();p.drawPath(pointer);
 } else if(name=="redact") {
  line(secondary,2);p.drawLine(3,5,21,5);p.drawLine(3,19,15,19);
  fill(color);rounded(2,9,20,6,1.5);
 } else if(name=="blur") {
  fill(secondary);for(int y:{4,12,20})for(int x:{4,12,20})p.drawEllipse(QPointF(x,y),1.6,1.6);
  fill(color);for(int y:{8,16})for(int x:{8,16})p.drawEllipse(QPointF(x,y),2.6,2.6);
 } else if(name=="crop") {
  line(secondary,2.5);p.drawLine(7,2,7,17);p.drawLine(7,17,22,17);
  line(color,2.5);p.drawLine(2,7,17,7);p.drawLine(17,7,17,22);
 } else if(name=="annotate-arrow") {
  line(color,2.5);p.drawLine(4,20,16,8);
  fill(color);QPainterPath head;head.moveTo(21,3);head.lineTo(19,13);head.lineTo(11,5);head.closeSubpath();p.drawPath(head);
 } else if(name=="rectangle") {
  line(secondary,2);rounded(3,5,18,14,2);line(color,2.5);rounded(6,8,12,8,1);
 } else if(name=="highlight") {
  fill(secondary);rounded(2,19,20,3,1);
  fill(color);QPainterPath marker;marker.moveTo(14,2);marker.lineTo(21,9);marker.lineTo(12,18);marker.lineTo(7,18);marker.lineTo(5,16);marker.lineTo(5,11);marker.closeSubpath();p.drawPath(marker);
 } else if(name=="chevron-down") {
  line(color,2.5);QPainterPath chevron;chevron.moveTo(6,9);chevron.lineTo(12,15);chevron.lineTo(18,9);p.drawPath(chevron);
 } else if(name=="text") {
  fill(secondary);rounded(2,2,20,20,3);
  line(color,3);p.drawLine(7,7,17,7);p.drawLine(12,7,12,18);
 } else if(name=="undo"||name=="redo") {
  if(name=="redo"){p.translate(24,0);p.scale(-1,1);}
  line(secondary,2.5);QPainterPath turn;turn.moveTo(7,9);turn.lineTo(15,9);turn.quadTo(21,9,21,15);turn.quadTo(21,21,15,21);turn.lineTo(10,21);p.drawPath(turn);
  fill(color);QPainterPath tip;tip.moveTo(2,9);tip.lineTo(9,3);tip.lineTo(9,15);tip.closeSubpath();p.drawPath(tip);
 } else if(name=="zoom-fit"||name=="zoom-actual") {
  line(secondary,2);p.drawEllipse(QRectF(3,3,14,14));line(color,3);p.drawLine(15,15,21,21);
  if(name=="zoom-fit"){line(color,1.6);p.drawRect(QRectF(7,7,6,6));}
  else{fill(color);rounded(9,6,2,8,0.5);}
 } else if(name=="save") {
  fill(secondary);rounded(3,3,18,18,3);
  fill(color);rounded(7,3,9,6,1);rounded(6,13,12,8,1.5);
 } else if(name=="activity") {
  fill(secondary);rounded(2,2,20,20,4);line(color);QPainterPath pulse;
  pulse.moveTo(4,13);pulse.lineTo(7,13);pulse.lineTo(10,6);pulse.lineTo(14,19);pulse.lineTo(17,11);pulse.lineTo(20,11);p.drawPath(pulse);
 } else {
  fill(secondary);QPainterPath file;file.moveTo(5,2);file.lineTo(14,2);file.lineTo(20,8);file.lineTo(20,20);file.quadTo(20,22,18,22);file.lineTo(6,22);file.quadTo(4,22,4,20);file.lineTo(4,4);file.quadTo(4,2,5,2);p.drawPath(file);
  fill(color);QPainterPath fold;fold.moveTo(14,2);fold.lineTo(14,8);fold.lineTo(20,8);fold.closeSubpath();p.drawPath(fold);
  if(name=="videos"||name=="video") {QPainterPath play;play.moveTo(9,11);play.lineTo(15,15);play.lineTo(9,19);play.closeSubpath();p.drawPath(play);}
  else {line(color);p.drawLine(8,12,15,12);p.drawLine(8,16,13,16);}
 }
 return image;
}
}
