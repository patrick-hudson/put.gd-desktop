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
 p.fillPath(tray(),Theme::brandTray());p.fillPath(arrow(),color);
 return image;
}
QImage symbol(const QString& name,const QColor& color,int scale,const QColor& secondaryColor) {
 if(name=="brand")return liftApp(color,scale);
 if(name=="tray")return liftTray(color,-1,scale);
 auto image=canvas(scale);QPainter p(&image);p.setRenderHint(QPainter::Antialiasing);
 const auto secondary=secondaryColor.isValid()?secondaryColor:color;
 // 24-unit grid: 1.8 units becomes 1.5 px at the normal 20 px size.
 // Square bodies span 18 units, horizontal bodies 20; circles overshoot to 21.
 auto fill=[&](const QColor& c){p.setPen(Qt::NoPen);p.setBrush(c);};
 auto line=[&](const QColor& c,qreal width=1.8){p.setBrush(Qt::NoBrush);p.setPen(QPen(c,width,Qt::SolidLine,Qt::RoundCap,Qt::RoundJoin));};
 auto rounded=[&](qreal x,qreal y,qreal w,qreal h,qreal radius=2){p.drawRoundedRect(QRectF(x,y,w,h),radius,radius);};
 auto wash=[&](const QPainterPath& path){auto c=secondary;c.setAlphaF(c.alphaF()*0.18);p.fillPath(path,c);};
 auto folder=[] {QPainterPath path;path.moveTo(3,8);path.lineTo(3,6);path.quadTo(3,4,5,4);path.lineTo(9,4);path.lineTo(12,7);path.lineTo(19,7);path.quadTo(21,7,21,9);path.lineTo(21,18);path.quadTo(21,20,19,20);path.lineTo(5,20);path.quadTo(3,20,3,18);path.closeSubpath();return path;};
 line(color);
 if(name=="upload"||name=="download"||name=="failed-uploads") {
  QPainterPath base;base.moveTo(3,15);base.lineTo(3,19);base.quadTo(3,21,5,21);base.lineTo(name=="failed-uploads"?13:19,21);if(name!="failed-uploads"){base.quadTo(21,21,21,19);base.lineTo(21,15);}p.drawPath(base);
  const bool down=name=="download";const qreal tip=down?16:3,tail=down?3:16,wing=down?11:8;
  p.drawLine(QPointF(12,tail),QPointF(12,tip));QPainterPath head;head.moveTo(7,wing);head.lineTo(12,tip);head.lineTo(17,wing);p.drawPath(head);
  if(name=="failed-uploads"){p.drawLine(19,14,19,17);p.drawPoint(QPointF(19,21));}
 } else if(name=="region"||name=="capture"||name=="edit") {
  QPainterPath corners;corners.moveTo(8,3);corners.lineTo(3,3);corners.lineTo(3,8);
  corners.moveTo(3,16);corners.lineTo(3,21);corners.lineTo(8,21);
  corners.moveTo(16,3);corners.lineTo(21,3);corners.lineTo(21,8);
  if(name!="edit"){corners.moveTo(16,21);corners.lineTo(21,21);corners.lineTo(21,16);}p.drawPath(corners);
  if(name=="edit"){QPainterPath pencil;pencil.moveTo(18,10);pencil.lineTo(21,13);pencil.lineTo(13,21);pencil.lineTo(9,21);pencil.lineTo(10,18);pencil.closeSubpath();p.drawPath(pencil);p.drawLine(16,12,19,15);}
  else{p.drawLine(8,12,16,12);p.drawLine(12,8,12,16);}
 } else if(name=="window") {
  rounded(3,4,18,16);p.drawLine(3,9,21,9);p.drawLine(7,6.5,9,6.5);
 } else if(name=="desktop") {
  rounded(2.9,3,18.2,14);p.drawLine(12,17,12,21);p.drawLine(7,21,17,21);
 } else if(name=="clipboard") {
  QPainterPath board;board.moveTo(8,5);board.lineTo(6,5);board.quadTo(4,5,4,7);board.lineTo(4,19);board.quadTo(4,21,6,21);board.lineTo(18,21);board.quadTo(20,21,20,19);board.lineTo(20,7);board.quadTo(20,5,18,5);board.lineTo(16,5);p.drawPath(board);
  rounded(8,3,8,4,1);p.drawLine(8,11,16,11);p.drawLine(8,15,14,15);
 } else if(name=="history") {
  p.drawArc(QRectF(2.4,2.4,19.2,19.2),135*16,-305*16);
  QPainterPath head;head.moveTo(3,3);head.lineTo(3,8);head.lineTo(8,8);p.drawPath(head);
  p.drawLine(12,7,12,12);p.drawLine(12,12,16,14);
 } else if(name=="destinations") {
  p.drawPath(folder());p.drawLine(12,17,12,10);p.drawLine(8,14,12,10);p.drawLine(12,10,16,14);
 } else if(name=="settings") {
  QPainterPath gear;bool first=true;p.save();p.translate(12,12);
  for(int i=0;i<8;++i){QTransform turn;turn.rotate(i*45);for(const auto& point:{QPointF(-1.7,-9.6),QPointF(1.7,-9.6),QPointF(2.3,-7.3),QPointF(3.5,-6.7)}){const auto v=turn.map(point);if(first){gear.moveTo(v);first=false;}else gear.lineTo(v);}}
  gear.closeSubpath();p.drawPath(gear);p.drawEllipse(QPointF(0,0),3,3);p.restore();
 } else if(name=="alert") {
  QPainterPath triangle;triangle.moveTo(10.7,3.8);triangle.quadTo(12,1.6,13.3,3.8);triangle.lineTo(21.1,18.3);triangle.quadTo(22.2,20.5,19.8,20.5);triangle.lineTo(4.2,20.5);triangle.quadTo(1.8,20.5,2.9,18.3);triangle.closeSubpath();wash(triangle);p.drawPath(triangle);
  p.drawLine(QPointF(12,8),QPointF(12,13));p.drawPoint(QPointF(12,17));
 } else if(name=="preferences") {
  for(const auto& pair:{QPoint(8,6),QPoint(16,12),QPoint(10,18)}){p.drawLine(3,pair.y(),pair.x()-3,pair.y());p.drawLine(pair.x()+3,pair.y(),21,pair.y());p.drawEllipse(QPointF(pair),2.5,2.5);}
 } else if(name=="info") {
  // Help uses a 14 px glyph: retain a 1.5 px ring at that size.
  line(color,1.5*24/14);p.drawEllipse(QRectF(3,3,18,18));line(color,2.1);p.drawPoint(QPointF(12,7.5));p.drawLine(12,11,12,17);
 } else if(name=="shortcuts") {
  rounded(2.9,5,18.2,14);for(int y:{9,12})for(int x:{7,12,17})p.drawPoint(QPointF(x,y));p.drawLine(8,16,16,16);
 } else if(name=="images"||name=="image") {
  rounded(4,4,16,16);QPainterPath sun;sun.addEllipse(QPointF(9,9),2,2);wash(sun);p.drawPath(sun);
  QPainterPath mountains;mountains.moveTo(4,18);mountains.lineTo(10,14);mountains.lineTo(14,17);mountains.lineTo(17,12);mountains.lineTo(20,15);p.drawPath(mountains);
 } else if(name=="stop") {
  rounded(4,4,16,16,1.5);
 } else if(name=="record") {
  rounded(3,4,18,16);QPainterPath dot;dot.addEllipse(QPointF(12,12),4,4);wash(dot);p.drawPath(dot);
 } else if(name=="select") {
  // 1.8 units on the 24-unit grid gives a 1.5 px stroke in 20 px tool icons.
  line(color,1.8);QPainterPath pointer;pointer.moveTo(5,3);pointer.lineTo(19,13);pointer.lineTo(12.5,14);pointer.lineTo(16,21);pointer.lineTo(13,21);pointer.lineTo(9.5,15.5);pointer.lineTo(5,20);pointer.closeSubpath();p.drawPath(pointer);
 } else if(name=="redact") {
  fill(color);rounded(3,5,18,11,1);
  line(color,1.8);p.drawLine(3,20,21,20);
 } else if(name=="blur") {
  // A soft droplet, distinct from an app grid or pixelation.
  line(color,1.8);QPainterPath drop;drop.moveTo(12,3);drop.cubicTo(9,7,5,10,5,15);drop.cubicTo(5,23,19,23,19,15);drop.cubicTo(19,10,15,7,12,3);p.drawPath(drop);
  line(color);p.drawArc(QRectF(8,11,8,8),195*16,150*16);
 } else if(name=="crop") {
  line(color,1.8);p.drawLine(7,3,7,17);p.drawLine(7,17,21,17);
  p.drawLine(3,7,17,7);p.drawLine(17,7,17,21);
 } else if(name=="annotate-arrow") {
  line(color,1.8);p.drawLine(4,20,20,4);p.drawLine(11,4,20,4);p.drawLine(20,4,20,13);
 } else if(name=="rectangle") {
  line(color,1.8);rounded(4,5,16,14,1);
 } else if(name=="highlight") {
  line(color,1.8);QPainterPath marker;
  marker.moveTo(16,3);marker.lineTo(21,8);marker.lineTo(12,18);marker.lineTo(6,12);marker.closeSubpath();
  marker.moveTo(14,4);marker.lineTo(20,10);marker.moveTo(7,13);marker.lineTo(3,19);marker.lineTo(9,19);marker.lineTo(11,17);p.drawPath(marker);
  p.drawLine(3,21,21,21);
 } else if(name=="chevron-down") {
  line(color);QPainterPath chevron;chevron.moveTo(6,9);chevron.lineTo(12,15);chevron.lineTo(18,9);p.drawPath(chevron);
 } else if(name=="text") {
  line(color,1.8);p.drawLine(5,5,19,5);p.drawLine(12,5,12,20);
 } else if(name=="undo"||name=="redo") {
  if(name=="redo"){p.translate(24,0);p.scale(-1,1);}
  line(color,1.8);QPainterPath turn;turn.moveTo(4,9);turn.lineTo(15,9);turn.quadTo(21,9,21,15);turn.quadTo(21,21,15,21);turn.lineTo(10,21);
  turn.moveTo(9,4);turn.lineTo(4,9);turn.lineTo(9,14);p.drawPath(turn);
 } else if(name=="zoom-fit") {
  line(color,1.8);rounded(4,4,16,16,2);
  QPainterPath arrows;arrows.moveTo(10,10);arrows.lineTo(7,7);arrows.lineTo(7,10);arrows.moveTo(7,7);arrows.lineTo(10,7);
  arrows.moveTo(14,14);arrows.lineTo(17,17);arrows.lineTo(14,17);arrows.moveTo(17,17);arrows.lineTo(17,14);p.drawPath(arrows);
 } else if(name=="zoom-actual") {
  line(color,1.8);rounded(2.9,4.4,18.2,15.2,3);
  QPainterPath ones;ones.moveTo(6.8,9.2);ones.lineTo(9.2,7.6);ones.lineTo(9.2,16.4);
  ones.moveTo(14.8,9.2);ones.lineTo(17.2,7.6);ones.lineTo(17.2,16.4);p.drawPath(ones);
  fill(color);p.drawEllipse(QPointF(12.4,9.9),0.9,0.9);p.drawEllipse(QPointF(12.4,14.3),0.9,0.9);
 } else if(name=="back"||name=="forward"||name=="up") {
  // The fallback file dialog's toolbar (#259): one arrow, turned.
  if(name=="forward"){p.translate(24,0);p.scale(-1,1);}
  if(name=="up"){p.translate(12,12);p.rotate(90);p.translate(-12,-12);}
  line(color);p.drawLine(QPointF(7,12),QPointF(20,12));
  line(color);QPainterPath head;head.moveTo(11,5);head.lineTo(4,12);head.lineTo(11,19);p.drawPath(head);
 } else if(name=="folder-new"||name=="save") {
  p.drawPath(folder());p.drawLine(12,10,12,17);
  if(name=="save"){p.drawLine(8,13,12,17);p.drawLine(12,17,16,13);}else p.drawLine(8,13,16,13);
 } else if(name=="view-list") {
  for(int y:{5,12,19}){p.drawPoint(QPointF(4,y));p.drawLine(9,y,21,y);}
 } else if(name=="view-detail") {
  rounded(3,3,18,18);p.drawLine(3,9,21,9);p.drawLine(3,15,21,15);p.drawLine(10,3,10,21);
 } else if(name=="play") {
  QPainterPath play;play.moveTo(7,4);play.lineTo(20,12);play.lineTo(7,20);play.closeSubpath();p.drawPath(play);
 } else if(name=="pause") {
  rounded(5,4,4,16,1);rounded(15,4,4,16,1);
 } else if(name=="trim-in"||name=="trim-out"||name=="trim-reset") {
  if(name=="trim-out"){p.translate(24,0);p.scale(-1,1);}
  const bool reset=name=="trim-reset";rounded(reset?7:10,9,reset?10:11,6,1);
  QPainterPath bracket;const auto cut=[&](qreal x,qreal dir){bracket.moveTo(x+3*dir,3);bracket.lineTo(x,3);bracket.lineTo(x,21);bracket.lineTo(x+3*dir,21);};
  if(reset){cut(3,1);cut(21,-1);}else{cut(6,1);p.drawLine(3,10,3,14);}p.drawPath(bracket);
 } else if(name=="mask-in"||name=="mask-out") {
  if(name=="mask-out"){p.translate(24,0);p.scale(-1,1);}
  const QRectF block(10,5,11,14);p.drawRoundedRect(block,1,1);
  p.save();p.setClipRect(block.adjusted(1,1,-1,-1));for(int d=-8;d<12;d+=5)p.drawLine(QPointF(block.left()+d,block.bottom()),QPointF(block.left()+d+14,block.top()));p.restore();
  QPainterPath bracket;bracket.moveTo(7,3);bracket.lineTo(3,3);bracket.lineTo(3,21);bracket.lineTo(7,21);p.drawPath(bracket);
 } else if(name=="remove") {
  line(color,1.8);QPainterPath bin;bin.moveTo(6,8);bin.lineTo(7.5,21);bin.lineTo(16.5,21);bin.lineTo(18,8);p.drawPath(bin);
  p.drawLine(4,5,20,5);p.drawLine(9,3,15,3);p.drawLine(9,3,9,5);p.drawLine(15,3,15,5);
  p.drawLine(10,9,10,17);p.drawLine(14,9,14,17);
 } else if(name=="bucket") {
  p.drawEllipse(QRectF(4,3,16,6));QPainterPath body;body.moveTo(4,6);body.lineTo(4,18);body.cubicTo(4,22,20,22,20,18);body.lineTo(20,6);p.drawPath(body);p.drawArc(QRectF(4,9,16,6),180*16,180*16);
 } else if(name=="proton") {
  // A clean cloud silhouette stays legible at the provider's actual 16 px.
  QPainterPath cloud;cloud.moveTo(7,19);cloud.cubicTo(1,19,1,10,6.5,10);
  cloud.cubicTo(6.5,3,16.5,2,18,9);cloud.cubicTo(23,9,23,19,18,19);
  cloud.closeSubpath();p.drawPath(cloud);
 } else if(name=="add") {
  p.drawEllipse(QRectF(2.4,2.4,19.2,19.2));p.drawLine(12,7,12,17);p.drawLine(7,12,17,12);
 } else {
  QPainterPath file;file.moveTo(6,3);file.lineTo(14,3);file.lineTo(20,9);file.lineTo(20,19);file.quadTo(20,21,18,21);file.lineTo(6,21);file.quadTo(4,21,4,19);file.lineTo(4,5);file.quadTo(4,3,6,3);file.closeSubpath();p.drawPath(file);
  QPainterPath fold;fold.moveTo(14,3);fold.lineTo(14,9);fold.lineTo(20,9);p.drawPath(fold);
  if(name=="videos"||name=="video"){QPainterPath play;play.moveTo(9,12);play.lineTo(15,15);play.lineTo(9,18);play.closeSubpath();p.drawPath(play);}
  else{p.drawLine(8,13,15,13);p.drawLine(8,17,13,17);}
 }
 return image;
}
}
