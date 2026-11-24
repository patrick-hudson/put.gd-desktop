// #1893: capture style through the resident's operation service and the real image-job child
// (putgd-ui --image-job). Off (absent or enabled false) must leave every capture byte for byte as
// it was; on, the capture is put on its canvas, the original kept beside it with a local copy.
#include "fixtures/U2Fixtures.h"
#include "core/AllocatorPolicy.h"
#include "core/CaptureStyle.h"
#include "fixtures/Waits.h"
#include <QBuffer>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDirIterator>
#include <QFileInfo>
#include <QRandomGenerator>
using namespace putgd;using namespace putgd::test;

namespace {
QImage noise(QSize size,quint32 seed){
 QImage image(size,QImage::Format_RGB32);QRandomGenerator random(seed);
 for(int y=0;y<size.height();++y){auto* line=reinterpret_cast<QRgb*>(image.scanLine(y));for(int x=0;x<size.width();++x)line[x]=random.generate()|0xff000000U;}
 return image;
}
QByteArray encoded(const QImage& image){QByteArray png;QBuffer buffer(&png);buffer.open(QIODevice::WriteOnly);image.save(&buffer,"PNG");return png;}
struct Call {Error error;QJsonObject result;bool done=false;};
Call call(std::function<void(JsonReply)> start){
 Call outcome;start([&](Error e,QJsonObject r){outcome={e,r,true};});
 (void)putgd::test::waitFor([&]{return outcome.done;},15000);
 return outcome;
}
struct Fixture {
 QTemporaryDir dir;ProviderRegistry registry;std::shared_ptr<OpaqueProvider> provider=std::make_shared<OpaqueProvider>();
 std::unique_ptr<SettingsService> settings;Source source;std::unique_ptr<OperationService> operations;
 // `style` is preferences.captureStyle; an undefined value leaves the key out.
 Fixture(QImage image,QJsonValue style,bool keepLocal=false,QJsonValue maxFileBytes={}){
  registry.add("opaque",provider);
  settings=std::make_unique<SettingsService>(dir.filePath("config.json"),[this](auto id,auto value){return registry.validate(id,value);});
  auto doc=configuration(dir.path());auto archive=doc["archive"].toObject();archive["keepUploads"]=keepLocal;doc["archive"]=archive;
  if(!maxFileBytes.isUndefined()){auto limits=doc["limits"].toObject();limits["maxFileBytes"]=maxFileBytes;doc["limits"]=limits;}
  if(!style.isUndefined()){auto preferences=doc["preferences"].toObject();preferences["captureStyle"]=style;doc["preferences"]=preferences;}
  if(!settings->save(doc,{}))qFatal("fixture configuration rejected");
  source.payload.kind="image";source.payload.image=std::move(image);
  operations=std::make_unique<OperationService>(dir.filePath("data"),*settings,registry,source);
 }
 QJsonObject inspect(const QString& id){return call([&](JsonReply r){operations->inspect(id,r);}).result;}
 QString capture(const QString& action="region"){return call([&](JsonReply r){operations->invoke(action,r,{});}).result["operationId"].toString();}
 QByteArray artifact(const QString& id){
  HistoryStore journal(operations->databasePath());const auto item=journal.operation(id).items.first();
  QFile file(item.artifact.path);return file.open(QIODevice::ReadOnly)?file.readAll():QByteArray();
 }
 QStringList archived(){QStringList files;QDirIterator it(dir.path()+"/archive",QDir::Files,QDirIterator::Subdirectories);while(it.hasNext())files.append(it.next());files.sort();return files;}
};
}

