#include "core/SanitizedPng.h"
#include "editor/EditDocument.h"
#include "editor/EditorCanvas.h"
#include "editor/EditorCommands.h"
#include <QApplication>
#include <QBuffer>
#include <QColorSpace>
#include <QImageWriter>
#include <QRandomGenerator>
#include <QTest>
#include <QtEndian>
using namespace putgd;using namespace putgd::editor;

namespace {
QImage noise(QSize size,quint32 seed,QImage::Format format=QImage::Format_RGB32){
 QImage image(size,format);QRandomGenerator random(seed);
 for(int y=0;y<size.height();++y){auto* line=reinterpret_cast<QRgb*>(image.scanLine(y));for(int x=0;x<size.width();++x)line[x]=format==QImage::Format_RGB32?(random.generate()|0xff000000U):random.generate();}
 return image;
}
// Identical outside `mask`, different everywhere inside it.
QImage secretVariant(const QImage& base,const QRect& mask,quint32 seed){
 auto image=base.copy();QRandomGenerator random(seed);
 for(int y=mask.top();y<=mask.bottom();++y)for(int x=mask.left();x<=mask.right();++x)image.setPixel(x,y,(random.generate()|0xff000000U)^image.pixel(x,y));
 return image;
}
Shape box(Shape::Kind kind,QRect rect){Shape shape;shape.kind=kind;shape.rect=rect;if(kind==Shape::Kind::Redact)shape.color=Qt::black;return shape;}
QByteArray chunk(const QByteArray& type,const QByteArray& data){
 QByteArray out(4,0);qToBigEndian<quint32>(quint32(data.size()),out.data());out+=type+data;
 // CRC-32 over type and data, computed independently of the inspector.
 quint32 c=0xffffffffU;for(const char byte:type+data){c^=quint8(byte);for(int k=0;k<8;++k)c=(c&1)?0xedb88320U^(c>>1):c>>1;}
 QByteArray crc(4,0);qToBigEndian<quint32>(c^0xffffffffU,crc.data());return out+crc;
}
QByteArray insertBeforeIend(QByteArray png,const QByteArray& extra){return png.insert(png.size()-12,extra);}
}

