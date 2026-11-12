#include "core/SanitizedPng.h"
#include "editor/EditDocument.h"
#include "editor/EditorCommands.h"
#include "editor/HighlightFilter.h"
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
QImage fixture(const char* name){QImage image(QString(PUTGD_TEST_FIXTURES)+"/"+name);return image.convertToFormat(QImage::Format_RGB32);}
Shape box(Shape::Kind kind,QRect rect){Shape shape;shape.kind=kind;shape.rect=rect;if(kind==Shape::Kind::Redact)shape.color=Qt::black;if(kind==Shape::Kind::Highlight)shape.color=QColor(250,204,21);return shape;}
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
 void snappingNeverTracesRedactedText(){
  // A word under a redaction is not text any more: no word there, and a line drag skips it.
  const auto paper=fixture("highlight-lcd-text.png");const auto whole=highlight::lineAt(paper,QPoint(60,25),{});QVERIFY(!whole.isNull());
  const auto word=highlight::wordAt(paper,QPoint(whole.left()+4,25),{});QVERIFY(!word.isNull());QVERIFY(word.left()<=whole.left()+4);  // the line's first word
  const QList<QRect> redactions{word};
  QVERIFY(highlight::wordAt(paper,word.center(),redactions).isNull());
  // The line found from a point further along starts after the redacted word (padding may touch it).
  const auto line=highlight::lineAt(paper,QPoint(300,25),redactions);QVERIFY(!line.isNull());QVERIFY2(line.left()>word.center().x(),qPrintable(QString("line %1 word %2").arg(line.left()).arg(word.center().x())));
  // A drag across the line snaps to the ink after the redaction; a drag over the redacted word alone finds no text.
  const auto snapped=highlight::snapToText(paper,QRect(40,24,440,3),redactions);QVERIFY(snapped.left()>word.center().x());QVERIFY(snapped.height()>=12);
  const QRect over(word.left(),word.top()+word.height()/2,word.width(),2);QCOMPARE(highlight::snapToText(paper,over,redactions),over);
  EditDocument doc(paper);doc.add(box(Shape::Kind::Redact,word));QJsonArray created;QString code,message;
  QVERIFY(!Commands::apply(doc,{QJsonObject{{"op","add"},{"shape",QJsonObject{{"kind","highlight"},{"at",QJsonArray{word.center().x(),word.center().y()}},{"snap","word"}}}}},created,code,message));
  QVERIFY2(message.contains("No text at that point"),qPrintable(message));
 }
 // #1362: step numbers, pen, ellipse, line and pixelate.
 void newMarksNeverRevealRedactedPixelsInAnyOrder_data(){
  QTest::addColumn<bool>("redactFirst");QTest::addColumn<bool>("cropped");
  QTest::newRow("redact-first")<<true<<false;QTest::newRow("redact-last")<<false<<false;QTest::newRow("redact-last-cropped")<<false<<true;
 }
 void newMarksNeverRevealRedactedPixelsInAnyOrder(){
  QFETCH(bool,redactFirst);QFETCH(bool,cropped);
  const QRect mask(40,30,60,24);const auto base=noise({220,160},31);
  const auto a=secretVariant(base,mask,41),b=secretVariant(base,mask,42);
  auto edit=[&](const QImage& source){
   EditDocument doc(source);if(redactFirst)doc.add(box(Shape::Kind::Redact,mask));
   // A pixelation and a blur straddling the mask read only redacted pixels.
   auto pixelate=box(Shape::Kind::Blur,mask.adjusted(-13,-7,17,9));pixelate.pixelate=true;pixelate.strength=9;doc.add(pixelate);
   auto mosaic=box(Shape::Kind::Blur,QRect(mask.right()+1,mask.top(),20,mask.height()));mosaic.pixelate=true;mosaic.strength=64;doc.add(mosaic);
   Shape step;step.kind=Shape::Kind::Step;step.rect=QRect(mask.left()+4,mask.top()-6,36,36);step.number=12;doc.add(step);
   Shape pen;pen.kind=Shape::Kind::Pen;pen.width=6;pen.points={{30,40},{55,36},{80,52},{120,44}};doc.add(pen);
   doc.add(box(Shape::Kind::Ellipse,mask.adjusted(-6,-6,6,6)));
   Shape line;line.kind=Shape::Kind::Line;line.from={10,10};line.to=mask.center();line.width=8;doc.add(line);
   if(!redactFirst)doc.add(box(Shape::Kind::Redact,mask));
   if(cropped)doc.setCrop(QRect(45,20,120,80));
   return doc;
  };
  const auto first=edit(a),second=edit(b);
  QCOMPARE(first.render(),second.render());
  const auto png=first.exportPng();QVERIFY(!png.isEmpty());QCOMPARE(png,second.exportPng());QVERIFY(!inspectSanitizedPng(png));
 }
};
QTEST_MAIN(EditorTest)
#include "test_editor.moc"
