#include "platform/kde/KdeRegionProcess.h"
#include <QCoreApplication>
#include <QJsonDocument>
#include <QtTest>
#include <chrono>
#include <iostream>
#include <limits>
#include <memory>
#include <thread>
#include <algorithm>
#include <csignal>
#include "fixtures/Waits.h"

using namespace putgd;
namespace {
QList<CaptureFrame> frames() {
 CaptureFrame left; left.identity="LEFT"; left.logicalRect=QRectF(-640,0,640,480);
 CaptureFrame right; right.identity="RIGHT"; right.logicalRect=QRectF(0,0,800,600);
 return {left,right};
}
// This executable is also a disposable pipe peer. It proves the resident's
// protocol/normal-exit gate, not real compositor selection or cursor behavior.
int fixture(const QString& mode) {
 std::string line; if(!std::getline(std::cin,line)) return 3;
 const auto first=QJsonDocument::fromJson(QByteArray::fromStdString(line)).object()["payload"].toObject();
 const auto nonce=first["nonce"],generation=first["generation"]; quint64 sequence=0;
 auto send=[&](const QString& type,QJsonObject payload) {
  const auto bytes=QJsonDocument(QJsonObject{{"version",1},{"sequence",QString::number(++sequence)},
      {"type",type},{"payload",payload}}).toJson(QJsonDocument::Compact);
  std::cout<<bytes.constData()<<'\n'<<std::flush;
 };
 auto envelope=[&]{return QJsonObject{{"nonce",nonce},{"generation",generation}};};
 if(mode=="early-eof") return 0;
 if(mode=="malformed") { std::cout<<"{broken-frame}\n"<<std::flush;std::this_thread::sleep_for(std::chrono::seconds(10));return 0; }
 auto ready=envelope(); ready["build"]=first["build"];
 if(mode=="wrong-build") ready["build"]=QJsonObject{};
 if(mode=="stale-generation") ready["generation"]="18446744073709551615";
 send("ready",ready);
 if(mode=="wrong-build" || mode=="stale-generation") return 0;
 if(!std::getline(std::cin,line)) return 3;
 const auto next=QJsonDocument::fromJson(QByteArray::fromStdString(line)).object();
 if(mode=="wait-cancel") {
  while(next["type"]!="cancel" && std::getline(std::cin,line)) {
   if(QJsonDocument::fromJson(QByteArray::fromStdString(line)).object()["type"]=="cancel") break;
  }
  auto cancelled=envelope(); cancelled["code"]="cancelled";send("cancelled",cancelled);return 0;
 }
 if(mode=="crash-before-teardown") { std::raise(SIGKILL); return 8; }
 if(mode=="terminal-flood" || mode=="mismatched-terminal") {
  auto terminal=envelope();terminal["code"]=mode=="terminal-flood"?"cancelled":"display_changed";
  for(int i=0;i<80;++i) { send("cancelled",terminal);std::this_thread::sleep_for(std::chrono::milliseconds(100)); }
  return 0;
 }
 auto result=envelope(); result["rectangle"]=kdeRegionRectangle(mode=="outside"?QRectF(900,0,20,20):QRectF(-20,20,60,40));
 send("result",result);
 if(mode=="duplicate") send("result",result);
 if(!std::getline(std::cin,line)) return 3;
 if(mode=="crash") { std::raise(SIGKILL); return 8; }
 if(mode=="no-exit") { std::this_thread::sleep_for(std::chrono::seconds(10));return 0; }
 std::this_thread::sleep_for(std::chrono::milliseconds(25));
 return mode=="nonzero-exit"?7:0;
}
}
class KdeRegionControlTest:public QObject {
 Q_OBJECT
private slots:
 void layoutAndRectangleRoundTrip() {
  Error e; const auto encoded=kdeRegionLayout(frames(),e);QVERIFY(!e);QCOMPARE(encoded.size(),2);
  const auto decoded=kdeRegionLayout(encoded,e);QVERIFY(!e);QVERIFY(kdeRegionLayoutsMatch(frames(),decoded));
  const QRectF selected(-20,20,60,40);QCOMPARE(kdeRegionRectangle(kdeRegionRectangle(selected),decoded,e),selected);QVERIFY(!e);
  auto reordered=decoded;std::reverse(reordered.begin(),reordered.end());QVERIFY(kdeRegionLayoutsMatch(frames(),reordered));
  reordered[0].logicalRect.translate(1,0);QVERIFY(!kdeRegionLayoutsMatch(frames(),reordered));
 }
 void invalidLayoutIsRejected() {
  for(int kind=0;kind<5;++kind) {
   auto f=frames();
   if(kind==0) f[1].identity=f[0].identity;
   if(kind==1) f[0].logicalRect.setWidth(0);
   if(kind==2) f[0].logicalRect.setX(std::numeric_limits<double>::infinity());
   if(kind==3) f[0].pixels=QImage(1,1,QImage::Format_ARGB32);
   if(kind==4) f[0].rotation=45;
   Error e;QVERIFY(kdeRegionLayout(f,e).isEmpty());QCOMPARE(e.code,QString("capture_geometry"));
  }
  Error e;auto encoded=kdeRegionLayout(frames(),e);auto row=encoded[0].toObject();row["pixels"]="forbidden";encoded[0]=row;
  QVERIFY(kdeRegionLayout(encoded,e).isEmpty());QVERIFY(e);
 }
 void invalidSelectionIsRejected() {
  for(const auto& rectangle:{QRectF(0,0,0,20),QRectF(900,0,20,20),QRectF(-700,0,100,20),
                            QRectF(0,std::numeric_limits<double>::quiet_NaN(),20,20)}) {
   Error e;QVERIFY(kdeRegionRectangle(kdeRegionRectangle(rectangle),frames(),e).isEmpty());QVERIFY(e);
  }
 }
 void privatePeerExitGate_data() {
  QTest::addColumn<QByteArray>("mode");QTest::addColumn<bool>("success");
  QTest::newRow("normal-exit-after-result")<<QByteArray("valid")<<true;
  for(const auto* mode:{"early-eof","wrong-build","stale-generation","outside","duplicate","nonzero-exit","crash","no-exit","malformed"})
   QTest::newRow(mode)<<QByteArray(mode)<<false;
 }
 void privatePeerExitGate() {
  QFETCH(QByteArray,mode);QFETCH(bool,success);
  qputenv("PUTGD_TEST_KDE_REGION_HELPER",mode);
  KdeRegionProcess process(QCoreApplication::applicationFilePath());int callbacks=0;QRectF result;Error error;
  process.start(frames(),[&](QRectF rect,Error e){QVERIFY(!process.busy());result=rect;error=e;++callbacks;});
  PUTGD_TRY_COMPARE(callbacks,1);qunsetenv("PUTGD_TEST_KDE_REGION_HELPER");
  QCOMPARE(!bool(error),success);QCOMPARE(result.isEmpty(),!success);
  putgd::test::settle(40);PUTGD_TRY_COMPARE(callbacks,1);PUTGD_TRY_COMPARE(process.processId(),qint64(0));
  const auto exit=process.diagnostics()["lastExit"].toObject();
  QVERIFY(exit["processId"].toInteger()>0);QVERIFY(exit["startTicks"].toString().toULongLong()>0);
  if(success) { QVERIFY(!process.cleanupRequired());QVERIFY(exit["teardownCertified"].toBool()); QCOMPARE(exit["code"].toInt(),0);QCOMPARE(exit["status"].toString(),QString("normal")); }
 }
 void cancellationAndBusyAreBounded() {
  qputenv("PUTGD_TEST_KDE_REGION_HELPER","wait-cancel");
  KdeRegionProcess process(QCoreApplication::applicationFilePath());int cancelled=0,busy=0;
  process.start(frames(),[&](QRectF rect,Error e){QVERIFY(rect.isEmpty());QCOMPARE(e.code,QString("cancelled"));++cancelled;});
  process.start(frames(),[&](QRectF rect,Error e){QVERIFY(rect.isEmpty());QCOMPARE(e.code,QString("busy_selecting"));++busy;});
  QCOMPARE(busy,1);putgd::test::settle(30);process.cancel();process.cancel();PUTGD_TRY_COMPARE(cancelled,1);
  qunsetenv("PUTGD_TEST_KDE_REGION_HELPER");QVERIFY(!process.busy());
  const auto exit=process.diagnostics()["lastExit"].toObject();
  QCOMPARE(exit["code"].toInt(),0);QCOMPARE(exit["status"].toString(),QString("normal"));
  QVERIFY(exit["teardownCertified"].toBool());QVERIFY(!process.cleanupRequired());
  QVERIFY(exit["processId"].toInteger()>0);QVERIFY(exit["startTicks"].toString().toULongLong()>0);
 }
 void terminalFramesCannotExtendShutdown_data() {
  QTest::addColumn<QByteArray>("mode");QTest::addColumn<QString>("expectedCode");
  QTest::newRow("terminal-flood")<<QByteArray("terminal-flood")<<QString("cancelled");
  QTest::newRow("mismatched-terminal")<<QByteArray("mismatched-terminal")<<QString("capture_protocol");
 }
 void terminalFramesCannotExtendShutdown() {
  QFETCH(QByteArray,mode);QFETCH(QString,expectedCode);qputenv("PUTGD_TEST_KDE_REGION_HELPER",mode);
  KdeRegionProcess process(QCoreApplication::applicationFilePath());int callbacks=0;Error error;QElapsedTimer elapsed;elapsed.start();
  process.start(frames(),[&](QRectF rectangle,Error e){QVERIFY(rectangle.isEmpty());error=e;++callbacks;});
  PUTGD_TRY_COMPARE_WITH_TIMEOUT(callbacks,1,4000);qunsetenv("PUTGD_TEST_KDE_REGION_HELPER");
  QVERIFY2(elapsed.elapsed()<4000,"Repeated terminal frames postponed the shutdown bound");
  QCOMPARE(error.code,expectedCode);QVERIFY(!process.busy());QCOMPARE(process.processId(),qint64(0));
  putgd::test::settle(40);PUTGD_TRY_COMPARE(callbacks,1);
 }
 void abruptPreFenceExitBlocksFurtherSelection() {
  qputenv("PUTGD_TEST_KDE_REGION_HELPER","crash-before-teardown");
  KdeRegionProcess process(QCoreApplication::applicationFilePath());int completed=0;Error error;
  process.start(frames(),[&](QRectF r,Error e){QVERIFY(r.isEmpty());error=e;++completed;});
  PUTGD_TRY_COMPARE(completed,1);qunsetenv("PUTGD_TEST_KDE_REGION_HELPER");
  QCOMPARE(error.code,QString("capture_crashed"));QVERIFY(process.cleanupRequired());QVERIFY(!process.busy());
  QVERIFY(!process.diagnostics()["lastExit"].toObject()["teardownCertified"].toBool());
  process.start(frames(),[&](QRectF r,Error e){QVERIFY(r.isEmpty());QCOMPARE(e.code,QString("capture_cleanup_pending"));++completed;});
  QCOMPARE(completed,2);QCOMPARE(process.processId(),qint64(0));
 }
 void ownerDestructionCannotReenterCallback_data() {
  QTest::addColumn<bool>("explicitShutdown");
  QTest::newRow("explicit-idempotent-shutdown")<<true;
  QTest::newRow("active-owner-destructor")<<false;
 }
 void ownerDestructionCannotReenterCallback() {
  QFETCH(bool,explicitShutdown);qputenv("PUTGD_TEST_KDE_REGION_HELPER","wait-cancel");
  bool tearingDown=false;int callbacks=0,reentries=0;
  struct Owner {
   bool& tearingDown;KdeRegionProcess process;
   Owner(bool& flag):tearingDown(flag),process(QCoreApplication::applicationFilePath()){}
   ~Owner(){tearingDown=true;}
  };
  auto owner=std::make_unique<Owner>(tearingDown);auto* raw=owner.get();
  owner->process.start(frames(),[&,raw](QRectF,Error){
   ++callbacks;
   // A callback during destruction would reenter its partially dying owner.
   if(tearingDown){++reentries;raw->process.cancel();}
  });
  PUTGD_TRY_VERIFY(owner->process.processId()>0);
  QPointer<QProcess> child=owner->process.findChild<QProcess*>();QVERIFY(child);
  if(explicitShutdown){
   owner->process.shutdown();owner->process.shutdown();
   QVERIFY(!owner->process.busy());QCOMPARE(owner->process.processId(),qint64(0));
   QCOMPARE(child->state(),QProcess::NotRunning);
  }
  owner.reset();QVERIFY(child.isNull());
  putgd::test::settle(40);qunsetenv("PUTGD_TEST_KDE_REGION_HELPER");
  QVERIFY(tearingDown);QCOMPARE(callbacks,0);QCOMPARE(reentries,0);
 }
 void missingHelperFailsOnce() {
  KdeRegionProcess process("/does-not-exist-putgd-kde-region");int callbacks=0;
  process.start(frames(),[&](QRectF r,Error e){QVERIFY(r.isEmpty());QCOMPARE(e.code,QString("capture_start_failed"));++callbacks;});
  PUTGD_TRY_COMPARE(callbacks,1);QVERIFY(!process.busy());putgd::test::settle(30);PUTGD_TRY_COMPARE(callbacks,1);
 }
};
int main(int argc,char** argv) {
 const auto mode=qEnvironmentVariable("PUTGD_TEST_KDE_REGION_HELPER");
 if(!mode.isEmpty()) return fixture(mode);
 QCoreApplication app(argc,argv);KdeRegionControlTest test;return QTest::qExec(&test,argc,argv);
}
#include "test_kde_region_control.moc"
