#include "IconArt.h"
#include "Theme.h"
#include <QPainter>
#include <QPainterPath>
#include <QTransform>

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
QImage liftTray(const QColor& color,int percent,int scale,int recording) {
 auto image=canvas(scale);QPainter p(&image);p.setRenderHint(QPainter::Antialiasing);
 const auto& light=Theme::trayTints(true);const auto& tints=Theme::trayTints(color==light.idle||color==light.upload||color==light.encoding||color==light.error);
 const auto shape=arrow().united(tray());
 if(percent<0) p.fillPath(shape,color);
 else {
  p.fillPath(shape,color);
  p.save();const auto fill=qBound(0,percent,100)/100.0;
  p.setClipRect(QRectF(0,23-21*fill,24,21*fill));p.fillPath(shape,tints.encoding);p.restore();
 }
 p.setPen(QPen(Theme::trayOutline(),0.65,Qt::SolidLine,Qt::RoundCap,Qt::RoundJoin));
 p.setBrush(Qt::NoBrush);p.drawPath(shape);
 if(percent==-2){p.setPen(QPen(Theme::trayOutline(),1));p.setBrush(color);p.drawEllipse(QRectF(14,0,10,10));p.setPen(QPen(Theme::trayOutline(),1.5,Qt::SolidLine,Qt::RoundCap));p.drawLine(QPointF(19,2),QPointF(19,5));p.drawPoint(QPointF(19,7));}
 if(recording){p.setPen(QPen(Theme::trayOutline(),1));p.setBrush(recording==2?tints.idle:tints.live);p.drawEllipse(QRectF(0,0,10,10));if(recording==2){p.setPen(QPen(Theme::trayOutline(),1.8,Qt::SolidLine,Qt::FlatCap));p.drawLine(QPointF(3.5,2.5),QPointF(3.5,7.5));p.drawLine(QPointF(6.5,2.5),QPointF(6.5,7.5));}}
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
 // The 24-unit artwork scales to a 1.5 px tool stroke at 20 px.
 auto fill=[&](const QColor& c){p.setPen(Qt::NoPen);p.setBrush(c);};
 auto line=[&](const QColor& c,qreal width=2){p.setBrush(Qt::NoBrush);p.setPen(QPen(c,width,Qt::SolidLine,Qt::RoundCap,Qt::RoundJoin));};
 auto rounded=[&](qreal x,qreal y,qreal w,qreal h,qreal radius=2){p.drawRoundedRect(QRectF(x,y,w,h),radius,radius);};
 if(name=="upload") {
  p.fillPath(tray(),secondary);p.fillPath(arrow(),color);
 } else if(name=="failed-uploads") {
  p.fillPath(tray(),secondary);p.fillPath(arrow(),color);fill(color);p.drawEllipse(QPointF(19,18),4.5,4.5);line(secondary,1.7);p.drawLine(QPointF(19,15.6),QPointF(19,18.2));fill(secondary);p.drawEllipse(QPointF(19,20.2),0.8,0.8);
 } else if(name=="download") {
  // The upload arrow turned to point into the tray (#293).
  p.fillPath(tray(),secondary);p.fillPath(QTransform(1,0,0,-1,0,18).map(arrow()),color);
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
 } else if(name=="settings") {
  // A gear: Advanced, distinct from General's sliders.
  fill(secondary);p.save();p.translate(12,12);for(int i=0;i<8;++i){p.drawRoundedRect(QRectF(-2.2,-11,4.4,6),1.2,1.2);p.rotate(45);}p.restore();
  p.drawEllipse(QPointF(12,12),7.6,7.6);line(color,2.6);p.drawEllipse(QPointF(12,12),3.2,3.2);
 } else if(name=="alert") {
  // A failure: a rounded warning triangle with its mark.
  fill(secondary);QPainterPath triangle;triangle.moveTo(10.3,3.2);triangle.quadTo(12,0.8,13.7,3.2);triangle.lineTo(22,18.6);triangle.quadTo(23.2,21,20.6,21);triangle.lineTo(3.4,21);triangle.quadTo(0.8,21,2,18.6);triangle.closeSubpath();p.drawPath(triangle);
  fill(color);rounded(10.9,7.5,2.2,7.5,1.1);p.drawEllipse(QPointF(12,17.6),1.35,1.35);
 } else if(name=="preferences") {
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
 } else if(name=="stop") {
  fill(color);rounded(5,5,14,14,1.5);
 } else if(name=="record") {
  line(secondary,2.2);rounded(2,4,20,16,3);
  fill(color);p.drawEllipse(QPointF(12,12),4.8,4.8);
 } else if(name=="select") {
  // 1.8 units on the 24-unit grid gives a 1.5 px stroke in 20 px tool icons.
  line(color,1.8);QPainterPath pointer;pointer.moveTo(5,2);pointer.lineTo(19,13);pointer.lineTo(12.5,14);pointer.lineTo(16,21);pointer.lineTo(13,22.5);pointer.lineTo(9.5,15.5);pointer.lineTo(5,20);pointer.closeSubpath();p.drawPath(pointer);
 } else if(name=="redact") {
  line(color,1.8);rounded(3,7,18,10,1);
  p.drawLine(5,3,11,3);p.drawLine(15,3,19,3);p.drawLine(5,21,9,21);p.drawLine(13,21,19,21);
  for(int x:{6,11,16})p.drawLine(x,14,x+3,10);
 } else if(name=="blur") {
  // A soft droplet, distinct from an app grid or pixelation.
  line(color,1.8);QPainterPath drop;drop.moveTo(12,2);drop.cubicTo(9,7,5,10,5,15);drop.cubicTo(5,24,19,24,19,15);drop.cubicTo(19,10,15,7,12,2);p.drawPath(drop);
  auto soft=color;soft.setAlpha(100);line(soft,1.8);p.drawArc(QRectF(8,11,8,8),195*16,150*16);
 } else if(name=="crop") {
  line(color,1.8);p.drawLine(7,2,7,17);p.drawLine(7,17,22,17);
  p.drawLine(2,7,17,7);p.drawLine(17,7,17,22);
 } else if(name=="annotate-arrow") {
  line(color,1.8);p.drawLine(4,20,20,4);p.drawLine(11,4,20,4);p.drawLine(20,4,20,13);
 } else if(name=="rectangle") {
  line(color,1.8);rounded(4,5,16,14,1);
 } else if(name=="highlight") {
  line(color,1.8);QPainterPath marker;
  marker.moveTo(16,2);marker.lineTo(22,8);marker.lineTo(12,18);marker.lineTo(6,12);marker.closeSubpath();
  marker.moveTo(14,4);marker.lineTo(20,10);marker.moveTo(7,13);marker.lineTo(3,19);marker.lineTo(9,19);marker.lineTo(11,17);p.drawPath(marker);
  p.drawLine(2,23,21,23);
 } else if(name=="chevron-down") {
  line(color,2.5);QPainterPath chevron;chevron.moveTo(6,9);chevron.lineTo(12,15);chevron.lineTo(18,9);p.drawPath(chevron);
 } else if(name=="text") {
  line(color,1.8);p.drawLine(5,5,19,5);p.drawLine(5,5,5,8);p.drawLine(19,5,19,8);p.drawLine(12,5,12,20);p.drawLine(8,20,16,20);
 } else if(name=="undo"||name=="redo") {
  if(name=="redo"){p.translate(24,0);p.scale(-1,1);}
  line(color,1.8);QPainterPath turn;turn.moveTo(4,9);turn.lineTo(15,9);turn.quadTo(21,9,21,15);turn.quadTo(21,21,15,21);turn.lineTo(10,21);
  turn.moveTo(9,4);turn.lineTo(4,9);turn.lineTo(9,14);p.drawPath(turn);
 } else if(name=="zoom-fit") {
  line(color,1.8);rounded(2,3,20,18,2);
  QPainterPath arrows;arrows.moveTo(10,10);arrows.lineTo(6,6);arrows.lineTo(6,10);arrows.moveTo(6,6);arrows.lineTo(10,6);
  arrows.moveTo(14,14);arrows.lineTo(18,18);arrows.lineTo(14,18);arrows.moveTo(18,18);arrows.lineTo(18,14);p.drawPath(arrows);
 } else if(name=="zoom-actual") {
  line(color,1.8);rounded(1.9,4.4,20.2,15.2,3);
  QPainterPath ones;ones.moveTo(6.8,9.2);ones.lineTo(9.2,7.6);ones.lineTo(9.2,16.4);
  ones.moveTo(14.8,9.2);ones.lineTo(17.2,7.6);ones.lineTo(17.2,16.4);p.drawPath(ones);
  fill(color);p.drawEllipse(QPointF(12.4,9.9),0.9,0.9);p.drawEllipse(QPointF(12.4,14.3),0.9,0.9);
 } else if(name=="back"||name=="forward"||name=="up") {
  // The fallback file dialog's toolbar (#259): one arrow, turned.
  if(name=="forward"){p.translate(24,0);p.scale(-1,1);}
  if(name=="up"){p.translate(12,12);p.rotate(90);p.translate(-12,-12);}
  line(secondary,2.5);p.drawLine(QPointF(7,12),QPointF(20,12));
  line(color,2.5);QPainterPath head;head.moveTo(11,5);head.lineTo(4,12);head.lineTo(11,19);p.drawPath(head);
 } else if(name=="folder-new"||name=="save") {
  fill(secondary);QPainterPath folder;folder.moveTo(2,6);folder.quadTo(2,4,4,4);folder.lineTo(9,4);folder.lineTo(11,6);folder.lineTo(20,6);folder.quadTo(22,6,22,8);
  folder.lineTo(22,18);folder.quadTo(22,20,20,20);folder.lineTo(4,20);folder.quadTo(2,20,2,18);folder.closeSubpath();p.drawPath(folder);
  line(color,2.5);p.drawLine(QPointF(12,9),QPointF(12,17));
  if(name=="save"){p.drawLine(QPointF(8,13),QPointF(12,17));p.drawLine(QPointF(12,17),QPointF(16,13));}
  else p.drawLine(QPointF(8,13),QPointF(16,13));
 } else if(name=="view-list") {
  fill(color);for(const qreal y:{5.,12.,19.})p.drawEllipse(QPointF(4.5,y),1.8,1.8);
  line(secondary,2.5);for(const qreal y:{5.,12.,19.})p.drawLine(QPointF(9,y),QPointF(21,y));
 } else if(name=="view-detail") {
  line(secondary,2);rounded(2,3,20,18,2);p.drawLine(QPointF(2,9),QPointF(22,9));p.drawLine(QPointF(2,15),QPointF(22,15));
  line(color,2);p.drawLine(QPointF(10,3),QPointF(10,21));
 } else if(name=="play") {
  fill(color);QPainterPath play;play.moveTo(8,4.5);play.lineTo(20,12);play.lineTo(8,19.5);play.closeSubpath();p.drawPath(play);
 } else if(name=="pause") {
  fill(color);rounded(6,4.5,4,15,1.2);rounded(14,4.5,4,15,1.2);
 } else if(name=="trim-in"||name=="trim-out"||name=="trim-reset") {
  // Trim: a long clip strip cut by a bracket at the playhead; the cut-away
  // end is only outlined, the kept part is solid.
  const bool reset=name=="trim-reset";
  if(name=="trim-out"){p.translate(24,0);p.scale(-1,1);}
  if(!reset){line(secondary,1.3);p.drawRoundedRect(QRectF(1.2,9.2,4.6,5.6),1.2,1.2);}
  fill(secondary);rounded(reset?6:9,9,reset?12:13.5,6,1.5);
  line(color,2.5);QPainterPath bracket;
  const auto cut=[&](qreal x,qreal dir){bracket.moveTo(x+3.5*dir,3.5);bracket.lineTo(x,3.5);bracket.lineTo(x,20.5);bracket.lineTo(x+3.5*dir,20.5);};
  if(reset){cut(3,1);cut(21,-1);}else cut(7.5,1);
  p.drawPath(bracket);
 } else if(name=="mask-in"||name=="mask-out") {
  // Mask timing: the same bracket at the playhead, on a covered region (a
  // hatched block) instead of the clip strip.
  if(name=="mask-out"){p.translate(24,0);p.scale(-1,1);}
  const QRectF block(9.5,4.5,12,15);
  fill(secondary);p.drawRoundedRect(block,2,2);
  p.save();p.setClipRect(block.adjusted(1,1,-1,-1));line(color,1.6);
  for(qreal d=-8;d<16;d+=4.5)p.drawLine(QPointF(block.left()+d,block.bottom()),QPointF(block.left()+d+15,block.top()));
  p.restore();
  line(color,2.5);QPainterPath bracket;bracket.moveTo(7,3.5);bracket.lineTo(3.5,3.5);bracket.lineTo(3.5,20.5);bracket.lineTo(7,20.5);p.drawPath(bracket);
 } else if(name=="remove") {
  line(color,1.8);QPainterPath bin;bin.moveTo(6,8);bin.lineTo(7.5,21);bin.lineTo(16.5,21);bin.lineTo(18,8);p.drawPath(bin);
  p.drawLine(4,5,20,5);p.drawLine(9,2,15,2);p.drawLine(9,2,9,5);p.drawLine(15,2,15,5);
  p.drawLine(10,9,10,17);p.drawLine(14,9,14,17);
 } else if(name=="bucket") {
  // Object storage (S3, Backblaze B2): a storage drum, its lid and a band.
  fill(secondary);QPainterPath drum;drum.addRect(QRectF(4,6,16,12));drum.addEllipse(QRectF(4,14,16,8));p.drawPath(drum.simplified());
  fill(color);p.drawEllipse(QRectF(4,2,16,8));line(color,1.8);p.drawArc(QRectF(4,9.5,16,7),180*16,180*16);
 } else if(name=="proton") {
  // Cloud storage with a distinct folder front, readable at sidebar sizes.
  fill(secondary);QPainterPath cloud;cloud.moveTo(5,15);cloud.cubicTo(-1,15,0,7,6,7);cloud.cubicTo(7,0,17,0,19,7);cloud.cubicTo(25,8,24,15,19,15);cloud.closeSubpath();p.drawPath(cloud);
  fill(color);QPainterPath folder;folder.moveTo(4,12);folder.lineTo(10,12);folder.lineTo(12,14);folder.lineTo(21,14);folder.lineTo(20,22);folder.lineTo(4,22);folder.closeSubpath();p.drawPath(folder);
 } else if(name=="add") {
  fill(secondary);p.drawEllipse(QRectF(2,2,20,20));
  line(color,2.5);p.drawLine(QPointF(12,7),QPointF(12,17));p.drawLine(QPointF(7,12),QPointF(17,12));
 } else {
  fill(secondary);QPainterPath file;file.moveTo(5,2);file.lineTo(14,2);file.lineTo(20,8);file.lineTo(20,20);file.quadTo(20,22,18,22);file.lineTo(6,22);file.quadTo(4,22,4,20);file.lineTo(4,4);file.quadTo(4,2,5,2);p.drawPath(file);
  fill(color);QPainterPath fold;fold.moveTo(14,2);fold.lineTo(14,8);fold.lineTo(20,8);fold.closeSubpath();p.drawPath(fold);
  if(name=="videos"||name=="video") {QPainterPath play;play.moveTo(9,11);play.lineTo(15,15);play.lineTo(9,19);play.closeSubpath();p.drawPath(play);}
  else {line(color);p.drawLine(8,12,15,12);p.drawLine(8,16,13,16);}
 }
 return image;
}
}