class EditorTest:public QObject {
 Q_OBJECT
private slots:
 void redactionRemovesSecretPixelsThroughEveryOperation_data(){
  QTest::addColumn<int>("variant");
  QTest::newRow("redact-only")<<0;QTest::newRow("blur-overlapping")<<1;QTest::newRow("blur-adjacent")<<2;
  QTest::newRow("annotations-over-mask")<<3;QTest::newRow("cropped-through-mask")<<4;QTest::newRow("all")<<5;
 }
 void redactionRemovesSecretPixelsThroughEveryOperation(){
  QFETCH(int,variant);
  const QRect mask(37,21,61,17);const auto base=noise({211,149},7);
  const auto a=secretVariant(base,mask,11),b=secretVariant(base,mask,12);
  QVERIFY(a!=b);
  auto edit=[&](const QImage& source){
   EditDocument doc(source);doc.add(box(Shape::Kind::Redact,mask));
   if(variant==1||variant==5){auto blur=box(Shape::Kind::Blur,mask.adjusted(-20,-9,25,14));blur.strength=40;doc.add(blur);}
   if(variant==2||variant==5){auto blur=box(Shape::Kind::Blur,QRect(mask.right()+1,mask.top(),30,mask.height()));blur.strength=64;doc.add(blur);}
   if(variant==3||variant==5){doc.add(box(Shape::Kind::Highlight,mask.adjusted(-5,-5,5,5)));doc.add(box(Shape::Kind::Rectangle,mask));Shape text;text.kind=Shape::Kind::Text;text.text="covered";text.rect=QRect(mask.topLeft(),QSize(60,20));doc.add(text);Shape arrow;arrow.kind=Shape::Kind::Arrow;arrow.from={0,0};arrow.to=mask.center();doc.add(arrow);}
   if(variant==4||variant==5)doc.setCrop(QRect(50,10,120,60));
   return doc;
  };
  const auto first=edit(a),second=edit(b);
  const auto renderedA=first.render(),renderedB=second.render();
  QCOMPARE(renderedA,renderedB);
  // Decoded uploads are byte-for-byte equal, not merely visually covered.
  const auto pngA=first.exportPng(),pngB=second.exportPng();QVERIFY(!pngA.isEmpty());QCOMPARE(pngA,pngB);
  if(variant==0){
   for(int y=mask.top();y<=mask.bottom();++y)for(int x=mask.left();x<=mask.right();++x)QCOMPARE(renderedA.pixel(x,y),qRgb(0,0,0));
   QCOMPARE(renderedA.pixel(mask.left()-1,mask.top()),base.pixel(mask.left()-1,mask.top()));
   QCOMPARE(renderedA.pixel(mask.right()+1,mask.bottom()),base.pixel(mask.right()+1,mask.bottom()));
  }
 }
 void tinyAndEdgeMasksCoverExactlyTheirPixels(){
  const auto base=noise({40,30},3);EditDocument doc(base);
  doc.add(box(Shape::Kind::Redact,QRect(0,0,1,1)));doc.add(box(Shape::Kind::Redact,QRect(39,29,1,1)));doc.add(box(Shape::Kind::Redact,EditDocument::clampRect(QRect(35,-5,20,8),doc.imageRect())));
  const auto out=doc.render();
  QCOMPARE(out.pixel(0,0),qRgb(0,0,0));QCOMPARE(out.pixel(39,29),qRgb(0,0,0));QCOMPARE(out.pixel(1,0),base.pixel(1,0));QCOMPARE(out.pixel(38,29),base.pixel(38,29));
  for(int x=35;x<40;++x)for(int y=0;y<3;++y)QCOMPARE(out.pixel(x,y),qRgb(0,0,0));
  QCOMPARE(out.pixel(34,0),base.pixel(34,0));QCOMPARE(out.pixel(35,3),base.pixel(35,3));
 }
 void transparencyClearsHiddenColorAndRedactionIsOpaque(){
  QImage source(20,10,QImage::Format_ARGB32);source.fill(qRgba(200,10,10,0));
  for(int x=0;x<10;++x)source.setPixel(x,5,qRgba(1,2,3,255));
  EditDocument doc(source);doc.add(box(Shape::Kind::Redact,QRect(12,1,4,4)));
  const auto out=doc.render();QVERIFY(out.hasAlphaChannel());
  QCOMPARE(out.pixel(19,9),0u);QCOMPARE(out.pixel(0,0),0u);QCOMPARE(out.pixel(3,5),qRgba(1,2,3,255));QCOMPARE(out.pixel(13,2),qRgba(0,0,0,255));
  const auto decoded=QImage::fromData(doc.exportPng(),"PNG").convertToFormat(QImage::Format_ARGB32);
  QCOMPARE(decoded.pixel(19,9),0u);QCOMPARE(decoded.pixel(13,2),qRgba(0,0,0,255));
 }
 void exportCarriesNoSourceMetadata(){
  auto source=noise({64,48},5);source.setText("Comment","CANARY-COMMENT");source.setText("Author","CANARY-AUTHOR");
  source.setColorSpace(QColorSpace(QColorSpace::DisplayP3));source.setDotsPerMeterX(1234);
  EditDocument doc(source);doc.add(box(Shape::Kind::Redact,QRect(1,1,5,5)));
  const auto png=doc.exportPng();QSize size;
  QVERIFY(!inspectSanitizedPng(png,&size));QCOMPARE(size,QSize(64,48));
  QVERIFY(!png.contains("CANARY"));QVERIFY(!png.contains("iCCP"));QVERIFY(!png.contains("tEXt"));QVERIFY(!png.contains("iTXt"));QVERIFY(!png.contains("zTXt"));
  QCOMPARE(QImage::fromData(png,"PNG").convertToFormat(QImage::Format_RGB32),doc.render());
 }
 void sanitizedPngInspectorRejectsMetadataAndTrailingData(){
  QByteArray clean;{QBuffer buffer(&clean);buffer.open(QIODevice::WriteOnly);QImageWriter writer(&buffer,"png");QVERIFY(writer.write(noise({9,7},1)));}
  QVERIFY(!inspectSanitizedPng(clean));
  for(const auto& type:{QByteArray("tEXt"),QByteArray("iTXt"),QByteArray("zTXt"),QByteArray("eXIf"),QByteArray("tIME"),QByteArray("iCCP"),QByteArray("prVt")})
   QCOMPARE(inspectSanitizedPng(insertBeforeIend(clean,chunk(type,"CANARY"))).code,QString("edit_output_invalid"));
  QCOMPARE(inspectSanitizedPng(clean+"appended").code,QString("edit_output_invalid"));
  QCOMPARE(inspectSanitizedPng(clean+clean).code,QString("edit_output_invalid"));
  auto corrupt=clean;corrupt[corrupt.size()-20]=char(corrupt[corrupt.size()-20]^0x40);QCOMPARE(inspectSanitizedPng(corrupt).code,QString("edit_output_invalid"));
  QCOMPARE(inspectSanitizedPng(clean.left(clean.size()-12)).code,QString("edit_output_invalid"));
  QCOMPARE(inspectSanitizedPng(QByteArray("GIF89a")).code,QString("edit_output_invalid"));
 }
 void undoHistoryIsBoundedWithoutChangingVisibleEdits(){
  EditDocument doc(noise({500,500},2));
  for(int i=0;i<1000;++i)doc.add(box(Shape::Kind::Rectangle,QRect(i%400,i%300,20,20)));
  QCOMPARE(doc.shapes().size(),1000);QVERIFY(doc.historyEntries()<=EditDocument::maximumHistory);QVERIFY(doc.historyBytes()<=EditDocument::maximumHistoryBytes);QVERIFY(doc.historyTruncated());
  // Snapshots of 1000 shapes reach the byte budget before the step count.
  const int retained=doc.historyEntries();QVERIFY(retained>10);
  int undone=0;while(doc.undo())++undone;QCOMPARE(undone,retained);QCOMPARE(doc.shapes().size(),1000-retained);QVERIFY(doc.historyBytes()<=EditDocument::maximumHistoryBytes);
  int redone=0;while(doc.redo())++redone;QCOMPARE(redone,retained);QCOMPARE(doc.shapes().size(),1000);
  EditDocument small(noise({50,50},3));for(int i=0;i<300;++i)small.add(box(Shape::Kind::Redact,QRect(1,1,2,2)));QCOMPARE(small.historyEntries(),EditDocument::maximumHistory);
  // Long text cannot grow history past its byte budget either.
  Shape text;text.kind=Shape::Kind::Text;text.text=QString(5000,'x');text.rect=QRect(1,1,10,10);const auto id=doc.add(text);QCOMPARE(doc.find(id)->text.size(),EditDocument::maximumTextCharacters);
 }
 void gesturesCommitOneUndoStep(){
  EditDocument doc(noise({100,100},4));doc.beginGesture();const auto id=doc.add(box(Shape::Kind::Redact,QRect(1,1,2,2)));
  for(int i=3;i<60;++i){auto shape=*doc.find(id);shape.rect=QRect(1,1,i,i);doc.preview(shape);}
  QVERIFY(!doc.canUndo());doc.commitGesture();QVERIFY(doc.canUndo());QCOMPARE(doc.find(id)->rect,QRect(1,1,59,59));
  QVERIFY(doc.undo());QVERIFY(doc.shapes().isEmpty());QVERIFY(!doc.canUndo());
  doc.beginGesture();doc.add(box(Shape::Kind::Blur,QRect(1,1,9,9)));doc.cancelGesture();QVERIFY(doc.shapes().isEmpty());QVERIFY(doc.canRedo());
 }
 void commandsAreAtomicAndValidated(){
  EditDocument doc(noise({300,200},6));QJsonArray created;QString code,message;
  const QJsonArray batch{
   QJsonObject{{"op","add"},{"shape",QJsonObject{{"kind","redact"},{"rect",QJsonArray{10,10,40,20}}}}},
   QJsonObject{{"op","add"},{"shape",QJsonObject{{"kind","blur"},{"rect",QJsonArray{60,10,40,20}},{"strength",30}}}},
   QJsonObject{{"op","add"},{"shape",QJsonObject{{"kind","arrow"},{"from",QJsonArray{0,0}},{"to",QJsonArray{50,50}},{"color","#22c55e"},{"width",6}}}},
   QJsonObject{{"op","add"},{"shape",QJsonObject{{"kind","text"},{"at",QJsonArray{5,150}},{"text","Look here"},{"fontPixels",24}}}},
   QJsonObject{{"op","add"},{"shape",QJsonObject{{"kind","highlight"},{"rect",QJsonArray{100,100,50,10}}}}},
   QJsonObject{{"op","add"},{"shape",QJsonObject{{"kind","rectangle"},{"rect",QJsonArray{120,40,30,30}}}}},
   QJsonObject{{"op","crop"},{"rect",QJsonArray{0,0,250,180}}}};
  QVERIFY2(Commands::apply(doc,batch,created,code,message),qPrintable(message));QCOMPARE(created.size(),6);QCOMPARE(doc.shapes().size(),6);QCOMPARE(doc.crop(),QRect(0,0,250,180));
  auto state=Commands::state(doc);QCOMPARE(state["shapes"].toArray()[0].toObject()["kind"].toString(),QString("redact"));QCOMPARE(state["crop"].toArray(),(QJsonArray{0,0,250,180}));
  QCOMPARE(state["shapes"].toArray()[2].toObject()["color"].toString(),QString("#22c55e"));QVERIFY(state["canUndo"].toBool());
  const auto before=Commands::state(doc);
  auto rejected=[&](const QJsonArray& commands,const QString& expected){QVERIFY(!Commands::apply(doc,commands,created,code,message));QCOMPARE(code,expected);QCOMPARE(Commands::state(doc),before);};
  const auto first=created[0].toInt();
  rejected({QJsonObject{{"op","remove"},{"shapeId",first}},QJsonObject{{"op","add"},{"shape",QJsonObject{{"kind","redact"},{"rect",QJsonArray{1,1,1,1}},{"color","#ffffff"}}}}},"edit_command_invalid");
  rejected({QJsonObject{{"op","update"},{"shapeId",999},{"shape",QJsonObject{{"rect",QJsonArray{1,1,5,5}}}}}},"edit_shape_missing");
  rejected({QJsonObject{{"op","add"},{"shape",QJsonObject{{"kind","redact"},{"rect",QJsonArray{900,900,5,5}}}}}},"edit_command_invalid");
  rejected({QJsonObject{{"op","add"},{"shape",QJsonObject{{"kind","blur"},{"rect",QJsonArray{1,1,5,5}},{"strength",100}}}}},"edit_command_invalid");
  rejected({QJsonObject{{"op","add"},{"shape",QJsonObject{{"kind","text"},{"at",QJsonArray{1,1}},{"text","two\nlines"}}}}},"edit_command_invalid");
  rejected({QJsonObject{{"op","shell"}}},"edit_command_invalid");
  rejected({},"edit_command_invalid");
  QVERIFY(Commands::apply(doc,{QJsonObject{{"op","update"},{"shapeId",first},{"shape",QJsonObject{{"rect",QJsonArray{-10,-10,30,30}}}}}},created,code,message));
  QCOMPARE(doc.find(quint64(first))->rect,QRect(0,0,20,20));
  QVERIFY(Commands::apply(doc,{QJsonObject{{"op","undo"}},QJsonObject{{"op","resetCrop"}}},created,code,message));
  QCOMPARE(doc.find(quint64(first))->rect,QRect(10,10,40,20));QCOMPARE(doc.crop(),doc.imageRect());
  QVERIFY(Commands::apply(doc,{QJsonObject{{"op","undo"}},QJsonObject{{"op","redo"}}},created,code,message));
  QVERIFY(Commands::apply(doc,{QJsonObject{{"op","remove"},{"shapeId",first}}},created,code,message));QCOMPARE(doc.shapes().size(),5);
 }
 void canvasDragCreatesOutwardRoundedRedaction(){
  EditDocument doc(noise({400,300},8));EditorCanvas canvas(doc);canvas.resize(832,632);canvas.show();QVERIFY(QTest::qWaitForWindowExposed(&canvas));
  canvas.setTool(Tool::Redact);canvas.fit();QCOMPARE(canvas.zoom(),1.0);
  // At 2x the widget-to-image mapping is fractional; partial pixels are covered.
  canvas.zoomBy(2.0,QPointF(0,0));QCOMPARE(canvas.zoom(),2.0);
  const QPoint origin=canvas.mapFromGlobal(canvas.mapToGlobal(QPoint(0,0)));Q_UNUSED(origin);
  QTest::mousePress(&canvas,Qt::LeftButton,{},QPoint(101,101));
  QMouseEvent move(QEvent::MouseMove,QPointF(161,131),canvas.mapToGlobal(QPointF(161,131)),Qt::NoButton,Qt::LeftButton,Qt::NoModifier);QApplication::sendEvent(&canvas,&move);
  QTest::mouseRelease(&canvas,Qt::LeftButton,{},QPoint(161,131));
  QCOMPARE(doc.shapes().size(),1);const auto rect=doc.shapes().first().rect;QCOMPARE(doc.shapes().first().kind,Shape::Kind::Redact);
  QVERIFY(rect.width()>=30&&rect.height()>=15);QVERIFY(doc.canUndo());
  // Delete removes the selection; undo restores it as one step.
  QTest::keyClick(&canvas,Qt::Key_Delete);QVERIFY(doc.shapes().isEmpty());QVERIFY(doc.undo());QCOMPARE(doc.shapes().first().rect,rect);
  // A click without a drag creates nothing.
  canvas.documentChanged();QTest::mouseClick(&canvas,Qt::LeftButton,{},QPoint(500,500));QCOMPARE(doc.shapes().size(),1);
 }
 void drawingToolsPickUpVisibleShapes(){
  EditDocument doc(noise({400,300},11));EditorCanvas canvas(doc);canvas.resize(832,632);canvas.show();QVERIFY(QTest::qWaitForWindowExposed(&canvas));
  canvas.setTool(Tool::Rectangle);canvas.fit();QCOMPARE(canvas.zoom(),1.0);
  // Fitted at 100% and centered: image point p is at widget origin+p.
  const QPoint origin((canvas.width()-400)/2,(canvas.height()-300)/2);
  auto drag=[&](QPoint from,QPoint to,Qt::KeyboardModifiers modifiers={}){
   QTest::mousePress(&canvas,Qt::LeftButton,modifiers,origin+from);
   QMouseEvent move(QEvent::MouseMove,QPointF(origin+to),canvas.mapToGlobal(QPointF(origin+to)),Qt::NoButton,Qt::LeftButton,modifiers);QApplication::sendEvent(&canvas,&move);
   QTest::mouseRelease(&canvas,Qt::LeftButton,modifiers,origin+to);
  };
  auto frame=box(Shape::Kind::Rectangle,QRect(100,100,120,80));frame.width=4;const auto frameId=doc.add(frame);
  const auto redactId=doc.add(box(Shape::Kind::Redact,QRect(260,40,60,40)));
  canvas.documentChanged();
  // A rectangle's outline picks it up and moves it; no new shape is drawn.
  drag({101,140},{131,160});QCOMPARE(doc.shapes().size(),2);QCOMPARE(doc.find(frameId)->rect,QRect(130,120,120,80));QCOMPARE(canvas.selected(),frameId);
  QVERIFY(doc.undo());QCOMPARE(doc.find(frameId)->rect,QRect(100,100,120,80));canvas.documentChanged();
  // Its interior stays drawable, and Ctrl draws even on the outline.
  drag({150,130},{190,160});QCOMPARE(doc.shapes().size(),3);
  drag({101,140},{60,60},Qt::ControlModifier);QCOMPARE(doc.shapes().size(),4);QCOMPARE(doc.find(frameId)->rect,QRect(100,100,120,80));
  // Filled shapes are picked up anywhere they cover, in any drawing tool.
  canvas.setTool(Tool::Arrow);QCOMPARE(canvas.selected(),quint64(0));
  drag({290,60},{300,90});QCOMPARE(doc.shapes().size(),4);QCOMPARE(doc.find(redactId)->rect,QRect(270,70,60,40));
  // A click selects without nudging or adding an undo step.
  const auto before=doc.find(redactId)->rect;QVERIFY(doc.undo());QVERIFY(doc.redo());
  QTest::mouseClick(&canvas,Qt::LeftButton,{},origin+QPoint(300,90));QCOMPARE(canvas.selected(),redactId);QCOMPARE(doc.find(redactId)->rect,before);QVERIFY(!doc.canRedo());
 }
 void textToolDragsLabelsAndClicksToEdit(){
  EditDocument doc(noise({400,300},12));EditorCanvas canvas(doc);canvas.resize(832,632);canvas.show();QVERIFY(QTest::qWaitForWindowExposed(&canvas));
  canvas.setTool(Tool::Text);canvas.fit();
  const QPoint origin((canvas.width()-400)/2,(canvas.height()-300)/2);
  auto label=box(Shape::Kind::Text,QRect(50,50,80,30));label.text="Note";label.fontPixels=28;const auto id=doc.add(label);canvas.documentChanged();
  QTest::mousePress(&canvas,Qt::LeftButton,{},origin+QPoint(60,60));
  QMouseEvent move(QEvent::MouseMove,QPointF(origin+QPoint(100,90)),canvas.mapToGlobal(QPointF(origin+QPoint(100,90))),Qt::NoButton,Qt::LeftButton,Qt::NoModifier);QApplication::sendEvent(&canvas,&move);
  QTest::mouseRelease(&canvas,Qt::LeftButton,{},origin+QPoint(100,90));
  QCOMPARE(doc.find(id)->rect.topLeft(),QPoint(90,80));QVERIFY(!canvas.editingText());
  QTest::mouseClick(&canvas,Qt::LeftButton,{},origin+QPoint(100,90));QVERIFY(canvas.editingText());
  canvas.finishText(false);QCOMPARE(doc.find(id)->text,QString("Note"));
 }
 void canvasCropRequiresExplicitApply(){
  EditDocument doc(noise({400,300},9));EditorCanvas canvas(doc);canvas.resize(500,400);canvas.show();QVERIFY(QTest::qWaitForWindowExposed(&canvas));
  canvas.setTool(Tool::Crop);
  const auto start=QPoint(int(canvas.width()/2-100),int(canvas.height()/2-60));
  QTest::mousePress(&canvas,Qt::LeftButton,{},start);
  QMouseEvent move(QEvent::MouseMove,QPointF(start+QPoint(120,80)),canvas.mapToGlobal(QPointF(start+QPoint(120,80))),Qt::NoButton,Qt::LeftButton,Qt::NoModifier);QApplication::sendEvent(&canvas,&move);
  QTest::mouseRelease(&canvas,Qt::LeftButton,{},start+QPoint(120,80));
  QCOMPARE(doc.crop(),doc.imageRect());
  QTest::keyClick(&canvas,Qt::Key_Escape);QCOMPARE(doc.crop(),doc.imageRect());QCOMPARE(canvas.tool(),Tool::Select);
  canvas.setTool(Tool::Crop);QTest::mousePress(&canvas,Qt::LeftButton,{},start);QApplication::sendEvent(&canvas,&move);QTest::mouseRelease(&canvas,Qt::LeftButton,{},start+QPoint(120,80));
  QTest::keyClick(&canvas,Qt::Key_Return);QVERIFY(doc.crop()!=doc.imageRect());QVERIFY(doc.crop().width()>50);QVERIFY(doc.canUndo());
 }
 void blurPreviewCacheIsBounded(){
  EditDocument doc(noise({3000,2000},10));EditorCanvas canvas(doc);canvas.resize(800,600);canvas.show();QVERIFY(QTest::qWaitForWindowExposed(&canvas));
  for(int i=0;i<40;++i){auto blur=box(Shape::Kind::Blur,QRect((i*61)%2500,(i*37)%1500,500,500));blur.strength=20;doc.add(blur);}
  canvas.documentChanged();canvas.actualSize();canvas.repaint();canvas.fit();canvas.repaint();
  QVERIFY(canvas.cachedBlurBytes()<=EditorCanvas::blurCacheBytes);QVERIFY(canvas.cachedBlurBytes()>0);
 }
};
QTEST_MAIN(EditorTest)
#include "test_editor.moc"
