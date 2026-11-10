#include "EditorCommands.h"
#include "HighlightFilter.h"
#include <QFont>
#include <QFontMetrics>
#include <QRegularExpression>
#include <cmath>

namespace putgd::editor::Commands {
namespace {
const QHash<QString,Shape::Kind>& kinds(){
 static const QHash<QString,Shape::Kind> values{{"redact",Shape::Kind::Redact},{"blur",Shape::Kind::Blur},{"arrow",Shape::Kind::Arrow},
  {"rectangle",Shape::Kind::Rectangle},{"text",Shape::Kind::Text},{"highlight",Shape::Kind::Highlight},
  {"step",Shape::Kind::Step},{"pen",Shape::Kind::Pen},{"ellipse",Shape::Kind::Ellipse},{"line",Shape::Kind::Line}};
 return values;
}
bool integer(const QJsonValue& value,qint64 minimum,qint64 maximum,int& out){
 if(!value.isDouble())return false;
 const double number=value.toDouble();
 if(!std::isfinite(number)||std::floor(number)!=number||number<minimum||number>maximum)return false;
 out=int(number);return true;
}
QJsonArray rectJson(const QRect& r){return {r.x(),r.y(),r.width(),r.height()};}
QJsonArray pointJson(const QPoint& p){return {p.x(),p.y()};}
bool rectFrom(const QJsonValue& value,QRect& out){
 const auto a=value.toArray();int x=0,y=0,w=0,h=0;
 if(!value.isArray()||a.size()!=4||!integer(a[0],-1000000,1000000,x)||!integer(a[1],-1000000,1000000,y)||!integer(a[2],1,1000000,w)||!integer(a[3],1,1000000,h))return false;
 out=QRect(x,y,w,h);return true;
}
bool pointFrom(const QJsonValue& value,QPoint& out){
 const auto a=value.toArray();int x=0,y=0;
 if(!value.isArray()||a.size()!=2||!integer(a[0],-1000000,1000000,x)||!integer(a[1],-1000000,1000000,y))return false;
 out=QPoint(x,y);return true;
}
bool only(const QJsonObject& object,const QStringList& allowed){for(auto it=object.begin();it!=object.end();++it)if(!allowed.contains(it.key()))return false;return true;}
QRect textBox(const QPoint& at,const QString& text,const QString& family,int pixels){
 const QFontMetrics metrics(textFont(family,pixels));
 return QRect(at,QSize(metrics.horizontalAdvance(text)+1,metrics.height()));
}
struct Failure{QString code,message;};
// Fields absent from `object` keep the values already in `shape`.
bool shapeFrom(const QJsonObject& object,Shape& shape,const EditDocument& doc,bool creating,Failure& failure){
 const auto image=doc.imageRect();
 auto bad=[&](const QString& message){failure={"edit_command_invalid",message};return false;};
 if(!only(object,{"kind","rect","from","to","at","color","width","strength","style","fontPixels","font","text","snap","number","size","points"}))return bad("Shape has unsupported fields");
 if(creating){
  if(!kinds().contains(object["kind"].toString()))return bad("Shape kind must be redact, blur, arrow, line, rectangle, ellipse, pen, step, text or highlight");
  shape.kind=kinds().value(object["kind"].toString());
  if(shape.kind==Shape::Kind::Redact)shape.color=Qt::black;
  if(shape.kind==Shape::Kind::Highlight)shape.color=QColor(250,204,21);
  if(shape.kind==Shape::Kind::Step){shape.number=doc.nextStepNumber();shape.rect=QRect(0,0,defaultStepPixels,defaultStepPixels);}
 }else if(object.contains("kind")&&kinds().value(object["kind"].toString(),Shape::Kind(-1))!=shape.kind)return bad("An existing shape cannot change kind");
 using Kind=Shape::Kind;
 const bool twoPoint=shape.twoPoint(),text=shape.kind==Kind::Text,step=shape.kind==Kind::Step,pen=shape.kind==Kind::Pen;
 if(object.contains("color")){
  static const QRegularExpression hex("^#[0-9A-Fa-f]{6}$");
  if(shape.kind==Kind::Redact||shape.kind==Kind::Blur)return bad("A redaction is always opaque black and a blur has no color");
  if(!hex.match(object["color"].toString()).hasMatch())return bad("Color must be #RRGGBB");
  shape.color=QColor(object["color"].toString());
 }
 if(object.contains("width")&&!integer(object["width"],1,64,shape.width))return bad("Width must be an integer from 1 to 64");
 if(object.contains("strength")&&!integer(object["strength"],EditDocument::minimumBlur,EditDocument::maximumBlur,shape.strength))return bad("Blur strength must be an integer from 2 to 64");
 if(object.contains("style")){
  if(shape.kind!=Kind::Blur||!object["style"].isString()||!QStringList{"blur","pixelate"}.contains(object["style"].toString()))return bad("style applies to blurs and must be blur or pixelate");
  shape.pixelate=object["style"]=="pixelate";
 }
 if(object.contains("fontPixels")&&!integer(object["fontPixels"],minimumFontPixels,maximumFontPixels,shape.fontPixels))return bad("Text size must be an integer from 8 to 200 pixels");
 if(object.contains("font")){
  if(!text)return bad("Only text shapes have a font");
  const auto family=object["font"].toString();bool printable=object["font"].isString()&&family.size()<=maximumFontFamilyCharacters;
  for(const auto c:family)printable=printable&&c.isPrint();
  if(!printable)return bad("font must be a family name of at most 100 printable characters; empty means the default");
  shape.fontFamily=family;
 }
 if(object.contains("text")){
  if(!text)return bad("Only text shapes have text");
  const auto value=object["text"].toString();
  if(!object["text"].isString()||value.trimmed().isEmpty()||value.size()>EditDocument::maximumTextCharacters||value.contains('\n')||value.contains('\r'))return bad("Text must be one nonempty line of at most 500 characters");
  shape.text=value;
 }
 if((object.contains("number")||object.contains("size"))&&!step)return bad("Only steps have a number and a size");
 if(object.contains("number")&&!integer(object["number"],1,maximumStepNumber,shape.number))return bad("number must be an integer from 1 to 999");
 if(object.contains("points")&&!pen)return bad("Only pen strokes have points");
 const auto snap=object["snap"].toString();
 if(object.contains("snap")&&(shape.kind!=Kind::Highlight||!object["snap"].isString()||!QStringList{"none","text","word","line"}.contains(snap)))return bad("snap applies to highlights and must be none, text, word or line");
 if(twoPoint){
  const auto name=shape.kind==Kind::Arrow?QStringLiteral("Arrows"):QStringLiteral("Lines");
  if(object.contains("rect")||object.contains("at"))return bad(name+" use from and to points");
  if(object.contains("from")&&!pointFrom(object["from"],shape.from))return bad("from must be [x, y]");
  if(object.contains("to")&&!pointFrom(object["to"],shape.to))return bad("to must be [x, y]");
  if(creating&&(!object.contains("from")||!object.contains("to")))return bad(name+" need from and to points");
  if(shape.from==shape.to)return bad(shape.kind==Kind::Arrow?"Arrow points must differ":"Line points must differ");
 }else if(text){
  if(object.contains("rect")||object.contains("from")||object.contains("to"))return bad("Text uses an at point; its box follows the text");
  QPoint at=shape.rect.topLeft();if(object.contains("at")&&!pointFrom(object["at"],at))return bad("at must be [x, y]");
  if(creating&&(!object.contains("at")||!object.contains("text")))return bad("Text needs at and text");
  shape.rect=textBox(at,shape.text,shape.fontFamily,shape.fontPixels);
 }else if(step){
  if(object.contains("rect")||object.contains("from")||object.contains("to"))return bad("A step uses an at point (its centre) and an optional size");
  int size=shape.rect.width();if(object.contains("size")&&!integer(object["size"],minimumStepPixels,maximumStepPixels,size))return bad("size must be an integer from 16 to 256 pixels");
  QPoint at=stepCentre(shape.rect);if(object.contains("at")&&!pointFrom(object["at"],at))return bad("at must be [x, y]");
  if(creating&&!object.contains("at"))return bad("A step needs at [x, y]");
  // The centre stays put when only the size changes.
  shape.rect=QRect(at.x()-size/2,at.y()-size/2,size,size);
 }else if(pen){
  if(object.contains("rect")||object.contains("from")||object.contains("to")||object.contains("at"))return bad("A pen stroke uses points");
  if(object.contains("points")){
   const auto values=object["points"].toArray();
   if(!object["points"].isArray()||values.isEmpty()||values.size()>maximumPenPoints)return bad("points must be 1 to 2000 [x, y] points");
   QList<QPoint> points;points.reserve(values.size());
   for(const auto& value:values){QPoint p;if(!pointFrom(value,p))return bad("points must be 1 to 2000 [x, y] points");points.append(p);}
   shape.points=points;
  }else if(creating)return bad("A pen stroke needs points");
 }else{
  const bool pointSnap=snap=="word"||snap=="line";
  if(object.contains("from")||object.contains("to")||(object.contains("at")&&!pointSnap))return bad("Box shapes use rect");
  if(pointSnap&&object.contains("rect"))return bad("snap word or line takes an at point, not rect");
  if(object.contains("rect")&&!rectFrom(object["rect"],shape.rect))return bad("rect must be [x, y, width, height] with positive size");
  if(creating&&!object.contains("rect")&&!pointSnap)return bad("Box shapes need rect");
  if(pointSnap){
   QPoint at;if(!pointFrom(object["at"],at))return bad("snap word or line needs at [x, y]");
   if(!image.contains(at))return bad("at lies outside the image");
   const auto found=snap=="word"?highlight::wordAt(doc.base(),at,doc.redactionRects()):highlight::lineAt(doc.base(),at,doc.redactionRects());
   if(found.isNull())return bad("No text at that point");
   shape.rect=found;
  }else if(snap=="text")shape.rect=highlight::snapToText(doc.base(),shape.rect,doc.redactionRects());
 }
 const auto bounds=twoPoint||pen?shape.box().adjusted(0,0,1,1):shape.rect;
 if(!bounds.intersects(image))return bad("Shape lies outside the image");
 // Redaction is clamped so its stored geometry equals the exported coverage.
 if(shape.kind==Kind::Redact||shape.kind==Kind::Blur)shape.rect=EditDocument::clampRect(shape.rect,image);
 return true;
}
}

QString kindName(Shape::Kind kind){return kinds().key(kind);}

QJsonObject shapeJson(const Shape& shape){
 using Kind=Shape::Kind;
 QJsonObject result{{"id",double(shape.id)},{"kind",kindName(shape.kind)}};
 if(shape.twoPoint()){result["from"]=pointJson(shape.from);result["to"]=pointJson(shape.to);}
 else if(shape.kind==Kind::Pen){QJsonArray points;for(const auto& p:shape.points)points.append(pointJson(p));result["points"]=points;result["rect"]=rectJson(shape.box());}
 else result["rect"]=rectJson(shape.rect);
 if(shape.kind!=Kind::Redact&&shape.kind!=Kind::Blur)result["color"]=shape.color.name();
 if(shape.twoPoint()||shape.kind==Kind::Rectangle||shape.kind==Kind::Ellipse||shape.kind==Kind::Pen)result["width"]=shape.width;
 if(shape.kind==Kind::Blur){result["strength"]=shape.strength;result["style"]=shape.pixelate?"pixelate":"blur";}
 if(shape.kind==Kind::Step){result["number"]=shape.number;result["size"]=shape.rect.width();result["at"]=pointJson(stepCentre(shape.rect));}
 if(shape.kind==Shape::Kind::Text){result["text"]=shape.text;result["fontPixels"]=shape.fontPixels;result["font"]=shape.fontFamily;result["at"]=pointJson(shape.rect.topLeft());}
 return result;
}

QJsonObject state(const EditDocument& doc){
 QJsonArray shapes;for(const auto& shape:doc.shapes())shapes.append(shapeJson(shape));
 return {{"width",doc.size().width()},{"height",doc.size().height()},{"crop",rectJson(doc.crop())},{"shapes",shapes},
  {"canUndo",doc.canUndo()},{"canRedo",doc.canRedo()},{"historyTruncated",doc.historyTruncated()},{"modified",doc.modified()}};
}

bool apply(EditDocument& doc,const QJsonArray& commands,QJsonArray& created,QString& code,QString& message){
 created={};
 if(commands.isEmpty()||commands.size()>maximumCommands){code="edit_command_invalid";message="Supply 1 to 100 commands";return false;}
 // The base raster is implicitly shared; only geometry/history are copied.
 auto draft=doc;QJsonArray ids;Failure failure;
 for(qsizetype index=0;index<commands.size();++index){
  const auto command=commands[index].toObject();const auto op=command["op"].toString();
  auto stop=[&](const QString& c,const QString& m){code=c;message=QString("Command %1: %2").arg(index).arg(m);return false;};
  if(!commands[index].isObject())return stop("edit_command_invalid","Each command must be an object");
  int shapeId=0;
  if(op=="add"){
   if(!only(command,{"op","shape"})||!command["shape"].isObject())return stop("edit_command_invalid","add needs only a shape object");
   Shape shape;if(!shapeFrom(command["shape"].toObject(),shape,draft,true,failure))return stop(failure.code,failure.message);
   ids.append(double(draft.add(shape)));
  }else if(op=="update"){
   if(!only(command,{"op","shapeId","shape"})||!integer(command["shapeId"],1,INT_MAX,shapeId)||!command["shape"].isObject())return stop("edit_command_invalid","update needs shapeId and a shape object");
   const auto* current=draft.find(quint64(shapeId));if(!current)return stop("edit_shape_missing","No shape has this id");
   auto shape=*current;if(!shapeFrom(command["shape"].toObject(),shape,draft,false,failure))return stop(failure.code,failure.message);
   draft.replace(shape);
  }else if(op=="remove"){
   if(!only(command,{"op","shapeId"})||!integer(command["shapeId"],1,INT_MAX,shapeId))return stop("edit_command_invalid","remove needs shapeId");
   if(!draft.remove(quint64(shapeId)))return stop("edit_shape_missing","No shape has this id");
  }else if(op=="crop"){
   QRect rect;if(!only(command,{"op","rect"})||!rectFrom(command["rect"],rect))return stop("edit_command_invalid","crop needs rect [x, y, width, height]");
   if(!rect.intersects(draft.imageRect()))return stop("edit_command_invalid","Crop lies outside the image");
   draft.setCrop(rect);
  }else if(op=="resetCrop"){if(!only(command,{"op"}))return stop("edit_command_invalid","resetCrop has no fields");draft.setCrop(draft.imageRect());}
  else if(op=="undo"||op=="redo"){
   if(!only(command,{"op"}))return stop("edit_command_invalid",op+" has no fields");
   if(!(op=="undo"?draft.undo():draft.redo()))return stop("edit_history_empty","Nothing to "+op);
  }else return stop("edit_command_invalid","op must be add, update, remove, crop, resetCrop, undo or redo");
 }
 doc=std::move(draft);created=ids;return true;
}
}
