#include "app/UiControl.h"
#include "ui/StdioDevice.h"
#include <QProcess>
#include <QTimer>
#include <QJsonDocument>
#include <QtTest>
#include <cstdio>
#include <cstring>
#include <memory>
#include "fixtures/Waits.h"
using namespace putgd;
class Duplex final:public QIODevice {
public:
 QByteArray input,output; qint64 writeLimit=1024*1024;
 Duplex(){open(QIODevice::ReadWrite|QIODevice::Unbuffered);}
 bool isSequential()const override{return true;}
 qint64 bytesAvailable()const override{return input.size()+QIODevice::bytesAvailable();}
 void deliver(QByteArray bytes){input+=bytes;emit readyRead();}
 void writable(){emit bytesWritten(0);}
 qint64 readData(char* data,qint64 max)override{auto count=qMin(max,qint64(input.size()));memcpy(data,input.constData(),count);input.remove(0,count);return count;}
 qint64 writeData(const char* data,qint64 count)override{auto amount=qMin(count,writeLimit);output.append(data,amount);return amount;}
};
class UiControlTest:public QObject {
 Q_OBJECT
private slots:
 void fragmentedFramesAndPartialWrites(){
  Duplex device;UiControl channel(device);int received=0;Error error;
  connect(&channel,&UiControl::failed,this,[&](Error e){error=e;});
  connect(&channel,&UiControl::frame,this,[&](QString type,QString seq,QJsonObject payload){++received;QCOMPARE(type,QString("ready"));QCOMPARE(seq,QString("1"));QCOMPARE(payload["residentEpoch"].toString(),QString("fixture"));});
  const auto incoming=QJsonDocument(QJsonObject{{"version",1},{"type","ready"},{"sequence","1"},{"payload",QJsonObject{{"residentEpoch","fixture"}}}}).toJson(QJsonDocument::Compact)+'\n';
  device.deliver(incoming.first(7));QCOMPARE(received,0);device.deliver(incoming.mid(7));QCOMPARE(received,1);QVERIFY(!error);
  device.writeLimit=3;QCOMPARE(channel.send("activate",{{"view","settings"}}),QString("1"));
  for(int i=0;i<100&&!device.output.endsWith('\n');++i)device.writable();
  const auto written=QJsonDocument::fromJson(device.output).object();QCOMPARE(written["type"].toString(),QString("activate"));QCOMPARE(written["payload"].toObject()["view"].toString(),QString("settings"));
 }
 void rejectsUnboundedAndInvalidFrames(){
  for(const QByteArray& bad:{QByteArray("{oops}\n"),QByteArray("{\"version\":1,\"type\":\"ready\",\"sequence\":\"2\",\"payload\":{}}\n"),QByteArray(UiControl::maximumFrameBytes+1,'x')}){
   Duplex device;UiControl channel(device);int errors=0;connect(&channel,&UiControl::failed,this,[&](Error e){++errors;QVERIFY(!e.code.isEmpty());QVERIFY(e.details.isEmpty());});
   device.deliver(bad);QCOMPARE(errors,1);device.deliver(bad);QCOMPARE(errors,1);QVERIFY(channel.send("activate").isEmpty());
  }
 }
 void actualHelperPipesCarryLargeFrameWithoutBlocking(){
#ifndef Q_OS_UNIX
  QSKIP("Native helper pipes are implemented for the Linux target");
#else
  for(bool detached:{false,true}){
  QProcess peer;peer.setProcessChannelMode(QProcess::SeparateChannels);UiControl channel(peer);bool ready=false;Error error;
  connect(&channel,&UiControl::failed,this,[&](Error e){error=e;});
  connect(&channel,&UiControl::frame,this,[&](QString type,QString,QJsonObject data){if(type=="ready"){QCOMPARE(data["receivedBytes"].toInt(),2*1024*1024);ready=true;}});
  peer.start(QCoreApplication::applicationFilePath(),{detached?"--private-pipe-peer":"--pipe-peer"});PUTGD_TRY_COMPARE(peer.state(),QProcess::Running);
  int ticks=0;QTimer heartbeat;heartbeat.setInterval(1);connect(&heartbeat,&QTimer::timeout,this,[&]{++ticks;});heartbeat.start();
  QVERIFY(!channel.send("bootstrap",{{"text",QString(2*1024*1024,'x')}}).isEmpty());PUTGD_TRY_VERIFY_WITH_TIMEOUT(ready,5000);QVERIFY(!error);QVERIFY(ticks>0);
  channel.send("shutdown");PUTGD_TRY_COMPARE(peer.state(),QProcess::NotRunning);QCOMPARE(peer.exitStatus(),QProcess::NormalExit);QCOMPARE(peer.exitCode(),0);const auto diagnostic=peer.readAllStandardError();if(detached)QVERIFY(diagnostic.contains("synthetic plugin stdout"));else QVERIFY(diagnostic.isEmpty());
  }
#endif
 }
 void validSequenceAndStop(){
  Duplex device;UiControl channel(device);int frames=0,errors=0;connect(&channel,&UiControl::frame,this,[&]{++frames;});connect(&channel,&UiControl::failed,this,[&]{++errors;});
  auto message=[](QString sequence){return QJsonDocument(QJsonObject{{"version",1},{"type","ready"},{"sequence",sequence},{"payload",QJsonObject{}}}).toJson(QJsonDocument::Compact)+'\n';};
  device.deliver(message("1")+message("2"));QCOMPARE(frames,2);device.deliver(message("2"));QCOMPARE(errors,1);QCOMPARE(frames,2);
  Duplex second;UiControl stopped(second);stopped.stop();second.deliver(message("1"));QVERIFY(stopped.send("ready").isEmpty());
 }
};
int main(int argc,char** argv){
 std::unique_ptr<UiControlPipes> handles;
 if(argc==2&&std::strcmp(argv[1],"--private-pipe-peer")==0){
  handles=std::make_unique<UiControlPipes>();if(!handles->valid())return 2;
  // Exercise a plugin's ordinary stdout write before Qt constructs the event loop.
  std::fputs("synthetic plugin stdout\n",stdout);std::fflush(stdout);
 }
 QCoreApplication app(argc,argv);
 if(handles||app.arguments().contains("--pipe-peer")){
  StdioDevice pipes(handles?handles->input():0,handles?handles->output():1);if(!pipes.isOpen())return 2;UiControl channel(pipes);
  QObject::connect(&channel,&UiControl::failed,&app,[&]{app.exit(3);});
  QObject::connect(&pipes,&QIODevice::readChannelFinished,&app,[&]{app.quit();});
  QObject::connect(&channel,&UiControl::frame,&app,[&](QString type,QString,QJsonObject payload){if(type=="bootstrap")channel.send("ready",{{"receivedBytes",payload["text"].toString().size()}});else if(type=="shutdown")app.quit();});
  return app.exec();
 }
 UiControlTest test;return QTest::qExec(&test,argc,argv);
}
#include "test_ui_control.moc"