class CaptureStyleTest:public QObject {
 Q_OBJECT
private slots:
 void initTestCase(){QVERIFY(applyResidentAllocatorPolicy());}
 // The hard rule: absent and enabled false both leave the artifact exactly as put.gd always stored it.
 void offLeavesTheCaptureByteForByte_data(){
  QTest::addColumn<QJsonValue>("style");
  QTest::newRow("absent")<<QJsonValue(QJsonValue::Undefined);
  QTest::newRow("enabled-false")<<QJsonValue(QJsonObject{{"enabled",false},{"backdrop",QJsonObject{{"kind","color"},{"color","#ff0000"}}},{"padding","large"}});
 }
 void offLeavesTheCaptureByteForByte(){
  QFETCH(QJsonValue,style);
  const auto image=noise({160,90},3);
  Fixture f(image,style,true);PUTGD_TRY_VERIFY(f.operations->ready());
  const auto id=f.capture();QVERIFY(!id.isEmpty());
  PUTGD_TRY_COMPARE_WITH_TIMEOUT(f.inspect(id)["state"].toString(),QString("completed"),20000);
  QCOMPARE(f.artifact(id),encoded(image));
  QVERIFY(!f.inspect(id).contains("style"));
  // One local copy, no -original beside it.
  const auto files=f.archived();QCOMPARE(files.size(),1);QCOMPARE(QFile(files.first()).size(),qint64(encoded(image).size()));
 }
 // On: the capture on its canvas, its pixels unchanged inside the rounded outline; the original kept beside it.
 void onStylesTheCaptureAndKeepsTheOriginal(){
  const auto image=noise({400,240},5);
  const auto style=CaptureStyle::preset("paper");
  Fixture f(image,style.toJson(),true);PUTGD_TRY_VERIFY(f.operations->ready());
  const auto id=f.capture("window");QVERIFY(!id.isEmpty());
  PUTGD_TRY_COMPARE_WITH_TIMEOUT(f.inspect(id)["state"].toString(),QString("completed"),20000);
  const auto op=f.inspect(id);QCOMPARE(op["style"].toObject()["applied"].toBool(),true);
  const auto styled=QImage::fromData(f.artifact(id),"PNG").convertToFormat(QImage::Format_RGB32);
  const auto geometry=style.geometry(image.size());
  QCOMPARE(styled.size(),geometry.canvas);
  // The corners are rounded; everything a radius in from them is the capture's own pixels.
  const int inset=geometry.radius;
  for(int y=inset;y<image.height()-inset;y+=7)for(int x=inset;x<image.width()-inset;x+=5)
   QCOMPARE(styled.pixel(x+geometry.padding,y+geometry.padding),image.pixel(x,y));
  // The padding is the backdrop.
  QCOMPARE(styled.pixel(2,2),QColor("#efeee9").rgb());
  const auto files=f.archived();QCOMPARE(files.size(),2);
  const auto original=std::find_if(files.cbegin(),files.cend(),[](const QString& path){return QFileInfo(path).completeBaseName().endsWith("-original");});
  QVERIFY(original!=files.cend());
  QFile kept(*original);QVERIFY(kept.open(QIODevice::ReadOnly));QCOMPARE(kept.readAll(),encoded(image));
  QCOMPARE(op["style"].toObject()["original"].toObject()["status"].toString(),QString("saved"));
  // The private copy before styling is gone once it is archived.
  QVERIFY(QDir(f.dir.filePath("data/styled")).entryList(QDir::Files).isEmpty());
 }
 // A picture backdrop that can't be read never fails the capture: Graphite stands in, and History says so.
 void aMissingPictureUsesGraphite(){
  const auto image=noise({200,120},9);
  QJsonObject style{{"enabled",true},{"backdrop",QJsonObject{{"kind","picture"},{"picture","/nonexistent/putgd-backdrop.png"}}}};
  Fixture f(image,style);PUTGD_TRY_VERIFY(f.operations->ready());
  const auto id=f.capture();QVERIFY(!id.isEmpty());
  PUTGD_TRY_COMPARE_WITH_TIMEOUT(f.inspect(id)["state"].toString(),QString("completed"),20000);
  const auto op=f.inspect(id);
  QCOMPARE(op["style"].toObject()["pictureMissing"].toBool(),true);
  QVERIFY(op["note"].toString().contains("Graphite"));
  const auto styled=QImage::fromData(f.artifact(id),"PNG");
  QCOMPARE(styled.size(),CaptureStyle::fromJson(style).geometry(image.size()).canvas);
  // Graphite's gradient starts at its first stop in the top-left corner.
  QVERIFY(qAbs(qRed(styled.pixel(0,0))-0x38)<=2);
 }
 // The store refuses the styled PNG (over the Largest file limit) where the capture as it was fits: it is stored instead.
 void aStyleThatWouldPassTheFileLimitIsLeftOff(){
  QImage plain(300,200,QImage::Format_RGB32);plain.fill(Qt::white);
  QVERIFY(encoded(plain).size()<4096);
  Fixture f(plain,CaptureStyle::preset("graphite").toJson(),false,4096);PUTGD_TRY_VERIFY(f.operations->ready());
  const auto id=f.capture();QVERIFY(!id.isEmpty());
  PUTGD_TRY_COMPARE_WITH_TIMEOUT(f.inspect(id)["state"].toString(),QString("completed"),20000);
  QCOMPARE(f.artifact(id),encoded(plain));
  const auto op=f.inspect(id);QVERIFY(!op.contains("style"));QVERIFY2(op["note"].toString().contains("too large to store or upload"),qPrintable(op["note"].toString()));
 }
};

QTEST_MAIN(CaptureStyleTest)
#include "test_capture_style.moc"
