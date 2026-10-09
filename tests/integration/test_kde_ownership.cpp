#include "platform/kde/KdeSourceAdapter.h"
#include <QtTest>
#include <QDBusConnection>
#include <QDBusContext>
#include <QDBusMessage>
#include <QDBusUnixFileDescriptor>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QGuiApplication>
#include <unistd.h>
using namespace putgd;
namespace {
const QString frozenId="{00000000-0000-0000-0000-000000000001}";
const QString otherId="{00000000-0000-0000-0000-000000000002}";
}
// Disposable-bus fixtures prove transport, ownership, and frozen identity. They
// cannot establish KWin desktop acceptance or actual capture permission.
class ScreenshotFixture final:public QObject,protected QDBusContext {
 Q_OBJECT
 Q_CLASSINFO("D-Bus Interface","org.kde.KWin.ScreenShot2")
 Q_PROPERTY(uint Version READ version)
public:
 uint version()const{return 4;}
 QString windowId=frozenId;int captures=0;
public slots:
 QVariantMap CaptureActiveWindow(const QVariantMap&,const QDBusUnixFileDescriptor& descriptor){
  ++captures;const auto id=windowId;const int fd=dup(descriptor.fileDescriptor());const auto request=message();const auto bus=connection();setDelayedReply(true);
  QTimer::singleShot(30,this,[fd,request,bus,id]{
   QImage image(2,2,QImage::Format_ARGB32);image.fill(Qt::cyan);
   const auto written=write(fd,image.constBits(),image.sizeInBytes());Q_UNUSED(written);close(fd);
   const QVariantMap metadata{{"type","raw"},{"format",uint(image.format())},{"width",uint(2)},{"height",uint(2)},{"stride",uint(image.bytesPerLine())},{"scale",1.0},{"windowId",id}};
   bus.send(request.createReply(QVariant::fromValue(metadata)));
  });return {};
 }
};
class WindowInfoFixture final:public QObject,protected QDBusContext {
 Q_OBJECT
 Q_CLASSINFO("D-Bus Interface","org.kde.KWin")
public:
 QStringList queries;QVariantMap info{{"uuid",frozenId},{"resourceClass","external-fixture"},{"desktopFile","external-fixture"}};
 bool fail=false;int delayMs=0;
public slots:
 QVariantMap getWindowInfo(const QString& id){
  queries.append(id);if(fail){sendErrorReply("org.kde.KWin.SyntheticFailure","Synthetic failure");return {};}
  if(delayMs){const auto request=message();const auto bus=connection();const auto frozen=info;setDelayedReply(true);QTimer::singleShot(delayMs,this,[request,bus,frozen]{bus.send(request.createReply(QVariant::fromValue(frozen)));});return {};}
  return info;
 }
};
class KdeOwnershipTest final:public QObject {
 Q_OBJECT
 QTemporaryDir data_;
 ScreenshotFixture screenshot_;WindowInfoFixture info_;
private slots:
 void initTestCase(){
  QVERIFY(data_.isValid());qputenv("XDG_DATA_HOME",data_.path().toUtf8());QVERIFY(QDir().mkpath(data_.filePath("applications")));
  QFile entry(data_.filePath("applications/putgd.desktop"));QVERIFY(entry.open(QIODevice::WriteOnly));
  entry.write("[Desktop Entry]\nExec=\""+QCoreApplication::applicationFilePath().toUtf8()+"\"\nX-KDE-DBUS-Restricted-Interfaces=org.kde.KWin.ScreenShot2;\n");entry.close();
  // A distinct connection forces the native wire path instead of Qt local-call dispatch.
  auto bus=QDBusConnection::connectToBus(QDBusConnection::SessionBus,"putgd-kde-ownership-fixture");QVERIFY(bus.isConnected());
  QVERIFY(bus.registerObject("/org/kde/KWin/ScreenShot2",&screenshot_,QDBusConnection::ExportAllSlots|QDBusConnection::ExportAllProperties));QVERIFY(bus.registerService("org.kde.KWin.ScreenShot2"));
  QVERIFY(bus.registerObject("/KWin",&info_,QDBusConnection::ExportAllSlots));QVERIFY(bus.registerService("org.kde.KWin"));
 }
 void cleanupTestCase(){QDBusConnection::disconnectFromBus("putgd-kde-ownership-fixture");}
 void init(){screenshot_.captures=0;screenshot_.windowId=frozenId;info_.queries.clear();info_.info={{"uuid",frozenId},{"resourceClass","external-fixture"},{"desktopFile","external-fixture"}};info_.fail=false;info_.delayMs=0;}
 void externalWindowCaptureRemainsNative(){
  KdeSourceAdapter source;QTRY_VERIFY(source.capabilities()["capture"].toObject()["window"].toObject()["available"].toBool());source.setUiProcess(424242);
  auto prepared=source.prepareAcquisition("window");
  bool done=false;PayloadResult result;prepared({},[&](PayloadResult r){result=std::move(r);done=true;});QTRY_VERIFY(done);
  QVERIFY2(!result.error,qPrintable(result.error.message));QCOMPARE(screenshot_.captures,1);QCOMPARE(result.payload.image.pixelColor(0,0),QColor(Qt::cyan));
 }
 void helperWindowRejectedAfterHelperExit(){
  KdeSourceAdapter source;QTRY_VERIFY(source.capabilities()["capture"].toObject()["window"].toObject()["available"].toBool());source.setUiProcess(424242);
  info_.info["resourceClass"]="putgd-ui";auto prepared=source.prepareAcquisition("window");source.setUiProcess(0);
  bool done=false;PayloadResult result;prepared({},[&](PayloadResult r){result=std::move(r);done=true;});QTRY_VERIFY(done);
  QCOMPARE(result.error.code,QString("target_unavailable"));QVERIFY(result.payload.image.isNull());QCOMPARE(screenshot_.captures,1);QCOMPARE(info_.queries,QStringList{frozenId});
 }
 void frozenUuidQueriedAfterFocusChanges(){
  KdeSourceAdapter source;QTRY_VERIFY(source.capabilities()["capture"].toObject()["window"].toObject()["available"].toBool());source.setUiProcess(424242);
  auto prepared=source.prepareAcquisition("window");QTRY_COMPARE(screenshot_.captures,1);screenshot_.windowId=otherId;
  bool done=false;PayloadResult result;prepared({},[&](PayloadResult r){result=std::move(r);done=true;});QTRY_VERIFY(done);
  QVERIFY(!result.error);QCOMPARE(info_.queries,QStringList{frozenId});QCOMPARE(screenshot_.captures,1);
 }
 void missingWrongAndUnavailableIdentityAreRejected(){
  for(int mode=0;mode<4;++mode){
   info_.queries.clear();info_.info={{"uuid",frozenId},{"resourceClass","external-fixture"},{"desktopFile","external-fixture"}};info_.fail=mode==3;
   if(mode==0)info_.info={};
   if(mode==1)info_.info["uuid"]=otherId;
   if(mode==2)info_.info["desktopFile"]="putgd-ui";
   KdeSourceAdapter source;QTRY_VERIFY(source.capabilities()["capture"].toObject()["window"].toObject()["available"].toBool());source.setUiProcess(424242);
   bool done=false;PayloadResult result;source.acquire("window",{},[&](PayloadResult r){result=std::move(r);done=true;});QTRY_VERIFY(done);
   QCOMPARE(result.error.code,QString("target_unavailable"));QVERIFY(result.payload.image.isNull());QCOMPARE(info_.queries,QStringList{frozenId});
  }
 }
 void emptyExternalAppIdIsAccepted(){
  KdeSourceAdapter source;QTRY_VERIFY(source.capabilities()["capture"].toObject()["window"].toObject()["available"].toBool());source.setUiProcess(424242);
  info_.info["resourceClass"]=QString{};info_.info["desktopFile"]=QString{};
  bool done=false;PayloadResult result;source.acquire("window",{},[&](PayloadResult r){result=std::move(r);done=true;});QTRY_VERIFY(done);
  QVERIFY(!result.error);QCOMPARE(info_.queries,QStringList{frozenId});
 }
 void cancellationDuringIdentityLookupCompletesOnce(){
  KdeSourceAdapter source;QTRY_VERIFY(source.capabilities()["capture"].toObject()["window"].toObject()["available"].toBool());source.setUiProcess(424242);info_.delayMs=200;
  int completed=0;PayloadResult result;source.acquire("window",{},[&](PayloadResult r){result=std::move(r);++completed;});QTRY_COMPARE(info_.queries.size(),1);QCOMPARE(completed,0);
  source.cancelAcquisition();QTRY_COMPARE(completed,1);QCOMPARE(result.error.code,QString("cancelled"));QVERIFY(result.payload.image.isNull());QTest::qWait(30);QCOMPARE(completed,1);
 }
 void noHelperDoesNotQueryWindowInfo(){
  KdeSourceAdapter source;QTRY_VERIFY(source.capabilities()["capture"].toObject()["window"].toObject()["available"].toBool());source.setUiProcess(424242);source.setUiProcess(0);info_.fail=true;
  bool done=false;PayloadResult result;source.acquire("window",{},[&](PayloadResult r){result=std::move(r);done=true;});QTRY_VERIFY(done);
  QVERIFY(!result.error);QVERIFY(info_.queries.isEmpty());
 }
};
QTEST_MAIN(KdeOwnershipTest)
#include "test_kde_ownership.moc"
