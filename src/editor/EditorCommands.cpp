#include "EditorCommands.h"
#include <QFont>
#include <QFontMetrics>
#include <QRegularExpression>
#include <cmath>

namespace putgd::editor::Commands {
namespace {
const QHash<QString,Shape::Kind>& kinds(){
 static const QHash<QString,Shape::Kind> values{{"redact",Shape::Kind::Redact},{"blur",Shape::Kind::Blur},{"arrow",Shape::Kind::Arrow},
  {"rectangle",Shape::Kind::Rectangle},{"text",Shape::Kind::Text},{"highlight",Shape::Kind::Highlight}};
 return values;
}
bool integer(const QJsonValue& value,qint64 minimum,qint64 maximum,int& out){
 if(!value.isDouble())return false;const double number=value.toDouble();
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
QRect textBox(const QPoint& at,const QString& text,int pixels){
 QFont font;font.setPixelSize(std::max(4,pixels));font.setBold(true);const QFontMetrics metrics(font);
 return QRect(at,QSize(metrics.horizontalAdvance(text)+1,metrics.height()));
}
struct Failure{QString code,message;};
// Fields absent from `object` keep the values already in `shape`.
bool shapeFrom(const QJsonObject& object,Shape& shape,const QRect& image,bool creating,Failure& failure){
 auto bad=[&](const QString& message){failure={"edit_command_invalid",message};return false;};
 if(!only(object,{"kind","rect","from","to","at","color","width","strength","fontPixels","text"}))return bad("Shape has unsupported fields");
 if(creating){
  if(!kinds().contains(object["kind"].toString()))return bad("Shape kind must be redact, blur, arrow, rectangle, text or highlight");
  shape.kind=kinds().value(object["kind"].toString());
  if(shape.kind==Shape::Kind::Redact)shape.color=Qt::black;
  if(shape.kind==Shape::Kind::Highlight)shape.color=QColor(250,204,21);
 }else if(object.contains("kind")&&kinds().value(object["kind"].toString(),Shape::Kind(-1))!=shape.kind)return bad("An existing shape cannot change kind");
 const bool arrow=shape.kind==Shape::Kind::Arrow,text=shape.kind==Shape::Kind::Text;
 if(object.contains("color")){
  static const QRegularExpression hex("^#[0-9A-Fa-f]{6}$");
  if(shape.kind==Shape::Kind::Redact||shape.kind==Shape::Kind::Blur)return bad("Redact is always opaque black and Blur has no color");
  if(!hex.match(object["color"].toString()).hasMatch())return bad("Color must be #RRGGBB");
  shape.color=QColor(object["color"].toString());
 }
 if(object.contains("width")&&!integer(object["width"],1,64,shape.width))return bad("Width must be an integer from 1 to 64");
 if(object.contains("strength")&&!integer(object["strength"],EditDocument::minimumBlur,EditDocument::maximumBlur,shape.strength))return bad("Blur strength must be an integer from 2 to 64");
 if(object.contains("fontPixels")&&!integer(object["fontPixels"],8,200,shape.fontPixels))return bad("Text size must be an integer from 8 to 200 pixels");
 if(object.contains("text")){
  if(!text)return bad("Only text shapes have text");
  const auto value=object["text"].toString();
  if(!object["text"].isString()||value.trimmed().isEmpty()||value.size()>EditDocument::maximumTextCharacters||value.contains('\n')||value.contains('\r'))return bad("Text must be one nonempty line of at most 500 characters");
  shape.text=value;
 }
 if(arrow){
  if(object.contains("rect")||object.contains("at"))return bad("Arrows use from and to points");
  if(object.contains("from")&&!pointFrom(object["from"],shape.from))return bad("from must be [x, y]");
  if(object.contains("to")&&!pointFrom(object["to"],shape.to))return bad("to must be [x, y]");
  if(creating&&(!object.contains("from")||!object.contains("to")))return bad("Arrows need from and to points");
  if(shape.from==shape.to)return bad("Arrow points must differ");
 }else if(text){
  if(object.contains("rect")||object.contains("from")||object.contains("to"))return bad("Text uses an at point; its box follows the text");
  QPoint at=shape.rect.topLeft();if(object.contains("at")&&!pointFrom(object["at"],at))return bad("at must be [x, y]");
  if(creating&&(!object.contains("at")||!object.contains("text")))return bad("Text needs at and text");
  shape.rect=textBox(at,shape.text,shape.fontPixels);
 }else{
  if(object.contains("from")||object.contains("to")||object.contains("at"))return bad("Box shapes use rect");
  if(object.contains("rect")&&!rectFrom(object["rect"],shape.rect))return bad("rect must be [x, y, width, height] with positive size");
  if(creating&&!object.contains("rect"))return bad("Box shapes need rect");
 }
 const auto bounds=arrow?QRect(shape.from,shape.to).normalized().adjusted(0,0,1,1):shape.rect;
 if(!bounds.intersects(image))return bad("Shape lies outside the image");
 // Redaction is clamped so its stored geometry equals the exported coverage.
 if(shape.kind==Shape::Kind::Redact||shape.kind==Shape::Kind::Blur)shape.rect=EditDocument::clampRect(shape.rect,image);
 return true;
}
}

QString kindName(Shape::Kind kind){return kinds().key(kind);}

QJsonObject shapeJson(const Shape& shape){
 QJsonObject result{{"id",double(shape.id)},{"kind",kindName(shape.kind)}};
 if(shape.kind==Shape::Kind::Arrow){result["from"]=pointJson(shape.from);result["to"]=pointJson(shape.to);}
 else result["rect"]=rectJson(shape.rect);
 if(shape.kind==Shape::Kind::Arrow||shape.kind==Shape::Kind::Rectangle||shape.kind==Shape::Kind::Text||shape.kind==Shape::Kind::Highlight)result["color"]=shape.color.name();
 if(shape.kind==Shape::Kind::Arrow||shape.kind==Shape::Kind::Rectangle)result["width"]=shape.width;
 if(shape.kind==Shape::Kind::Blur)result["strength"]=shape.strength;
 if(shape.kind==Shape::Kind::Text){result["text"]=shape.text;result["fontPixels"]=shape.fontPixels;result["at"]=pointJson(shape.rect.topLeft());}
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
   Shape shape;if(!shapeFrom(command["shape"].toObject(),shape,draft.imageRect(),true,failure))return stop(failure.code,failure.message);
   ids.append(double(draft.add(shape)));
  }else if(op=="update"){
   if(!only(command,{"op","shapeId","shape"})||!integer(command["shapeId"],1,INT_MAX,shapeId)||!command["shape"].isObject())return stop("edit_command_invalid","update needs shapeId and a shape object");
   const auto* current=draft.find(quint64(shapeId));if(!current)return stop("edit_shape_missing","No shape has this id");
   auto shape=*current;if(!shapeFrom(command["shape"].toObject(),shape,draft.imageRect(),false,failure))return stop(failure.code,failure.message);
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
