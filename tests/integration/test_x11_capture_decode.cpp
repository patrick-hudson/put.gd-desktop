#include "platform/x11/CaptureProcessController.h"
#include "platform/x11/CinnamonRegionCapture.h"
#include "../fixtures/ImagePluginMappings.h"
#include <QtTest>
#include <QDBusConnection>
#include <QDBusContext>
#include <QJsonArray>
#include <QPointer>
using namespace putgd;
using test::loadedImagePlugins;
namespace {
QJsonObject desktopSnapshot(){return {{"schemaVersion",1},{"action","desktop"},{"window","0"},{"windowRect",QJsonArray{}},{"layout",QJsonObject{{"outputs",QJsonArray{QJsonObject{{"identity","monitor"},{"native",QJsonArray{0,0,640,480}},{"rotation",1}}}},{"configuration","1"},{"root",QJsonArray{0,0,640,480}},{"scale",QString(64,'a')}}}};}
}
// Writes the hand-encoded PNG as Cinnamon would, so this process never encodes.
class CinnamonPngFixture final:public QObject,protected QDBusContext {
 Q_OBJECT
 Q_CLASSINFO("D-Bus Interface","org.Cinnamon")
public:
 CinnamonPngFixture(){auto bus=QDBusConnection::sessionBus();registered=bus.registerObject("/org/Cinnamon",this,QDBusConnection::ExportAllSlots)&&bus.registerService("org.Cinnamon");}
 ~CinnamonPngFixture() override{auto bus=QDBusConnection::sessionBus();if(registered)bus.unregisterService("org.Cinnamon");bus.unregisterObject("/org/Cinnamon");}
 bool registered=false;int requests=0;
public slots:
 void ScreenshotArea(bool,int,int,int,int,bool,const QString& filename){
  ++requests;QFile output(filename);if(output.open(QIODevice::WriteOnly))output.write(test::syntheticPng);
 }
};
// The resident decodes PNG that it has already verified: the capture helper's
// output in the Cinnamon Core resident and Cinnamon's own region frames.
// Qt 6.4.2 and 6.8.2 load every installed image plugin for a "PNG"-named
// reader unless format detection is off.
class X11CaptureDecodeTest final:public QObject {
 Q_OBJECT
private slots:
 void initTestCase(){
  // A synthetic plugin is always installed, so an in-process plugin load is
  // observable even where the system ships no image plugins.
  QCoreApplication::addLibraryPath(QStringLiteral(PUTGD_IMAGE_PLUGIN_FIXTURE_DIR));
  QCOMPARE(loadedImagePlugins(),QStringList{});
 }
 void cleanup(){QCOMPARE(loadedImagePlugins(),QStringList{});}
 void helperPngDecodesWithoutPlugins(){
  // The fixture helper encodes its 3x2 blue PNG in its own process.
  CaptureProcessController controller(PUTGD_CAPTURE_FIXTURE);int completed=0;PayloadResult result;
  controller.start(desktopSnapshot(),{},[&](PayloadResult value){result=std::move(value);++completed;});
  QTRY_COMPARE(completed,1);QVERIFY2(!result.error,qPrintable(result.error.code));
  QCOMPARE(result.payload.image.size(),QSize(3,2));QCOMPARE(result.payload.image.pixelColor(1,1),QColor(Qt::blue));
 }
 void cinnamonRegionPngDecodesWithoutPlugins(){
  CinnamonPngFixture fixture;QVERIFY(fixture.registered);
  const CaptureFrame native{"fixture",QRectF(30,40,3,2),QRect(30,40,3,2),1,0,{}};
  auto* request=new CinnamonRegionCapture({native},{});QPointer<CinnamonRegionCapture> guard=request;bool done=false;QList<CaptureFrame> frames;Error error;
  connect(request,&CinnamonRegionCapture::acquired,this,[&](QList<CaptureFrame> captured,const Error& value){frames=std::move(captured);error=value;done=true;});
  request->start();QTRY_VERIFY(done);QVERIFY2(!error,qPrintable(error.code));QCOMPARE(fixture.requests,1);
  QCOMPARE(frames.size(),1);QVERIFY(test::syntheticPixels(frames[0].pixels));QTRY_VERIFY(guard.isNull());
 }
};
QTEST_GUILESS_MAIN(X11CaptureDecodeTest)
#include "test_x11_capture_decode.moc"
