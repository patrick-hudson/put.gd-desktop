#include "core/SanitizedPng.h"
#include "editor/EditDocument.h"
#include "editor/EditorCanvas.h"
#include "editor/EditorCommands.h"
#include "ui/Theme.h"
#include <QApplication>
#include <QBuffer>
#include <QColorSpace>
#include <QImageWriter>
#include <QPainter>
#include <QRandomGenerator>
#include <QTest>
#include <QScopeGuard>
#include <thread>
#include <QtEndian>
#include "fixtures/Waits.h"
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
 void textOutputBoundsMeasureExportInkInsteadOfTheEditableBox(){
  Shape text;text.kind=Shape::Kind::Text;text.rect={60,60,1,1};text.text="jWé";text.fontPixels=60;
  const auto bounds=textOutputBounds(text);QVERIFY(bounds.width()>text.rect.width());QVERIFY(bounds.height()>text.rect.height());
  QImage image(400,250,QImage::Format_ARGB32);image.fill(Qt::transparent);
  {QPainter painter(&image);EditDocument::paintAnnotation(painter,text);}
  QRect ink;
  for(int y=0;y<image.height();++y)for(int x=0;x<image.width();++x)if(qAlpha(image.pixel(x,y)))ink=ink.united(QRect(x,y,1,1));
  QVERIFY(!ink.isEmpty());QVERIFY(bounds.adjusted(-1,-1,1,1).contains(QRectF(ink)));
  QVERIFY(qAbs(ink.left()-bounds.left())<=1);QVERIFY(qAbs(ink.top()-bounds.top())<=1);
  QVERIFY(qAbs(ink.right()+1-bounds.right())<=1);QVERIFY(qAbs(ink.bottom()+1-bounds.bottom())<=1);
  text.text="   ";QVERIFY(textOutputBounds(text).isEmpty());
 }
 void exportSnapshotKeepsGuiTextOutlinesOnTheWorker(){
  const auto font=qApp->font();auto restore=qScopeGuard([&]{qApp->setFont(font);});
  EditDocument document(noise({320,200},21));Shape text;text.kind=Shape::Kind::Text;text.rect={10,40,200,40};text.text="synthetic";document.add(text);
  const auto expected=document.exportPng();QVERIFY(!expected.isEmpty());const auto snapshot=document.snapshotForExport();
  auto changed=font;changed.setStretch(150);qApp->setFont(changed);QVERIFY(document.exportPng()!=expected);
  QByteArray saved;std::thread worker([&]{saved=snapshot.exportPng();});worker.join();QCOMPARE(saved,expected);
 }
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
  EditDocument small(noise({50,50},3));for(int i=0;i<210;++i)small.add(box(Shape::Kind::Redact,QRect(1,1,2,2)));QCOMPARE(small.historyEntries(),EditDocument::maximumHistory);
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
  EditDocument doc(noise({400,300},8));EditorCanvas canvas(doc);canvas.resize(832,632);canvas.show();QVERIFY(putgd::test::waitForWindowExposed(&canvas));
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
  EditDocument doc(noise({400,300},11));EditorCanvas canvas(doc);canvas.resize(832,632);canvas.show();QVERIFY(putgd::test::waitForWindowExposed(&canvas));
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
 void pointerEditsStayOnTheImage(){
  EditDocument doc(noise({400,300},15));EditorCanvas canvas(doc);canvas.resize(832,632);canvas.show();QVERIFY(putgd::test::waitForWindowExposed(&canvas));
  canvas.setTool(Tool::Rectangle);canvas.fit();QCOMPARE(canvas.zoom(),1.0);
  const QPoint origin((canvas.width()-400)/2,(canvas.height()-300)/2);
  auto drag=[&](QPoint from,QPoint to){
   QTest::mousePress(&canvas,Qt::LeftButton,{},origin+from);
   QMouseEvent move(QEvent::MouseMove,QPointF(origin+to),canvas.mapToGlobal(QPointF(origin+to)),Qt::NoButton,Qt::LeftButton,Qt::NoModifier);QApplication::sendEvent(&canvas,&move);
   QTest::mouseRelease(&canvas,Qt::LeftButton,{},origin+to);
  };
  // Drawing past an edge stops at it, and a press in the margin starts on the edge.
  drag({300,200},{460,380});QCOMPARE(doc.shapes().size(),1);const auto big=doc.shapes().last().id;QCOMPARE(doc.find(big)->rect,QRect(300,200,100,100));
  canvas.select(0);drag({-40,-30},{20,10});QCOMPARE(doc.shapes().size(),2);const auto small=doc.shapes().last().id;QCOMPARE(doc.find(small)->rect,QRect(0,0,20,10));
  // A dragged shape stops at the edge, and a resize handle stops there too.
  canvas.setTool(Tool::Select);drag({350,250},{-100,250});QCOMPARE(doc.find(big)->rect,QRect(0,200,100,100));
  drag({100,300},{600,500});QCOMPARE(doc.find(big)->rect,QRect(0,200,400,100));
  // Arrow ends stay on the image.
  canvas.setTool(Tool::Arrow);drag({350,50},{500,-80});QCOMPARE(doc.shapes().size(),3);
  QCOMPARE(doc.shapes().last().from,QPoint(350,50));QCOMPARE(doc.shapes().last().to,QPoint(400,0));
  // A text click in the margin opens no label.
  canvas.setTool(Tool::Text);QTest::mouseClick(&canvas,Qt::LeftButton,{},origin+QPoint(-30,100));QVERIFY(!canvas.editingText());
  // Keyboard nudges stop at the edge without adding an undo step.
  canvas.setTool(Tool::Select);canvas.select(small);const auto history=doc.historyEntries();
  QTest::keyClick(&canvas,Qt::Key_Left);QTest::keyClick(&canvas,Qt::Key_Up);QCOMPARE(doc.find(small)->rect,QRect(0,0,20,10));QCOMPARE(doc.historyEntries(),history);
  // After a crop, shrinking by keyboard stops at the crop's near edge.
  const auto wide=doc.add(box(Shape::Kind::Rectangle,QRect(0,20,100,40)));QVERIFY(doc.setCrop(QRect(50,0,200,150)));canvas.documentChanged();canvas.select(wide);
  for(int i=0;i<5;++i)QTest::keyClick(&canvas,Qt::Key_Left,Qt::ShiftModifier|Qt::ControlModifier);
  QCOMPARE(doc.find(wide)->rect,QRect(0,20,51,40));
  // A sideways corner drag leaves an edge already outside the crop where it is.
  const auto tall=doc.add(box(Shape::Kind::Rectangle,QRect(100,100,50,100)));canvas.documentChanged();canvas.select(tall);
  const QPoint cropped((canvas.width()-200)/2-50,(canvas.height()-150)/2);
  QTest::mousePress(&canvas,Qt::LeftButton,{},cropped+QPoint(150,200));
  QMouseEvent sideways(QEvent::MouseMove,QPointF(cropped+QPoint(170,200)),canvas.mapToGlobal(QPointF(cropped+QPoint(170,200))),Qt::NoButton,Qt::LeftButton,Qt::NoModifier);QApplication::sendEvent(&canvas,&sideways);
  QTest::mouseRelease(&canvas,Qt::LeftButton,{},cropped+QPoint(170,200));
  QCOMPARE(doc.find(tall)->rect,QRect(100,100,70,100));
 }
 void squaresAndArrowEndsKeepTheirAnchors(){
  EditDocument doc(noise({400,300},17));EditorCanvas canvas(doc);canvas.resize(832,632);canvas.show();QVERIFY(putgd::test::waitForWindowExposed(&canvas));
  canvas.setTool(Tool::Rectangle);canvas.fit();QCOMPARE(canvas.zoom(),1.0);
  const QPoint origin((canvas.width()-400)/2,(canvas.height()-300)/2);
  auto drag=[&](QPoint from,QPoint to,Qt::KeyboardModifiers modifiers){
   QTest::mousePress(&canvas,Qt::LeftButton,modifiers,origin+from);
   QMouseEvent move(QEvent::MouseMove,QPointF(origin+to),canvas.mapToGlobal(QPointF(origin+to)),Qt::NoButton,Qt::LeftButton,modifiers);QApplication::sendEvent(&canvas,&move);
   QTest::mouseRelease(&canvas,Qt::LeftButton,modifiers,origin+to);
  };
  // A Shift-drag in the left margin draws nothing, not a square on the image.
  drag({-30,50},{-10,150},Qt::ShiftModifier);QVERIFY(doc.shapes().isEmpty());
  // A square drawn up and left still starts at the press point.
  drag({390,290},{100,200},Qt::ShiftModifier);QCOMPARE(doc.shapes().size(),1);QCOMPARE(doc.shapes().first().rect,QRect(100,0,290,290));
  // An arrow head left of a later crop keeps its x when resized vertically.
  Shape arrow;arrow.kind=Shape::Kind::Arrow;arrow.from={150,50};arrow.to={60,80};const auto id=doc.add(arrow);
  QVERIFY(doc.setCrop(QRect(100,0,200,150)));canvas.setTool(Tool::Select);canvas.documentChanged();canvas.select(id);
  QTest::keyClick(&canvas,Qt::Key_Down,Qt::ShiftModifier);QCOMPARE(doc.find(id)->to,QPoint(60,81));
 }
 void marginDragDrawsNothing(){
  EditDocument doc(noise({400,300},16));EditorCanvas canvas(doc);canvas.resize(832,632);canvas.show();QVERIFY(putgd::test::waitForWindowExposed(&canvas));
  canvas.setTool(Tool::Rectangle);canvas.fit();
  // At 4x the image overflows the canvas and its right edge stops 48 px from the widget edge.
  canvas.zoomBy(4.0,QPointF((canvas.width()-400)/2+400,canvas.height()/2));QCOMPARE(canvas.zoom(),4.0);
  const int edge=canvas.width()-48;
  QTest::mousePress(&canvas,Qt::LeftButton,{},QPoint(edge+24,250));
  QMouseEvent move(QEvent::MouseMove,QPointF(edge+38,400),canvas.mapToGlobal(QPointF(edge+38,400)),Qt::NoButton,Qt::LeftButton,Qt::NoModifier);QApplication::sendEvent(&canvas,&move);
  QTest::mouseRelease(&canvas,Qt::LeftButton,{},QPoint(edge+38,400));
  QVERIFY(doc.shapes().isEmpty());QVERIFY(!doc.canUndo());
  // The same drag starting on the image draws up to the edge.
  QTest::mousePress(&canvas,Qt::LeftButton,{},QPoint(edge-40,250));
  QMouseEvent inward(QEvent::MouseMove,QPointF(edge+38,400),canvas.mapToGlobal(QPointF(edge+38,400)),Qt::NoButton,Qt::LeftButton,Qt::NoModifier);QApplication::sendEvent(&canvas,&inward);
  QTest::mouseRelease(&canvas,Qt::LeftButton,{},QPoint(edge+38,400));
  QCOMPARE(doc.shapes().size(),1);QCOMPARE(doc.shapes().first().rect.right(),399);
 }
 void textToolDragsLabelsAndClicksToEdit(){
  EditDocument doc(noise({400,300},12));EditorCanvas canvas(doc);canvas.resize(832,632);canvas.show();QVERIFY(putgd::test::waitForWindowExposed(&canvas));
  canvas.setTool(Tool::Text);canvas.fit();
  const QPoint origin((canvas.width()-400)/2,(canvas.height()-300)/2);
  auto label=box(Shape::Kind::Text,QRect(50,50,80,30));label.text="Note";label.fontPixels=28;const auto id=doc.add(label);canvas.documentChanged();
  QTest::mousePress(&canvas,Qt::LeftButton,{},origin+QPoint(60,60));
  QMouseEvent move(QEvent::MouseMove,QPointF(origin+QPoint(100,90)),canvas.mapToGlobal(QPointF(origin+QPoint(100,90))),Qt::NoButton,Qt::LeftButton,Qt::NoModifier);QApplication::sendEvent(&canvas,&move);
  QTest::mouseRelease(&canvas,Qt::LeftButton,{},origin+QPoint(100,90));
  QCOMPARE(doc.find(id)->rect.topLeft(),QPoint(90,80));QVERIFY(!canvas.editingText());
  QTest::mouseClick(&canvas,Qt::LeftButton,{},origin+doc.find(id)->rect.center());QVERIFY(canvas.editingText());
  canvas.finishText(false);QCOMPARE(doc.find(id)->text,QString("Note"));
 }
 void textFontFamilyIsAgentSettableAndSizesTheBox(){
  EditDocument doc(noise({400,200},13));QJsonArray created;QString code,message;
  QVERIFY2(Commands::apply(doc,{QJsonObject{{"op","add"},{"shape",QJsonObject{{"kind","text"},{"at",QJsonArray{10,10}},{"text","Wide WWW"},{"fontPixels",40},{"font","DejaVu Sans Mono"}}}}},created,code,message),qPrintable(message));
  const auto id=quint64(created.first().toInteger());const auto* shape=doc.find(id);QCOMPARE(shape->fontFamily,QString("DejaVu Sans Mono"));
  QCOMPARE(QFontMetrics(textFont(shape->fontFamily,40)).height(),shape->rect.height());
  const auto state=Commands::state(doc)["shapes"].toArray().first().toObject();QCOMPARE(state["font"].toString(),QString("DejaVu Sans Mono"));QCOMPARE(state["fontPixels"].toInt(),40);
  QVERIFY(Commands::apply(doc,{QJsonObject{{"op","update"},{"shapeId",double(id)},{"shape",QJsonObject{{"font",""}}}}},created,code,message));QVERIFY(doc.find(id)->fontFamily.isEmpty());
  QVERIFY(!Commands::apply(doc,{QJsonObject{{"op","update"},{"shapeId",double(id)},{"shape",QJsonObject{{"font",QString(101,'a')}}}}},created,code,message));QCOMPARE(code,QString("edit_command_invalid"));
  QVERIFY(!Commands::apply(doc,{QJsonObject{{"op","update"},{"shapeId",double(id)},{"shape",QJsonObject{{"font","Bad\nName"}}}}},created,code,message));
  QVERIFY(!Commands::apply(doc,{QJsonObject{{"op","add"},{"shape",QJsonObject{{"kind","rectangle"},{"rect",QJsonArray{1,1,5,5}},{"font","Sans"}}}}},created,code,message));
  QVERIFY(!doc.exportPng().isEmpty());
 }
 void textCornersScaleTheFontAroundTheOppositeCorner(){
  EditDocument doc(noise({400,300},14));EditorCanvas canvas(doc);canvas.resize(832,632);canvas.show();QVERIFY(putgd::test::waitForWindowExposed(&canvas));
  canvas.setTool(Tool::Select);canvas.fit();QCOMPARE(canvas.zoom(),1.0);
  const QPoint origin((canvas.width()-400)/2,(canvas.height()-300)/2);
  QJsonArray created;QString code,message;
  QVERIFY(Commands::apply(doc,{QJsonObject{{"op","add"},{"shape",QJsonObject{{"kind","text"},{"at",QJsonArray{50,50}},{"text","Note"},{"fontPixels",28}}}}},created,code,message));
  const auto id=quint64(created.first().toInteger());canvas.documentChanged();const auto before=doc.find(id)->rect;
  auto drag=[&](QPoint from,QPoint to){
   QTest::mousePress(&canvas,Qt::LeftButton,{},origin+from);
   QMouseEvent move(QEvent::MouseMove,QPointF(origin+to),canvas.mapToGlobal(QPointF(origin+to)),Qt::NoButton,Qt::LeftButton,Qt::NoModifier);QApplication::sendEvent(&canvas,&move);
   QTest::mouseRelease(&canvas,Qt::LeftButton,{},origin+to);
  };
  QTest::mouseClick(&canvas,Qt::LeftButton,{},origin+before.center());QCOMPARE(canvas.selected(),id);
  // Pull the bottom-right corner out along the diagonal to twice the size.
  const QPoint corner(before.x()+before.width(),before.y()+before.height());
  drag(corner,QPoint(before.x()+2*before.width(),before.y()+2*before.height()));
  const auto* grown=doc.find(id);QVERIFY2(qAbs(grown->fontPixels-56)<=1,qPrintable(QString::number(grown->fontPixels)));
  QCOMPARE(grown->rect.topLeft(),before.topLeft());QCOMPARE(grown->rect.height(),QFontMetrics(textFont({},grown->fontPixels)).height());
  QCOMPARE(doc.shapes().size(),1);QVERIFY(doc.undo());QCOMPARE(doc.find(id)->fontPixels,28);QVERIFY(doc.redo());canvas.documentChanged();
  // With the Text tool, the top-left corner shrinks it toward the fixed bottom-right corner.
  canvas.setTool(Tool::Text);QTest::mouseClick(&canvas,Qt::LeftButton,{},origin+doc.find(id)->rect.center());canvas.finishText(false);QCOMPARE(canvas.selected(),id);
  const auto big=doc.find(id)->rect;
  drag(big.topLeft(),big.topLeft()+QPoint(big.width()/2,big.height()/2));
  const auto* shrunk=doc.find(id);QVERIFY2(qAbs(shrunk->fontPixels-28)<=1,qPrintable(QString::number(shrunk->fontPixels)));
  QCOMPARE(shrunk->rect.bottomRight(),big.bottomRight());QVERIFY(!canvas.editingText());
 }
 void canvasCropRequiresExplicitApply(){
  EditDocument doc(noise({400,300},9));EditorCanvas canvas(doc);canvas.resize(500,400);canvas.show();QVERIFY(putgd::test::waitForWindowExposed(&canvas));
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
 // A replace outside a gesture is its own undo step (it used to write
 // through to the snapshot it had just taken, so undo skipped it).
 void replaceIsOneUndoStep(){
  EditDocument doc(noise({40,30},17));const auto id=doc.add(box(Shape::Kind::Rectangle,QRect(1,2,10,10)));
  auto moved=*doc.find(id);moved.rect.translate(5,0);QVERIFY(doc.replace(moved));
  auto recoloured=moved;recoloured.color=Qt::green;QVERIFY(doc.replace(recoloured));
  QVERIFY(doc.undo());QCOMPARE(*doc.find(id),moved);QVERIFY(doc.undo());QCOMPARE(doc.find(id)->rect,QRect(1,2,10,10));
  QVERIFY(doc.undo());QVERIFY(doc.shapes().isEmpty());QVERIFY(doc.redo());QVERIFY(doc.redo());QCOMPARE(*doc.find(id),moved);
 }
 // Every change to what export renders moves the generation, so a saved copy
 // can tell it is out of date; refused or unchanged edits do not.
 void generationFollowsEveryRenderedChange(){
  EditDocument doc(noise({40,30},18));auto last=doc.generation();
  auto moved=[&]{const bool changed=doc.generation()!=last;last=doc.generation();return changed;};
  const auto id=doc.add(box(Shape::Kind::Redact,QRect(1,2,10,10)));QVERIFY(moved());
  auto shape=*doc.find(id);QVERIFY(!doc.replace(shape));QVERIFY(!moved());
  shape.rect.translate(3,0);QVERIFY(doc.replace(shape));QVERIFY(moved());
  QVERIFY(doc.setCrop(QRect(0,0,20,20)));QVERIFY(moved());QVERIFY(!doc.setCrop(QRect(0,0,20,20)));QVERIFY(!moved());
  // A live drag changes the render before its one undo step commits.
  doc.beginGesture();shape.rect.translate(1,1);doc.preview(shape);QVERIFY(moved());doc.cancelGesture();QVERIFY(moved());
  QVERIFY(doc.undo());QVERIFY(moved());QVERIFY(doc.redo());QVERIFY(moved());
  QVERIFY(doc.remove(id));QVERIFY(moved());QVERIFY(!doc.remove(id));QVERIFY(!moved());
 }
 // Keyboard-only spatial editing: Insert places the tool's mark (centred, a
 // quarter of the image), brackets select marks, arrows move,
 // Shift+arrows resize, Ctrl takes ten-pixel steps. Each key is one undo step.
 void keyboardPlacesSelectsMovesAndResizesMarks(){
  EditDocument doc(noise({400,300},15));EditorCanvas canvas(doc);canvas.resize(832,632);canvas.show();QVERIFY(putgd::test::waitForWindowExposed(&canvas));
  canvas.setTool(Tool::Redact);canvas.setFocus();
  QTest::keyClick(&canvas,Qt::Key_Insert);QCOMPARE(doc.shapes().size(),1);
  const auto first=doc.shapes().first();QCOMPARE(first.rect,QRect(100,75,200,150));QCOMPARE(canvas.selected(),first.id);
  QVERIFY2(canvas.accessibleDescription().startsWith("Redaction 1 of 1 selected"),qPrintable(canvas.accessibleDescription()));
  // Another placement does not hide under the first.
  QTest::keyClick(&canvas,Qt::Key_Insert);QCOMPARE(doc.shapes().size(),2);QVERIFY(doc.shapes().last().rect!=first.rect);
  const auto second=doc.shapes().last().id;QCOMPARE(canvas.selected(),second);
  auto before=*doc.find(second);
  QTest::keyClick(&canvas,Qt::Key_Left);QCOMPARE(doc.find(second)->rect,before.rect.translated(-1,0));
  QTest::keyClick(&canvas,Qt::Key_Down,Qt::ControlModifier);QCOMPARE(doc.find(second)->rect,before.rect.translated(-1,10));
  before=*doc.find(second);
  QTest::keyClick(&canvas,Qt::Key_Right,Qt::ShiftModifier);QCOMPARE(doc.find(second)->rect,before.rect.adjusted(0,0,1,0));
  QTest::keyClick(&canvas,Qt::Key_Up,Qt::ShiftModifier|Qt::ControlModifier);QCOMPARE(doc.find(second)->rect,before.rect.adjusted(0,0,1,-10));
  QVERIFY(doc.undo());QCOMPARE(doc.find(second)->rect,before.rect.adjusted(0,0,1,0));QVERIFY(doc.redo());canvas.documentChanged();
  // Brackets cycle and wrap; selection methods can also stop at either end.
  QTest::keyClick(&canvas,Qt::Key_BracketRight);QCOMPARE(canvas.selected(),first.id);
  QTest::keyClick(&canvas,Qt::Key_BracketLeft);QCOMPARE(canvas.selected(),second);
  canvas.select(0);QVERIFY(canvas.accessibleDescription().startsWith("2 marks, none selected."));
  QVERIFY(canvas.selectAdjacent(1,false));QCOMPARE(canvas.selected(),first.id);
  QVERIFY(canvas.selectAdjacent(1,false));QCOMPARE(canvas.selected(),second);
  QVERIFY(!canvas.selectAdjacent(1,false));QCOMPARE(canvas.selected(),second);
  // Geometry fields: one replace, one undo step.
  const int undoable=doc.historyEntries();
  QVERIFY(canvas.setSelectionGeometry(QRect(10,20,30,40)));QCOMPARE(doc.find(second)->rect,QRect(10,20,30,40));QCOMPARE(doc.historyEntries(),undoable+1);
  QCOMPARE(*canvas.selectionGeometry(),QRect(10,20,30,40));
  QVERIFY(!canvas.setSelectionGeometry(QRect(1000,1000,10,10)));QCOMPARE(doc.find(second)->rect,QRect(10,20,30,40));
  // An arrow is placed tail to head and resized by its head.
  canvas.setTool(Tool::Arrow);QTest::keyClick(&canvas,Qt::Key_Insert);const auto arrow=*doc.find(canvas.selected());
  QCOMPARE(arrow.kind,Shape::Kind::Arrow);QVERIFY(arrow.from!=arrow.to);
  QTest::keyClick(&canvas,Qt::Key_Right,Qt::ShiftModifier);QCOMPARE(doc.find(arrow.id)->from,arrow.from);QCOMPARE(doc.find(arrow.id)->to,arrow.to+QPoint(1,0));
  QVERIFY(canvas.setSelectionGeometry(QRect(5,6,20,-3)));QCOMPARE(doc.find(arrow.id)->from,QPoint(5,6));QCOMPARE(doc.find(arrow.id)->to,QPoint(25,3));
  // Select and Crop place nothing.
  canvas.setTool(Tool::Select);QCOMPARE(canvas.placeMark(),quint64(0));
 }
 void keyboardPansWithoutEditingMarksOrCrop(){
  EditDocument doc(noise({400,300},17));EditorCanvas canvas(doc);canvas.resize(600,500);canvas.show();QVERIFY(putgd::test::waitForWindowExposed(&canvas));
  const auto id=doc.add(box(Shape::Kind::Rectangle,QRect(100,100,80,60)));canvas.documentChanged();canvas.select(id);canvas.setZoom(4);
  const auto shape=*doc.find(id);const int history=doc.historyEntries();
  const struct {Qt::Key key;QPoint delta;} moves[]={{Qt::Key_Left,{-32,0}},{Qt::Key_Right,{32,0}},{Qt::Key_Up,{0,-32}},{Qt::Key_Down,{0,32}}};
  for(const auto& move:moves)for(const bool fast:{false,true}){
   canvas.setZoom(4);canvas.panBy({100000,100000});canvas.panBy({-600,-400});
   const auto before=canvas.visibleImageRect();
   QTest::keyClick(&canvas,move.key,Qt::AltModifier|(fast?Qt::ShiftModifier:Qt::NoModifier));const auto actual=canvas.visibleImageRect();
   canvas.panBy(-QPointF(move.delta)*(fast?4:1));QCOMPARE(canvas.visibleImageRect(),before);QVERIFY(actual!=before);
   QCOMPARE(*doc.find(id),shape);QCOMPARE(canvas.selected(),id);QCOMPARE(doc.historyEntries(),history);
  }
  canvas.setTool(Tool::Crop);canvas.setZoom(4);const auto crop=canvas.cropDraft();
  QTest::keyClick(&canvas,Qt::Key_Left,Qt::AltModifier|Qt::ShiftModifier);QCOMPARE(canvas.cropDraft(),crop);QCOMPARE(doc.historyEntries(),history);
  QVERIFY(canvas.accessibleDescription().contains("Alt+arrow keys pan"));
 }
 void resizeHandlesUseLogicalTargetsAndNearestVisibleHandle_data(){
  QTest::addColumn<int>("kind");QTest::addColumn<QRect>("rect");QTest::addColumn<QPoint>("press");QTest::addColumn<QPoint>("delta");QTest::addColumn<QRect>("expected");QTest::addColumn<double>("zoom");
  QTest::newRow("box-corner-32px")<<int(Shape::Kind::Rectangle)<<QRect(100,100,100,80)<<QPoint(85,85)<<QPoint(-10,-10)<<QRect(90,90,110,90)<<1.;
  QTest::newRow("box-edge-32px")<<int(Shape::Kind::Rectangle)<<QRect(100,100,100,80)<<QPoint(215,155)<<QPoint(10,0)<<QRect(100,100,110,80)<<1.;
  QTest::newRow("overlapping-box-handles")<<int(Shape::Kind::Rectangle)<<QRect(100,100,20,20)<<QPoint(119,119)<<QPoint(10,10)<<QRect(100,100,30,30)<<1.;
  QTest::newRow("arrow-end-32px")<<int(Shape::Kind::Arrow)<<QRect(100,100,100,80)<<QPoint(215,195)<<QPoint(10,10)<<QRect(100,100,110,90)<<1.;
  QTest::newRow("overlapping-arrow-ends")<<int(Shape::Kind::Arrow)<<QRect(100,100,10,10)<<QPoint(109,109)<<QPoint(10,10)<<QRect(100,100,20,20)<<1.;
  QTest::newRow("zoomed-out-target")<<int(Shape::Kind::Rectangle)<<QRect(100,100,100,80)<<QPoint(70,70)<<QPoint(-20,-20)<<QRect(80,80,120,100)<<.5;
 }
 void resizeHandlesUseLogicalTargetsAndNearestVisibleHandle(){
  QFETCH(int,kind);QFETCH(QRect,rect);QFETCH(QPoint,press);QFETCH(QPoint,delta);QFETCH(QRect,expected);QFETCH(double,zoom);
  EditDocument doc(noise({400,300},18));EditorCanvas canvas(doc);canvas.resize(600,500);canvas.show();QVERIFY(putgd::test::waitForWindowExposed(&canvas));
  auto shape=box(Shape::Kind(kind),rect);shape.from=rect.topLeft();shape.to=rect.topLeft()+QPoint(rect.width(),rect.height());const auto id=doc.add(shape);
  canvas.setTool(Tool::Select);canvas.documentChanged();canvas.select(id);canvas.setZoom(zoom);
  const QPointF origin((canvas.width()-400*zoom)/2,(canvas.height()-300*zoom)/2);const auto from=(origin+QPointF(press)*zoom).toPoint(),to=(origin+QPointF(press+delta)*zoom).toPoint();
  const int history=doc.historyEntries();QTest::mousePress(&canvas,Qt::LeftButton,{},from);
  QMouseEvent move(QEvent::MouseMove,QPointF(to),canvas.mapToGlobal(QPointF(to)),Qt::NoButton,Qt::LeftButton,Qt::NoModifier);QApplication::sendEvent(&canvas,&move);
  QTest::mouseRelease(&canvas,Qt::LeftButton,{},to);
  const auto* actual=doc.find(id);QVERIFY(actual);
  if(actual->kind==Shape::Kind::Arrow){QCOMPARE(actual->from,expected.topLeft());QCOMPARE(actual->to,expected.topLeft()+QPoint(expected.width(),expected.height()));}else QCOMPARE(actual->rect,expected);
  QCOMPARE(doc.historyEntries(),history+1);QCOMPARE(canvas.selected(),id);
 }
 void cropAndTextUseEnlargedVisibleHandleTargets(){
  EditDocument doc(noise({400,300},19));EditorCanvas canvas(doc);canvas.resize(600,500);canvas.show();QVERIFY(putgd::test::waitForWindowExposed(&canvas));
  const QPoint origin(100,100);
  auto drag=[&](QPoint from,QPoint to){
   QTest::mousePress(&canvas,Qt::LeftButton,{},origin+from);
   QMouseEvent move(QEvent::MouseMove,QPointF(origin+to),canvas.mapToGlobal(QPointF(origin+to)),Qt::NoButton,Qt::LeftButton,Qt::NoModifier);QApplication::sendEvent(&canvas,&move);
   QTest::mouseRelease(&canvas,Qt::LeftButton,{},origin+to);
  };
  canvas.setTool(Tool::Crop);QVERIFY(canvas.setSelectionGeometry(QRect(100,100,20,20)));
  drag({119,119},{129,129});QCOMPARE(canvas.cropDraft(),QRect(100,100,30,30));QVERIFY(!doc.modified());canvas.cancelCrop();
  auto text=box(Shape::Kind::Text,QRect(100,100,20,20));text.text="Label";text.fontPixels=20;const QFontMetrics metrics(textFont({},20));text.rect.setSize(QSize(metrics.horizontalAdvance(text.text)+1,metrics.height()));const auto id=doc.add(text);text=*doc.find(id);canvas.documentChanged();canvas.setTool(Tool::Select);canvas.select(id);
  // Near the lower-left corner, the unpainted left midpoint must not steal its target.
  drag(text.rect.bottomLeft()+QPoint(-15,-5),text.rect.bottomLeft()+QPoint(-25,5));QVERIFY(doc.find(id)->fontPixels>text.fontPixels);QVERIFY(doc.find(id)->rect.x()<text.rect.x());
  QVERIFY(doc.undo());canvas.documentChanged();canvas.select(id);
  // A point just outside the 32 px square does not resize or move the mark.
  drag({83,83},{73,73});QCOMPARE(*doc.find(id),text);
 }
 void keyboardCropMovesResizesAndApplies(){
  EditDocument doc(noise({400,300},16));EditorCanvas canvas(doc);canvas.resize(600,500);canvas.show();QVERIFY(putgd::test::waitForWindowExposed(&canvas));
  canvas.setTool(Tool::Crop);canvas.setFocus();QCOMPARE(*canvas.selectionGeometry(),doc.imageRect());
  QVERIFY(canvas.accessibleDescription().startsWith("Crop area x 0, y 0, 400 × 300 px"));
  for(int i=0;i<10;++i)QTest::keyClick(&canvas,Qt::Key_Left,Qt::ShiftModifier|Qt::ControlModifier);
  QCOMPARE(canvas.cropDraft(),QRect(0,0,300,300));
  QTest::keyClick(&canvas,Qt::Key_Right,Qt::ControlModifier);QCOMPARE(canvas.cropDraft(),QRect(10,0,300,300));
  // Moving past the image edge is refused rather than clamped into a resize.
  for(int i=0;i<20;++i)QTest::keyClick(&canvas,Qt::Key_Right,Qt::ControlModifier);QCOMPARE(canvas.cropDraft(),QRect(100,0,300,300));
  QVERIFY(canvas.setSelectionGeometry(QRect(20,30,200,100)));QCOMPARE(doc.crop(),doc.imageRect());
  QTest::keyClick(&canvas,Qt::Key_Return);QCOMPARE(doc.crop(),QRect(20,30,200,100));QCOMPARE(canvas.tool(),Tool::Select);
 }
 // Crop and selection guides read over pale and dark images alike: each
 // carries a dark under-stroke and a light over-stroke, whatever the theme.
 void guidesUseDualStrokesOverAnyImage(){
  for(const auto& fill:{QColor(Qt::white),QColor(Qt::black),QColor("#e8b66d")}){
   QImage plain(400,300,QImage::Format_RGB32);plain.fill(fill);
   EditDocument doc(plain);EditorCanvas canvas(doc);canvas.resize(500,400);canvas.show();QVERIFY(putgd::test::waitForWindowExposed(&canvas));
   canvas.setTool(Tool::Crop);QVERIFY(canvas.setSelectionGeometry(QRect(100,80,200,140)));
   // A dark stroke (3:1 against white) and a light one (3:1 against black) along the edge.
   auto strokes=[&](const QImage& shot,int x0,int y,int x1){
    double dark=0,light=0;
    for(int x=x0;x<=x1;++x)for(int dy=-3;dy<=3;++dy){const auto c=shot.pixelColor(x,y+dy);dark=std::max(dark,Theme::contrast(c,Qt::white));light=std::max(light,Theme::contrast(c,Qt::black));}
    return std::pair{dark,light};
   };
   auto shot=canvas.grab().toImage().convertToFormat(QImage::Format_RGB32);
   // The crop's top edge in widget pixels: the image is shown at 100%, centred.
   const QPoint origin((canvas.width()-400)/2,(canvas.height()-300)/2);
   auto [dark,light]=strokes(shot,origin.x()+140,origin.y()+80,origin.x()+160);
   QVERIFY2(dark>=3&&light>=3,qPrintable(QString("crop on %1: dark %2, light %3").arg(fill.name()).arg(dark).arg(light)));
   // A selected blur (invisible on a flat image) shows only its outline.
   canvas.cancelCrop();canvas.setTool(Tool::Blur);canvas.placeMark();canvas.setTool(Tool::Select);canvas.select(doc.shapes().first().id);
   shot=canvas.grab().toImage().convertToFormat(QImage::Format_RGB32);
   const auto box=doc.shapes().first().rect;
   std::tie(dark,light)=strokes(shot,origin.x()+box.center().x()-10,origin.y()+box.top()-2,origin.x()+box.center().x()+10);
   QVERIFY2(dark>=3&&light>=3,qPrintable(QString("selection on %1: dark %2, light %3").arg(fill.name()).arg(dark).arg(light)));
  }
 }
 void blurPreviewCacheIsBounded(){
  EditDocument doc(noise({3000,2000},10));EditorCanvas canvas(doc);canvas.resize(800,600);canvas.show();QVERIFY(putgd::test::waitForWindowExposed(&canvas));
  for(int i=0;i<40;++i){auto blur=box(Shape::Kind::Blur,QRect((i*61)%2500,(i*37)%1500,500,500));blur.strength=20;doc.add(blur);}
  canvas.documentChanged();canvas.actualSize();canvas.repaint();canvas.fit();canvas.repaint();
  QVERIFY(canvas.cachedBlurBytes()<=EditorCanvas::blurCacheBytes);QVERIFY(canvas.cachedBlurBytes()>0);
 }
};
QTEST_MAIN(EditorTest)
#include "test_editor.moc"
